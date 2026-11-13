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

#include "vdo/links.hpp"

#include <gtest/gtest.h>

using namespace qc::vdo;

TEST(Links, SeatLink)
{
	InviteOptions options;
	options.room = "amber_falcon_482";
	options.password = "s3cret";
	options.streamId = "k3x9a0qzab";
	options.label = "Alice Smith";
	EXPECT_EQ(
		buildInviteLink(options),
		"https://vdo.ninja/?room=amber_falcon_482&password=s3cret&push=k3x9a0qzab&label=Alice%20Smith&webcam");
}

TEST(Links, OpenLinkPromptsForName)
{
	InviteOptions options;
	options.room = "my room";
	options.cameraOnly = false;
	// No password parameter: the service default applies. Bare &label asks the guest for a name.
	EXPECT_EQ(buildInviteLink(options), "https://vdo.ninja/?room=my_room&label");
}

TEST(Links, DisabledPasswordAndCustomBase)
{
	InviteOptions options;
	options.baseUrl = "https://example.com/app///";
	options.room = "r";
	options.password = "false";
	options.cameraOnly = false;
	options.label = "A&B";
	EXPECT_EQ(buildInviteLink(options), "https://example.com/app/?room=r&password=false&label=A%26B");
}

TEST(Links, ParsesWhatWeBuild)
{
	InviteOptions options;
	options.room = "amber_falcon_482";
	options.password = "s3cret & more";
	options.streamId = "k3x9a0qzab";
	options.label = "Alice Smith";
	const auto parsed = parseInviteLink(buildInviteLink(options));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->room, "amber_falcon_482");
	EXPECT_EQ(parsed->password, "s3cret & more");
	EXPECT_EQ(parsed->streamId, "k3x9a0qzab");
	EXPECT_EQ(parsed->label, "Alice Smith");
}

TEST(Links, ParsesOpenLink)
{
	InviteOptions options;
	options.room = "my room";
	const auto parsed = parseInviteLink(buildInviteLink(options));
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->room, "my_room");
	EXPECT_TRUE(parsed->password.empty());
	EXPECT_TRUE(parsed->streamId.empty());
	EXPECT_TRUE(parsed->label.empty());
}

TEST(Links, ParsesHandWrittenLinks)
{
	// Short parameter names, mixed case, a fragment, and trailing whitespace from a chat client.
	const auto parsed = parseInviteLink("https://vdo.ninja/?R=Game+Night&pw=false&ID=bob%5F1&l=Bob#top \n");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(parsed->room, "Game_Night");
	EXPECT_EQ(parsed->password, "false");
	EXPECT_EQ(parsed->streamId, "bob_1");
	EXPECT_EQ(parsed->label, "Bob");
}

TEST(Links, RejectsWhatIsNotAnInvite)
{
	EXPECT_FALSE(parseInviteLink("").has_value());
	EXPECT_FALSE(parseInviteLink("hello there").has_value());
	EXPECT_FALSE(parseInviteLink("https://vdo.ninja/").has_value());
	// A link straight to one stream, with no room to join.
	EXPECT_FALSE(parseInviteLink("https://vdo.ninja/?view=abc123").has_value());
	// The host's own control link must not be mistaken for a seat in the show.
	EXPECT_FALSE(parseInviteLink("https://vdo.ninja/?director=amber_falcon_482&password=x").has_value());
	EXPECT_FALSE(parseInviteLink("https://vdo.ninja/?room=&push=abc").has_value());
}
