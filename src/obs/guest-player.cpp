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

#include "guest-player.hpp"

#include "media/audio-decoder.hpp"
#include "media/video-decoder.hpp"

#include <media-io/video-io.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cstdlib>

namespace qc {

namespace {

/** If the decoder falls this far behind, catching up frame by frame would only add latency. */
constexpr size_t kMaxQueuedVideoFrames = 30;
constexpr size_t kMaxQueuedItems = 400;

bool toObsVideoFormat(int avFormat, video_format &format, bool &fullRange)
{
	switch (avFormat) {
	case AV_PIX_FMT_YUV420P:
		format = VIDEO_FORMAT_I420;
		fullRange = false;
		return true;
	case AV_PIX_FMT_YUVJ420P:
		format = VIDEO_FORMAT_I420;
		fullRange = true;
		return true;
	case AV_PIX_FMT_NV12:
		format = VIDEO_FORMAT_NV12;
		fullRange = false;
		return true;
	case AV_PIX_FMT_P010:
		// 10-bit output of hardware decoders
		format = VIDEO_FORMAT_P010;
		fullRange = false;
		return true;
	case AV_PIX_FMT_YUV422P:
		format = VIDEO_FORMAT_I422;
		fullRange = false;
		return true;
	case AV_PIX_FMT_YUV444P:
		format = VIDEO_FORMAT_I444;
		fullRange = false;
		return true;
	default:
		return false;
	}
}

bool toObsAudioFormat(int avFormat, audio_format &format)
{
	switch (avFormat) {
	case AV_SAMPLE_FMT_FLTP:
		format = AUDIO_FORMAT_FLOAT_PLANAR;
		return true;
	case AV_SAMPLE_FMT_FLT:
		format = AUDIO_FORMAT_FLOAT;
		return true;
	case AV_SAMPLE_FMT_S16:
		format = AUDIO_FORMAT_16BIT;
		return true;
	case AV_SAMPLE_FMT_S16P:
		format = AUDIO_FORMAT_16BIT_PLANAR;
		return true;
	default:
		return false;
	}
}

} // namespace

GuestPlayer::GuestPlayer(std::function<void()> requestKeyframe)
	: requestKeyframe_(std::move(requestKeyframe)),
	  thread_([this] { run(); })
{
}

GuestPlayer::~GuestPlayer()
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void GuestPlayer::addSource(obs_source_t *source)
{
	{
		std::lock_guard<std::mutex> lock(sourcesMutex_);
		if (std::find(sources_.begin(), sources_.end(), source) == sources_.end())
			sources_.push_back(source);
		haveSources_ = !sources_.empty();
	}
	// A freshly attached source has nothing to show until the next keyframe arrives.
	if (requestKeyframe_)
		requestKeyframe_();
}

void GuestPlayer::removeSource(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(sourcesMutex_);
	sources_.erase(std::remove(sources_.begin(), sources_.end(), source), sources_.end());
	haveSources_ = !sources_.empty();
}

bool GuestPlayer::hasSources() const
{
	return haveSources_.load();
}

void GuestPlayer::forEachSource(const std::function<void(obs_source_t *)> &apply) const
{
	std::lock_guard<std::mutex> lock(sourcesMutex_);
	for (obs_source_t *source : sources_)
		apply(source);
}

void GuestPlayer::enqueue(Item &&item)
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (stopping_)
			return;

		if (auto *video = std::get_if<EncodedVideoFrame>(&item)) {
			if (dropUntilKeyframe_) {
				if (!video->keyframe)
					return;
				dropUntilKeyframe_ = false;
			}
			if (queuedVideo_ >= kMaxQueuedVideoFrames) {
				// Decoder cannot keep up: throw away the backlog and restart from a keyframe.
				for (auto it = queue_.begin(); it != queue_.end();) {
					if (std::holds_alternative<EncodedVideoFrame>(*it))
						it = queue_.erase(it);
					else
						++it;
				}
				queuedVideo_ = 0;
				if (!video->keyframe) {
					dropUntilKeyframe_ = true;
					if (requestKeyframe_)
						requestKeyframe_();
					return;
				}
			}
			++queuedVideo_;
		} else if (queue_.size() >= kMaxQueuedItems) {
			return;
		}
		queue_.push_back(std::move(item));
	}
	wake_.notify_one();
}

void GuestPlayer::onVideoFrame(EncodedVideoFrame &&frame)
{
	if (haveSources_.load())
		enqueue(std::move(frame));
}

