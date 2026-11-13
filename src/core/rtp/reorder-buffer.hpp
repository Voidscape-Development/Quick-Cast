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

#include "rtp-packet.hpp"

#include <functional>
#include <map>
#include <vector>

namespace qc::rtp {

/**
 * Puts RTP packets back into sequence order and decides when a missing packet is lost for good.
 *
 * Packets are released strictly in order. When the next expected packet is missing the buffer holds what arrived
 * after it for up to `maxWaitUs` (giving a NACK retransmission time to land) and then skips the gap, reporting the
 * number of packets that were given up on. It is deliberately not a playout buffer: frames are released as soon as
 * they are complete so latency stays minimal; smoothing is left to OBS' async source timing.
 */
class ReorderBuffer {
public:
	struct Config {
		/** How long to wait for a missing packet before skipping it. */
		int64_t maxWaitUs = 150'000;
		/** Hard cap on buffered packets; exceeding it forces the gap to be skipped. */
		size_t maxPackets = 1024;
		/** Minimum spacing between NACKs for the same packet. */
		int64_t nackIntervalUs = 40'000;
		/** How many times one packet is requested before giving up on it. */
		int maxNackAttempts = 4;
	};

	using PacketSink = std::function<void(Packet &&)>;
	using LossSink = std::function<void(int64_t lostPackets)>;

	ReorderBuffer() = default;
	explicit ReorderBuffer(const Config &config) : config_(config) {}

	/** Inserts a packet (extendedSequence and arrivalUs must be set) and releases whatever became deliverable. */
	void push(Packet &&packet, int64_t nowUs, const PacketSink &onPacket, const LossSink &onLoss);

	/** Sequence numbers (16-bit, wire format) that should be requested again right now. */
	std::vector<uint16_t> collectNacks(int64_t nowUs);

	void reset();

	size_t bufferedPackets() const { return packets_.size(); }

private:
	struct Missing {
		int64_t firstSeenMissingUs = 0;
		int64_t lastNackUs = 0;
		int attempts = 0;
	};

	void release(int64_t nowUs, const PacketSink &onPacket, const LossSink &onLoss);
	void skipTo(int64_t sequence, const LossSink &onLoss);

	Config config_;
	bool started_ = false;
	int64_t nextSequence_ = 0;
	std::map<int64_t, Packet> packets_;
	std::map<int64_t, Missing> missing_;
};

} // namespace qc::rtp
