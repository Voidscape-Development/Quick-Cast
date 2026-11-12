#pragma once

#include "messages.hpp"

#include <optional>
#include <string>

namespace qc::vdo {

/**
 * What we understand of a peer's data-channel message. One message can carry several independent fields, so this
 * is a bag of optionals rather than a tagged union.
 */
struct PeerDataMessage {
	/** The message is (also) a relayed signaling envelope: description / candidate / candidates. */
	bool carriesSignaling = false;
	/** Liveness probe; must be answered with the identical token. */
	std::optional<Json> pingToken;
	/** The peer is going away. */
	bool bye = false;
	/** The viewer lost connectivity and wants a fresh offer. */
	bool iceRestartRequested = false;
	bool keyframeRequested = false;

	/** Display name (from `info.label` or a `changeLabel` update). Empty string means "cleared". */
	std::optional<std::string> label;
	std::optional<bool> microphoneMuted;
	std::optional<bool> cameraMuted;

	/** Viewer preferences about what it wants from us. */
	std::optional<bool> wantsAudio;
	std::optional<bool> wantsVideo;
	/** Requested video bitrate in kbps; 0 means "pause video", negative means "no limit". */
	std::optional<int> videoBitrateKbps;

	/** A director-only command of ours was refused (we do not hold the director role). */
	std::optional<std::string> rejected;
};

PeerDataMessage parsePeerDataMessage(const Json &message);

/** First message a viewer sends to a publisher: asks for full-quality audio and video as the room's director. */
Json buildViewerHello(const std::string &hostLabel, bool director);
/** Raises the publisher's bitrate ceiling and applies it. Send after buildViewerHello. */
Json buildBitrateRequest(uint32_t kbps);
/** Asks the publisher to scale its capture to fit the given size (never upscales). */
Json buildResolutionRequest(uint32_t width, uint32_t height);

/** First message a publisher sends to a viewer. */
Json buildPublisherInfo(const std::string &hostLabel, uint32_t width, uint32_t height, uint32_t framesPerSecond);

Json buildPong(const Json &token);
Json buildBye();
/** Director command: ends the guest's session. */
Json buildHangup();
/** Director command: renames the guest for everyone in the room. */
Json buildChangeLabel(const std::string &label);

} // namespace qc::vdo
