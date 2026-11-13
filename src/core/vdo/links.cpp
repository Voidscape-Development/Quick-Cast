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

#include "links.hpp"

#include <cctype>
#include <string_view>

namespace qc::vdo {

std::string encodeQueryValue(const std::string &value)
{
	static const char digits[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (const char c : value) {
		const auto u = static_cast<unsigned char>(c);
		if ((u < 0x80 && std::isalnum(u) != 0) || c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(c);
		} else {
			out.push_back('%');
			out.push_back(digits[u >> 4]);
			out.push_back(digits[u & 0x0F]);
		}
	}
	return out;
}

std::string buildInviteLink(const InviteOptions &options)
{
	std::string url = options.baseUrl.empty() ? std::string("https://vdo.ninja/") : options.baseUrl;
	while (url.size() > 1 && url.back() == '/')
		url.pop_back();
	url += "/?room=" + encodeQueryValue(sanitizeId(options.room, kMaxRoomIdLength));

	const Credentials credentials(options.password, kDefaultSalt);
	if (!credentials.encryptionEnabled())
		url += "&password=false";
	else if (credentials.hasCustomPassword())
		url += "&password=" + encodeQueryValue(credentials.password());

	if (!options.streamId.empty())
		url += "&push=" + encodeQueryValue(sanitizeId(options.streamId, kMaxStreamIdLength));

	// A bare "&label" makes the web app ask the guest for their name.
	if (options.label.empty())
		url += "&label";
	else
		url += "&label=" + encodeQueryValue(options.label);

	if (options.cameraOnly)
		url += "&webcam";
	return url;
}

namespace {

int hexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/** Query-string decoding as browsers do it: %XX escapes, and '+' for a space. */
std::string decodeQueryValue(std::string_view value)
{
	std::string out;
	out.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i) {
		const char c = value[i];
		if (c == '%' && i + 2 < value.size() && hexValue(value[i + 1]) >= 0 && hexValue(value[i + 2]) >= 0) {
			out.push_back(static_cast<char>(hexValue(value[i + 1]) * 16 + hexValue(value[i + 2])));
			i += 2;
		} else if (c == '+') {
			out.push_back(' ');
		} else {
			out.push_back(c);
		}
	}
	return out;
}

} // namespace

std::optional<ParsedInvite> parseInviteLink(const std::string &link)
{
	const size_t question = link.find('?');
	if (question == std::string::npos)
		return std::nullopt;
	std::string_view query = std::string_view(link).substr(question + 1);
	if (const size_t fragment = query.find('#'); fragment != std::string_view::npos)
		query = query.substr(0, fragment);
	while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back())) != 0)
		query.remove_suffix(1);

	ParsedInvite result;
	bool directorLink = false;
	while (!query.empty()) {
		const size_t ampersand = query.find('&');
		const std::string_view pair = query.substr(0, ampersand);
		query = ampersand == std::string_view::npos ? std::string_view{} : query.substr(ampersand + 1);

		const size_t equals = pair.find('=');
		std::string key(pair.substr(0, equals));
		for (char &c : key)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const std::string value = equals == std::string_view::npos ? std::string()
									   : decodeQueryValue(pair.substr(equals + 1));

		// The web app accepts several spellings of each parameter.
		if (key == "room" || key == "r" || key == "roomid")
			result.room = sanitizeId(value, kMaxRoomIdLength);
		else if (key == "password" || key == "pass" || key == "pw" || key == "p")
			result.password = value;
		else if (key == "push" || key == "id" || key == "permaid")
			result.streamId = sanitizeId(value, kMaxStreamIdLength);
		else if (key == "label" || key == "l")
			result.label = value;
		else if (key == "director" || key == "dir")
			directorLink = true;
	}

	if (directorLink || result.room.empty())
		return std::nullopt;
	return result;
}

} // namespace qc::vdo
