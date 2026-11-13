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

#include "rtp/receive-stream.hpp"
#include "rtp/rtcp.hpp"

#include <gtest/gtest.h>

using namespace qc::rtp;

namespace {
std::vector<uint8_t> makeRtp(uint16_t sequence, uint32_t timestamp, uint32_t ssrc, bool marker = false,
			     uint8_t payloadType = 96, size_t payloadSize = 10)
{
	std::vector<uint8_t> packet(12 + payloadSize, 0xAB);
	packet[0] = 0x80;
	packet[1] = static_cast<uint8_t>(payloadType | (marker ? 0x80 : 0));
	writeU16(packet.data() + 2, sequence);
	writeU32(packet.data() + 4, timestamp);
	writeU32(packet.data() + 8, ssrc);
	return packet;
}
} // namespace

TEST(RtpPacket, ParsesHeaderExtensionAndPadding)
{
	// V=2, P=1, X=1, CC=1
	std::vector<uint8_t> raw = {0xB1, 0xE0, 0x12, 0x34, 0, 0, 0, 100, 0, 0, 0, 7};
	raw.insert(raw.end(), {0, 0, 0, 9});                   // CSRC
	raw.insert(raw.end(), {0xBE, 0xDE, 0, 1, 1, 2, 3, 4}); // one-word extension
	raw.insert(raw.end(), {0xAA, 0xBB, 0xCC});             // payload
	raw.insert(raw.end(), {0, 0, 3});                      // 3 bytes padding

	const auto packet = parsePacket(raw.data(), raw.size());
	ASSERT_TRUE(packet);
	EXPECT_TRUE(packet->marker);
	EXPECT_EQ(packet->payloadType, 96);
	EXPECT_EQ(packet->sequence, 0x1234);
	EXPECT_EQ(packet->timestamp, 100u);
	EXPECT_EQ(packet->ssrc, 7u);
	EXPECT_EQ(packet->payload, (std::vector<uint8_t>{0xAA, 0xBB, 0xCC}));
}

TEST(RtpPacket, RejectsGarbage)
{
	const uint8_t tooShort[] = {0x80, 96, 0};
	EXPECT_FALSE(parsePacket(tooShort, sizeof(tooShort)));
	auto wrongVersion = makeRtp(1, 1, 1);
	wrongVersion[0] = 0x40;
	EXPECT_FALSE(parsePacket(wrongVersion.data(), wrongVersion.size()));
	auto badPadding = makeRtp(1, 1, 1);
	badPadding[0] |= 0x20;
	badPadding.back() = 200;
	EXPECT_FALSE(parsePacket(badPadding.data(), badPadding.size()));
}

TEST(Rtcp, PliLayout)
{
	const auto pli = buildPli(0x01020304, 0x0A0B0C0D);
	EXPECT_EQ(pli, (std::vector<uint8_t>{0x81, 206, 0, 2, 1, 2, 3, 4, 0x0A, 0x0B, 0x0C, 0x0D}));

	ReceivedFeedback feedback;
	parseCompound(pli.data(), pli.size(), nullptr, &feedback);
	EXPECT_TRUE(feedback.keyframeRequested);
}

TEST(Rtcp, NackPacksRunsIntoBitmask)
{
	const auto nack = buildNack(1, 2, {100, 101, 103, 116, 117, 400});
	// 100 + mask(101,103,116), then 117, then 400
	ASSERT_EQ(nack.size(), 12u + 3 * 4);
	EXPECT_EQ(readU16(nack.data() + 12), 100);
	EXPECT_EQ(readU16(nack.data() + 14), (1 << 0) | (1 << 2) | (1 << 15));
	EXPECT_EQ(readU16(nack.data() + 16), 117);
	EXPECT_EQ(readU16(nack.data() + 20), 400);

	ReceivedFeedback feedback;
	parseCompound(nack.data(), nack.size(), nullptr, &feedback);
	EXPECT_EQ(feedback.nackedSequences, (std::vector<uint16_t>{100, 101, 103, 116, 117, 400}));
}

