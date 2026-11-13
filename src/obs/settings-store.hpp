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

/**
 * Someone else's show we have joined, kept so that joining it again is one click and so that the people in it keep
 * their places in our scenes.
 */
struct Collaborator {
	/** Local identifier. Followed by ':' it prefixes the ID of every seat in `seats`. */
	std::string id;
	/** What the dock calls this show. Empty until the user or the host's own name fills it in. */
	std::string name;
	/** Where the show is and which seat in it is ours, as read from the invite link. */
	InviteTarget target;
	/** The host and the other guests we have met in that show. */
	std::vector<GuestSeat> seats;
	/** When we last joined, in seconds since the Unix epoch; orders the list. */
	int64_t lastJoined = 0;

	std::string seatPrefix() const { return id + ":"; }
	const GuestSeat *findSeat(const std::string &seatId) const;
	GuestSeat *findSeat(const std::string &seatId);
};

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
	/** Decode guest video on the GPU when it can; falls back to software by itself. */
	bool hardwareDecoding = true;

	// Return feed
	bool returnFeedEnabled = true;
	/** OBS audio track (1-6) guests hear. Guest sources are kept out of this track to avoid echo. */
	int returnAudioTrack = 6;
	/**
	 * Name of a single OBS source whose audio guests hear instead of the track above (for example just the
	 * microphone). Empty means the track is used.
	 */
	std::string returnAudioSource;
	/**
	 * Name of the OBS source or scene guests see (typically the host's camera). Empty means the program output,
	 * i.e. exactly what the audience sees.
	 */
	std::string returnVideoSource;
	/**
	 * Second, video-only feed shown to guests as a separate tile, e.g. the program or a screen capture next to
	 * the host's camera. Empty source name means the program output.
	 */
	bool secondFeedEnabled = false;
	std::string secondVideoSource;
	uint32_t returnHeight = 720;
	uint32_t returnVideoBitrateKbps = 1500;
	/** The second feed has its own quality: a screen usually wants more pixels and bits than a face. */
	uint32_t secondHeight = 720;
	uint32_t secondVideoBitrateKbps = 2500;
	uint32_t returnAudioBitrateKbps = 96;

	// Collaboration: what we send when we join someone else's show. The return-feed settings above describe what
	// guests of our own show get, which by default is the program; in another streamer's show the program would
	// send them their own picture back, so joining uses these instead.
	/** False until the user has chosen what to send; until then the UI suggests their camera and microphone. */
	bool collabConfigured = false;
	/** Our camera. Empty means the program output. */
	std::string collabVideoSource;
	/** Our microphone. Empty means the return audio track (everything except guests). */
	std::string collabAudioSource;
	/** Optionally also share one more source, e.g. gameplay, as a second tile. */
	bool collabSecondEnabled = false;
	std::string collabSecondSource;
	/** What we send ends up on someone else's stream, so it gets more pixels and bits than a return feed. */
	uint32_t collabHeight = 720;
	uint32_t collabVideoBitrateKbps = 3000;
	std::vector<Collaborator> collaborators;

	/** Whether "side by side" and "grid" also place our own camera, when it is in the scene. */
	bool layoutIncludesCamera = true;

	// Optional TURN relay for restrictive networks
	std::string turnUrl;
	std::string turnUsername;
	std::string turnPassword;

	ShowConfig toShowConfig() const;
	/** Configuration for taking our seat in a collaborator's show. */
	ShowConfig toJoinConfig(const Collaborator &collaborator) const;
	/** These settings as the return feeds should see them while we are a guest in someone else's show. */
	Settings forJoinedShow() const;
	/** A seat of our own show. */
	const GuestSeat *findSeat(const std::string &id) const;
	GuestSeat *findSeat(const std::string &id);
	const Collaborator *findCollaborator(const std::string &id) const;
	Collaborator *findCollaborator(const std::string &id);
};

/** Loads settings from the plugin's config directory, generating a room and password on first run. */
Settings loadSettings();
void saveSettings(const Settings &settings);

} // namespace qc
