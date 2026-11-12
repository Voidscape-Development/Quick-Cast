#include "rtp/reorder-buffer.hpp"

#include <gtest/gtest.h>

using namespace qc::rtp;

namespace {
struct Harness {
	ReorderBuffer buffer;
	std::vector<int64_t> delivered;
	int64_t lost = 0;

	explicit Harness(ReorderBuffer::Config config = {}) : buffer(config) {}

	void push(int64_t sequence, int64_t nowUs)
	{
		Packet packet;
		packet.extendedSequence = sequence;
		packet.sequence = static_cast<uint16_t>(sequence);
		packet.arrivalUs = nowUs;
		buffer.push(
			std::move(packet), nowUs, [&](Packet &&p) { delivered.push_back(p.extendedSequence); },
			[&](int64_t count) { lost += count; });
	}
};
} // namespace

TEST(ReorderBuffer, InOrderPassesStraightThrough)
{
	Harness h;
	for (int i = 100; i < 105; ++i)
		h.push(i, i * 1000);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{100, 101, 102, 103, 104}));
	EXPECT_EQ(h.lost, 0);
	EXPECT_EQ(h.buffer.bufferedPackets(), 0u);
}

TEST(ReorderBuffer, ReordersWithinWindow)
{
	Harness h;
	h.push(1, 0);
	h.push(3, 1000);
	h.push(4, 2000);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{1}));
	h.push(2, 3000);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{1, 2, 3, 4}));
	EXPECT_EQ(h.lost, 0);
}

TEST(ReorderBuffer, GivesUpAfterMaxWait)
{
	ReorderBuffer::Config config;
	config.maxWaitUs = 100'000;
	Harness h(config);
	h.push(1, 0);
	h.push(3, 10'000);
	h.push(4, 50'000);
	EXPECT_EQ(h.delivered.size(), 1u);
	h.push(5, 120'000); // hole at 2 has now been open for 110 ms
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{1, 3, 4, 5}));
	EXPECT_EQ(h.lost, 1);

	// A retransmission that arrives after we gave up is discarded.
	h.push(2, 130'000);
	EXPECT_EQ(h.delivered.size(), 4u);
}

TEST(ReorderBuffer, DuplicatesAreDropped)
{
	Harness h;
	h.push(1, 0);
	h.push(1, 10);
	h.push(2, 20);
	h.push(2, 30);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{1, 2}));
}

TEST(ReorderBuffer, NacksMissingPacketsWithBackoff)
{
	ReorderBuffer::Config config;
	config.nackIntervalUs = 40'000;
	config.maxNackAttempts = 2;
	config.maxWaitUs = 1'000'000;
	Harness h(config);
	h.push(10, 0);
	h.push(13, 1000);

	EXPECT_EQ(h.buffer.collectNacks(1000), (std::vector<uint16_t>{11, 12}));
	EXPECT_TRUE(h.buffer.collectNacks(20'000).empty()) << "too soon to ask again";
	EXPECT_EQ(h.buffer.collectNacks(45'000), (std::vector<uint16_t>{11, 12}));
	EXPECT_TRUE(h.buffer.collectNacks(200'000).empty()) << "attempts exhausted";

	h.push(11, 210'000);
	h.push(12, 211'000);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{10, 11, 12, 13}));
	EXPECT_TRUE(h.buffer.collectNacks(300'000).empty());
}

TEST(ReorderBuffer, HugeJumpResynchronises)
{
	Harness h;
	h.push(1, 0);
	h.push(100'000, 1000);
	EXPECT_EQ(h.delivered, (std::vector<int64_t>{1, 100'000}));
	EXPECT_GT(h.lost, 0);
	EXPECT_TRUE(h.buffer.collectNacks(2000).empty());
}

TEST(Unwrapper, HandlesWrapAroundAndReordering)
{
	Unwrapper<uint16_t> unwrapper;
	EXPECT_EQ(unwrapper.unwrap(65534), 65534);
	EXPECT_EQ(unwrapper.unwrap(65535), 65535);
	EXPECT_EQ(unwrapper.unwrap(0), 65536);
	EXPECT_EQ(unwrapper.unwrap(65535), 65535);
	EXPECT_EQ(unwrapper.unwrap(2), 65538);
}
