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

#include "util/crypto.hpp"
#include "vdo/messages.hpp"

#include <gtest/gtest.h>

using namespace qc;

TEST(Crypto, Sha256KnownVector)
{
	const auto digest = crypto::sha256("abc");
	EXPECT_EQ(crypto::toHex(digest.data(), digest.size()),
		  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, AesCbcKnownVector)
{
	// NIST SP 800-38A F.2.5 (CBC-AES256.Encrypt), first block.
	crypto::Sha256Digest key{};
	const auto keyBytes = *crypto::fromHex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
	std::copy(keyBytes.begin(), keyBytes.end(), key.begin());
	crypto::AesIv iv{};
	const auto ivBytes = *crypto::fromHex("000102030405060708090a0b0c0d0e0f");
	std::copy(ivBytes.begin(), ivBytes.end(), iv.begin());
	const auto plain = *crypto::fromHex("6bc1bee22e409f96e93d7e117393172a");

	const auto encrypted = crypto::aes256CbcEncrypt(
		key, iv, std::string_view(reinterpret_cast<const char *>(plain.data()), plain.size()));
	ASSERT_TRUE(encrypted);
	// One data block plus one full block of PKCS#7 padding.
	ASSERT_EQ(encrypted->size(), 32u);
	EXPECT_EQ(crypto::toHex(encrypted->data(), 16), "f58c4c04d6e5f1ba779eabfb5f7bfbd6");
}

TEST(Crypto, AesRoundTripAndWrongKey)
{
	const auto key = crypto::sha256("someEncryptionKey123vdo.ninja");
	const auto other = crypto::sha256("different");
	crypto::AesIv iv{};
	crypto::randomBytes(iv.data(), iv.size());

	for (const std::string text : {std::string(), std::string("a"), std::string(16, 'x'), std::string(1000, 'y')}) {
		const auto encrypted = crypto::aes256CbcEncrypt(key, iv, text);
		ASSERT_TRUE(encrypted);
		EXPECT_EQ(encrypted->size() % 16, 0u);
		EXPECT_GT(encrypted->size(), text.size());
		const auto decrypted = crypto::aes256CbcDecrypt(key, iv, encrypted->data(), encrypted->size());
		ASSERT_TRUE(decrypted);
		EXPECT_EQ(*decrypted, text);
	}

	const auto encrypted = crypto::aes256CbcEncrypt(key, iv, "{\"type\":\"offer\",\"sdp\":\"v=0\"}");
	ASSERT_TRUE(encrypted);
	const auto wrong = crypto::aes256CbcDecrypt(other, iv, encrypted->data(), encrypted->size());
	if (wrong) {
		EXPECT_NE(*wrong, "{\"type\":\"offer\",\"sdp\":\"v=0\"}");
	}
	EXPECT_FALSE(crypto::aes256CbcDecrypt(key, iv, encrypted->data(), 15));
}

TEST(Crypto, HexRoundTrip)
{
	const uint8_t bytes[] = {0x00, 0x0f, 0xa5, 0xff};
	EXPECT_EQ(crypto::toHex(bytes, sizeof(bytes)), "000fa5ff");
	const auto parsed = crypto::fromHex("000FA5ff");
	ASSERT_TRUE(parsed);
	EXPECT_EQ(parsed->size(), 4u);
	EXPECT_EQ((*parsed)[2], 0xa5);
	EXPECT_FALSE(crypto::fromHex("abc"));
	EXPECT_FALSE(crypto::fromHex("zz"));
}

TEST(Crypto, PayloadEncryptionRoundTrip)
{
	const vdo::Credentials credentials("secret", "vdo.ninja");
	const auto encrypted = vdo::encryptPayload(credentials, "hello");
	ASSERT_TRUE(encrypted);
	EXPECT_EQ(encrypted->vectorHex.size(), 32u);
	const auto decrypted = vdo::decryptPayload(credentials, encrypted->ciphertextHex, encrypted->vectorHex);
	ASSERT_TRUE(decrypted);
	EXPECT_EQ(*decrypted, "hello");

	const vdo::Credentials disabled("false", "vdo.ninja");
	EXPECT_FALSE(vdo::encryptPayload(disabled, "hello"));
}
