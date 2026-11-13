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

#include "vdo/messages.hpp"

#include <gtest/gtest.h>

using namespace qc::vdo;

namespace {
/** Returns a copy so callers may pass a temporary message. */
template<typename T> T expect(const SignalingMessage &message)
{
	EXPECT_TRUE(std::holds_alternative<T>(message));
	if (!std::holds_alternative<T>(message))
		return T{};
	return std::get<T>(message);
}

SignalingMessage parse(const char *text, const std::vector<const Credentials *> &keys = {})
{
	return parseSignalingMessage(Json::parse(text), keys);
}
} // namespace

TEST(Messages, Listing)
{
	const auto message = parse(
		R"({"request":"listing","list":[{"UUID":"a","streamID":"cam_1"},{"UUID":"b"},"cam_3"],"director":"me","claim":true})");
	const auto &listing = expect<ListingMessage>(message);
	ASSERT_EQ(listing.members.size(), 2u);
	EXPECT_EQ(listing.members[0].streamId, "cam_1");
	EXPECT_EQ(listing.members[0].uuid, "a");
	EXPECT_EQ(listing.members[1].streamId, "cam_3");
	ASSERT_TRUE(listing.director.has_value());
	EXPECT_TRUE(*listing.director);

	const auto denied = parse(R"({"request":"listing","list":[],"claim":false})");
	EXPECT_FALSE(*expect<ListingMessage>(denied).director);
}

TEST(Messages, StreamEvents)
{
	const auto &added =
		expect<StreamAddedMessage>(parse(R"({"request":"someonejoined","UUID":"peer","streamID":"cam_5"})"));
	EXPECT_EQ(added.member.streamId, "cam_5");
	EXPECT_EQ(added.member.uuid, "peer");

	expect<IgnoredMessage>(parse(R"({"request":"someonejoined","UUID":"peer"})"));
	expect<StreamAddedMessage>(parse(R"({"request":"videoaddedtoroom","UUID":"p","streamID":"x"})"));
	const auto &removed =
		expect<StreamRemovedMessage>(parse(R"({"request":"videoremovedfromroom","UUID":"p","streamID":"x"})"));
	EXPECT_EQ(removed.member.streamId, "x");
}

TEST(Messages, OfferRequestAndCleanup)
{
	const auto &request = expect<OfferRequestMessage>(parse(R"({"request":"offerSDP","UUID":"viewer"})"));
	EXPECT_EQ(request.uuid, "viewer");
	EXPECT_TRUE(request.session.empty());
	expect<IgnoredMessage>(parse(R"({"request":"offerSDP"})"));

	EXPECT_EQ(expect<PeerGoneMessage>(parse(R"({"request":"cleanup","UUID":"x"})")).uuid, "x");
	EXPECT_EQ(expect<PeerGoneMessage>(parse(R"({"bye":true,"UUID":"y"})")).uuid, "y");
	expect<IgnoredMessage>(parse(R"({"bye":false,"UUID":"y"})"));
	expect<IgnoredMessage>(parse(R"({"request":"bye","UUID":"y"})"));
}

TEST(Messages, Alerts)
{
	EXPECT_EQ(expect<AlertMessage>(parse(R"({"request":"alert","message":"Room is already claimed"})")).text,
		  "Room is already claimed");
	EXPECT_EQ(expect<AlertMessage>(parse(R"({"alert":"hi"})")).text, "hi");
}

TEST(Messages, PlainDescription)
{
	const auto &description = expect<DescriptionMessage>(parse(
		R"({"UUID":"pub","session":"s1","streamID":"cam808d64","description":{"type":"offer","sdp":"v=0\r\n"}})"));
	EXPECT_TRUE(description.isOffer);
	EXPECT_EQ(description.uuid, "pub");
	EXPECT_EQ(description.session, "s1");
	EXPECT_EQ(description.streamId, "cam808d64");
	EXPECT_EQ(description.sdp, "v=0\r\n");
}

TEST(Messages, EscapedNewlinesInSdpAreRepaired)
{
	Json message = {{"UUID", "p"}, {"description", {{"type", "answer"}, {"sdp", "v=0\\r\\ns=-\\r\\n"}}}};
	const auto parsed = parseSignalingMessage(message, {});
	EXPECT_EQ(expect<DescriptionMessage>(parsed).sdp, "v=0\r\ns=-\r\n");
}

