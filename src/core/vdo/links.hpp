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

#include "identity.hpp"

#include <optional>
#include <string>

namespace qc::vdo {

struct InviteOptions {
	/** Web app the guest opens. */
	std::string baseUrl = "https://vdo.ninja/";
	std::string room;
	/** Raw password as typed by the host; empty means the service default. */
	std::string password;
	/** Fixed stream ID so the guest always lands in the same seat. Empty for the open link. */
	std::string streamId;
	/** Pre-filled display name. When empty the guest is asked for one. */
	std::string label;
	/** Skip the camera/screen-share chooser and go straight to camera setup. */
	bool cameraOnly = true;
};

/** Percent-encodes everything except RFC 3986 unreserved characters. */
std::string encodeQueryValue(const std::string &value);

/** Builds the link a guest opens to join the show. */
std::string buildInviteLink(const InviteOptions &options);

struct ParsedInvite {
	/** Sanitised room name. */
	std::string room;
	/** Password as the link carries it; empty means the service default, "false" means none. */
	std::string password;
	/** Sanitised stream ID the link reserves; empty for an open link. */
	std::string streamId;
	std::string label;
};

/**
 * Reads a guest link (ours, or one written by hand for the web app) back into its parts, so Quick Cast can join
 * the show itself instead of a browser. Returns nullopt for anything that is not a link into a room, including
 * a director's own link.
 */
std::optional<ParsedInvite> parseInviteLink(const std::string &link);

} // namespace qc::vdo
