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

#include "source-audio.hpp"

#include <media-io/audio-io.h>

#include <algorithm>

namespace qc {

namespace {
/** Longest backlog kept, in frames. Beyond this the oldest audio is dropped so latency cannot build up. */
constexpr size_t kMaxQueuedFrames = AUDIO_OUTPUT_FRAMES * 4;
} // namespace

SourceAudioTap::~SourceAudioTap()
{
	close();
}

bool SourceAudioTap::open(const std::string &sourceName)
{
	close();

	OBSSourceAutoRelease source = obs_get_source_by_name(sourceName.c_str());
	if (!source || (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return false;

	// Use OBS' own audio format: that is the format capture callbacks deliver, so nothing needs resampling here
	// and the encoder converts exactly as it would for a normal track.
	obs_audio_info obsAudio = {};
	if (!obs_get_audio_info(&obsAudio))
		return false;
	channels_ = std::min<size_t>(get_audio_channels(obsAudio.speakers), MAX_AUDIO_CHANNELS);

	audio_output_info info = {};
	info.name = "Quick Cast Source Audio";
	info.samples_per_sec = obsAudio.samples_per_sec;
	info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	info.speakers = obsAudio.speakers;
	info.input_callback = onPull;
	info.input_param = this;
	if (audio_output_open(&audio_, &info) != AUDIO_OUTPUT_SUCCESS) {
		audio_ = nullptr;
		return false;
	}

	source_ = obs_source_get_weak_source(source);
	obs_source_add_audio_capture_callback(source, onCapture, this);
	return true;
}

void SourceAudioTap::close()
{
	if (source_) {
		OBSSourceAutoRelease source = obs_weak_source_get_source(source_);
		if (source)
			obs_source_remove_audio_capture_callback(source, onCapture, this);
		source_ = nullptr;
	}
	if (audio_) {
		audio_output_close(audio_);
		audio_ = nullptr;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &channel : queue_)
		channel.clear();
}

void SourceAudioTap::onCapture(void *param, obs_source_t *source, const audio_data *audio, bool muted)
{
	auto *self = static_cast<SourceAudioTap *>(param);
	if (!audio || audio->frames == 0)
		return;

	// The callback sees audio before the mixer applies the fader, so apply it here to match what a track would
	// carry. A muted source contributes silence rather than nothing, which keeps the queue's timing steady.
	const float gain = muted ? 0.0f : obs_source_get_volume(source);

	std::lock_guard<std::mutex> lock(self->mutex_);
	for (size_t channel = 0; channel < self->channels_; ++channel) {
		// Mono sources fill only the first plane; mirror it so they are heard on both sides.
		const uint8_t *plane = audio->data[channel] ? audio->data[channel] : audio->data[0];
		if (!plane)
			continue;
		const auto *samples = reinterpret_cast<const float *>(plane);
		auto &queue = self->queue_[channel];
		for (uint32_t frame = 0; frame < audio->frames; ++frame)
			queue.push_back(samples[frame] * gain);
		if (queue.size() > kMaxQueuedFrames)
			queue.erase(queue.begin(),
				    queue.begin() + static_cast<std::ptrdiff_t>(queue.size() - kMaxQueuedFrames));
	}
}

bool SourceAudioTap::onPull(void *param, uint64_t startTs, uint64_t, uint64_t *newTs, uint32_t activeMixers,
			    audio_output_data *mixes)
{
	auto *self = static_cast<SourceAudioTap *>(param);
	*newTs = startTs;
	if ((activeMixers & 1) == 0)
		return true;

	std::lock_guard<std::mutex> lock(self->mutex_);
	for (size_t channel = 0; channel < self->channels_; ++channel) {
		float *out = mixes[0].data[channel];
		if (!out)
			continue;
		auto &queue = self->queue_[channel];
		const size_t available = std::min<size_t>(queue.size(), AUDIO_OUTPUT_FRAMES);
		std::copy(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(available), out);
		std::fill(out + available, out + AUDIO_OUTPUT_FRAMES, 0.0f);
		queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(available));
	}
	return true;
}

} // namespace qc
