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

#include "audio-decoder.hpp"

#include "util/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

#include <cstring>

namespace qc {

AudioDecoder::~AudioDecoder()
{
	reset();
}

void AudioDecoder::reset()
{
	if (context_)
		avcodec_free_context(&context_);
	if (frame_)
		av_frame_free(&frame_);
	if (packet_)
		av_packet_free(&packet_);
}

bool AudioDecoder::open()
{
	reset();

	// Prefer FFmpeg's built-in decoder (planar float output); fall back to whatever handles Opus.
	const AVCodec *decoder = avcodec_find_decoder_by_name("opus");
	if (!decoder)
		decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!decoder) {
		QC_LOG_ERROR("no Opus decoder available in this FFmpeg build");
		return false;
	}

	context_ = avcodec_alloc_context3(decoder);
	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (!context_ || !frame_ || !packet_) {
		reset();
		return false;
	}

	// WebRTC Opus is always signalled as 48 kHz / 2 channels; mono senders are upmixed by the decoder.
	context_->sample_rate = 48000;
	av_channel_layout_default(&context_->ch_layout, 2);

	if (avcodec_open2(context_, decoder, nullptr) < 0) {
		QC_LOG_ERROR("could not open Opus decoder");
		reset();
		return false;
	}
	return true;
}

bool AudioDecoder::decode(const EncodedAudioPacket &packet, const FrameSink &onFrame)
{
	if (!context_ && !open())
		return false;
	if (packet.data.empty())
		return true;

	if (av_new_packet(packet_, static_cast<int>(packet.data.size())) < 0)
		return false;
	std::memcpy(packet_->data, packet.data.data(), packet.data.size());
	packet_->pts = static_cast<int64_t>(packet.timestampNs);
	packet_->dts = packet_->pts;

	const int sent = avcodec_send_packet(context_, packet_);
	av_packet_unref(packet_);
	if (sent < 0 && sent != AVERROR(EAGAIN)) {
		avcodec_flush_buffers(context_);
		return false;
	}

	for (;;) {
		const int received = avcodec_receive_frame(context_, frame_);
		if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
			break;
		if (received < 0)
			return false;
		// The decoder rescales pts into its own time base; we want our nanosecond stamp untouched.
		frame_->pts = static_cast<int64_t>(packet.timestampNs);
		if (onFrame)
			onFrame(frame_);
		av_frame_unref(frame_);
	}
	return true;
}

} // namespace qc
