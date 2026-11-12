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

#include "session/guest-service.hpp"

#include <string>
#include <vector>

namespace qc {

/** Everything Quick Cast remembers between OBS sessions. */
struct Settings {
	/** Room name and password determine every invite link; they stay fixed so links can be reused show to show. */
	std::string room;
	std::string password;
	std::string hostName = "Host";
	std::vector<GuestSeat> seats;

	bool startWithObs = false;
	/** Whether the dock has been revealed once; plugin docks start hidden and are easy to miss after install. */
	bool dockIntroduced = false;
	/** Fit a guest to the canvas when adding them to a scene. */
	bool fitToCanvas = true;

	// Guest video
	uint32_t guestBitrateKbps = 4000;
	VideoCodec preferredCodec = VideoCodec::H264;

	// Return feed
	bool returnFeedEnabled = true;
	/** OBS audio track (1-6) guests hear. Guest sources are kept out of this track to avoid echo. */
	int returnAudioTrack = 6;
	/**
	 * Name of the OBS source or scene guests see (typically the host's camera). Empty means the program output,
	 * i.e. exactly what the audience sees.
	 */
	std::string returnVideoSource;
	uint32_t returnHeight = 720;
	uint32_t returnVideoBitrateKbps = 1500;
	uint32_t returnAudioBitrateKbps = 96;

	// Optional TURN relay for restrictive networks
	std::string turnUrl;
	std::string turnUsername;
	std::string turnPassword;

	ShowConfig toShowConfig() const;
	const GuestSeat *findSeat(const std::string &id) const;
	GuestSeat *findSeat(const std::string &id);
};

/** Loads settings from the plugin's config directory, generating a room and password on first run. */
Settings loadSettings();
void saveSettings(const Settings &settings);

} // namespace qc
