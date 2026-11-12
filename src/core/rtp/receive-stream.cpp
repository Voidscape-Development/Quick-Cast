/*
Quick Cast - remote guests for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "receive-stream.hpp"

#include <algorithm>
#include <cmath>

namespace qc::rtp {

namespace {
constexpr int64_t kKeyframeRequestIntervalUs = 500'000;
}

ReceiveStream::ReceiveStream(const Config &config) : config_(config), reorder_(config.reorder) {}

void ReceiveStream::resetForNewSsrc(uint32_t ssrc)
{
	haveSsrc_ = true;
	ssrc_ = ssrc;
	sequenceUnwrapper_.reset();
	reorder_.reset();
	haveTransit_ = false;
	jitter_ = 0.0;
	expectedPrior_ = 0;
	receivedPrior_ = 0;
	receivedInOrder_ = 0;
	lastSenderReport_.reset();
}

void ReceiveStream::receiveRtp(const uint8_t *data, size_t size, int64_t nowUs, const PacketSink &onPacket,
			       const LossSink &onLoss, const RtcpSender &sendRtcp)
{
	auto parsed = parsePacket(data, size);
	if (!parsed)
		return;
	Packet packet = std::move(*parsed);

	const bool firstPacket = !haveSsrc_;
	if (firstPacket || packet.ssrc != ssrc_) {
		// A changed SSRC means the sender restarted its encoder/track; start over rather than mixing streams.
		resetForNewSsrc(packet.ssrc);
	}

	packet.extendedSequence = sequenceUnwrapper_.unwrap(packet.sequence);
	packet.arrivalUs = nowUs;

	if (receivedInOrder_ == 0 && expectedPrior_ == 0 && receivedPrior_ == 0 && !haveTransit_) {
		baseSequence_ = packet.extendedSequence;
		highestSequence_ = packet.extendedSequence;
	}
	highestSequence_ = std::max(highestSequence_, packet.extendedSequence);

	++stats_.packetsReceived;
	++receivedInOrder_;
	stats_.bytesReceived += size;

	// Interarrival jitter, RFC 3550 appendix A.8.
	const int64_t arrivalRtp =
		static_cast<int64_t>(std::llround(static_cast<double>(nowUs) * config_.clockRate / 1'000'000.0));
	const int64_t transit = arrivalRtp - static_cast<int64_t>(packet.timestamp);
	if (haveTransit_) {
		const double d = std::abs(static_cast<double>(transit - lastTransit_));
		// Ignore the discontinuity produced by 32-bit timestamp wrap-around.
		if (d < config_.clockRate * 10.0)
			jitter_ += (d - jitter_) / 16.0;
	}
	haveTransit_ = true;
	lastTransit_ = transit;
	stats_.jitterMs = jitter_ * 1000.0 / config_.clockRate;

	reorder_.push(std::move(packet), nowUs, onPacket, [&](int64_t lost) {
		if (onLoss)
			onLoss(lost);
	});

	if (config_.enableNack && sendRtcp) {
		auto nacks = reorder_.collectNacks(nowUs);
		if (!nacks.empty()) {
			stats_.nacksSent += nacks.size();
			sendRtcp(buildNack(config_.localSsrc, ssrc_, nacks));
		}
	}

	if (firstPacket)
		lastReportUs_ = nowUs - config_.reportIntervalUs; // report (and REMB) immediately on the first packet
	if (sendRtcp && nowUs - lastReportUs_ >= config_.reportIntervalUs)
		sendReports(nowUs, sendRtcp);
}

void ReceiveStream::sendReports(int64_t nowUs, const RtcpSender &sendRtcp)
{
	lastReportUs_ = nowUs;

	const int64_t expected = highestSequence_ - baseSequence_ + 1;
	const int64_t received = static_cast<int64_t>(receivedInOrder_);
	const int64_t expectedInterval = expected - expectedPrior_;
	const int64_t receivedInterval = received - receivedPrior_;
	expectedPrior_ = expected;
	receivedPrior_ = received;

	const int64_t lostInterval = expectedInterval - receivedInterval;
	double fraction = 0.0;
	if (expectedInterval > 0 && lostInterval > 0)
		fraction = static_cast<double>(lostInterval) / static_cast<double>(expectedInterval);

	stats_.packetsLost = std::max<int64_t>(0, expected - received);
	stats_.recentLossFraction = fraction;

	ReportBlock block;
	block.mediaSsrc = ssrc_;
	block.fractionLost = static_cast<uint8_t>(std::min(255.0, fraction * 256.0));
	block.cumulativeLost = static_cast<int32_t>(std::clamp<int64_t>(expected - received, -0x800000, 0x7FFFFF));
	block.extendedHighestSequence = static_cast<uint32_t>(highestSequence_ & 0xFFFFFFFF);
	block.jitter = static_cast<uint32_t>(jitter_);
	if (lastSenderReport_) {
		block.lastSenderReport = lastSenderReport_->report.compactNtp();
		const int64_t delayUs = nowUs - lastSenderReport_->arrivalUs;
		block.delaySinceLastSenderReport = static_cast<uint32_t>(delayUs * 65536 / 1'000'000);
	}

	std::vector<uint8_t> compound = buildReceiverReport(config_.localSsrc, &block);
	if (config_.rembBitrateBps > 0) {
		auto remb = buildRemb(config_.localSsrc, ssrc_, config_.rembBitrateBps);
		compound.insert(compound.end(), remb.begin(), remb.end());
	}
	sendRtcp(std::move(compound));
}

void ReceiveStream::receiveRtcp(const uint8_t *data, size_t size, int64_t nowUs)
{
	parseCompound(data, size, [&](const SenderReport &report) {
		if (haveSsrc_ && report.ssrc != ssrc_)
			return;
		lastSenderReport_ = SenderReportInfo{report, nowUs};
	});
}

bool ReceiveStream::requestKeyframe(int64_t nowUs, const RtcpSender &sendRtcp)
{
	if (!haveSsrc_ || !sendRtcp)
		return false;
	if (lastKeyframeRequestUs_ != 0 && nowUs - lastKeyframeRequestUs_ < kKeyframeRequestIntervalUs)
		return false;
	lastKeyframeRequestUs_ = nowUs;
	++stats_.plisSent;
	sendRtcp(buildPli(config_.localSsrc, ssrc_));
	return true;
}

} // namespace qc::rtp
