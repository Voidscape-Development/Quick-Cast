#include "rtp/sync-clock.hpp"

#include <gtest/gtest.h>

using namespace qc::rtp;

namespace {
double seconds(uint64_t ns)
{
	return static_cast<double>(ns) / 1e9;
}
} // namespace

TEST(SyncClock, FollowsRtpCadenceNotArrivalJitter)
{
	SyncClock clock;
	clock.setClockRate(SyncClock::Kind::Video, 90000);

	const uint64_t first = clock.toLocalNs(SyncClock::Kind::Video, 0, 1'000'000);
	// Second frame is 33.3 ms later in media time but arrives 20 ms late.
	const uint64_t second = clock.toLocalNs(SyncClock::Kind::Video, 3000, 1'053'333);
	EXPECT_NEAR(seconds(second) - seconds(first), 3000.0 / 90000.0, 0.001);
}

TEST(SyncClock, SenderReportsAlignAudioAndVideo)
{
	SyncClock clock;
	clock.setClockRate(SyncClock::Kind::Video, 90000);
	clock.setClockRate(SyncClock::Kind::Audio, 48000);

	// Sender wall clock 1000.0 s corresponds to video RTP 900000 and audio RTP 480000.
	clock.onSenderReport(SyncClock::Kind::Video, 1000.0, 900'000);
	clock.onSenderReport(SyncClock::Kind::Audio, 1000.0, 480'000);

	// Media captured at the same instant (sender time 1000.5 s). Video arrives 80 ms after the audio.
	const uint64_t audio = clock.toLocalNs(SyncClock::Kind::Audio, 480'000 + 24'000, 5'000'000);
	const uint64_t video = clock.toLocalNs(SyncClock::Kind::Video, 900'000 + 45'000, 5'080'000);
	EXPECT_NEAR(seconds(audio), seconds(video), 0.001) << "same capture time must map to the same local time";
}

TEST(SyncClock, TimestampsAreMonotonicPerStream)
{
	SyncClock clock;
	clock.setClockRate(SyncClock::Kind::Audio, 48000);
	uint64_t last = 0;
	for (uint32_t i = 0; i < 100; ++i) {
		// Arrival times jump around; output must still move forward.
		const int64_t arrival = 1'000'000 + i * 20'000 + ((i % 7) * 3000);
		const uint64_t ts = clock.toLocalNs(SyncClock::Kind::Audio, i * 960, arrival);
		EXPECT_GT(ts, last);
		last = ts;
	}
}

TEST(SyncClock, SurvivesRtpTimestampWrap)
{
	SyncClock clock;
	clock.setClockRate(SyncClock::Kind::Video, 90000);
	const uint64_t before = clock.toLocalNs(SyncClock::Kind::Video, 0xFFFFF000u, 1'000'000);
	const uint64_t after = clock.toLocalNs(SyncClock::Kind::Video, 0x00000BB8u, 1'078'000);
	EXPECT_NEAR(seconds(after) - seconds(before), (4096 + 3000) / 90000.0, 0.002);
}
