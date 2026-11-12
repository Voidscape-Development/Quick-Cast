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

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace qc::rtp {

/** The parts of an RTCP sender report (RFC 3550 section 6.4.1) needed for lip sync and round-trip estimation. */
struct SenderReport {
	uint32_t ssrc = 0;
	/** 64-bit NTP timestamp (32.32 fixed point seconds since 1900). */
	uint64_t ntpTimestamp = 0;
	uint32_t rtpTimestamp = 0;
	uint32_t packetCount = 0;
	uint32_t octetCount = 0;

	/** Middle 32 bits of the NTP timestamp, as echoed back in receiver reports (LSR). */
	uint32_t compactNtp() const { return static_cast<uint32_t>(ntpTimestamp >> 16); }
	double ntpSeconds() const { return static_cast<double>(ntpTimestamp) / 4294967296.0; }
};

struct ReportBlock {
	uint32_t mediaSsrc = 0;
	uint8_t fractionLost = 0;
	int32_t cumulativeLost = 0;
	uint32_t extendedHighestSequence = 0;
	uint32_t jitter = 0;
	uint32_t lastSenderReport = 0;
	uint32_t delaySinceLastSenderReport = 0;
};

/** Feedback a remote receiver sent about media we are sending. */
struct ReceivedFeedback {
	bool keyframeRequested = false; // PLI or FIR
	std::optional<uint64_t> rembBitrate;
	std::vector<uint16_t> nackedSequences;
	std::vector<ReportBlock> reportBlocks;
};

/** Walks a compound RTCP packet. Malformed trailing data is ignored. */
void parseCompound(const uint8_t *data, size_t size, const std::function<void(const SenderReport &)> &onSenderReport,
		   ReceivedFeedback *feedback = nullptr);

std::vector<uint8_t> buildReceiverReport(uint32_t senderSsrc, const ReportBlock *block);
std::vector<uint8_t> buildPli(uint32_t senderSsrc, uint32_t mediaSsrc);
std::vector<uint8_t> buildNack(uint32_t senderSsrc, uint32_t mediaSsrc, const std::vector<uint16_t> &sequences);
std::vector<uint8_t> buildRemb(uint32_t senderSsrc, uint32_t mediaSsrc, uint64_t bitrateBps);

} // namespace qc::rtp
