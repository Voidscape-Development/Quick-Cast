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

#include "reorder-buffer.hpp"

namespace qc::rtp {

void ReorderBuffer::reset()
{
	started_ = false;
	nextSequence_ = 0;
	packets_.clear();
	missing_.clear();
}

void ReorderBuffer::push(Packet &&packet, int64_t nowUs, const PacketSink &onPacket, const LossSink &onLoss)
{
	const int64_t sequence = packet.extendedSequence;

	if (!started_) {
		started_ = true;
		nextSequence_ = sequence;
	}

	// Already delivered or given up on (late retransmission / duplicate).
	if (sequence < nextSequence_)
		return;

	// A jump this large is a stream restart rather than loss; resynchronise instead of buffering thousands of holes.
	if (sequence - nextSequence_ > static_cast<int64_t>(config_.maxPackets) * 4) {
		const int64_t skipped = sequence - nextSequence_;
		packets_.clear();
		missing_.clear();
		nextSequence_ = sequence;
		if (onLoss)
			onLoss(skipped);
	}

	missing_.erase(sequence);

	// Register every hole between what we have and this packet so it can be NACKed.
	const int64_t highestKnown = packets_.empty() ? nextSequence_ - 1 : packets_.rbegin()->first;
	for (int64_t s = highestKnown + 1; s < sequence; ++s) {
		Missing m;
		m.firstSeenMissingUs = nowUs;
		missing_.emplace(s, m);
	}

	packets_.emplace(sequence, std::move(packet));
	release(nowUs, onPacket, onLoss);
}

void ReorderBuffer::skipTo(int64_t sequence, const LossSink &onLoss)
{
	const int64_t lost = sequence - nextSequence_;
	nextSequence_ = sequence;
	missing_.erase(missing_.begin(), missing_.lower_bound(sequence));
	if (lost > 0 && onLoss)
		onLoss(lost);
}

void ReorderBuffer::release(int64_t nowUs, const PacketSink &onPacket, const LossSink &onLoss)
{
	while (!packets_.empty()) {
		auto head = packets_.begin();

		if (head->first != nextSequence_) {
			// Head-of-line gap: keep waiting unless the hole has been open too long, retransmission was
			// exhausted, or the buffer is overflowing.
			bool giveUp = packets_.size() > config_.maxPackets;
			if (!giveUp) {
				auto hole = missing_.find(nextSequence_);
				if (hole == missing_.end())
					giveUp = true;
				else
					giveUp = nowUs - hole->second.firstSeenMissingUs >= config_.maxWaitUs;
			}
			if (!giveUp)
				return;
			skipTo(head->first, onLoss);
		}

		Packet packet = std::move(head->second);
		packets_.erase(head);
		++nextSequence_;
		if (onPacket)
			onPacket(std::move(packet));
	}
}

std::vector<uint16_t> ReorderBuffer::collectNacks(int64_t nowUs)
{
	std::vector<uint16_t> result;
	for (auto &[sequence, state] : missing_) {
		if (state.attempts >= config_.maxNackAttempts)
			continue;
		if (state.attempts > 0 && nowUs - state.lastNackUs < config_.nackIntervalUs)
			continue;
		state.lastNackUs = nowUs;
		++state.attempts;
		result.push_back(static_cast<uint16_t>(sequence & 0xFFFF));
		if (result.size() >= 256)
			break;
	}
	return result;
}

} // namespace qc::rtp
