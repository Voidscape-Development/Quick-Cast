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

/** Opus decoder (FFmpeg). Output is always 48 kHz stereo in whatever sample format the decoder produces. */
class AudioDecoder {
public:
	using FrameSink = std::function<void(const AVFrame *frame)>;

	AudioDecoder() = default;
	~AudioDecoder();

	AudioDecoder(const AudioDecoder &) = delete;
	AudioDecoder &operator=(const AudioDecoder &) = delete;

	bool decode(const EncodedAudioPacket &packet, const FrameSink &onFrame);
	void reset();

private:
	bool open();

	AVCodecContext *context_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *packet_ = nullptr;
};

} // namespace qc
