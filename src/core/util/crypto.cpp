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

#include "crypto.hpp"

#include <random>

#if defined(QC_TLS_MBEDTLS)
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#else
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

namespace qc::crypto {

namespace {
constexpr size_t kBlock = 16;
}

#if defined(QC_TLS_MBEDTLS)

Sha256Digest sha256(std::string_view data)
{
	Sha256Digest digest{};
	mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest.data(), 0);
	return digest;
}

std::optional<std::vector<uint8_t>> aes256CbcEncrypt(const Sha256Digest &key, const AesIv &iv,
						     std::string_view plaintext)
{
	// PKCS#7: always pad, with a full block when the input is already block aligned.
	const size_t padding = kBlock - (plaintext.size() % kBlock);
	std::vector<uint8_t> input(plaintext.begin(), plaintext.end());
	input.insert(input.end(), padding, static_cast<uint8_t>(padding));

	std::vector<uint8_t> output(input.size());
	AesIv workingIv = iv; // mbedtls updates the IV in place

	mbedtls_aes_context context;
	mbedtls_aes_init(&context);
	bool ok = mbedtls_aes_setkey_enc(&context, key.data(), 256) == 0 &&
		  mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_ENCRYPT, input.size(), workingIv.data(), input.data(),
					output.data()) == 0;
	mbedtls_aes_free(&context);
	if (!ok)
		return std::nullopt;
	return output;
}

std::optional<std::string> aes256CbcDecrypt(const Sha256Digest &key, const AesIv &iv, const uint8_t *ciphertext,
					    size_t size)
{
	if (size == 0 || size % kBlock != 0)
		return std::nullopt;

	std::vector<uint8_t> output(size);
	AesIv workingIv = iv;

	mbedtls_aes_context context;
	mbedtls_aes_init(&context);
	bool ok = mbedtls_aes_setkey_dec(&context, key.data(), 256) == 0 &&
		  mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_DECRYPT, size, workingIv.data(), ciphertext,
					output.data()) == 0;
	mbedtls_aes_free(&context);
	if (!ok)
		return std::nullopt;

	const uint8_t padding = output.back();
	if (padding == 0 || padding > kBlock || padding > output.size())
		return std::nullopt;
	for (size_t i = output.size() - padding; i < output.size(); ++i) {
		if (output[i] != padding)
			return std::nullopt;
	}
	return std::string(output.begin(), output.end() - padding);
}

#else // OpenSSL

Sha256Digest sha256(std::string_view data)
{
	Sha256Digest digest{};
	SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest.data());
	return digest;
}

std::optional<std::vector<uint8_t>> aes256CbcEncrypt(const Sha256Digest &key, const AesIv &iv,
						     std::string_view plaintext)
{
	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	if (!context)
		return std::nullopt;

	std::vector<uint8_t> output(plaintext.size() + kBlock);
	int written = 0;
	int finalWritten = 0;
	const bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) == 1 &&
			EVP_EncryptUpdate(context, output.data(), &written,
					  reinterpret_cast<const unsigned char *>(plaintext.data()),
					  static_cast<int>(plaintext.size())) == 1 &&
			EVP_EncryptFinal_ex(context, output.data() + written, &finalWritten) == 1;
	EVP_CIPHER_CTX_free(context);
	if (!ok)
		return std::nullopt;
	output.resize(static_cast<size_t>(written + finalWritten));
	return output;
}

std::optional<std::string> aes256CbcDecrypt(const Sha256Digest &key, const AesIv &iv, const uint8_t *ciphertext,
					    size_t size)
{
	if (size == 0 || size % kBlock != 0)
		return std::nullopt;

	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	if (!context)
		return std::nullopt;

	std::string output(size + kBlock, '\0');
	int written = 0;
	int finalWritten = 0;
	const bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) == 1 &&
			EVP_DecryptUpdate(context, reinterpret_cast<unsigned char *>(output.data()), &written,
					  ciphertext, static_cast<int>(size)) == 1 &&
			EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char *>(output.data()) + written,
					    &finalWritten) == 1;
	EVP_CIPHER_CTX_free(context);
	if (!ok)
		return std::nullopt;
	output.resize(static_cast<size_t>(written + finalWritten));
	return output;
}

#endif

void randomBytes(uint8_t *out, size_t size)
{
	// std::random_device is backed by the OS CSPRNG on every platform OBS supports.
	std::random_device device;
	size_t i = 0;
	while (i < size) {
		const unsigned int value = device();
		for (size_t b = 0; b < sizeof(value) && i < size; ++b, ++i)
			out[i] = static_cast<uint8_t>(value >> (b * 8));
	}
}

std::string randomString(size_t length, std::string_view alphabet)
{
	std::string result;
	if (alphabet.empty())
		return result;
	result.reserve(length);
	std::random_device device;
	std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);
	for (size_t i = 0; i < length; ++i)
		result.push_back(alphabet[pick(device)]);
	return result;
}

std::string toHex(const uint8_t *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		out.push_back(digits[data[i] >> 4]);
		out.push_back(digits[data[i] & 0x0F]);
	}
	return out;
}

std::optional<std::vector<uint8_t>> fromHex(std::string_view hex)
{
	if (hex.size() % 2 != 0)
		return std::nullopt;
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	std::vector<uint8_t> out;
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int high = nibble(hex[i]);
		const int low = nibble(hex[i + 1]);
		if (high < 0 || low < 0)
			return std::nullopt;
		out.push_back(static_cast<uint8_t>((high << 4) | low));
	}
	return out;
}

} // namespace qc::crypto