TEST(Messages, EncryptedDescriptionRoundTrip)
{
	const Credentials room("hunter2", "vdo.ninja");
	const Credentials other("nope", "vdo.ninja");
	const Credentials fallback;

	const auto built = buildDescription(room, "peer", "sess", true, "v=0\r\nm=video\r\n", "cam123456");
	ASSERT_TRUE(built);
	EXPECT_TRUE((*built)["description"].is_string());
	EXPECT_EQ((*built)["vector"].get<std::string>().size(), 32u);
	EXPECT_EQ((*built)["streamID"], "cam123456");

	// The first key that decrypts wins.
	const auto parsed = parseSignalingMessage(*built, {&other, &room, &fallback});
	const auto &description = expect<DescriptionMessage>(parsed);
	EXPECT_TRUE(description.isOffer);
	EXPECT_EQ(description.sdp, "v=0\r\nm=video\r\n");

	expect<IgnoredMessage>(parseSignalingMessage(*built, {&other}));
}

TEST(Messages, PlainDescriptionWhenEncryptionDisabled)
{
	const Credentials disabled("false", "vdo.ninja");
	const auto built = buildDescription(disabled, "peer", "sess", false, "v=0\r\n");
	ASSERT_TRUE(built);
	EXPECT_TRUE((*built)["description"].is_object());
	EXPECT_FALSE(built->contains("vector"));
	EXPECT_FALSE(built->contains("streamID"));
}

TEST(Messages, CandidateForms)
{
	const auto &single = expect<CandidatesMessage>(parse(
		R"({"UUID":"p","session":"s","type":"local","candidate":{"candidate":"a=candidate:1 1 UDP 1 1.2.3.4 5 typ host","sdpMid":"0"}})"));
	EXPECT_EQ(single.origin, CandidateOrigin::Offerer);
	ASSERT_EQ(single.candidates.size(), 1u);
	EXPECT_EQ(single.candidates[0].candidate, "candidate:1 1 UDP 1 1.2.3.4 5 typ host");
	EXPECT_EQ(single.candidates[0].mid, "0");

	const auto &bundle = expect<CandidatesMessage>(parse(
		R"({"UUID":"p","type":"remote","candidates":[{"candidate":"candidate:2","sdpMid":"1"},{"candidate":""},"candidate:3"],"sdpMid":"9"})"));
	EXPECT_EQ(bundle.origin, CandidateOrigin::Answerer);
	ASSERT_EQ(bundle.candidates.size(), 2u);
	EXPECT_EQ(bundle.candidates[1].candidate, "candidate:3");
	EXPECT_EQ(bundle.candidates[1].mid, "9");

	const auto &flat = expect<CandidatesMessage>(parse(R"({"UUID":"p","candidate":"candidate:4","sdpMid":"2"})"));
	EXPECT_EQ(flat.origin, CandidateOrigin::Unknown);
	EXPECT_EQ(flat.candidates[0].mid, "2");
}

TEST(Messages, EncryptedCandidatesRoundTrip)
{
	const Credentials room("hunter2", "vdo.ninja");
	const auto built = buildCandidates(room, "peer", "sess", true,
					   {IceCandidate{"a=candidate:1 1 UDP 1 1.2.3.4 5 typ host", "video"}});
	ASSERT_TRUE(built);
	EXPECT_EQ((*built)["type"], "local");
	EXPECT_TRUE((*built)["candidates"].is_string());

	const auto parsed = parseSignalingMessage(*built, {&room});
	const auto &candidates = expect<CandidatesMessage>(parsed);
	ASSERT_EQ(candidates.candidates.size(), 1u);
	EXPECT_EQ(candidates.candidates[0].candidate, "candidate:1 1 UDP 1 1.2.3.4 5 typ host");
	EXPECT_EQ(candidates.candidates[0].mid, "video");
	EXPECT_EQ(candidates.origin, CandidateOrigin::Offerer);
}

TEST(Messages, FallbackUuidForRelayedMessages)
{
	const auto parsed =
		parseSignalingMessage(Json::parse(R"({"description":{"type":"offer","sdp":"v=0"}})"), {}, "relay-peer");
	EXPECT_EQ(expect<DescriptionMessage>(parsed).uuid, "relay-peer");
}

TEST(Messages, Builders)
{
	const Json join = buildJoinRoom("abcd", true);
	EXPECT_EQ(join["request"], "joinroom");
	EXPECT_EQ(join["roomid"], "abcd");
	EXPECT_EQ(join["claim"], true);
	EXPECT_FALSE(buildJoinRoom("abcd", false).contains("claim"));
	EXPECT_EQ(buildSeed("x")["request"], "seed");
	EXPECT_EQ(buildPlay("x")["streamID"], "x");
}
