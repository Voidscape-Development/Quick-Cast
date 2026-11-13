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

#include "identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>

namespace qc::vdo {

namespace {

std::string_view trim(std::string_view value)
{
	const char *whitespace = " \t\r\n";
	const size_t begin = value.find_first_not_of(whitespace);
	if (begin == std::string_view::npos)
		return {};
	const size_t end = value.find_last_not_of(whitespace);
	return value.substr(begin, end - begin + 1);
}

bool isDisabledToken(std::string_view value)
{
	std::string lowered(value);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lowered == "false" || lowered == "0" || lowered == "off" || lowered == "no";
}

} // namespace

std::string sanitizeId(std::string_view id, size_t maxLength)
{
	id = trim(id);
	std::string out;
	out.reserve(id.size());
	bool inRun = false;
	for (const char c : id) {
		const auto u = static_cast<unsigned char>(c);
		if (std::isalnum(u) != 0 && u < 0x80) {
			out.push_back(c);
			inRun = false;
		} else if (c == '_') {
			out.push_back('_');
			inRun = false;
		} else if (!inRun) {
			out.push_back('_');
			inRun = true;
		}
	}
	if (out.size() > maxLength)
		out.resize(maxLength);
	return out;
}

std::string encodeUriComponent(std::string_view value)
{
	static const char digits[] = "0123456789ABCDEF";
	static const std::string_view unreserved = "-_.!~*'()";
	std::string out;
	out.reserve(value.size());
	for (const char c : value) {
		const auto u = static_cast<unsigned char>(c);
		if ((u < 0x80 && std::isalnum(u) != 0) || unreserved.find(c) != std::string_view::npos) {
			out.push_back(c);
		} else {
			out.push_back('%');
			out.push_back(digits[u >> 4]);
			out.push_back(digits[u & 0x0F]);
		}
	}
	return out;
}

Credentials::Credentials(std::string_view password, std::string_view salt)
{
	salt_ = std::string(trim(salt));
	if (salt_.empty())
		salt_ = std::string(kDefaultSalt);

	const std::string_view trimmed = trim(password);
	if (isDisabledToken(trimmed)) {
		enabled_ = false;
		return;
	}

	custom_ = !trimmed.empty();
	password_ = std::string(custom_ ? trimmed : kDefaultPassword);
	encodedPassword_ = encodeUriComponent(password_);

	key_ = crypto::sha256(encodedPassword_ + salt_);
	streamSuffix_ = crypto::toHex(key_.data(), key_.size()).substr(0, kStreamSuffixLength);
}

std::string Credentials::wireStreamId(std::string_view streamId) const
{
	std::string sanitized = sanitizeId(streamId, kMaxStreamIdLength);
	if (!enabled_)
		return sanitized;
	return sanitized + streamSuffix_;
}

std::string Credentials::wireRoomId(std::string_view roomId) const
{
	const std::string sanitized = sanitizeId(roomId, kMaxRoomIdLength);
	if (!enabled_)
		return sanitized;
	const auto digest = crypto::sha256(sanitized + encodedPassword_ + salt_);
	return crypto::toHex(digest.data(), digest.size()).substr(0, kRoomHashLength);
}

std::string Credentials::displayStreamId(std::string_view wireStreamId) const
{
	if (enabled_ && wireStreamId.size() > kStreamSuffixLength &&
	    wireStreamId.substr(wireStreamId.size() - kStreamSuffixLength) == streamSuffix_)
		return std::string(wireStreamId.substr(0, wireStreamId.size() - kStreamSuffixLength));
	return std::string(wireStreamId);
}

std::string generateStreamId(size_t length)
{
	return crypto::randomString(length, "abcdefghijklmnopqrstuvwxyz0123456789");
}

std::string generateSessionId()
{
	return crypto::randomString(8, "0123456789abcdefghijklmnopqrstuvwxyz");
}

std::string generateRoomName()
{
	static const std::array<const char *, 24> adjectives = {"amber",  "bold",  "calm",  "clever", "cosmic",
								"crisp",  "dawn",  "eager", "fresh",  "gold",
								"happy",  "ivory", "jolly", "keen",   "lucky",
								"mellow", "noble", "quick", "royal",  "silver",
								"sunny",  "swift", "vivid", "witty"};
	static const std::array<const char *, 24> nouns = {"badger", "comet",  "falcon",  "forest",  "harbor",
							   "heron",  "island", "lantern", "maple",   "meadow",
							   "otter",  "panda",  "pebble",  "phoenix", "piano",
							   "river",  "rocket", "signal",  "sparrow", "studio",
							   "summit", "tiger",  "voyage",  "willow"};

	std::random_device device;
	std::uniform_int_distribution<size_t> pickAdjective(0, adjectives.size() - 1);
	std::uniform_int_distribution<size_t> pickNoun(0, nouns.size() - 1);
	std::uniform_int_distribution<int> pickNumber(100, 999);
	// '_' rather than '-' so the name survives sanitizeId() unchanged.
	return std::string(adjectives[pickAdjective(device)]) + "_" + nouns[pickNoun(device)] + "_" +
	       std::to_string(pickNumber(device));
}

std::string generatePassword(size_t length)
{
	return crypto::randomString(length, "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789");
}

} // namespace qc::vdo
