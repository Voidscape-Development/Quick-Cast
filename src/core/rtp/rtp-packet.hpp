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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace qc::rtp {

inline uint16_t readU16(const uint8_t *p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t readU32(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void writeU16(uint8_t *p, uint16_t v)
{
	p[0] = static_cast<uint8_t>(v >> 8);
	p[1] = static_cast<uint8_t>(v);
}

inline void writeU32(uint8_t *p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v >> 24);
	p[1] = static_cast<uint8_t>(v >> 16);
	p[2] = static_cast<uint8_t>(v >> 8);
	p[3] = static_cast<uint8_t>(v);
}

/** A parsed RTP packet that owns its payload. */
struct Packet {
	uint8_t payloadType = 0;
	bool marker = false;
	uint16_t sequence = 0;
	uint32_t timestamp = 0;
	uint32_t ssrc = 0;
	std::vector<uint8_t> payload;

	/** Sequence number extended to 64 bits by the receive stream (monotonic across wrap-around). */
	int64_t extendedSequence = 0;
	/** Local monotonic arrival time in microseconds. */
	int64_t arrivalUs = 0;
};

/**
 * Parses an RTP packet (RFC 3550 section 5.1), stripping CSRCs, header extensions and padding.
 * Returns nullopt when the buffer is not a well-formed RTP packet.
 */
std::optional<Packet> parsePacket(const uint8_t *data, size_t size);

/** RTCP packets share the port with RTP; payload types 64-95 (after masking the marker bit) are RTCP (RFC 5761). */
inline bool looksLikeRtcp(const uint8_t *data, size_t size)
{
	if (size < 8)
		return false;
	const uint8_t pt = data[1] & 0x7F;
	return pt >= 64 && pt <= 95;
}

/** Extends 16-bit sequence numbers / 32-bit timestamps into a monotonic 64-bit space. */
template<typename T> class Unwrapper {
public:
	int64_t unwrap(T value)
	{
		if (!initialized_) {
			initialized_ = true;
			last_ = static_cast<int64_t>(value);
			return last_;
		}
		using Signed = std::make_signed_t<T>;
		const auto delta = static_cast<Signed>(static_cast<T>(value - static_cast<T>(last_)));
		last_ += delta;
		return last_;
	}

	void reset() { initialized_ = false; }

private:
	bool initialized_ = false;
	int64_t last_ = 0;
};

} // namespace qc::rtp
