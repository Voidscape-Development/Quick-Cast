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

} // namespace qc::vdo
