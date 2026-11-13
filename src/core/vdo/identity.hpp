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

#include "util/crypto.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace qc::vdo {

inline constexpr std::string_view kDefaultPassword = "someEncryptionKey123";
inline constexpr std::string_view kDefaultSalt = "vdo.ninja";
inline constexpr size_t kMaxStreamIdLength = 64;
inline constexpr size_t kMaxRoomIdLength = 30;
inline constexpr size_t kStreamSuffixLength = 6;
inline constexpr size_t kRoomHashLength = 16;

/** Keeps [A-Za-z0-9_], collapses every run of anything else into one '_', trims and truncates. */
std::string sanitizeId(std::string_view id, size_t maxLength);

/** JavaScript encodeURIComponent semantics (UTF-8 bytes, uppercase hex). */
std::string encodeUriComponent(std::string_view value);

/**
 * The password and salt that scope a VDO.Ninja session.
 *
 * VDO.Ninja never runs "without a password": an empty password means the well-known default one. Only an explicit
 * opt-out ("false", "0", "off", "no") disables hashing and encryption.
 */
class Credentials {
public:
	Credentials() : Credentials(std::string_view{}, kDefaultSalt) {}
	Credentials(std::string_view password, std::string_view salt);

	bool encryptionEnabled() const { return enabled_; }
	/** True when the user supplied their own password rather than relying on the default. */
	bool hasCustomPassword() const { return custom_; }
	const std::string &password() const { return password_; }
	const std::string &salt() const { return salt_; }

	/** Stream ID as it appears on the wire. */
	std::string wireStreamId(std::string_view streamId) const;
	/** Room ID as it appears on the wire. */
	std::string wireRoomId(std::string_view roomId) const;
	/** Reverses wireStreamId for display purposes (strips our suffix when present). */
	std::string displayStreamId(std::string_view wireStreamId) const;

	const crypto::Sha256Digest &key() const { return key_; }

private:
	bool enabled_ = true;
	bool custom_ = false;
	std::string password_; // normalised, not URI-encoded
	std::string encodedPassword_;
	std::string salt_;
	std::string streamSuffix_;
	crypto::Sha256Digest key_{};
};

/** Random stream ID in the style the web client generates (lowercase alphanumerics). */
std::string generateStreamId(size_t length = 10);
/** Random signaling session token: 8 characters of [0-9a-z]. */
std::string generateSessionId();
/** Human-friendly random room name, e.g. "amber-falcon-482" (sanitises to a valid room ID). */
std::string generateRoomName();
/** Random room password without look-alike characters. */
std::string generatePassword(size_t length = 12);

} // namespace qc::vdo
