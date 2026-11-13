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
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include <algorithm>
#include <cstring>
#include <thread>

namespace qc {

namespace {

/** Hardware errors in a row before the GPU path is given up on. */
constexpr int kMaxHardwareFailures = 3;

/** Hardware decode APIs worth trying on this platform, best first. */
const AVHWDeviceType kHardwareTypes[] = {
#if defined(_WIN32)
	AV_HWDEVICE_TYPE_D3D11VA,
	AV_HWDEVICE_TYPE_DXVA2,
#elif defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
	AV_HWDEVICE_TYPE_VAAPI,
#endif
};

AVCodecID toCodecId(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::VP8:
		return AV_CODEC_ID_VP8;
	case VideoCodec::VP9:
		return AV_CODEC_ID_VP9;
	case VideoCodec::H264:
		break;
	}
	return AV_CODEC_ID_H264;
}

AVPixelFormat getFormatCallback(AVCodecContext *context, const AVPixelFormat *formats)
{
	const auto *decoder = static_cast<const VideoDecoder *>(context->opaque);
	return static_cast<AVPixelFormat>(decoder->chooseFormat(reinterpret_cast<const int *>(formats)));
}

} // namespace

VideoDecoder::~VideoDecoder()
{
	reset();
}

void VideoDecoder::setHardwareAllowed(bool allowed)
{
	hardwareAllowed_ = allowed;
}

void VideoDecoder::closeContext()
{
	if (context_)
		avcodec_free_context(&context_);
	if (hardwareDevice_)
		av_buffer_unref(&hardwareDevice_);
	hardwareActive_ = false;
	hardwarePixelFormat_ = -1;
}

void VideoDecoder::reset()
{
	closeContext();
	if (frame_)
		av_frame_free(&frame_);
	if (transferFrame_)
		av_frame_free(&transferFrame_);
	if (packet_)
		av_packet_free(&packet_);
	needKeyframe_ = true;
	consecutiveFailures_ = 0;
}

int VideoDecoder::chooseFormat(const int *formats) const
{
	// Take the hardware surface format when FFmpeg offers it for this stream. When it does not (an unsupported
	// profile, for example) the first software format in the list makes FFmpeg decode on the CPU instead.
	for (const int *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
		if (*format == hardwarePixelFormat_)
			return *format;
	}
	for (const int *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
		if (*format == AV_PIX_FMT_YUV420P || *format == AV_PIX_FMT_YUVJ420P || *format == AV_PIX_FMT_NV12 ||
		    *format == AV_PIX_FMT_YUV420P10)
			return *format;
	}
	return formats[0];
}

bool VideoDecoder::tryOpenHardware(const AVCodec *decoder)
{
	for (const AVHWDeviceType type : kHardwareTypes) {
		// Does this decoder support this API through a device context?
		AVPixelFormat surfaceFormat = AV_PIX_FMT_NONE;
		for (int index = 0;; ++index) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, index);
			if (!config)
				break;
			if (config->device_type == type &&
			    (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				surfaceFormat = config->pix_fmt;
				break;
			}
		}
		if (surfaceFormat == AV_PIX_FMT_NONE)
			continue;

		AVBufferRef *device = nullptr;
		if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0 || !device)
			continue;

		AVCodecContext *context = avcodec_alloc_context3(decoder);
		if (!context) {
			av_buffer_unref(&device);
			continue;
		}
		context->flags |= AV_CODEC_FLAG_LOW_DELAY;
		context->opaque = this;
		context->get_format = getFormatCallback;
		context->hw_device_ctx = av_buffer_ref(device);
		// The GPU does the work; extra CPU threads would only add delay.
		context->thread_count = 1;

		hardwarePixelFormat_ = surfaceFormat;
		if (!context->hw_device_ctx || avcodec_open2(context, decoder, nullptr) < 0) {
			avcodec_free_context(&context);
			av_buffer_unref(&device);
			hardwarePixelFormat_ = -1;
			continue;
		}

		context_ = context;
		hardwareDevice_ = device;
		hardwareActive_ = true;
		QC_LOG_INFO("decoding %s video with hardware acceleration (%s)", decoder->name,
			    av_hwdevice_get_type_name(type));
		return true;
	}
	return false;
}

