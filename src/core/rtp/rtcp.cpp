#include "rtcp.hpp"
#include "rtp-packet.hpp"

#include <algorithm>

namespace qc::rtp {

namespace {
constexpr uint8_t kSenderReport = 200;
constexpr uint8_t kReceiverReport = 201;
constexpr uint8_t kTransportFeedback = 205; // RTPFB
constexpr uint8_t kPayloadFeedback = 206;   // PSFB

constexpr uint8_t kFmtNack = 1;
constexpr uint8_t kFmtPli = 1;
constexpr uint8_t kFmtFir = 4;
constexpr uint8_t kFmtApplication = 15; // REMB lives here

std::vector<uint8_t> makeHeader(uint8_t countOrFormat, uint8_t type, size_t totalBytes)
{
	std::vector<uint8_t> out(totalBytes, 0);
	out[0] = static_cast<uint8_t>(0x80 | (countOrFormat & 0x1F));
	out[1] = type;
	writeU16(out.data() + 2, static_cast<uint16_t>(totalBytes / 4 - 1));
	return out;
}

void parseReportBlocks(const uint8_t *data, size_t count, size_t available, ReceivedFeedback *feedback)
{
	if (!feedback)
		return;
	for (size_t i = 0; i < count && (i + 1) * 24 <= available; ++i) {
		const uint8_t *p = data + i * 24;
		ReportBlock block;
		block.mediaSsrc = readU32(p);
		block.fractionLost = p[4];
		int32_t lost = (p[5] << 16) | (p[6] << 8) | p[7];
		if (lost & 0x800000)
			lost |= ~0xFFFFFF;
		block.cumulativeLost = lost;
		block.extendedHighestSequence = readU32(p + 8);
		block.jitter = readU32(p + 12);
		block.lastSenderReport = readU32(p + 16);
		block.delaySinceLastSenderReport = readU32(p + 20);
		feedback->reportBlocks.push_back(block);
	}
}
} // namespace

void parseCompound(const uint8_t *data, size_t size, const std::function<void(const SenderReport &)> &onSenderReport,
		   ReceivedFeedback *feedback)
{
	size_t offset = 0;
	while (offset + 4 <= size) {
		const uint8_t *p = data + offset;
		if ((p[0] >> 6) != 2)
			return;
		const uint8_t count = p[0] & 0x1F;
		const uint8_t type = p[1];
		const size_t length = (static_cast<size_t>(readU16(p + 2)) + 1) * 4;
		if (offset + length > size)
			return;

		if (type == kSenderReport && length >= 28) {
			SenderReport report;
			report.ssrc = readU32(p + 4);
			report.ntpTimestamp = (static_cast<uint64_t>(readU32(p + 8)) << 32) | readU32(p + 12);
			report.rtpTimestamp = readU32(p + 16);
			report.packetCount = readU32(p + 20);
			report.octetCount = readU32(p + 24);
			if (onSenderReport)
				onSenderReport(report);
			parseReportBlocks(p + 28, count, length - 28, feedback);
		} else if (type == kReceiverReport && length >= 8) {
			parseReportBlocks(p + 8, count, length - 8, feedback);
		} else if (feedback && type == kPayloadFeedback && length >= 12) {
			if (count == kFmtPli || count == kFmtFir) {
				feedback->keyframeRequested = true;
			} else if (count == kFmtApplication && length >= 20 && p[12] == 'R' && p[13] == 'E' &&
				   p[14] == 'M' && p[15] == 'B') {
				const uint32_t exponent = p[17] >> 2;
				const uint64_t mantissa = (static_cast<uint64_t>(p[17] & 0x03) << 16) |
							  (static_cast<uint64_t>(p[18]) << 8) | p[19];
				feedback->rembBitrate = mantissa << exponent;
			}
		} else if (feedback && type == kTransportFeedback && count == kFmtNack) {
			for (size_t fci = 12; fci + 4 <= length; fci += 4) {
				const uint16_t pid = readU16(p + fci);
				const uint16_t blp = readU16(p + fci + 2);
				feedback->nackedSequences.push_back(pid);
				for (int bit = 0; bit < 16; ++bit) {
					if (blp & (1 << bit))
						feedback->nackedSequences.push_back(
							static_cast<uint16_t>(pid + bit + 1));
				}
			}
		}

		offset += length;
	}
}

std::vector<uint8_t> buildReceiverReport(uint32_t senderSsrc, const ReportBlock *block)
{
	auto out = makeHeader(block ? 1 : 0, kReceiverReport, block ? 32 : 8);
	writeU32(out.data() + 4, senderSsrc);
	if (block) {
		uint8_t *p = out.data() + 8;
		writeU32(p, block->mediaSsrc);
		const int32_t lost = std::clamp(block->cumulativeLost, -0x800000, 0x7FFFFF);
		p[4] = block->fractionLost;
		p[5] = static_cast<uint8_t>((lost >> 16) & 0xFF);
		p[6] = static_cast<uint8_t>((lost >> 8) & 0xFF);
		p[7] = static_cast<uint8_t>(lost & 0xFF);
		writeU32(p + 8, block->extendedHighestSequence);
		writeU32(p + 12, block->jitter);
		writeU32(p + 16, block->lastSenderReport);
		writeU32(p + 20, block->delaySinceLastSenderReport);
	}
	return out;
}

std::vector<uint8_t> buildPli(uint32_t senderSsrc, uint32_t mediaSsrc)
{
	auto out = makeHeader(kFmtPli, kPayloadFeedback, 12);
	writeU32(out.data() + 4, senderSsrc);
	writeU32(out.data() + 8, mediaSsrc);
	return out;
}

std::vector<uint8_t> buildNack(uint32_t senderSsrc, uint32_t mediaSsrc, const std::vector<uint16_t> &sequences)
{
	// Each FCI entry covers a packet id plus a 16-bit mask of the following packets.
	std::vector<std::pair<uint16_t, uint16_t>> entries;
	for (uint16_t sequence : sequences) {
		if (!entries.empty()) {
			const uint16_t delta = static_cast<uint16_t>(sequence - entries.back().first);
			if (delta >= 1 && delta <= 16) {
				entries.back().second |= static_cast<uint16_t>(1 << (delta - 1));
				continue;
			}
			if (delta == 0)
				continue;
		}
		entries.emplace_back(sequence, static_cast<uint16_t>(0));
	}

	auto out = makeHeader(kFmtNack, kTransportFeedback, 12 + entries.size() * 4);
	writeU32(out.data() + 4, senderSsrc);
	writeU32(out.data() + 8, mediaSsrc);
	size_t offset = 12;
	for (const auto &[pid, blp] : entries) {
		writeU16(out.data() + offset, pid);
		writeU16(out.data() + offset + 2, blp);
		offset += 4;
	}
	return out;
}

std::vector<uint8_t> buildRemb(uint32_t senderSsrc, uint32_t mediaSsrc, uint64_t bitrateBps)
{
	auto out = makeHeader(kFmtApplication, kPayloadFeedback, 24);
	writeU32(out.data() + 4, senderSsrc);
	// Media source SSRC is always zero for REMB; the affected SSRCs are listed in the FCI.
	out[12] = 'R';
	out[13] = 'E';
	out[14] = 'M';
	out[15] = 'B';
	out[16] = 1; // one SSRC

	uint32_t exponent = 0;
	uint64_t mantissa = bitrateBps;
	while (mantissa > 0x3FFFF) {
		mantissa >>= 1;
		++exponent;
	}
	out[17] = static_cast<uint8_t>((exponent << 2) | ((mantissa >> 16) & 0x03));
	out[18] = static_cast<uint8_t>((mantissa >> 8) & 0xFF);
	out[19] = static_cast<uint8_t>(mantissa & 0xFF);
	writeU32(out.data() + 20, mediaSsrc);
	return out;
}

} // namespace qc::rtp
