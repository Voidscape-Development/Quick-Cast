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
	EXPECT_EQ(buildInviteLink(options),
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
