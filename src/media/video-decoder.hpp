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

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace qc {

/**
 * Video decoder (FFmpeg) tuned for low delay: one frame out per frame in, no reordering queue.
 *
 * Decoding runs on the GPU when the platform, the codec and the stream allow it (D3D11VA on Windows, VideoToolbox on
 * macOS, VAAPI on Linux) and in software otherwise. Hardware frames are copied back to system memory, so callers
 * always receive ordinary frames. If the hardware path keeps failing it is abandoned for the rest of this decoder's
 * life and software takes over.
 */
class VideoDecoder {
public:
	using FrameSink = std::function<void(const AVFrame *frame)>;

	VideoDecoder() = default;
	~VideoDecoder();

	VideoDecoder(const VideoDecoder &) = delete;
	VideoDecoder &operator=(const VideoDecoder &) = delete;

	/** Allows or forbids hardware decoding. A change is applied at the next keyframe. */
	void setHardwareAllowed(bool allowed);

	/**
	 * Decodes one coded frame. Returns false when the bitstream could not be decoded, in which case the caller
	 * should ask the sender for a keyframe; the decoder has already been reset.
	 */
	bool decode(const EncodedVideoFrame &frame, const FrameSink &onFrame);

	void reset();

	/** True while frames are being decoded on the GPU. */
	bool usingHardware() const { return hardwareActive_; }

	/** FFmpeg calls this to pick an output format; public only so the C callback can reach it. */
	int chooseFormat(const int *formats) const;

private:
	bool open(VideoCodec codec);
	bool tryOpenHardware(const AVCodec *decoder);
	void closeContext();
	void noteFailure();

	AVCodecContext *context_ = nullptr;
	AVBufferRef *hardwareDevice_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVFrame *transferFrame_ = nullptr;
	AVPacket *packet_ = nullptr;
	VideoCodec codec_ = VideoCodec::H264;
	bool needKeyframe_ = true;

	bool hardwareAllowed_ = true;
	/** Set when the hardware path proved unusable; never retried by this decoder. */
	bool hardwareBroken_ = false;
	bool hardwareActive_ = false;
	bool openedWithHardwareAllowed_ = true;
	int hardwarePixelFormat_ = -1;
	int consecutiveFailures_ = 0;
	bool announced_ = false;
};

} // namespace qc
