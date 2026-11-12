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

#include "video-depacketizer.hpp"

namespace qc::rtp {

const char *toString(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::H264:
		return "H264";
	case VideoCodec::VP8:
		return "VP8";
	}
	return "unknown";
}

std::unique_ptr<VideoDepacketizer> VideoDepacketizer::create(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::H264:
		return std::make_unique<H264Depacketizer>();
	case VideoCodec::VP8:
		return std::make_unique<Vp8Depacketizer>();
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// H.264 (RFC 6184), packetization-mode 0 and 1: single NAL units, STAP-A aggregates and FU-A fragments.
// ---------------------------------------------------------------------------------------------------------------

namespace {
constexpr uint8_t kNalSlice = 1;
constexpr uint8_t kNalIdr = 5;
constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;
constexpr uint8_t kNalStapA = 24;
constexpr uint8_t kNalFuA = 28;
} // namespace

void H264Depacketizer::appendNal(const uint8_t *data, size_t size)
{
	if (size == 0)
		return;
	const uint8_t type = data[0] & 0x1F;
	if (type == kNalIdr)
		frameHasIdr_ = true;
	else if (type == kNalSps || type == kNalPps)
		frameHasParameterSets_ = true;

	static const uint8_t startCode[] = {0, 0, 0, 1};
	frame_.insert(frame_.end(), startCode, startCode + 4);
	frame_.insert(frame_.end(), data, data + size);
	hasData_ = true;
}

void H264Depacketizer::discard()
{
	frame_.clear();
	hasData_ = false;
	frameHasIdr_ = false;
	frameHasParameterSets_ = false;
	frameCorrupt_ = false;
	fragmentOpen_ = false;
}

void H264Depacketizer::notifyLoss()
{
	// The access unit in flight is incomplete and every later inter frame references it.
	discard();
	frameCorrupt_ = true;
	waitingForKeyframe_ = true;
}

void H264Depacketizer::flush(const FrameSink &onFrame)
{
	if (hasData_ && !frameCorrupt_) {
		// Chrome sends SPS/PPS in-band with every IDR, so an IDR access unit is self-contained.
		const bool keyframe = frameHasIdr_;
		if (keyframe)
			waitingForKeyframe_ = false;
		if (!waitingForKeyframe_ && onFrame) {
			EncodedFrame frame;
			frame.data = std::move(frame_);
			frame.rtpTimestamp = timestamp_;
			frame.arrivalUs = arrivalUs_;
			frame.keyframe = keyframe;
			onFrame(std::move(frame));
		}
	}
	discard();
}

void H264Depacketizer::push(const Packet &packet, const FrameSink &onFrame)
{
	// A timestamp change without a marker bit still terminates the previous access unit.
	if (hasData_ && packet.timestamp != timestamp_)
		flush(onFrame);

	if (!hasData_ && !frameCorrupt_) {
		timestamp_ = packet.timestamp;
		arrivalUs_ = packet.arrivalUs;
	} else if (frameCorrupt_ && packet.timestamp != timestamp_) {
		// First packet of a new access unit after a loss: start clean.
		frameCorrupt_ = false;
		timestamp_ = packet.timestamp;
		arrivalUs_ = packet.arrivalUs;
	}

	const uint8_t *data = packet.payload.data();
	const size_t size = packet.payload.size();

	if (size > 0 && !frameCorrupt_) {
		const uint8_t type = data[0] & 0x1F;

		if (type >= kNalSlice && type < kNalStapA) {
			appendNal(data, size);
		} else if (type == kNalStapA) {
			size_t offset = 1;
			while (offset + 2 <= size) {
				const size_t nalSize = readU16(data + offset);
				offset += 2;
				if (nalSize == 0 || offset + nalSize > size) {
					frameCorrupt_ = true;
					break;
				}
				appendNal(data + offset, nalSize);
				offset += nalSize;
			}
		} else if (type == kNalFuA && size >= 2) {
			const bool start = (data[1] & 0x80) != 0;
			const bool end = (data[1] & 0x40) != 0;
			if (start) {
				const uint8_t header = static_cast<uint8_t>((data[0] & 0xE0) | (data[1] & 0x1F));
				appendNal(&header, 1);
				fragmentOpen_ = true;
			}
			if (fragmentOpen_) {
				frame_.insert(frame_.end(), data + 2, data + size);
			} else {
				// Fragment continuation without its start: the start was lost.
				frameCorrupt_ = true;
				waitingForKeyframe_ = true;
			}
			if (end)
				fragmentOpen_ = false;
		}
		// Other aggregation/fragmentation types (STAP-B, MTAP, FU-B) require interleaved mode, which is never
		// negotiated, so they are ignored.
	}

	if (packet.marker) {
		timestamp_ = packet.timestamp;
		flush(onFrame);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// VP8 (RFC 7741)
// ---------------------------------------------------------------------------------------------------------------

void Vp8Depacketizer::discard()
{
	frame_.clear();
	frameOpen_ = false;
}

void Vp8Depacketizer::notifyLoss()
{
	discard();
	waitingForKeyframe_ = true;
}

void Vp8Depacketizer::push(const Packet &packet, const FrameSink &onFrame)
{
	const uint8_t *data = packet.payload.data();
	const size_t size = packet.payload.size();
	if (size < 1)
		return;

	// Payload descriptor.
	size_t offset = 1;
	const bool extended = (data[0] & 0x80) != 0;
	const bool startOfPartition = (data[0] & 0x10) != 0;
	const uint8_t partitionId = data[0] & 0x07;

	if (extended) {
		if (offset >= size)
			return;
		const uint8_t ext = data[offset++];
		if (ext & 0x80) { // PictureID, 7 or 15 bits
			if (offset >= size)
				return;
			offset += (data[offset] & 0x80) ? 2 : 1;
		}
		if (ext & 0x40) // TL0PICIDX
			offset += 1;
		if (ext & 0x30) // TID / KEYIDX
			offset += 1;
	}
	if (offset >= size)
		return;

	const bool frameStart = startOfPartition && partitionId == 0;

	if (frameStart) {
		// A new frame started before the previous one saw its marker: the tail was lost.
		if (frameOpen_)
			waitingForKeyframe_ = true;
		frame_.clear();
		frameOpen_ = true;
		timestamp_ = packet.timestamp;
		arrivalUs_ = packet.arrivalUs;
	} else if (!frameOpen_ || packet.timestamp != timestamp_) {
		// Continuation of a frame whose beginning we never saw.
		discard();
		waitingForKeyframe_ = true;
		return;
	}

	frame_.insert(frame_.end(), data + offset, data + size);

	if (packet.marker) {
		// The P bit (LSB of the first payload byte) is 0 for key frames.
		const bool keyframe = !frame_.empty() && (frame_[0] & 0x01) == 0;
		if (keyframe)
			waitingForKeyframe_ = false;
		if (!waitingForKeyframe_ && onFrame) {
			EncodedFrame frame;
			frame.data = std::move(frame_);
			frame.rtpTimestamp = timestamp_;
			frame.arrivalUs = arrivalUs_;
			frame.keyframe = keyframe;
			onFrame(std::move(frame));
		}
		discard();
	}
}

} // namespace qc::rtp
