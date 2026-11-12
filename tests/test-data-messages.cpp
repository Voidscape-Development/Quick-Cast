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

#include "vdo/data-messages.hpp"

#include <gtest/gtest.h>

using namespace qc::vdo;

TEST(DataMessages, PublisherInfoCarriesLabelAndMuteState)
{
	const auto parsed = parsePeerDataMessage(
		Json::parse(R"({"info":{"label":"Alice","muted":true,"video_muted_init":false}})"));
	ASSERT_TRUE(parsed.label);
	EXPECT_EQ(*parsed.label, "Alice");
	EXPECT_TRUE(*parsed.microphoneMuted);
	EXPECT_FALSE(*parsed.cameraMuted);
	EXPECT_FALSE(parsed.bye);
}

TEST(DataMessages, UnsetLabelIsEmpty)
{
	const auto parsed = parsePeerDataMessage(Json::parse(R"({"info":{"label":false}})"));
	ASSERT_TRUE(parsed.label);
	EXPECT_TRUE(parsed.label->empty());
}

TEST(DataMessages, MidCallUpdates)
{
	EXPECT_TRUE(*parsePeerDataMessage(Json::parse(R"({"muteState":true})")).microphoneMuted);
	EXPECT_FALSE(*parsePeerDataMessage(Json::parse(R"({"videoMuted":false})")).cameraMuted);
	const auto renamed = parsePeerDataMessage(Json::parse(R"({"changeLabel":true,"value":"Bob"})"));
	EXPECT_EQ(*renamed.label, "Bob");
}

TEST(DataMessages, ByeIsPresenceBasedAndWinsOverMute)
{
	const auto parsed = parsePeerDataMessage(Json::parse(R"({"videoMuted":true,"bye":true})"));
	EXPECT_TRUE(parsed.bye);
	EXPECT_TRUE(parsePeerDataMessage(Json::parse(R"({"bye":false})")).bye);
}

TEST(DataMessages, PingTokenIsEchoedVerbatim)
{
	const auto parsed = parsePeerDataMessage(Json::parse(R"({"ping":1731400000000})"));
	ASSERT_TRUE(parsed.pingToken);
	EXPECT_EQ(buildPong(*parsed.pingToken).dump(), R"({"pong":1731400000000})");
}

TEST(DataMessages, ViewerPreferences)
{
	const auto parsed =
		parsePeerDataMessage(Json::parse(R"({"audio":true,"video":false,"bitrate":0,"guest":true})"));
	EXPECT_TRUE(*parsed.wantsAudio);
	EXPECT_FALSE(*parsed.wantsVideo);
	EXPECT_EQ(*parsed.videoBitrateKbps, 0);
}

TEST(DataMessages, SignalingRelayDetected)
{
	EXPECT_TRUE(parsePeerDataMessage(Json::parse(R"({"description":"abcd","vector":"00"})")).carriesSignaling);
	EXPECT_TRUE(parsePeerDataMessage(Json::parse(R"({"candidates":[]})")).carriesSignaling);
	EXPECT_FALSE(parsePeerDataMessage(Json::parse(R"({"info":{}})")).carriesSignaling);
}

TEST(DataMessages, ViewerHelloAsksForFullQualityMedia)
{
	const Json hello = buildViewerHello("Host", true);
	EXPECT_EQ(hello["audio"], true);
	EXPECT_EQ(hello["video"], true);
	// Identifying as a guest would make publishers apply their room bitrate cap to us.
	EXPECT_EQ(hello["guest"], false);
	EXPECT_EQ(hello["director"], true);
	EXPECT_EQ(hello["allowchunked"], false);
	EXPECT_EQ(hello["info"]["label"], "Host");

	const Json bitrate = buildBitrateRequest(6000);
	EXPECT_EQ(bitrate["targetBitrate"], 6000);
	EXPECT_EQ(bitrate["bitrate"], 6000);
}

TEST(DataMessages, DirectorCommands)
{
	EXPECT_EQ(buildHangup().dump(), R"({"hangup":true})");
	EXPECT_EQ(buildBye().dump(), R"({"bye":true})");
	EXPECT_EQ(buildChangeLabel("Zed")["value"], "Zed");
}