bool VideoDecoder::open(VideoCodec codec)
{
	closeContext();
	needKeyframe_ = true;

	const AVCodecID id = toCodecId(codec);
	const AVCodec *decoder = avcodec_find_decoder(id);
	if (!decoder) {
		QC_LOG_ERROR("no %s decoder available in this FFmpeg build", avcodec_get_name(id));
		return false;
	}

	if (!frame_)
		frame_ = av_frame_alloc();
	if (!transferFrame_)
		transferFrame_ = av_frame_alloc();
	if (!packet_)
		packet_ = av_packet_alloc();
	if (!frame_ || !transferFrame_ || !packet_)
		return false;

	codec_ = codec;
	openedWithHardwareAllowed_ = hardwareAllowed_;

	if (hardwareAllowed_ && !hardwareBroken_ && tryOpenHardware(decoder))
		return true;

	context_ = avcodec_alloc_context3(decoder);
	if (!context_)
		return false;

	// Slice threading keeps the one-in/one-out behaviour; frame threading would add a frame of delay per thread.
	context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	context_->thread_type = FF_THREAD_SLICE;
	context_->thread_count = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));

	if (avcodec_open2(context_, decoder, nullptr) < 0) {
		QC_LOG_ERROR("could not open %s decoder", decoder->name);
		closeContext();
		return false;
	}
	if (!announced_ || hardwareBroken_) {
		QC_LOG_INFO("decoding %s video in software", decoder->name);
		announced_ = true;
	}
	return true;
}

void VideoDecoder::noteFailure()
{
	needKeyframe_ = true;
	if (!hardwareActive_) {
		if (context_)
			avcodec_flush_buffers(context_);
		return;
	}
	// A hardware decoder that keeps failing is usually a driver that cannot handle this stream. Stop using it
	// rather than leaving the guest frozen.
	if (++consecutiveFailures_ >= kMaxHardwareFailures) {
		QC_LOG_WARN("hardware video decoding keeps failing; switching to software decoding");
		hardwareBroken_ = true;
		consecutiveFailures_ = 0;
		closeContext();
	} else if (context_) {
		avcodec_flush_buffers(context_);
	}
}

bool VideoDecoder::decode(const EncodedVideoFrame &frame, const FrameSink &onFrame)
{
	// The hardware preference changed: reopen at a clean point in the stream.
	if (context_ && frame.keyframe && openedWithHardwareAllowed_ != hardwareAllowed_)
		closeContext();

	if (!context_ || codec_ != frame.codec) {
		if (!frame.keyframe)
			return false;
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
		noteFailure();
		return false;
	}

	for (;;) {
		const int received = avcodec_receive_frame(context_, frame_);
		if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
			break;
		if (received < 0) {
			noteFailure();
			return false;
		}
		if (frame_->pts == AV_NOPTS_VALUE)
			frame_->pts = static_cast<int64_t>(frame.timestampNs);

		const AVFrame *output = frame_;
		if (hardwareActive_ && frame_->format == hardwarePixelFormat_) {
			// Bring the picture back from the GPU so the rest of the pipeline sees an ordinary frame.
			av_frame_unref(transferFrame_);
			if (av_hwframe_transfer_data(transferFrame_, frame_, 0) < 0) {
				av_frame_unref(frame_);
				noteFailure();
				return false;
			}
			av_frame_copy_props(transferFrame_, frame_);
			output = transferFrame_;
		}

		consecutiveFailures_ = 0;
		if (onFrame)
			onFrame(output);
		av_frame_unref(frame_);
		av_frame_unref(transferFrame_);
	}
	return true;
}

} // namespace qc
