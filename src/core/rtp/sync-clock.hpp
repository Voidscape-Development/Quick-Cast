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

#include <cstdint>
#include <mutex>

namespace qc::rtp {

/**
 * Maps the RTP timestamps of one guest's audio and video onto a single local timeline so OBS can keep them in sync.
 *
 * Each RTP stream has its own arbitrary timestamp origin. RTCP sender reports tie each stream to the sender's wall
 * clock; once a report has been seen for a stream its media is placed on that shared sender timeline and shifted to
 * local time with one common offset, which preserves the sender's A/V relationship exactly. Until then a stream
 * falls back to its own arrival-time offset, which is close enough for the first second or two.
 *
 * Offsets follow the *fastest* packets (minimum transit), so network jitter does not leak into timestamps, and creep
 * upward slowly to follow clock drift between the two machines.
 */
class SyncClock {
public:
	enum class Kind { Audio = 0, Video = 1 };

	void setClockRate(Kind kind, uint32_t clockRate);

	/** Records a sender report: `ntpSeconds` is the sender wall clock at RTP time `rtpTimestamp`. */
	void onSenderReport(Kind kind, double ntpSeconds, uint32_t rtpTimestamp);

	/** Local presentation timestamp in nanoseconds for media stamped `rtpTimestamp` that arrived at `arrivalUs`. */
	uint64_t toLocalNs(Kind kind, uint32_t rtpTimestamp, int64_t arrivalUs);

	void reset();

private:
	struct Offset {
		bool valid = false;
		double seconds = 0.0;
		double update(double sample);
	};

	struct Track {
		uint32_t clockRate = 90000;
		Unwrapper<uint32_t> unwrapper;
		bool haveReport = false;
		double reportNtpSeconds = 0.0;
		int64_t reportRtp = 0;
		Offset ownOffset;
		uint64_t lastOutputNs = 0;
	};

	std::mutex mutex_;
	Track tracks_[2];
	Offset sharedOffset_;
};

} // namespace qc::rtp
