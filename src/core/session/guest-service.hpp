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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * The seam between Quick Cast and whatever service actually connects the host to their guests.
 *
 * Everything above this interface (OBS sources, the return-feed output, the dock) is service agnostic; everything
 * below it is one implementation. A show is symmetric at this level: whichever role we play, the other people in it
 * are seats whose media we receive, and the "return feed" is simply what we send them. Today that is VDO.Ninja (vdo/vdo-service.hpp). A first-party service only has to
 * implement GuestService.
 */
namespace qc {

enum class ServiceState {
	Stopped,
	Connecting,
	/** Connected to the service; guests can join. */
	Live,
	/** Connection to the service was lost and is being re-established. Existing guest media keeps flowing. */
	Reconnecting,
	/** The service refused us in a way that retrying will not fix. */
	Failed,
};

enum class GuestConnection {
	/** Known guest that is not currently in the show. */
	Offline,
	/** Guest is in the room; media is being negotiated. */
	Connecting,
	Connected,
};

enum class VideoCodec { H264, VP8, VP9 };

/** A seat in the show. Seats are created by the host and persist; a guest occupies one via their invite link. */
struct GuestSeat {
	/** Stable identifier, unique within the show. Also the key OBS sources bind to. */
	std::string id;
	std::string name;
};

struct GuestStatus {
	std::string id;
	/** Name reported by the guest's client, when it differs from the seat name. */
	std::string reportedName;
	GuestConnection connection = GuestConnection::Offline;
	bool microphoneMuted = false;
	bool cameraMuted = false;
	/** Whether this guest is currently receiving the return feed. */
	bool receivingReturnFeed = false;
};

struct GuestMediaStats {
	double videoBitrateKbps = 0.0;
	double audioBitrateKbps = 0.0;
	/** Recent video packet loss, 0..1. */
	double videoLoss = 0.0;
	double jitterMs = 0.0;
	uint64_t keyframeRequests = 0;
};

struct EncodedVideoFrame {
	VideoCodec codec = VideoCodec::H264;
	/** H.264: Annex-B access unit. VP8/VP9: one frame. */
	std::vector<uint8_t> data;
	/** Presentation time on the local monotonic clock, shared with the guest's audio. */
	uint64_t timestampNs = 0;
	bool keyframe = false;
};

struct EncodedAudioPacket {
	/** One Opus packet (48 kHz). */
	std::vector<uint8_t> data;
	uint64_t timestampNs = 0;
};

/**
 * Receives one guest's media. Called on network threads: implementations must hand work off quickly and never
 * call back into the GuestService from these methods.
 */
class GuestMediaSink {
public:
	virtual ~GuestMediaSink() = default;
	virtual void onVideoFrame(EncodedVideoFrame &&frame) = 0;
	virtual void onAudioPacket(EncodedAudioPacket &&packet) = 0;
	/** The guest's media stopped (they left or the connection dropped); decoders should be reset. */
	virtual void onMediaStopped() = 0;
};

/** Quality the host asks a guest's client to send. */
struct GuestQuality {
	uint32_t maxVideoBitrateKbps = 4000;
	uint32_t maxWidth = 1920;
	uint32_t maxHeight = 1080;
	VideoCodec preferredCodec = VideoCodec::H264;
};

struct IceServer {
	/** e.g. "stun:stun.l.google.com:19302" or "turn:turn.example.com:3478" */
	std::string url;
	std::string username;
	std::string credential;
};

/** Which side of a show this installation is on. */
enum class ShowRole {
	/** Our own show: we invite, guests take seats. */
	Host,
	/** Someone else's show, joined with their invite link: we take a seat and they are the host. */
	Guest,
};

/** What an invite link says about the show it leads to. */
struct InviteTarget {
	std::string room;
	std::string password;
	/** The seat the link reserves; empty for an open link. */
	std::string seatId;
	/** The name the host gave that seat, when the link carries one. */
	std::string name;
};

struct ShowConfig {
	ShowRole role = ShowRole::Host;
	/** Guest role: the seat we take in the host's show. The host's sources stay bound to it across shows. */
	std::string ownSeatId;
	/**
	 * Guest role: prepended to the ID of every seat this show reports (the host and the other guests), so the
	 * people in someone else's show can never collide with the seats of our own.
	 */
	std::string seatPrefix;
	/** Name of the show; with the password it determines every invite link. */
	std::string room;
	std::string password;
	/** How we appear to everyone else in the show. */
	std::string hostName = "Host";
	std::vector<GuestSeat> seats;
	GuestQuality defaultQuality;
	/** Publish the return feed so guests can see and hear the show. Always on in the guest role. */
	bool returnFeedEnabled = true;
	/** Also publish a second, video-only feed (e.g. the program next to the host's camera). */
	bool secondFeedEnabled = false;
	/** Extra ICE servers (TURN) for guests behind restrictive networks. */
	std::vector<IceServer> iceServers;
};

/**
 * The host can publish up to two feeds to guests. The main feed carries video and the host's audio; the second is
 * video only and appears to guests as a separate tile.
 */
enum class ReturnFeedId { Main = 0, Second = 1 };

/** Format of the return feed the host publishes to guests. */
struct ReturnFeedFormat {
	uint32_t width = 1280;
	uint32_t height = 720;
	uint32_t framesPerSecond = 30;
	uint32_t videoBitrateKbps = 1500;
	uint32_t audioBitrateKbps = 96;
};

/** Events from the service. Delivered on a service-owned thread, one at a time. */
class GuestServiceListener {
public:
	virtual ~GuestServiceListener() = default;
	virtual void onServiceState(ServiceState state, const std::string &detail) = 0;
	virtual void onGuestStatus(const GuestStatus &status) = 0;
	/**
	 * Someone joined through the open invite link and was given a new seat. The listener is expected to persist
	 * the seat if it wants the guest to keep it.
	 */
	virtual void onSeatCreated(const GuestSeat &seat) = 0;
	/** Called when at least one guest wants the return feed (or the last one stopped watching). */
	virtual void onReturnFeedDemand(bool wanted) = 0;
};

class GuestService {
public:
	virtual ~GuestService() = default;

