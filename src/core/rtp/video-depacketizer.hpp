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

#include "rtp-packet.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qc::rtp {

/**
 * Codecs the receive path can reassemble.
 *
 * Future work: AV1. It needs its own depacketizer (OBU aggregation and fragmentation per the AV1 RTP payload
 * specification, including dependency-descriptor handling) and a decoder story: FFmpeg's built-in AV1 decoder is
 * hardware-only, so software playback would depend on libdav1d being present in the FFmpeg OBS ships. Once both
 * exist it slots in here, in chooseVideoCodec() and in VideoDecoder the same way VP9 does.
 */
enum class VideoCodec { H264, VP8, VP9 };

const char *toString(VideoCodec codec);

/** One complete coded picture, ready for a decoder. */
struct EncodedFrame {
	/** H.264: Annex-B byte stream (start-code prefixed NAL units). VP8/VP9: raw frame. */
	std::vector<uint8_t> data;
	uint32_t rtpTimestamp = 0;
	int64_t arrivalUs = 0;
	bool keyframe = false;
};

/**
 * Turns in-order RTP payloads into complete coded frames.
 *
 * Implementations expect packets in sequence order (see ReorderBuffer). After packet loss they drop everything until
 * the next keyframe so a decoder is never fed a broken reference chain; `needsKeyframe()` tells the caller to ask
 * the sender for one.
 */
class VideoDepacketizer {
public:
	using FrameSink = std::function<void(EncodedFrame &&)>;

	virtual ~VideoDepacketizer() = default;

	virtual VideoCodec codec() const = 0;
	virtual void push(const Packet &packet, const FrameSink &onFrame) = 0;
	/** Called when one or more packets preceding the next push() were lost. */
	virtual void notifyLoss() = 0;
	virtual bool needsKeyframe() const = 0;

	static std::unique_ptr<VideoDepacketizer> create(VideoCodec codec);
};

class H264Depacketizer final : public VideoDepacketizer {
public:
	VideoCodec codec() const override { return VideoCodec::H264; }
	void push(const Packet &packet, const FrameSink &onFrame) override;
	void notifyLoss() override;
	bool needsKeyframe() const override { return waitingForKeyframe_; }

private:
	void appendNal(const uint8_t *data, size_t size);
	void flush(const FrameSink &onFrame);
	void discard();

	std::vector<uint8_t> frame_;
	uint32_t timestamp_ = 0;
	int64_t arrivalUs_ = 0;
	bool hasData_ = false;
	bool frameHasIdr_ = false;
	bool frameHasParameterSets_ = false;
	bool frameCorrupt_ = false;
	bool fragmentOpen_ = false;
	bool waitingForKeyframe_ = true;
};

class Vp8Depacketizer final : public VideoDepacketizer {
public:
	VideoCodec codec() const override { return VideoCodec::VP8; }
	void push(const Packet &packet, const FrameSink &onFrame) override;
	void notifyLoss() override;
	bool needsKeyframe() const override { return waitingForKeyframe_; }

private:
	void discard();

	std::vector<uint8_t> frame_;
	uint32_t timestamp_ = 0;
	int64_t arrivalUs_ = 0;
	bool frameOpen_ = false;
	bool waitingForKeyframe_ = true;
};

/** VP9 (RFC 9628). Layers of one picture are concatenated into a single superframe-style buffer. */
class Vp9Depacketizer final : public VideoDepacketizer {
public:
	VideoCodec codec() const override { return VideoCodec::VP9; }
	void push(const Packet &packet, const FrameSink &onFrame) override;
	void notifyLoss() override;
	bool needsKeyframe() const override { return waitingForKeyframe_; }

private:
	void discard();

	std::vector<uint8_t> frame_;
	uint32_t timestamp_ = 0;
	int64_t arrivalUs_ = 0;
	bool frameOpen_ = false;
	bool frameIsKey_ = false;
	bool waitingForKeyframe_ = true;
};

} // namespace qc::rtp
