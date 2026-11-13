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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qc::crypto {

using Sha256Digest = std::array<uint8_t, 32>;
using AesIv = std::array<uint8_t, 16>;

Sha256Digest sha256(std::string_view data);

/** AES-256-CBC with PKCS#7 padding. Returns nullopt on backend failure. */
std::optional<std::vector<uint8_t>> aes256CbcEncrypt(const Sha256Digest &key, const AesIv &iv,
						     std::string_view plaintext);

/** Returns nullopt when the ciphertext is malformed or the padding is invalid (i.e. the key is wrong). */
std::optional<std::string> aes256CbcDecrypt(const Sha256Digest &key, const AesIv &iv, const uint8_t *ciphertext,
					    size_t size);

void randomBytes(uint8_t *out, size_t size);

/** Random string drawn uniformly from `alphabet`. */
std::string randomString(size_t length, std::string_view alphabet);

std::string toHex(const uint8_t *data, size_t size);
std::optional<std::vector<uint8_t>> fromHex(std::string_view hex);

} // namespace qc::crypto