	virtual void setListener(GuestServiceListener *listener) = 0;

	/** Starts the show. Asynchronous; progress is reported through the listener. */
	virtual void start(const ShowConfig &config) = 0;
	/** Ends the show and disconnects everyone. Blocks until network activity has stopped. */
	virtual void stop() = 0;

	virtual void addSeat(const GuestSeat &seat) = 0;
	virtual void renameSeat(const std::string &guestId, const std::string &name) = 0;
	/** Removes a seat, disconnecting its guest. */
	virtual void removeSeat(const std::string &guestId) = 0;

	/** Link the guest opens in a browser to take the given seat. */
	virtual std::string inviteLink(const ShowConfig &config, const GuestSeat &seat) const = 0;
	/** Link anyone can use to join; each person who uses it gets a new seat. */
	virtual std::string openInviteLink(const ShowConfig &config) const = 0;
	/** Reads an invite link produced by this service (ours or another host's). nullopt if it is not one. */
	virtual std::optional<InviteTarget> parseInviteLink(const std::string &link) const = 0;

	/** Routes a guest's media to `sink` (nullptr detaches). The sink must outlive its registration. */
	virtual void setMediaSink(const std::string &guestId, std::shared_ptr<GuestMediaSink> sink) = 0;

	virtual void setGuestQuality(const std::string &guestId, const GuestQuality &quality) = 0;
	virtual void requestKeyframe(const std::string &guestId) = 0;
	/** Disconnects the guest's current session. Their seat (and link) stays valid. */
	virtual void disconnectGuest(const std::string &guestId) = 0;
	virtual bool guestStats(const std::string &guestId, GuestMediaStats &stats) const = 0;

	// --- Return feed (host -> guests). Safe to call from any thread. ---
	virtual void setReturnFeedFormat(ReturnFeedId feed, const ReturnFeedFormat &format) = 0;
	/** One H.264 access unit (Annex-B, parameter sets in-band on keyframes). */
	virtual void sendReturnVideo(ReturnFeedId feed, const uint8_t *data, size_t size, int64_t timestampUs,
				     bool keyframe) = 0;
	/** One Opus packet for the main feed. */
	virtual void sendReturnAudio(const uint8_t *data, size_t size, int64_t timestampUs) = 0;
};

} // namespace qc
