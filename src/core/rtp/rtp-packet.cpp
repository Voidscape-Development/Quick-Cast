#include "rtp-packet.hpp"

namespace qc::rtp {

std::optional<Packet> parsePacket(const uint8_t *data, size_t size)
{
	constexpr size_t kFixedHeader = 12;
	if (!data || size < kFixedHeader)
		return std::nullopt;

	const uint8_t version = data[0] >> 6;
	if (version != 2)
		return std::nullopt;

	const bool hasPadding = (data[0] & 0x20) != 0;
	const bool hasExtension = (data[0] & 0x10) != 0;
	const size_t csrcCount = data[0] & 0x0F;

	Packet packet;
	packet.marker = (data[1] & 0x80) != 0;
	packet.payloadType = data[1] & 0x7F;
	packet.sequence = readU16(data + 2);
	packet.timestamp = readU32(data + 4);
	packet.ssrc = readU32(data + 8);

	size_t offset = kFixedHeader + csrcCount * 4;
	if (offset > size)
		return std::nullopt;

	if (hasExtension) {
		if (offset + 4 > size)
			return std::nullopt;
		const size_t extensionWords = readU16(data + offset + 2);
		offset += 4 + extensionWords * 4;
		if (offset > size)
			return std::nullopt;
	}

	size_t end = size;
	if (hasPadding) {
		const size_t padding = data[size - 1];
		if (padding == 0 || padding > end - offset)
			return std::nullopt;
		end -= padding;
	}

	packet.payload.assign(data + offset, data + end);
	return packet;
}

} // namespace qc::rtp