void GuestPlayer::onAudioPacket(EncodedAudioPacket &&packet)
{
	if (haveSources_.load())
		enqueue(std::move(packet));
}

void GuestPlayer::onMediaStopped()
{
	enqueue(Stop{});
}

void GuestPlayer::clearSources()
{
	std::lock_guard<std::mutex> lock(sourcesMutex_);
	for (obs_source_t *source : sources_)
		obs_source_output_video(source, nullptr);
	width_ = 0;
	height_ = 0;
}

void GuestPlayer::run()
{
	VideoDecoder videoDecoder;
	AudioDecoder audioDecoder;
	bool warnedPixelFormat = false;

	const auto outputVideo = [&](const AVFrame *decoded) {
		video_format format;
		bool fullRange = false;
		if (!toObsVideoFormat(decoded->format, format, fullRange)) {
			if (!warnedPixelFormat) {
				warnedPixelFormat = true;
				blog(LOG_WARNING, "[quick-cast] unsupported decoded pixel format %d", decoded->format);
			}
			return;
		}
		if (decoded->color_range == AVCOL_RANGE_JPEG)
			fullRange = true;

		obs_source_frame frame = {};
		for (size_t plane = 0; plane < MAX_AV_PLANES && plane < AV_NUM_DATA_POINTERS; ++plane) {
			frame.data[plane] = decoded->data[plane];
			frame.linesize[plane] = static_cast<uint32_t>(std::abs(decoded->linesize[plane]));
		}
		frame.width = static_cast<uint32_t>(decoded->width);
		frame.height = static_cast<uint32_t>(decoded->height);
		frame.format = format;
		frame.timestamp = static_cast<uint64_t>(decoded->pts);
		frame.full_range = fullRange;

		// Browsers convert camera RGB to YUV with BT.601 unless the stream says otherwise.
		const video_colorspace colorspace = decoded->colorspace == AVCOL_SPC_BT709 ? VIDEO_CS_709
											   : VIDEO_CS_601;
		video_format_get_parameters_for_format(colorspace, fullRange ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
						       format, frame.color_matrix, frame.color_range_min,
						       frame.color_range_max);

		width_ = frame.width;
		height_ = frame.height;

		std::lock_guard<std::mutex> lock(sourcesMutex_);
		for (obs_source_t *source : sources_)
			obs_source_output_video(source, &frame);
	};

	const auto outputAudio = [&](const AVFrame *decoded) {
		audio_format format;
		if (!toObsAudioFormat(decoded->format, format))
			return;
		const int channels = decoded->ch_layout.nb_channels;
		if (channels < 1 || channels > 2)
			return;

		obs_source_audio audio = {};
		const bool planar = format == AUDIO_FORMAT_FLOAT_PLANAR || format == AUDIO_FORMAT_16BIT_PLANAR;
		if (planar) {
			for (int channel = 0; channel < channels; ++channel)
				audio.data[channel] = decoded->extended_data[channel];
		} else {
			audio.data[0] = decoded->extended_data[0];
		}
		audio.frames = static_cast<uint32_t>(decoded->nb_samples);
		audio.speakers = channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
		audio.format = format;
		audio.samples_per_sec = static_cast<uint32_t>(decoded->sample_rate > 0 ? decoded->sample_rate : 48000);
		audio.timestamp = static_cast<uint64_t>(decoded->pts);

		std::lock_guard<std::mutex> lock(sourcesMutex_);
		for (obs_source_t *source : sources_)
			obs_source_output_audio(source, &audio);
	};

	for (;;) {
		Item item;
		{
			std::unique_lock<std::mutex> lock(queueMutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_)
				return;
			item = std::move(queue_.front());
			queue_.pop_front();
			if (std::holds_alternative<EncodedVideoFrame>(item) && queuedVideo_ > 0)
				--queuedVideo_;
		}

		if (auto *video = std::get_if<EncodedVideoFrame>(&item)) {
			videoDecoder.setHardwareAllowed(hardwareDecoding_.load());
			if (!videoDecoder.decode(*video, outputVideo) && requestKeyframe_)
				requestKeyframe_();
		} else if (auto *audio = std::get_if<EncodedAudioPacket>(&item)) {
			audioDecoder.decode(*audio, outputAudio);
		} else {
			videoDecoder.reset();
			audioDecoder.reset();
			clearSources();
		}
	}
}

} // namespace qc
