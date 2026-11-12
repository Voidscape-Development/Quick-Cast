#pragma once

#include "identity.hpp"

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

} // namespace qc::vdo