TEST(Rtcp, RembRoundTrip)
{
	for (uint64_t bitrate : {uint64_t(64'000), uint64_t(2'500'000), uint64_t(25'000'000)}) {
		const auto remb = buildRemb(1, 0xDEADBEEF, bitrate);
		ASSERT_EQ(remb.size(), 24u);
		EXPECT_EQ(readU32(remb.data() + 20), 0xDEADBEEFu);
		ReceivedFeedback feedback;
		parseCompound(remb.data(), remb.size(), nullptr, &feedback);
		ASSERT_TRUE(feedback.rembBitrate);
		// Mantissa is 18 bits, so large values are rounded down slightly.
		EXPECT_LE(*feedback.rembBitrate, bitrate);
		EXPECT_GT(*feedback.rembBitrate, bitrate - bitrate / 1000 - 1);
	}
}

TEST(Rtcp, SenderReportInCompound)
{
	std::vector<uint8_t> sr(28, 0);
	sr[0] = 0x80;
	sr[1] = 200;
	writeU16(sr.data() + 2, 6);
	writeU32(sr.data() + 4, 0x11223344);
	writeU32(sr.data() + 8, 100);         // NTP seconds
	writeU32(sr.data() + 12, 0x80000000); // NTP fraction = .5
	writeU32(sr.data() + 16, 90000);
	const auto pli = buildPli(1, 2);
	sr.insert(sr.end(), pli.begin(), pli.end());

	int reports = 0;
	parseCompound(sr.data(), sr.size(), [&](const SenderReport &report) {
		++reports;
		EXPECT_EQ(report.ssrc, 0x11223344u);
		EXPECT_DOUBLE_EQ(report.ntpSeconds(), 100.5);
		EXPECT_EQ(report.rtpTimestamp, 90000u);
		EXPECT_EQ(report.compactNtp(), (100u << 16) | 0x8000u);
	});
	EXPECT_EQ(reports, 1);
}

TEST(ReceiveStream, ReportsLossAndRequestsRetransmission)
{
	ReceiveStream::Config config;
	config.rembBitrateBps = 4'000'000;
	ReceiveStream stream(config);

	std::vector<std::vector<uint8_t>> rtcp;
	std::vector<uint16_t> delivered;
	const auto feed = [&](uint16_t sequence, int64_t nowUs) {
		const auto raw = makeRtp(sequence, sequence * 3000u, 0xCAFE);
		stream.receiveRtp(
			raw.data(), raw.size(), nowUs, [&](Packet &&p) { delivered.push_back(p.sequence); }, nullptr,
			[&](std::vector<uint8_t> &&packet) { rtcp.push_back(std::move(packet)); });
	};

	feed(1, 0);
	ASSERT_EQ(rtcp.size(), 1u) << "first packet triggers an immediate report";
	ReceivedFeedback first;
	parseCompound(rtcp[0].data(), rtcp[0].size(), nullptr, &first);
	ASSERT_EQ(first.reportBlocks.size(), 1u);
	EXPECT_EQ(first.reportBlocks[0].mediaSsrc, 0xCAFEu);
	ASSERT_TRUE(first.rembBitrate);

	rtcp.clear();
	feed(2, 10'000);
	feed(5, 20'000); // 3 and 4 missing
	ASSERT_EQ(rtcp.size(), 1u);
	ReceivedFeedback nack;
	parseCompound(rtcp[0].data(), rtcp[0].size(), nullptr, &nack);
	EXPECT_EQ(nack.nackedSequences, (std::vector<uint16_t>{3, 4}));
	EXPECT_EQ(delivered, (std::vector<uint16_t>{1, 2}));

	feed(3, 30'000); // retransmission
	feed(4, 31'000);
	EXPECT_EQ(delivered, (std::vector<uint16_t>{1, 2, 3, 4, 5}));
	EXPECT_EQ(stream.stats().packetsReceived, 5u);
	EXPECT_EQ(*stream.remoteSsrc(), 0xCAFEu);

	rtcp.clear();
	EXPECT_TRUE(stream.requestKeyframe(1'000'000, [&](std::vector<uint8_t> &&p) { rtcp.push_back(std::move(p)); }));
	EXPECT_FALSE(stream.requestKeyframe(1'100'000, [&](std::vector<uint8_t> &&p) { rtcp.push_back(std::move(p)); }))
		<< "rate limited";
	EXPECT_EQ(rtcp.size(), 1u);
}

TEST(ReceiveStream, SequenceWrapIsNotLoss)
{
	ReceiveStream stream(ReceiveStream::Config{});
	std::vector<uint16_t> delivered;
	int64_t lost = 0;
	for (uint32_t i = 0; i < 10; ++i) {
		const uint16_t sequence = static_cast<uint16_t>(65530 + i);
		const auto raw = makeRtp(sequence, i * 3000, 1);
		stream.receiveRtp(
			raw.data(), raw.size(), i * 1000, [&](Packet &&p) { delivered.push_back(p.sequence); },
			[&](int64_t n) { lost += n; }, nullptr);
	}
	EXPECT_EQ(delivered.size(), 10u);
	EXPECT_EQ(delivered.back(), 3);
	EXPECT_EQ(lost, 0);
}
