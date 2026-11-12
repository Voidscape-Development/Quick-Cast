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

#include "media-receiver.hpp"

#include <chrono>

namespace qc::vdo {

int64_t monotonicNowUs()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

namespace {

rtp::ReceiveStream::Config makeStreamConfig(const MediaReceiver::Config &config)
{
	rtp::ReceiveStream::Config stream;
	stream.clockRate = config.clockRate;
	stream.rembBitrateBps = config.rembBitrateBps;
	if (config.video) {
		stream.enableNack = true;
		stream.reorder.maxWaitUs = 150'000;
	} else {
		// Late audio is useless: wait just long enough to undo trivial reordering and let Opus conceal the rest.
		stream.enableNack = false;
		stream.reorder.maxWaitUs = 20'000;
		stream.reorder.maxPackets = 64;
	}
	return stream;
}

rtc::message_ptr makeControl(std::vector<uint8_t> &&bytes)
{
	const auto *begin = reinterpret_cast<const std::byte *>(bytes.data());
	return rtc::make_message(begin, begin + bytes.size(), rtc::Message::Control);
}

} // namespace

MediaReceiver::MediaReceiver(Config config) : config_(std::move(config)), stream_(makeStreamConfig(config_))
{
	if (config_.video)
		depacketizer_ = rtp::VideoDepacketizer::create(config_.videoCodec);
	if (config_.clock) {
		config_.clock->setClockRate(config_.video ? rtp::SyncClock::Kind::Video : rtp::SyncClock::Kind::Audio,
					    config_.clockRate);
	}
}

void MediaReceiver::incoming(rtc::message_vector &messages, const rtc::message_callback &send)
{
	std::vector<std::vector<uint8_t>> rtcpOut;
	std::vector<VideoOutput> videoOut;
	std::vector<AudioOutput> audioOut;

	const auto kind = config_.video ? rtp::SyncClock::Kind::Video : rtp::SyncClock::Kind::Audio;
	const int64_t nowUs = monotonicNowUs();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto sendRtcp = [&](std::vector<uint8_t> &&packet) {
			rtcpOut.push_back(std::move(packet));
		};

		for (const rtc::message_ptr &message : messages) {
			if (!message || message->empty())
				continue;
			const auto *data = reinterpret_cast<const uint8_t *>(message->data());
			const size_t size = message->size();

			if (message->type == rtc::Message::Control || rtp::looksLikeRtcp(data, size)) {
				stream_.receiveRtcp(data, size, nowUs);
				if (auto report = stream_.lastSenderReport(); report && config_.clock) {
					config_.clock->onSenderReport(kind, report->report.ntpSeconds(),
								      report->report.rtpTimestamp);
				}
				continue;
			}

			// Anything on another payload type (RTX, RED, FEC) was not negotiated by us.
			if (size < 2 || (data[1] & 0x7F) != config_.payloadType)
				continue;

			lastPacketUs_ = nowUs;
			stream_.receiveRtp(
				data, size, nowUs,
				[&](rtp::Packet &&packet) {
					if (depacketizer_) {
						depacketizer_->push(packet, [&](rtp::EncodedFrame &&frame) {
							VideoOutput output;
							output.timestampNs =
								config_.clock
									? config_.clock->toLocalNs(kind,
												   frame.rtpTimestamp,
												   frame.arrivalUs)
									: static_cast<uint64_t>(frame.arrivalUs) * 1000;
							output.frame = std::move(frame);
							videoOut.push_back(std::move(output));
						});
					} else {
						AudioOutput output;
						output.timestampNs =
							config_.clock ? config_.clock->toLocalNs(kind, packet.timestamp,
												 packet.arrivalUs)
								      : static_cast<uint64_t>(packet.arrivalUs) * 1000;
						output.payload = std::move(packet.payload);
						audioOut.push_back(std::move(output));
					}
				},
				[&](int64_t) {
					if (depacketizer_)
						depacketizer_->notifyLoss();
				},
				sendRtcp);
		}

		if (depacketizer_ && depacketizer_->needsKeyframe())
			stream_.requestKeyframe(nowUs, sendRtcp);
	}

	// This handler is the end of the chain: nothing is passed on to Track::onMessage.
	messages.clear();

	for (auto &packet : rtcpOut)
		send(makeControl(std::move(packet)));
	if (config_.onVideo) {
		for (auto &output : videoOut)
			config_.onVideo(std::move(output));
	}
	if (config_.onAudio) {
		for (auto &output : audioOut)
			config_.onAudio(std::move(output));
	}
}

bool MediaReceiver::requestKeyframe(const rtc::message_callback &send)
{
	std::vector<uint8_t> packet;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stream_.requestKeyframe(monotonicNowUs(),
					[&](std::vector<uint8_t> &&bytes) { packet = std::move(bytes); });
	}
	if (packet.empty())
		return false;
	send(makeControl(std::move(packet)));
	return true;
}

bool MediaReceiver::requestBitrate(unsigned int bitrate, const rtc::message_callback &)
{
	// Picked up by the next periodic receiver report, which carries the REMB.
	std::lock_guard<std::mutex> lock(mutex_);
	stream_.setRembBitrate(bitrate);
	return true;
}

rtp::ReceiveStats MediaReceiver::stats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stream_.stats();
}

int64_t MediaReceiver::lastPacketUs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return lastPacketUs_;
}

} // namespace qc::vdo
