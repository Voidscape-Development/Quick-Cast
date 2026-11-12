#include "rtp/video-depacketizer.hpp"

#include <gtest/gtest.h>

using namespace qc::rtp;

namespace {
Packet makePacket(std::vector<uint8_t> payload, uint32_t timestamp, bool marker)
{
	Packet packet;
	packet.payload = std::move(payload);
	packet.timestamp = timestamp;
	packet.marker = marker;
	packet.arrivalUs = timestamp;
	return packet;
}

const std::vector<uint8_t> kStart = {0, 0, 0, 1};

std::vector<uint8_t> annexB(std::initializer_list<std::vector<uint8_t>> nals)
{
	std::vector<uint8_t> out;
	for (const auto &nal : nals) {
		out.insert(out.end(), kStart.begin(), kStart.end());
		out.insert(out.end(), nal.begin(), nal.end());
	}
	return out;
}
} // namespace

TEST(H264Depacketizer, StapAAndFuAKeyframe)
{
	H264Depacketizer depacketizer;
	std::vector<EncodedFrame> frames;
	const auto sink = [&](EncodedFrame &&frame) { frames.push_back(std::move(frame)); };
	EXPECT_TRUE(depacketizer.needsKeyframe());

	// STAP-A carrying SPS (0x67 ..) and PPS (0x68 ..)
	depacketizer.push(makePacket({24, 0, 2, 0x67, 0x42, 0, 2, 0x68, 0xCE}, 1000, false), sink);
	// IDR slice (type 5) fragmented over three FU-A packets; NRI bits from the indicator are preserved.
	depacketizer.push(makePacket({0x7C, 0x85, 1, 2}, 1000, false), sink);
	depacketizer.push(makePacket({0x7C, 0x05, 3, 4}, 1000, false), sink);
	depacketizer.push(makePacket({0x7C, 0x45, 5}, 1000, true), sink);

	ASSERT_EQ(frames.size(), 1u);
	EXPECT_TRUE(frames[0].keyframe);
	EXPECT_EQ(frames[0].rtpTimestamp, 1000u);
	EXPECT_EQ(frames[0].data, annexB({{0x67, 0x42}, {0x68, 0xCE}, {0x65, 1, 2, 3, 4, 5}}));
	EXPECT_FALSE(depacketizer.needsKeyframe());

	// Single NAL delta frame.
	depacketizer.push(makePacket({0x41, 9, 9}, 4000, true), sink);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_FALSE(frames[1].keyframe);
	EXPECT_EQ(frames[1].data, annexB({{0x41, 9, 9}}));
}

TEST(H264Depacketizer, DeltaFramesBeforeKeyframeAreDropped)
{
	H264Depacketizer depacketizer;
	int frames = 0;
	depacketizer.push(makePacket({0x41, 1}, 1000, true), [&](EncodedFrame &&) { ++frames; });
	EXPECT_EQ(frames, 0);
	EXPECT_TRUE(depacketizer.needsKeyframe());
}

TEST(H264Depacketizer, LossDropsUntilNextKeyframe)
{
	H264Depacketizer depacketizer;
	std::vector<EncodedFrame> frames;
	const auto sink = [&](EncodedFrame &&frame) { frames.push_back(std::move(frame)); };

	depacketizer.push(makePacket({0x65, 1}, 1000, true), sink);
	ASSERT_EQ(frames.size(), 1u);

	depacketizer.push(makePacket({0x7C, 0x81, 1}, 2000, false), sink); // start of a fragmented delta frame
	depacketizer.notifyLoss();                                          // middle lost
	depacketizer.push(makePacket({0x7C, 0x41, 3}, 2000, true), sink);  // tail
	EXPECT_EQ(frames.size(), 1u) << "incomplete frame must not be emitted";
	EXPECT_TRUE(depacketizer.needsKeyframe());

	depacketizer.push(makePacket({0x41, 7}, 3000, true), sink);
	EXPECT_EQ(frames.size(), 1u) << "delta frames depend on the lost one";

	depacketizer.push(makePacket({0x65, 8}, 4000, true), sink);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_TRUE(frames[1].keyframe);
	EXPECT_FALSE(depacketizer.needsKeyframe());
}

TEST(H264Depacketizer, TimestampChangeFlushesWithoutMarker)
{
	H264Depacketizer depacketizer;
	std::vector<EncodedFrame> frames;
	const auto sink = [&](EncodedFrame &&frame) { frames.push_back(std::move(frame)); };
	depacketizer.push(makePacket({0x65, 1}, 1000, false), sink);
	depacketizer.push(makePacket({0x41, 2}, 2000, true), sink);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_EQ(frames[0].rtpTimestamp, 1000u);
	EXPECT_EQ(frames[1].rtpTimestamp, 2000u);
}

TEST(H264Depacketizer, MalformedStapAIsRejected)
{
	H264Depacketizer depacketizer;
	int frames = 0;
	// Declares a 200 byte NAL but carries 2.
	depacketizer.push(makePacket({24, 0, 200, 0x65, 1}, 1000, true), [&](EncodedFrame &&) { ++frames; });
	EXPECT_EQ(frames, 0);
}

TEST(Vp8Depacketizer, AssemblesKeyframeAcrossPackets)
{
	Vp8Depacketizer depacketizer;
	std::vector<EncodedFrame> frames;
	const auto sink = [&](EncodedFrame &&frame) { frames.push_back(std::move(frame)); };

	// X=1,S=1,PID=0 with a 15-bit picture ID; payload byte 0 has P=0 (keyframe).
	depacketizer.push(makePacket({0x90, 0x80, 0x80, 0x01, 0x10, 0xAA}, 1000, false), sink);
	// Continuation: X=1, S=0
	depacketizer.push(makePacket({0x80, 0x80, 0x80, 0x01, 0xBB}, 1000, true), sink);

	ASSERT_EQ(frames.size(), 1u);
	EXPECT_TRUE(frames[0].keyframe);
	EXPECT_EQ(frames[0].data, (std::vector<uint8_t>{0x10, 0xAA, 0xBB}));

	// Delta frame, no extension: S=1, P=1
	depacketizer.push(makePacket({0x10, 0x11, 0xCC}, 4000, true), sink);
	ASSERT_EQ(frames.size(), 2u);
	EXPECT_FALSE(frames[1].keyframe);
}

TEST(Vp8Depacketizer, WaitsForKeyframeAfterLoss)
{
	Vp8Depacketizer depacketizer;
	int frames = 0;
	const auto sink = [&](EncodedFrame &&) { ++frames; };
	depacketizer.push(makePacket({0x10, 0x10}, 1000, true), sink);
	EXPECT_EQ(frames, 1);
	depacketizer.notifyLoss();
	depacketizer.push(makePacket({0x10, 0x11}, 2000, true), sink);
	EXPECT_EQ(frames, 1);
	EXPECT_TRUE(depacketizer.needsKeyframe());
	depacketizer.push(makePacket({0x10, 0x10}, 3000, true), sink);
	EXPECT_EQ(frames, 2);
}
