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

#include "session/guest-service.hpp"

#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace qc {

/** Software video decoder (FFmpeg) tuned for low delay: one frame out per frame in, no reordering queue. */
class VideoDecoder {
public:
	using FrameSink = std::function<void(const AVFrame *frame)>;

	VideoDecoder() = default;
	~VideoDecoder();

	VideoDecoder(const VideoDecoder &) = delete;
	VideoDecoder &operator=(const VideoDecoder &) = delete;

	/**
	 * Decodes one coded frame. Returns false when the bitstream could not be decoded, in which case the caller
	 * should ask the sender for a keyframe; the decoder has already been reset.
	 */
	bool decode(const EncodedVideoFrame &frame, const FrameSink &onFrame);

	void reset();

private:
	bool open(VideoCodec codec);

	AVCodecContext *context_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
	VideoCodec codec_ = VideoCodec::H264;
	bool needKeyframe_ = true;
};

} // namespace qc
