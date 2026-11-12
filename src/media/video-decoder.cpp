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

#include "video-decoder.hpp"

#include "util/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstring>
#include <thread>

namespace qc {

VideoDecoder::~VideoDecoder()
{
	reset();
}

void VideoDecoder::reset()
{
	if (context_)
		avcodec_free_context(&context_);
	if (frame_)
		av_frame_free(&frame_);
	if (packet_)
		av_packet_free(&packet_);
	needKeyframe_ = true;
}

bool VideoDecoder::open(VideoCodec codec)
{
	reset();

	const AVCodecID id = codec == VideoCodec::VP8 ? AV_CODEC_ID_VP8 : AV_CODEC_ID_H264;
	const AVCodec *decoder = avcodec_find_decoder(id);
	if (!decoder) {
		QC_LOG_ERROR("no %s decoder available in this FFmpeg build", avcodec_get_name(id));
		return false;
	}

	context_ = avcodec_alloc_context3(decoder);
	frame_ = av_frame_alloc();
	packet_ = av_packet_alloc();
	if (!context_ || !frame_ || !packet_) {
		reset();
		return false;
	}

	// Slice threading keeps the one-in/one-out behaviour; frame threading would add a frame of delay per thread.
	context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	context_->thread_type = FF_THREAD_SLICE;
	context_->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));

	if (avcodec_open2(context_, decoder, nullptr) < 0) {
		QC_LOG_ERROR("could not open %s decoder", decoder->name);
		reset();
		return false;
	}
	codec_ = codec;
	return true;
}

bool VideoDecoder::decode(const EncodedVideoFrame &frame, const FrameSink &onFrame)
{
	if (!context_ || codec_ != frame.codec) {
		if (!open(frame.codec))
			return false;
	}

	if (needKeyframe_) {
		if (!frame.keyframe)
			return false;
		needKeyframe_ = false;
	}

	if (av_new_packet(packet_, static_cast<int>(frame.data.size())) < 0)
		return false;
	std::memcpy(packet_->data, frame.data.data(), frame.data.size());
	packet_->pts = static_cast<int64_t>(frame.timestampNs);
	packet_->dts = packet_->pts;
	if (frame.keyframe)
		packet_->flags |= AV_PKT_FLAG_KEY;

	const int sent = avcodec_send_packet(context_, packet_);
	av_packet_unref(packet_);
	if (sent < 0 && sent != AVERROR(EAGAIN)) {
		avcodec_flush_buffers(context_);
		needKeyframe_ = true;
		return false;
	}

	for (;;) {
		const int received = avcodec_receive_frame(context_, frame_);
		if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
			break;
		if (received < 0) {
			avcodec_flush_buffers(context_);
			needKeyframe_ = true;
			return false;
		}
		if (frame_->pts == AV_NOPTS_VALUE)
			frame_->pts = static_cast<int64_t>(frame.timestampNs);
		if (onFrame)
			onFrame(frame_);
		av_frame_unref(frame_);
	}
	return true;
}

} // namespace qc
