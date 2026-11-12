#include "vdo/identity.hpp"

#include <gtest/gtest.h>

using namespace qc::vdo;

TEST(Identity, SanitizeKeepsWordCharactersAndCollapsesRuns)
{
	EXPECT_EQ(sanitizeId("test-stream", 64), "test_stream");
	EXPECT_EQ(sanitizeId("test@stream!", 64), "test_stream_");
	EXPECT_EQ(sanitizeId("test..  stream", 64), "test_stream");
	EXPECT_EQ(sanitizeId("HeLLo", 64), "HeLLo");
	EXPECT_EQ(sanitizeId("  padded\t\n", 64), "padded");
	EXPECT_EQ(sanitizeId("caf\xC3\xA9", 64), "caf_");
	EXPECT_EQ(sanitizeId("abcdef", 3), "abc");
}

TEST(Identity, EncodeUriComponentMatchesJavaScript)
{
	EXPECT_EQ(encodeUriComponent("Test@Room#1!"), "Test%40Room%231!");
	EXPECT_EQ(encodeUriComponent("caf\xC3\xA9"), "caf%C3%A9");
	EXPECT_EQ(encodeUriComponent("a b"), "a%20b");
	EXPECT_EQ(encodeUriComponent("-_.!~*'()"), "-_.!~*'()");
}

TEST(Identity, EmptyPasswordMeansDefaultPassword)
{
	const Credentials credentials;
	EXPECT_TRUE(credentials.encryptionEnabled());
	EXPECT_FALSE(credentials.hasCustomPassword());
	EXPECT_EQ(credentials.wireStreamId("HLBKRuy"), "HLBKRuy808d64");
	EXPECT_EQ(credentials.wireRoomId("myroom"), "2f1a0139acdd5bdc");
}

TEST(Identity, CustomPasswordHashes)
{
	const Credentials credentials("Test@Room#1!", "vdo.ninja");
	EXPECT_TRUE(credentials.hasCustomPassword());
	EXPECT_EQ(credentials.wireStreamId("mystream"), "mystream932512");
	EXPECT_EQ(credentials.wireRoomId("myroom"), "97dd633996a146ff");
	EXPECT_EQ(credentials.displayStreamId("mystream932512"), "mystream");
	EXPECT_EQ(credentials.displayStreamId("mystream808d64"), "mystream808d64");
}

TEST(Identity, DisabledPasswordSkipsHashing)
{
	for (const char *token : {"false", "0", "off", "No", " FALSE "}) {
		const Credentials credentials(token, "vdo.ninja");
		EXPECT_FALSE(credentials.encryptionEnabled()) << token;
		EXPECT_EQ(credentials.wireStreamId("my-stream"), "my_stream");
		EXPECT_EQ(credentials.wireRoomId("my room"), "my_room");
	}
}

TEST(Identity, GeneratedNamesAreValidIdentifiers)
{
	for (int i = 0; i < 50; ++i) {
		const std::string room = generateRoomName();
		EXPECT_EQ(sanitizeId(room, kMaxRoomIdLength), room);
		const std::string stream = generateStreamId();
		EXPECT_EQ(sanitizeId(stream, kMaxStreamIdLength), stream);
		EXPECT_EQ(generateSessionId().size(), 8u);
	}
	EXPECT_NE(generatePassword(), generatePassword());
}
