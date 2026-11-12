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

#include "sync-clock.hpp"

#include <cmath>

namespace qc::rtp {

namespace {
/** How quickly an offset is allowed to grow, per sample, to follow sender/receiver clock drift. */
constexpr double kDriftGain = 0.0005;
/** A sample this far from the current estimate means the sender's timeline jumped; start over. */
constexpr double kResyncThresholdSeconds = 2.0;
} // namespace

double SyncClock::Offset::update(double sample)
{
	if (!valid || std::abs(sample - seconds) > kResyncThresholdSeconds) {
		valid = true;
		seconds = sample;
	} else if (sample < seconds) {
		seconds = sample; // a faster packet: this is closer to the true clock offset
	} else {
		seconds += (sample - seconds) * kDriftGain;
	}
	return seconds;
}

void SyncClock::setClockRate(Kind kind, uint32_t clockRate)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (clockRate > 0)
		tracks_[static_cast<int>(kind)].clockRate = clockRate;
}

void SyncClock::reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (Track &track : tracks_) {
		const uint32_t rate = track.clockRate;
		track = Track{};
		track.clockRate = rate;
	}
	sharedOffset_ = Offset{};
}

void SyncClock::onSenderReport(Kind kind, double ntpSeconds, uint32_t rtpTimestamp)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Track &track = tracks_[static_cast<int>(kind)];
	track.reportRtp = track.unwrapper.unwrap(rtpTimestamp);
	track.reportNtpSeconds = ntpSeconds;
	track.haveReport = true;
}

uint64_t SyncClock::toLocalNs(Kind kind, uint32_t rtpTimestamp, int64_t arrivalUs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Track &track = tracks_[static_cast<int>(kind)];

	const int64_t rtp = track.unwrapper.unwrap(rtpTimestamp);
	const double arrival = static_cast<double>(arrivalUs) / 1'000'000.0;

	double local;
	if (track.haveReport) {
		const double senderTime =
			track.reportNtpSeconds + static_cast<double>(rtp - track.reportRtp) / track.clockRate;
		local = senderTime + sharedOffset_.update(arrival - senderTime);
	} else {
		const double mediaTime = static_cast<double>(rtp) / track.clockRate;
		local = mediaTime + track.ownOffset.update(arrival - mediaTime);
	}

	if (local < 0.0)
		local = 0.0;
	uint64_t ns = static_cast<uint64_t>(std::llround(local * 1'000'000'000.0));
	// Timestamps handed to OBS must never run backwards within a stream.
	if (ns <= track.lastOutputNs)
		ns = track.lastOutputNs + 1;
	track.lastOutputNs = ns;
	return ns;
}

} // namespace qc::rtp
