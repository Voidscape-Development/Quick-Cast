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

#include <obs.hpp>

#include <deque>
#include <mutex>
#include <string>

namespace qc {

/**
 * Makes one OBS source's audio available as a private libobs audio output, so an encoder can be attached to that
 * source alone instead of to one of OBS' mix tracks.
 *
 * The source's post-filter audio is tapped with an audio capture callback, has the source's fader and mute applied
 * (as it would in a mix), and is queued. libobs pulls it from the queue on the private output's own clock; a short
 * queue absorbs the jitter between the two threads and silence is produced when the source is quiet or gone.
 */
class SourceAudioTap {
public:
	SourceAudioTap() = default;
	~SourceAudioTap();

	SourceAudioTap(const SourceAudioTap &) = delete;
	SourceAudioTap &operator=(const SourceAudioTap &) = delete;

	/** Starts tapping the named source. Returns false if it does not exist or produces no audio. */
	bool open(const std::string &sourceName);
	void close();

	/** The audio output to give to obs_encoder_set_audio(); null until open() succeeds. */
	audio_t *audio() const { return audio_; }

private:
	static void onCapture(void *param, obs_source_t *source, const audio_data *audio, bool muted);
	static bool onPull(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
			   audio_output_data *mixes);

	OBSWeakSourceAutoRelease source_;
	audio_t *audio_ = nullptr;
	size_t channels_ = 2;

	std::mutex mutex_;
	std::deque<float> queue_[MAX_AUDIO_CHANNELS];
};

} // namespace qc
