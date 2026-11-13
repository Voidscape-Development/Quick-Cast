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

#include "vdo/sdp-utils.hpp"

#include <gtest/gtest.h>

using namespace qc::vdo;

namespace {
const char *kChromeOffer = "v=0\r\n"
			   "o=- 46117317 2 IN IP4 127.0.0.1\r\n"
			   "s=-\r\n"
			   "t=0 0\r\n"
			   "a=group:BUNDLE 0 1 2\r\n"
			   "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
			   "a=mid:0\r\n"
			   "a=sctp-port:5000\r\n"
			   "m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 104 105\r\n"
			   "a=mid:1\r\n"
			   "a=sendonly\r\n"
			   "a=rtpmap:96 VP8/90000\r\n"
			   "a=rtpmap:97 rtx/90000\r\n"
			   "a=fmtp:97 apt=96\r\n"
			   "a=rtpmap:102 H264/90000\r\n"
			   "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f\r\n"
			   "a=rtpmap:103 H264/90000\r\n"
			   "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"
			   "a=rtpmap:104 H264/90000\r\n"
			   "a=fmtp:104 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
			   "a=rtpmap:105 H264/90000\r\n"
			   "a=fmtp:105 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c1f\r\n"
			   "m=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\n"
			   "a=mid:2\r\n"
			   "a=sendonly\r\n"
			   "a=rtpmap:111 opus/48000/2\r\n"
			   "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
			   "a=rtpmap:63 red/48000/2\r\n";
} // namespace

TEST(SdpUtils, ParsesSections)
{
	const auto sections = parseSdpMedia(kChromeOffer);
	ASSERT_EQ(sections.size(), 3u);
	EXPECT_EQ(sections[0].kind, "application");
	EXPECT_EQ(sections[1].kind, "video");
	EXPECT_EQ(sections[1].mid, "1");
	EXPECT_TRUE(sections[1].remoteSends);
	EXPECT_EQ(sections[1].codecs.size(), 6u);
	EXPECT_EQ(sections[2].codecs[0].name, "OPUS");
	EXPECT_EQ(sections[2].codecs[0].clockRate, 48000);
	EXPECT_EQ(sections[2].codecs[0].channels, 2);
}

TEST(SdpUtils, PrefersConstrainedBaselineNonInterleaved)
{
	const auto sections = parseSdpMedia(kChromeOffer);
	const auto h264 = chooseVideoCodec(sections[1], VideoCodecPreference::H264);
	ASSERT_TRUE(h264);
	EXPECT_EQ(h264->payloadType, 104);

	const auto vp8 = chooseVideoCodec(sections[1], VideoCodecPreference::VP8);
	ASSERT_TRUE(vp8);
	EXPECT_EQ(vp8->payloadType, 96);

	const auto opus = chooseAudioCodec(sections[2]);
	ASSERT_TRUE(opus);
	EXPECT_EQ(opus->payloadType, 111);
}

TEST(SdpUtils, FallsBackWhenPreferredCodecMissing)
{
	const auto sections = parseSdpMedia("m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:v\r\na=rtpmap:96 VP8/90000\r\n");
	ASSERT_EQ(sections.size(), 1u);
	const auto codec = chooseVideoCodec(sections[0], VideoCodecPreference::H264);
	ASSERT_TRUE(codec);
	EXPECT_EQ(codec->name, "VP8");

	const auto none = parseSdpMedia("m=video 9 UDP/TLS/RTP/SAVPF 98\r\na=mid:v\r\na=rtpmap:98 AV1/90000\r\n");
	EXPECT_FALSE(chooseVideoCodec(none[0], VideoCodecPreference::H264));
}

TEST(SdpUtils, DirectionAndRejectedSections)
{
	const auto sections = parseSdpMedia("m=video 0 UDP/TLS/RTP/SAVPF 96\r\na=mid:a\r\na=rtpmap:96 VP8/90000\r\n"
					    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:b\r\na=recvonly\r\n"
					    "a=rtpmap:111 opus/48000/2\r\n");
	ASSERT_EQ(sections.size(), 2u);
	EXPECT_FALSE(sections[0].remoteSends) << "port 0 means the section is rejected";
	EXPECT_FALSE(sections[1].remoteSends) << "recvonly means nothing will be sent to us";
}

TEST(SdpUtils, FmtpValueLookup)
{
	EXPECT_EQ(fmtpValue("level-asymmetry-allowed=1; Packetization-Mode=1;profile-level-id=42E01F",
			    "packetization-mode"),
		  "1");
	EXPECT_EQ(fmtpValue("a=1;b=2", "profile-level-id"), "");
	EXPECT_EQ(fmtpValue("profile-level-id=42E01F", "PROFILE-LEVEL-ID"), "42e01f");
}

TEST(SdpUtils, Vp9OnlyWhenPreferredAndProfileZero)
{
	const auto sections = parseSdpMedia("m=video 9 UDP/TLS/RTP/SAVPF 96 98 100 102\r\na=mid:v\r\n"
					    "a=rtpmap:96 VP8/90000\r\n"
					    "a=rtpmap:100 VP9/90000\r\na=fmtp:100 profile-id=2\r\n"
					    "a=rtpmap:98 VP9/90000\r\na=fmtp:98 profile-id=0\r\n"
					    "a=rtpmap:102 H264/90000\r\n"
					    "a=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n");
	ASSERT_EQ(sections.size(), 1u);

	const auto vp9 = chooseVideoCodec(sections[0], VideoCodecPreference::VP9);
	ASSERT_TRUE(vp9);
	EXPECT_EQ(vp9->payloadType, 98) << "the 10-bit profile is not decodable into what OBS expects";

	// VP9 is never picked over H.264 unless asked for.
	EXPECT_EQ(chooseVideoCodec(sections[0], VideoCodecPreference::H264)->name, "H264");

	const auto noVp9 = parseSdpMedia("m=video 9 UDP/TLS/RTP/SAVPF 96 102\r\na=mid:v\r\na=rtpmap:96 VP8/90000\r\n"
					 "a=rtpmap:102 H264/90000\r\na=fmtp:102 packetization-mode=1\r\n");
	EXPECT_EQ(chooseVideoCodec(noVp9[0], VideoCodecPreference::VP9)->name, "H264");
}
