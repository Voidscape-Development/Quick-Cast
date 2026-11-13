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

#include "vdo-service.hpp"

#include "data-messages.hpp"
#include "identity.hpp"
#include "links.hpp"
#include "media-receiver.hpp"
#include "messages.hpp"
#include "sdp-utils.hpp"
#include "signaling-client.hpp"
#include "util/executor.hpp"
#include "util/log.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>

#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

namespace qc::vdo {

namespace {

constexpr auto kStatsInterval = std::chrono::milliseconds(1000);
constexpr auto kReplayDelay = std::chrono::milliseconds(3000);
constexpr auto kDisconnectGrace = std::chrono::milliseconds(8000);
constexpr auto kNegotiationResendWindow = std::chrono::seconds(10);
/** How long to wait for a guest to answer a re-request before concluding they have left the show. */
constexpr auto kReplayGiveUp = std::chrono::milliseconds(10000);
/**
 * A connected guest that sends no media at all for this long is gone. Browsers keep sending (silence, black frames)
 * while muted, and the server does not announce departures, so without this a guest who closes their tab would
 * linger until ICE gives up tens of seconds later.
 */
constexpr int64_t kMediaSilenceUs = 5'000'000;
constexpr uint8_t kReturnVideoPayloadType = 96;
constexpr uint8_t kReturnAudioPayloadType = 111;
constexpr size_t kMaxPendingCandidates = 100;

/** Holder that lets network threads read a seat's sink while the UI thread swaps it. */
struct SinkSlot {
	std::mutex mutex;
	std::shared_ptr<GuestMediaSink> sink;

	std::shared_ptr<GuestMediaSink> get()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return sink;
	}
	void set(std::shared_ptr<GuestMediaSink> value)
	{
		std::lock_guard<std::mutex> lock(mutex);
		sink = std::move(value);
	}
};

struct Seat {
	GuestSeat seat;
	GuestQuality quality;
	GuestStatus status;
	std::shared_ptr<SinkSlot> slot = std::make_shared<SinkSlot>();
	/** Signaling UUID of the peer currently occupying the seat. */
	std::string uuid;
	/** Wire stream ID to re-request when the connection drops while the guest is still in the room. */
	std::string wireStreamId;
	uint64_t lastVideoBytes = 0;
	uint64_t lastAudioBytes = 0;
};

struct InboundTrack {
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<MediaReceiver> receiver;
	bool video = false;
};

/** Connection on which a guest publishes to us. */
struct InboundPeer {
	std::string uuid;
	std::string session;
	std::string seatId;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::DataChannel> channel;
	std::shared_ptr<rtp::SyncClock> clock = std::make_shared<rtp::SyncClock>();
	std::map<std::string, InboundTrack> tracks; // by mid
	bool haveVideoTrack = false;
	bool haveAudioTrack = false;
	bool remoteDescriptionSet = false;
	bool helloSent = false;
	std::vector<IceCandidate> pendingCandidates;
};

/** Connection on which we publish the return feed to a guest. */
struct OutboundPeer {
	/** Which of the host's feeds this connection carries (0 = main, 1 = second). */
	int feed = 0;
	/** Key in the outbound map; a guest can hold one connection per feed. */
	std::string key;
	std::string uuid;
	std::string session;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::DataChannel> channel;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> audioTrack;
	std::optional<Json> cachedOffer;
	std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
	bool remoteDescriptionSet = false;
	bool connected = false;
	std::vector<IceCandidate> pendingCandidates;
	/** A new viewer cannot decode anything until it has seen a keyframe. */
	std::atomic<bool> waitingForKeyframe{true};
	std::atomic<bool> videoWanted{true};
	std::atomic<bool> audioWanted{true};
};

uint32_t randomSsrc()
{
	uint32_t value = 0;
	while (value == 0)
		crypto::randomBytes(reinterpret_cast<uint8_t *>(&value), sizeof(value));
	return value;
}

bool sessionsConflict(const std::string &a, const std::string &b)
{
	return !a.empty() && !b.empty() && a != b;
}

bool containsInsensitive(const std::string &haystack, const char *needle)
{
	std::string lowered = haystack;
	for (char &c : lowered)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return lowered.find(needle) != std::string::npos;
}

void installRtcLogger()
{
	static std::once_flag once;
	std::call_once(once, [] {
		rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
			if (level <= rtc::LogLevel::Error)
				QC_LOG_WARN("webrtc: %s", message.c_str());
			else
				QC_LOG_DEBUG("webrtc: %s", message.c_str());
		});
	});
}

std::optional<rtc::IceServer> toRtcIceServer(const IceServer &server)
{
	try {
		if (server.username.empty())
			return rtc::IceServer(server.url);

		// scheme:host[:port][?transport=tcp]
		std::string rest = server.url;
		const size_t colon = rest.find(':');
		if (colon == std::string::npos)
			return std::nullopt;
		const std::string scheme = rest.substr(0, colon);
		rest.erase(0, colon + 1);

		bool tcp = false;
		if (const size_t query = rest.find('?'); query != std::string::npos) {
			tcp = rest.find("transport=tcp", query) != std::string::npos;
			rest.erase(query);
		}
		std::string host = rest;
		std::string port = scheme == "turns" ? "5349" : "3478";
		if (const size_t portColon = rest.rfind(':'); portColon != std::string::npos) {
			host = rest.substr(0, portColon);
			port = rest.substr(portColon + 1);
		}
		auto relay = rtc::IceServer::RelayType::TurnUdp;
		if (scheme == "turns")
			relay = rtc::IceServer::RelayType::TurnTls;
		else if (tcp)
			relay = rtc::IceServer::RelayType::TurnTcp;
		return rtc::IceServer(host, port, server.username, server.credential, relay);
	} catch (const std::exception &error) {
		QC_LOG_WARN("ignoring invalid ICE server '%s': %s", server.url.c_str(), error.what());
		return std::nullopt;
	}
}

bool sendJson(const std::shared_ptr<rtc::DataChannel> &channel, const Json &message)
{
	if (!channel || !channel->isOpen())
		return false;
	try {
		channel->send(message.dump());
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

/**
 * Signaling relayed over a data channel names its *addressee* in `UUID` (the web client fills in the target), so
 * the sender has to be taken from the channel the message arrived on.
 */
Json relayedFrom(const Json &message, const std::string &senderUuid)
{
	Json copy = message;
	copy.erase("uuid");
	copy.erase("from");
	copy["UUID"] = senderUuid;
	return copy;
}

VideoCodec toPublicCodec(rtp::VideoCodec codec)
{
	switch (codec) {
	case rtp::VideoCodec::VP8:
		return VideoCodec::VP8;
	case rtp::VideoCodec::VP9:
		return VideoCodec::VP9;
	case rtp::VideoCodec::H264:
		break;
	}
	return VideoCodec::H264;
}

} // namespace

// =====================================================================================================================

class VdoService::Impl : public std::enable_shared_from_this<VdoService::Impl> {
public:
	explicit Impl(VdoServiceOptions options) : options_(std::move(options)) {}

	VdoServiceOptions options_;
	Executor executor_;
	std::atomic<GuestServiceListener *> listener_{nullptr};

	// --- state owned by the executor thread ---
	bool running_ = false;
	ShowConfig config_;
	Credentials credentials_;
	Credentials defaultCredentials_;
	std::string wireRoom_;
	std::string hostWireStreamId_;
	std::string secondWireStreamId_;
	bool director_ = false;
	bool openedBefore_ = false;
	ServiceState state_ = ServiceState::Stopped;
	std::shared_ptr<SignalingClient> signaling_;
	/** Second room membership that publishes the second feed; null when that feed is off. */
	std::shared_ptr<SignalingClient> secondSignaling_;
	bool secondOpenedBefore_ = false;
	/** Feed whose signaling connection the message being handled arrived on. */
	int feed_ = 0;
	std::map<std::string, std::shared_ptr<Seat>> seats_;
	std::map<std::string, std::shared_ptr<InboundPeer>> inbound_;
	std::map<std::string, std::shared_ptr<OutboundPeer>> outbound_;
	/** Streams currently announced in the room: wire stream ID -> publisher UUID (may be empty). */
	std::map<std::string, std::string> roomStreams_;
	Executor::TimerId statsTimer_ = 0;
	bool returnDemand_ = false;

	// --- shared with other threads ---
	mutable std::mutex statsMutex_;
	std::map<std::string, GuestMediaStats> stats_;

	std::mutex returnMutex_;
	std::vector<std::shared_ptr<OutboundPeer>> returnTargets_;
	ReturnFeedFormat returnFormat_[2];

	// ---------------------------------------------------------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------------------------------------------------------

	template<typename F> void post(F &&task)
	{
		std::weak_ptr<Impl> weak = weak_from_this();
		executor_.post([weak, task = std::forward<F>(task)]() mutable {
			if (auto self = weak.lock())
				task(*self);
		});
	}

	/** Wraps a callback coming from a libdatachannel thread so it runs on the executor if we still exist. */
	template<typename F> auto bounce(F &&task)
	{
		std::weak_ptr<Impl> weak = weak_from_this();
		return [weak, task = std::forward<F>(task)](auto &&...args) mutable {
			auto self = weak.lock();
			if (!self)
				return;
			self->executor_.post([weak, task,
					      argsTuple = std::make_tuple(std::decay_t<decltype(args)>(
						      std::forward<decltype(args)>(args))...)]() mutable {
				if (auto strong = weak.lock())
					std::apply([&](auto &...unpacked) { task(*strong, unpacked...); }, argsTuple);
			});
		};
	}

	void setState(ServiceState state, const std::string &detail = {})
	{
		state_ = state;
		if (auto *listener = listener_.load())
			listener->onServiceState(state, detail);
	}

	void notifyStatus(const Seat &seat)
	{
		if (auto *listener = listener_.load())
			listener->onGuestStatus(seat.status);
	}

	rtc::Configuration makeRtcConfiguration() const
	{
		rtc::Configuration configuration;
		configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
		configuration.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
		for (const IceServer &server : config_.iceServers) {
			if (auto converted = toRtcIceServer(server))
				configuration.iceServers.push_back(*converted);
		}
		// Offers and answers are generated explicitly so that adding tracks never triggers a surprise offer.
		configuration.disableAutoNegotiation = true;
		// Browsers first connect with only a data channel and add camera/microphone in a renegotiation, so the
		// DTLS transport must be SRTP-capable from the start.
		configuration.forceMediaTransport = true;
		return configuration;
	}

	std::vector<const Credentials *> decryptionKeys() const { return {&credentials_, &defaultCredentials_}; }

	/** Signaling for an established peer prefers its data channel (as the web client does), else the WebSocket. */
	void sendPeerSignal(const std::shared_ptr<rtc::DataChannel> &channel, const Json &message, int feed = 0)
	{
		if (sendJson(channel, message))
			return;
		if (auto *signaling = signalingFor(feed))
			signaling->send(message);
	}

	SignalingClient *signalingFor(int feed) const { return feed == 0 ? signaling_.get() : secondSignaling_.get(); }

	const std::string &wireStreamIdFor(int feed) const
	{
		return feed == 0 ? hostWireStreamId_ : secondWireStreamId_;
	}

	static std::string outboundKey(int feed, const std::string &uuid)
	{
		return feed == 0 ? uuid : "#" + std::to_string(feed) + ":" + uuid;
	}

	bool isOwnStream(const std::string &wireStreamId) const
	{
		return wireStreamId == hostWireStreamId_ || wireStreamId == secondWireStreamId_;
	}

	// ---------------------------------------------------------------------------------------------------------
	// Lifecycle
	// ---------------------------------------------------------------------------------------------------------

	void start(const ShowConfig &config)
	{
		if (running_)
			stopInternal();

		config_ = config;
		credentials_ = Credentials(config.password, kDefaultSalt);
		defaultCredentials_ = Credentials();
		wireRoom_ = credentials_.wireRoomId(config.room);
		// A fresh stream ID per run avoids "already in use" when the show is restarted quickly.
		hostWireStreamId_ = credentials_.wireStreamId("host_" + generateStreamId(8));
		secondWireStreamId_ = credentials_.wireStreamId("host2_" + generateStreamId(8));
		director_ = false;
		openedBefore_ = false;
		secondOpenedBefore_ = false;

		if (sanitizeId(config.room, kMaxRoomIdLength).empty()) {
			setState(ServiceState::Failed, "The show has no name.");
			return;
		}

		// Seats survive restarts: media sinks are attached to them independently of the show running.
		for (const GuestSeat &seat : config.seats) {
			auto existing = ensureSeat(seat, false);
			existing->seat.name = seat.name;
			existing->quality = config.defaultQuality;
		}

		running_ = true;
		setState(ServiceState::Connecting);

		SignalingClient::Callbacks callbacks;
		std::weak_ptr<Impl> weak = weak_from_this();
		callbacks.onOpen = [weak] {
			if (auto self = weak.lock())
				self->onSignalingOpen();
		};
		callbacks.onClosed = [weak](const std::string &reason, int) {
			if (auto self = weak.lock(); self && self->running_)
				self->setState(ServiceState::Reconnecting, reason);
		};
		callbacks.onMessage = [weak](const Json &message) {
			if (auto self = weak.lock())
				self->onSignalingMessage(message, {});
		};
		signaling_ = SignalingClient::create(executor_, options_.signalingUrl, std::move(callbacks));
		signaling_->connect();

		if (config_.returnFeedEnabled && config_.secondFeedEnabled) {
			SignalingClient::Callbacks second;
			second.onOpen = [weak] {
				if (auto self = weak.lock())
					self->onSecondSignalingOpen();
			};
			second.onMessage = [weak](const Json &message) {
				if (auto self = weak.lock())
					self->onSignalingMessage(message, {}, 1);
			};
			secondSignaling_ = SignalingClient::create(executor_, options_.signalingUrl, std::move(second));
			secondSignaling_->connect();
		}

		scheduleStats();
	}

	void stopInternal()
	{
		if (!running_ && !signaling_)
			return;
		running_ = false;

		executor_.cancel(statsTimer_);
		statsTimer_ = 0;

		closeAllPeers(true);

		if (signaling_) {
			signaling_->close();
			signaling_.reset();
		}
		if (secondSignaling_) {
			secondSignaling_->close();
			secondSignaling_.reset();
		}
		roomStreams_.clear();

		for (auto &[id, seat] : seats_) {
			if (seat->status.connection != GuestConnection::Offline) {
				seat->status.connection = GuestConnection::Offline;
				seat->status.receivingReturnFeed = false;
				notifyStatus(*seat);
			}
		}
		setState(ServiceState::Stopped);
	}

	void closeAllPeers(bool sayGoodbye)
	{
		auto inbound = std::move(inbound_);
		inbound_.clear();
		for (auto &[uuid, peer] : inbound) {
			if (sayGoodbye)
				sendJson(peer->channel, buildBye());
			releaseInbound(peer);
		}

		auto outbound = std::move(outbound_);
		outbound_.clear();
		{
			std::lock_guard<std::mutex> lock(returnMutex_);
			returnTargets_.clear();
		}
		for (auto &[uuid, peer] : outbound) {
			if (sayGoodbye)
				sendJson(peer->channel, buildBye());
			releaseOutbound(peer);
		}
		updateReturnDemand();
	}

	// ---------------------------------------------------------------------------------------------------------
	// Signaling
	// ---------------------------------------------------------------------------------------------------------

	void onSignalingOpen()
	{
		if (!running_)
			return;

		// The server gives us a new identity on every connection and tells the room our old one left, so
		// guests drop their connections to us. Start from a clean slate rather than keep half-dead peers.
		if (openedBefore_)
			closeAllPeers(false);
		openedBefore_ = true;
		roomStreams_.clear();

		signaling_->send(buildJoinRoom(wireRoom_, true));
		if (config_.returnFeedEnabled)
			signaling_->send(buildSeed(hostWireStreamId_));
	}

	void onSecondSignalingOpen()
	{
		if (!running_ || !secondSignaling_)
			return;
		// As with the main connection, a reconnect gives us a new identity; drop what hung off the old one.
		if (secondOpenedBefore_) {
			std::vector<std::string> stale;
			for (const auto &[key, peer] : outbound_) {
				if (peer->feed == 1)
					stale.push_back(key);
			}
			for (const std::string &key : stale)
				closeOutbound(key);
		}
		secondOpenedBefore_ = true;
		secondSignaling_->send(buildJoinRoom(wireRoom_, false));
		secondSignaling_->send(buildSeed(secondWireStreamId_));
	}

	void onSignalingMessage(const Json &message, const std::string &fallbackUuid, int feed = 0)
	{
		if (!running_)
			return;
		feed_ = feed;

		// Server keepalive.
		if (auto request = message.find("request");
		    request != message.end() && request->is_string() && request->get<std::string>() == "ping") {
			Json pong = {{"request", "pong"}};
			if (auto id = message.find("id"); id != message.end())
				pong["id"] = *id;
			if (auto *signaling = signalingFor(feed))
				signaling->send(pong);
			return;
		}

		SignalingMessage parsed = parseSignalingMessage(message, decryptionKeys(), fallbackUuid);
		std::visit([this](auto &&value) { handle(value); }, parsed);
	}

	void handle(const IgnoredMessage &message) { QC_LOG_DEBUG("signaling: ignored (%s)", message.reason.c_str()); }

	void handle(const ListingMessage &listing)
	{
		if (feed_ != 0)
			return; // the second membership only publishes; the main one tracks the room
		director_ = listing.director.value_or(true);
		if (!director_)
			QC_LOG_WARN("room is already claimed by another director; removing guests will not work");

		QC_LOG_INFO("joined show room (%zu stream(s) present, director=%d)", listing.members.size(),
			    director_ ? 1 : 0);
		setState(ServiceState::Live);
		for (const RoomMember &member : listing.members)
			onStreamAvailable(member);
	}

	void handle(const StreamAddedMessage &added)
	{
		if (feed_ == 0)
			onStreamAvailable(added.member);
	}

	void handle(const StreamRemovedMessage &removed)
	{
		if (feed_ != 0)
			return;
		std::string uuid = removed.member.uuid;
		if (!removed.member.streamId.empty()) {
			if (auto it = roomStreams_.find(removed.member.streamId); it != roomStreams_.end()) {
				if (uuid.empty())
					uuid = it->second;
				roomStreams_.erase(it);
			}
		}
		if (!uuid.empty())
			closeInbound(uuid, "stream removed");
	}

	void handle(const PeerGoneMessage &gone)
	{
		if (feed_ != 0) {
			closeOutbound(outboundKey(feed_, gone.uuid));
			return;
		}
		for (auto it = roomStreams_.begin(); it != roomStreams_.end();) {
			if (it->second == gone.uuid)
				it = roomStreams_.erase(it);
			else
				++it;
		}
		closeInbound(gone.uuid, "peer left");
		closeOutbound(outboundKey(0, gone.uuid));
	}

	void handle(const AlertMessage &alert)
	{
		QC_LOG_WARN("server alert: %s", alert.text.c_str());
		if (feed_ != 0) {
			if (containsInsensitive(alert.text, "already in use") && secondSignaling_) {
				secondWireStreamId_ = credentials_.wireStreamId("host2_" + generateStreamId(8));
				secondSignaling_->send(buildSeed(secondWireStreamId_));
			}
			return;
		}
		if (containsInsensitive(alert.text, "already claimed")) {
			director_ = false;
			return; // we can still host, just without director powers
		}
		if (containsInsensitive(alert.text, "already in use")) {
			// Stream ID collision on our return feed: pick another one.
			hostWireStreamId_ = credentials_.wireStreamId("host_" + generateStreamId(8));
			if (signaling_ && config_.returnFeedEnabled)
				signaling_->send(buildSeed(hostWireStreamId_));
			return;
		}
		if (containsInsensitive(alert.text, "busy") || containsInsensitive(alert.text, "capacity") ||
		    containsInsensitive(alert.text, "try again")) {
			if (signaling_)
				signaling_->deferReconnect(15000);
		}
		if (auto *listener = listener_.load())
			listener->onServiceState(state_, alert.text);
	}

	void handle(const OfferRequestMessage &request)
	{
		if (!config_.returnFeedEnabled)
			return;

		const int feed = feed_;
		const std::string key = outboundKey(feed, request.uuid);
		if (auto it = outbound_.find(key); it != outbound_.end()) {
			const auto &existing = it->second;
			const bool negotiating = !existing->connected &&
						 std::chrono::steady_clock::now() - existing->created <
							 kNegotiationResendWindow &&
						 !sessionsConflict(existing->session, request.session);
			if (negotiating) {
				// Duplicate request while our first offer is still in flight: repeat it verbatim.
				if (existing->cachedOffer) {
					if (auto *signaling = signalingFor(feed))
						signaling->send(*existing->cachedOffer);
				}
				return;
			}
			closeOutbound(key);
		}
		createOutbound(feed, request.uuid, request.session.empty() ? generateSessionId() : request.session);
	}

	void handle(const IceRestartRequestMessage &request) { restartOutbound(outboundKey(feed_, request.uuid)); }

	void handle(const DescriptionMessage &description)
	{
		if (description.isOffer) {
			if (feed_ == 0)
				onRemoteOffer(description);
		} else
			onRemoteAnswer(description);
	}

	void handle(const CandidatesMessage &message)
	{
		auto inboundIt = inbound_.find(message.uuid);
		auto outboundIt = outbound_.find(outboundKey(feed_, message.uuid));
		// Guests only ever publish to the main membership.
		std::shared_ptr<InboundPeer> inboundPeer = feed_ == 0 && inboundIt != inbound_.end() ? inboundIt->second
												     : nullptr;
		std::shared_ptr<OutboundPeer> outboundPeer = outboundIt != outbound_.end() ? outboundIt->second
											   : nullptr;

		// VDO.Ninja runs one connection per direction between the same two peers; `type` says which one a
		// candidate belongs to ("local" = sent by the offering/publishing side).
		bool toInbound;
		switch (message.origin) {
		case CandidateOrigin::Offerer:
			toInbound = true;
			break;
		case CandidateOrigin::Answerer:
			toInbound = false;
			break;
		default:
			if (inboundPeer && outboundPeer)
				toInbound = !message.session.empty() && inboundPeer->session == message.session;
			else
				toInbound = inboundPeer != nullptr;
			break;
		}

		if (toInbound && inboundPeer) {
			if (sessionsConflict(inboundPeer->session, message.session))
				return;
			addRemoteCandidates(*inboundPeer->pc, inboundPeer->remoteDescriptionSet,
					    inboundPeer->pendingCandidates, message.candidates);
		} else if (!toInbound && outboundPeer) {
			if (sessionsConflict(outboundPeer->session, message.session))
				return;
			addRemoteCandidates(*outboundPeer->pc, outboundPeer->remoteDescriptionSet,
					    outboundPeer->pendingCandidates, message.candidates);
		}
	}

	static void applyCandidate(rtc::PeerConnection &pc, const IceCandidate &candidate)
	{
		try {
			pc.addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
		} catch (const std::exception &error) {
			QC_LOG_DEBUG("rejected remote candidate: %s", error.what());
		}
	}

	static void addRemoteCandidates(rtc::PeerConnection &pc, bool remoteDescriptionSet,
					std::vector<IceCandidate> &pending, const std::vector<IceCandidate> &candidates)
	{
		for (const IceCandidate &candidate : candidates) {
			if (remoteDescriptionSet)
				applyCandidate(pc, candidate);
			else if (pending.size() < kMaxPendingCandidates)
				pending.push_back(candidate);
		}
	}

	static void flushCandidates(rtc::PeerConnection &pc, std::vector<IceCandidate> &pending)
	{
		for (const IceCandidate &candidate : pending)
			applyCandidate(pc, candidate);
		pending.clear();
	}

	// ---------------------------------------------------------------------------------------------------------
	// Seats
	// ---------------------------------------------------------------------------------------------------------

	std::shared_ptr<Seat> ensureSeat(const GuestSeat &seat, bool announce)
	{
		auto it = seats_.find(seat.id);
		if (it != seats_.end())
			return it->second;

		auto created = std::make_shared<Seat>();
		created->seat = seat;
		created->quality = config_.defaultQuality;
		created->status.id = seat.id;
		seats_.emplace(seat.id, created);
		if (announce) {
			if (auto *listener = listener_.load())
				listener->onSeatCreated(seat);
		}
		return created;
	}

	std::shared_ptr<Seat> findSeat(const std::string &id) const
	{
		auto it = seats_.find(id);
		return it != seats_.end() ? it->second : nullptr;
	}

	void onStreamAvailable(const RoomMember &member)
	{
		if (member.streamId.empty() || isOwnStream(member.streamId))
			return;

		roomStreams_[member.streamId] = member.uuid;
		const std::string seatId = credentials_.displayStreamId(member.streamId);
		auto seat = ensureSeat(GuestSeat{seatId, "Guest"}, true);
		seat->wireStreamId = member.streamId;

		// Already receiving this guest (e.g. the server re-announced the stream).
		if (!seat->uuid.empty() && inbound_.count(seat->uuid) &&
		    (member.uuid.empty() || member.uuid == seat->uuid))
			return;

		if (seat->status.connection == GuestConnection::Offline) {
			seat->status.connection = GuestConnection::Connecting;
			notifyStatus(*seat);
		}
		if (signaling_)
			signaling_->send(buildPlay(member.streamId));
	}

	void scheduleReplay(const std::string &seatId)
	{
		std::weak_ptr<Impl> weak = weak_from_this();
		executor_.postDelayed(kReplayDelay, [weak, seatId] {
			auto self = weak.lock();
			if (!self || !self->running_)
				return;
			auto seat = self->findSeat(seatId);
			if (!seat || seat->wireStreamId.empty() || !self->roomStreams_.count(seat->wireStreamId))
				return; // guest really left
			if (!seat->uuid.empty() && self->inbound_.count(seat->uuid))
				return; // already back
			// The guest stays "offline" in the UI until they actually answer: this request is a guess that
			// they are still in the room, and showing "connecting" for someone who left would mislead.
			QC_LOG_INFO("re-requesting stream for guest '%s'", seat->seat.name.c_str());
			if (self->signaling_)
				self->signaling_->send(buildPlay(seat->wireStreamId));

			const std::string wireStreamId = seat->wireStreamId;
			self->executor_.postDelayed(kReplayGiveUp, [weak, seatId, wireStreamId] {
				auto strong = weak.lock();
				if (!strong || !strong->running_)
					return;
				auto current = strong->findSeat(seatId);
				if (!current || (!current->uuid.empty() && strong->inbound_.count(current->uuid)))
					return; // they came back
				// No answer: they have left. The server announces them again if they return.
				strong->roomStreams_.erase(wireStreamId);
			});
		});
	}

	// ---------------------------------------------------------------------------------------------------------
	// Inbound: guests publishing to us
	// ---------------------------------------------------------------------------------------------------------

	void onRemoteOffer(const DescriptionMessage &offer)
	{
		std::shared_ptr<InboundPeer> peer;
		if (auto it = inbound_.find(offer.uuid); it != inbound_.end()) {
			if (sessionsConflict(it->second->session, offer.session))
				closeInbound(offer.uuid, "session replaced", false);
			else
				peer = it->second;
		}

		if (!peer) {
			// Work out which seat this publisher belongs to.
			std::string wireStreamId = offer.streamId;
			if (wireStreamId.empty()) {
				for (const auto &[stream, uuid] : roomStreams_) {
					if (uuid == offer.uuid) {
						wireStreamId = stream;
						break;
					}
				}
			}
			if (wireStreamId.empty() || isOwnStream(wireStreamId)) {
				QC_LOG_DEBUG("ignoring offer from unknown publisher %s", offer.uuid.c_str());
				return;
			}
			const std::string seatId = credentials_.displayStreamId(wireStreamId);
			auto seat = ensureSeat(GuestSeat{seatId, "Guest"}, true);
			seat->wireStreamId = wireStreamId;
			roomStreams_[wireStreamId] = offer.uuid;

			// One connection per seat: a newer publisher on the same stream replaces the old one.
			if (!seat->uuid.empty() && seat->uuid != offer.uuid)
				closeInbound(seat->uuid, "replaced by new connection", false);

			peer = createInbound(offer.uuid, offer.session, seat);
			if (!peer)
				return;
		}

		if (peer->session.empty())
			peer->session = offer.session;
		applyOffer(peer, offer.sdp);
	}

	std::shared_ptr<InboundPeer> createInbound(const std::string &uuid, const std::string &session,
						   const std::shared_ptr<Seat> &seat)
	{
		auto peer = std::make_shared<InboundPeer>();
		peer->uuid = uuid;
		peer->session = session;
		peer->seatId = seat->seat.id;

		try {
			peer->pc = std::make_shared<rtc::PeerConnection>(makeRtcConfiguration());
		} catch (const std::exception &error) {
			QC_LOG_ERROR("could not create peer connection: %s", error.what());
			return nullptr;
		}

		std::weak_ptr<InboundPeer> weakPeer = peer;

		peer->pc->onLocalDescription(bounce([weakPeer](Impl &self, rtc::Description description) {
			auto strong = weakPeer.lock();
			if (!strong || !self.isCurrent(strong))
				return;
			auto message = buildDescription(self.credentials_, strong->uuid, strong->session,
							description.type() == rtc::Description::Type::Offer,
							std::string(description));
			if (message)
				self.sendPeerSignal(strong->channel, *message);
		}));

		peer->pc->onLocalCandidate(bounce([weakPeer](Impl &self, rtc::Candidate candidate) {
			auto strong = weakPeer.lock();
			if (!strong || !self.isCurrent(strong))
				return;
			auto message = buildCandidates(self.credentials_, strong->uuid, strong->session, false,
						       {IceCandidate{candidate.candidate(), candidate.mid()}});
			if (message)
				self.sendPeerSignal(strong->channel, *message);
		}));

		peer->pc->onStateChange(bounce([weakPeer](Impl &self, rtc::PeerConnection::State state) {
			if (auto strong = weakPeer.lock())
				self.onInboundState(strong, state);
		}));

		peer->pc->onDataChannel(bounce([weakPeer](Impl &self, std::shared_ptr<rtc::DataChannel> channel) {
			if (auto strong = weakPeer.lock())
				self.onInboundChannel(strong, channel);
		}));

		inbound_[uuid] = peer;
		seat->uuid = uuid;
		seat->lastVideoBytes = 0;
		seat->lastAudioBytes = 0;
		if (seat->status.connection != GuestConnection::Connected) {
			seat->status.connection = GuestConnection::Connecting;
			notifyStatus(*seat);
		}
		return peer;
	}

	bool isCurrent(const std::shared_ptr<InboundPeer> &peer) const
	{
		auto it = inbound_.find(peer->uuid);
		return it != inbound_.end() && it->second == peer;
	}

	bool isCurrent(const std::shared_ptr<OutboundPeer> &peer) const
	{
		auto it = outbound_.find(peer->key);
		return it != outbound_.end() && it->second == peer;
	}

	void applyOffer(const std::shared_ptr<InboundPeer> &peer, const std::string &sdp)
	{
		auto seat = findSeat(peer->seatId);
		if (!seat)
			return;

		try {
			for (const SdpMediaSection &section : parseSdpMedia(sdp)) {
				if (section.mid.empty() || peer->tracks.count(section.mid))
					continue;
				if (section.kind == "video")
					addInboundVideo(peer, *seat, section);
				else if (section.kind == "audio")
					addInboundAudio(peer, *seat, section);
			}

			peer->pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));
			peer->remoteDescriptionSet = true;
			flushCandidates(*peer->pc, peer->pendingCandidates);
			peer->pc->setLocalDescription(rtc::Description::Type::Answer);
		} catch (const std::exception &error) {
			QC_LOG_ERROR("could not negotiate with guest '%s': %s", seat->seat.name.c_str(), error.what());
			closeInbound(peer->uuid, "negotiation failed");
		}
	}

	void addInboundVideo(const std::shared_ptr<InboundPeer> &peer, Seat &seat, const SdpMediaSection &section)
	{
		auto preference = VideoCodecPreference::H264;
		if (seat.quality.preferredCodec == VideoCodec::VP8)
			preference = VideoCodecPreference::VP8;
		else if (seat.quality.preferredCodec == VideoCodec::VP9)
			preference = VideoCodecPreference::VP9;
		const auto codec = chooseVideoCodec(section, preference);
		if (!codec) {
			QC_LOG_WARN("guest '%s' offered no video codec we can decode", seat.seat.name.c_str());
			return;
		}

		// Only one camera per seat is shown; anything else the guest offers is declined so it is not sent.
		const bool use = section.remoteSends && !peer->haveVideoTrack;
		rtc::Description::Video media(section.mid, use ? rtc::Description::Direction::RecvOnly
							       : rtc::Description::Direction::Inactive);
		const std::optional<std::string> offeredFormat =
			codec->fmtp.empty() ? std::nullopt : std::optional<std::string>(codec->fmtp);
		rtp::VideoCodec rtpCodec = rtp::VideoCodec::H264;
		if (codec->name == "H264") {
			media.addH264Codec(codec->payloadType, offeredFormat);
		} else if (codec->name == "VP9") {
			rtpCodec = rtp::VideoCodec::VP9;
			media.addVP9Codec(codec->payloadType, offeredFormat);
		} else {
			rtpCodec = rtp::VideoCodec::VP8;
			media.addVP8Codec(codec->payloadType);
		}

		InboundTrack entry;
		entry.video = true;
		entry.track = peer->pc->addTrack(media);
		if (use) {
			peer->haveVideoTrack = true;

			MediaReceiver::Config receiverConfig;
			receiverConfig.video = true;
			receiverConfig.payloadType = codec->payloadType;
			receiverConfig.clockRate = codec->clockRate > 0 ? static_cast<uint32_t>(codec->clockRate)
									: 90000;
			receiverConfig.videoCodec = rtpCodec;
			receiverConfig.rembBitrateBps = static_cast<uint64_t>(seat.quality.maxVideoBitrateKbps) * 1000;
			receiverConfig.clock = peer->clock;
			receiverConfig.onVideo = [slot = seat.slot, rtpCodec](MediaReceiver::VideoOutput &&output) {
				auto sink = slot->get();
				if (!sink)
					return;
				EncodedVideoFrame frame;
				frame.codec = toPublicCodec(rtpCodec);
				frame.data = std::move(output.frame.data);
				frame.timestampNs = output.timestampNs;
				frame.keyframe = output.frame.keyframe;
				sink->onVideoFrame(std::move(frame));
			};
			entry.receiver = std::make_shared<MediaReceiver>(std::move(receiverConfig));
			entry.track->setMediaHandler(entry.receiver);
			QC_LOG_INFO("guest '%s': receiving %s video", seat.seat.name.c_str(), codec->name.c_str());
		}
		peer->tracks.emplace(section.mid, std::move(entry));
	}

	void addInboundAudio(const std::shared_ptr<InboundPeer> &peer, Seat &seat, const SdpMediaSection &section)
	{
		const auto codec = chooseAudioCodec(section);
		if (!codec)
			return;

		const bool use = section.remoteSends && !peer->haveAudioTrack;
		rtc::Description::Audio media(section.mid, use ? rtc::Description::Direction::RecvOnly
							       : rtc::Description::Direction::Inactive);
		// Our receive capabilities: stereo and in-band FEC, so the guest's client may use both.
		media.addOpusCodec(codec->payloadType,
				   std::string("minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1"));

		InboundTrack entry;
		entry.video = false;
		entry.track = peer->pc->addTrack(media);
		if (use) {
			peer->haveAudioTrack = true;
			MediaReceiver::Config receiverConfig;
			receiverConfig.video = false;
			receiverConfig.payloadType = codec->payloadType;
			receiverConfig.clockRate = 48000;
			receiverConfig.clock = peer->clock;
			receiverConfig.onAudio = [slot = seat.slot](MediaReceiver::AudioOutput &&output) {
				auto sink = slot->get();
				if (!sink)
					return;
				EncodedAudioPacket packet;
				packet.data = std::move(output.payload);
				packet.timestampNs = output.timestampNs;
				sink->onAudioPacket(std::move(packet));
			};
			entry.receiver = std::make_shared<MediaReceiver>(std::move(receiverConfig));
			entry.track->setMediaHandler(entry.receiver);
		}
		peer->tracks.emplace(section.mid, std::move(entry));
	}

	void onInboundChannel(const std::shared_ptr<InboundPeer> &peer,
			      const std::shared_ptr<rtc::DataChannel> &channel)
	{
		if (!isCurrent(peer) || !channel)
			return;
		const std::string label = channel->label();
		if (!label.empty() && label != "sendChannel")
			return;

		peer->channel = channel;
		std::weak_ptr<InboundPeer> weakPeer = peer;

		channel->onOpen(bounce([weakPeer](Impl &self) {
			if (auto strong = weakPeer.lock())
				self.sendViewerHello(strong);
		}));
		channel->onMessage(bounce([weakPeer](Impl &self, rtc::message_variant data) {
			auto strong = weakPeer.lock();
			if (!strong || !std::holds_alternative<std::string>(data))
				return;
			Json message = Json::parse(std::get<std::string>(data), nullptr, false);
			if (!message.is_discarded())
				self.onInboundData(strong, message);
		}));

		if (channel->isOpen())
			sendViewerHello(peer);
	}

	void sendViewerHello(const std::shared_ptr<InboundPeer> &peer)
	{
		if (!isCurrent(peer) || peer->helloSent)
			return;
		auto seat = findSeat(peer->seatId);
		if (!seat)
			return;
		// The guest's client only attaches its camera and microphone after it has seen this message.
		if (!sendJson(peer->channel, buildViewerHello(config_.hostName, director_)))
			return;
		peer->helloSent = true;
		sendQuality(peer, *seat);
	}

	void sendQuality(const std::shared_ptr<InboundPeer> &peer, const Seat &seat)
	{
		sendJson(peer->channel, buildBitrateRequest(seat.quality.maxVideoBitrateKbps));
		if (seat.quality.maxWidth > 0 && seat.quality.maxHeight > 0 && seat.quality.maxHeight < 1080)
			sendJson(peer->channel, buildResolutionRequest(seat.quality.maxWidth, seat.quality.maxHeight));
		for (auto &[mid, entry] : peer->tracks) {
			if (entry.video && entry.receiver && entry.track) {
				try {
					entry.track->requestBitrate(seat.quality.maxVideoBitrateKbps * 1000);
				} catch (const std::exception &) {
				}
			}
		}
	}

	void onInboundData(const std::shared_ptr<InboundPeer> &peer, const Json &message)
	{
		if (!isCurrent(peer))
			return;
		const PeerDataMessage data = parsePeerDataMessage(message);

		if (data.pingToken)
			sendJson(peer->channel, buildPong(*data.pingToken));

		if (data.bye) {
			if (auto seat = findSeat(peer->seatId); seat && !seat->wireStreamId.empty())
				roomStreams_.erase(seat->wireStreamId);
			closeInbound(peer->uuid, "guest left");
			return;
		}

		if (data.carriesSignaling)
			onSignalingMessage(relayedFrom(message, peer->uuid), peer->uuid);

		if (auto seat = findSeat(peer->seatId)) {
			bool changed = false;
			if (data.label && *data.label != seat->status.reportedName) {
				seat->status.reportedName = *data.label;
				changed = true;
			}
			if (data.microphoneMuted && *data.microphoneMuted != seat->status.microphoneMuted) {
				seat->status.microphoneMuted = *data.microphoneMuted;
				changed = true;
			}
			if (data.cameraMuted && *data.cameraMuted != seat->status.cameraMuted) {
				seat->status.cameraMuted = *data.cameraMuted;
				changed = true;
			}
			if (changed)
				notifyStatus(*seat);
		}

		if (data.rejected)
			QC_LOG_WARN("guest refused command '%s' (not the room director?)", data.rejected->c_str());
	}

	void onInboundState(const std::shared_ptr<InboundPeer> &peer, rtc::PeerConnection::State state)
	{
		if (!isCurrent(peer))
			return;
		auto seat = findSeat(peer->seatId);

		switch (state) {
		case rtc::PeerConnection::State::Connected:
			if (seat && seat->status.connection != GuestConnection::Connected) {
				QC_LOG_INFO("guest '%s' connected", seat->seat.name.c_str());
				seat->status.connection = GuestConnection::Connected;
				notifyStatus(*seat);
			}
			break;
		case rtc::PeerConnection::State::Disconnected: {
			// ICE often recovers by itself; only give up if it has not after a grace period.
			std::weak_ptr<Impl> weak = weak_from_this();
			std::weak_ptr<InboundPeer> weakPeer = peer;
			executor_.postDelayed(kDisconnectGrace, [weak, weakPeer] {
				auto self = weak.lock();
				auto strong = weakPeer.lock();
				if (!self || !strong || !self->isCurrent(strong))
					return;
				if (strong->pc->state() != rtc::PeerConnection::State::Connected)
					self->closeInbound(strong->uuid, "connection lost");
			});
			break;
		}
		case rtc::PeerConnection::State::Failed:
		case rtc::PeerConnection::State::Closed:
			closeInbound(peer->uuid, "connection failed");
			break;
		default:
			break;
		}
	}

	static void releaseInbound(const std::shared_ptr<InboundPeer> &peer)
	{
		if (peer->channel)
			peer->channel->resetCallbacks();
		if (peer->pc) {
			peer->pc->resetCallbacks();
			peer->pc->close();
		}
	}

	void closeInbound(const std::string &uuid, const char *reason, bool allowReplay = true)
	{
		auto it = inbound_.find(uuid);
		if (it == inbound_.end())
			return;
		auto peer = it->second;
		inbound_.erase(it);
		releaseInbound(peer);

		auto seat = findSeat(peer->seatId);
		if (!seat || seat->uuid != uuid)
			return;

		QC_LOG_INFO("guest '%s' disconnected (%s)", seat->seat.name.c_str(), reason);
		seat->uuid.clear();
		seat->status.connection = GuestConnection::Offline;
		seat->status.microphoneMuted = false;
		seat->status.cameraMuted = false;
		notifyStatus(*seat);
		if (auto sink = seat->slot->get())
			sink->onMediaStopped();
		{
			std::lock_guard<std::mutex> lock(statsMutex_);
			stats_.erase(seat->seat.id);
		}

		// Still listed in the room: the connection dropped but the guest did not leave, so ask again.
		if (allowReplay && running_ && !seat->wireStreamId.empty() && roomStreams_.count(seat->wireStreamId))
			scheduleReplay(seat->seat.id);
	}

	// ---------------------------------------------------------------------------------------------------------
	// Outbound: the return feed
	// ---------------------------------------------------------------------------------------------------------

	void createOutbound(int feed, const std::string &uuid, const std::string &session)
	{
		auto peer = std::make_shared<OutboundPeer>();
		peer->feed = feed;
		peer->key = outboundKey(feed, uuid);
		peer->uuid = uuid;
		peer->session = session;

		try {
			peer->pc = std::make_shared<rtc::PeerConnection>(makeRtcConfiguration());
			std::weak_ptr<OutboundPeer> weakPeer = peer;

			peer->pc->onLocalDescription(bounce([weakPeer](Impl &self, rtc::Description description) {
				auto strong = weakPeer.lock();
				if (!strong || !self.isCurrent(strong))
					return;
				auto message = buildDescription(self.credentials_, strong->uuid, strong->session, true,
								std::string(description),
								self.wireStreamIdFor(strong->feed));
				if (!message)
					return;
				strong->cachedOffer = *message;
				if (auto *signaling = self.signalingFor(strong->feed))
					signaling->send(*message);
			}));

			peer->pc->onLocalCandidate(bounce([weakPeer](Impl &self, rtc::Candidate candidate) {
				auto strong = weakPeer.lock();
				if (!strong || !self.isCurrent(strong))
					return;
				auto message = buildCandidates(self.credentials_, strong->uuid, strong->session, true,
							       {IceCandidate{candidate.candidate(), candidate.mid()}});
				if (message)
					self.sendPeerSignal(strong->channel, *message, strong->feed);
			}));

			peer->pc->onStateChange(bounce([weakPeer](Impl &self, rtc::PeerConnection::State state) {
				if (auto strong = weakPeer.lock())
					self.onOutboundState(strong, state);
			}));

			// Video: H.264 advertised as constrained baseline, which every browser accepts regardless of the
			// profile the encoder actually produces.
			const uint32_t videoSsrc = randomSsrc();
			rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
			video.addH264Codec(kReturnVideoPayloadType);
			video.addSSRC(videoSsrc, "quick-cast", "quick-cast-return", "video");
			peer->videoTrack = peer->pc->addTrack(video);
			{
				auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
					videoSsrc, "quick-cast", kReturnVideoPayloadType,
					rtc::H264RtpPacketizer::ClockRate);
				auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
					rtc::H264RtpPacketizer::Separator::StartSequence, rtpConfig);
				packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
				packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
				peer->videoTrack->setMediaHandler(packetizer);
			}

			// Only the main feed carries the host's audio; a second copy would just double it for guests.
			if (feed == 0) {
				const uint32_t audioSsrc = randomSsrc();
				rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
				audio.addOpusCodec(kReturnAudioPayloadType);
				audio.addSSRC(audioSsrc, "quick-cast", "quick-cast-return", "audio");
				peer->audioTrack = peer->pc->addTrack(audio);

				auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
					audioSsrc, "quick-cast", kReturnAudioPayloadType,
					rtc::OpusRtpPacketizer::DefaultClockRate);
				auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
				packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
				packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
				peer->audioTrack->setMediaHandler(packetizer);
			}

			// The web client expects the publisher to open this channel and introduce itself on it.
			peer->channel = peer->pc->createDataChannel("sendChannel");
			peer->channel->onOpen(bounce([weakPeer](Impl &self) {
				auto strong = weakPeer.lock();
				if (!strong || !self.isCurrent(strong))
					return;
				ReturnFeedFormat format;
				{
					std::lock_guard<std::mutex> lock(self.returnMutex_);
					format = self.returnFormat_[strong->feed];
				}
				// Guests see each feed as its own tile; the suffix tells the two apart.
				const std::string label = strong->feed == 0 ? self.config_.hostName
									    : self.config_.hostName + " (2)";
				sendJson(strong->channel, buildPublisherInfo(label, format.width, format.height,
									     format.framesPerSecond));
			}));
			peer->channel->onMessage(bounce([weakPeer](Impl &self, rtc::message_variant data) {
				auto strong = weakPeer.lock();
				if (!strong || !std::holds_alternative<std::string>(data))
					return;
				Json message = Json::parse(std::get<std::string>(data), nullptr, false);
				if (!message.is_discarded())
					self.onOutboundData(strong, message);
			}));

			outbound_[peer->key] = peer;
			{
				std::lock_guard<std::mutex> lock(returnMutex_);
				returnTargets_.push_back(peer);
			}
			updateReturnDemand();

			peer->pc->setLocalDescription(rtc::Description::Type::Offer);
		} catch (const std::exception &error) {
			QC_LOG_ERROR("could not start return feed for a guest: %s", error.what());
			closeOutbound(peer->key);
		}
	}

	void restartOutbound(const std::string &key)
	{
		auto it = outbound_.find(key);
		if (it == outbound_.end())
			return;
		const int feed = it->second->feed;
		const std::string uuid = it->second->uuid;
		const std::string session = it->second->session;
		closeOutbound(key);
		createOutbound(feed, uuid, session);
	}

	void onRemoteAnswer(const DescriptionMessage &answer)
	{
		const std::string key = outboundKey(feed_, answer.uuid);
		auto it = outbound_.find(key);
		if (it == outbound_.end())
			return;
		auto peer = it->second;
		if (sessionsConflict(peer->session, answer.session) || peer->remoteDescriptionSet)
			return;
		try {
			peer->pc->setRemoteDescription(rtc::Description(answer.sdp, rtc::Description::Type::Answer));
			peer->remoteDescriptionSet = true;
			flushCandidates(*peer->pc, peer->pendingCandidates);
		} catch (const std::exception &error) {
			QC_LOG_WARN("guest sent an unusable answer for the return feed: %s", error.what());
			closeOutbound(key);
		}
	}

	void onOutboundData(const std::shared_ptr<OutboundPeer> &peer, const Json &message)
	{
		if (!isCurrent(peer))
			return;
		const PeerDataMessage data = parsePeerDataMessage(message);

		if (data.pingToken)
			sendJson(peer->channel, buildPong(*data.pingToken));
		if (data.bye) {
			closeOutbound(peer->key);
			return;
		}
		if (data.iceRestartRequested) {
			restartOutbound(peer->key);
			return;
		}
		if (data.carriesSignaling)
			onSignalingMessage(relayedFrom(message, peer->uuid), peer->uuid, peer->feed);

		if (data.wantsAudio)
			peer->audioWanted = *data.wantsAudio;
		if (data.wantsVideo)
			setVideoWanted(*peer, *data.wantsVideo);
		// Browsers pause video for tiles that are hidden by sending a bitrate of zero.
		if (data.videoBitrateKbps)
			setVideoWanted(*peer, *data.videoBitrateKbps != 0);
		if (data.keyframeRequested)
			peer->waitingForKeyframe = true;
	}

	static void setVideoWanted(OutboundPeer &peer, bool wanted)
	{
		if (wanted && !peer.videoWanted.load())
			peer.waitingForKeyframe = true; // resume cleanly on the next keyframe
		peer.videoWanted = wanted;
	}

	void onOutboundState(const std::shared_ptr<OutboundPeer> &peer, rtc::PeerConnection::State state)
	{
		if (!isCurrent(peer))
			return;
		if (state == rtc::PeerConnection::State::Connected) {
			peer->connected = true;
			if (peer->feed == 0)
				markReturnFeed(peer->uuid, true);
		} else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
			closeOutbound(peer->key);
		}
	}

	void markReturnFeed(const std::string &uuid, bool receiving)
	{
		for (auto &[id, seat] : seats_) {
			if (seat->uuid == uuid && seat->status.receivingReturnFeed != receiving) {
				seat->status.receivingReturnFeed = receiving;
				notifyStatus(*seat);
			}
		}
	}

	static void releaseOutbound(const std::shared_ptr<OutboundPeer> &peer)
	{
		if (peer->channel)
			peer->channel->resetCallbacks();
		if (peer->pc) {
			peer->pc->resetCallbacks();
			peer->pc->close();
		}
	}

	void closeOutbound(const std::string &key)
	{
		auto it = outbound_.find(key);
		if (it == outbound_.end())
			return;
		auto peer = it->second;
		outbound_.erase(it);
		{
			std::lock_guard<std::mutex> lock(returnMutex_);
			returnTargets_.erase(std::remove(returnTargets_.begin(), returnTargets_.end(), peer),
					     returnTargets_.end());
		}
		releaseOutbound(peer);
		if (peer->feed == 0)
			markReturnFeed(peer->uuid, false);
		updateReturnDemand();
	}

	void updateReturnDemand()
	{
		const bool wanted = !outbound_.empty();
		if (wanted == returnDemand_)
			return;
		returnDemand_ = wanted;
		if (auto *listener = listener_.load())
			listener->onReturnFeedDemand(wanted);
	}

	// ---------------------------------------------------------------------------------------------------------
	// Statistics
	// ---------------------------------------------------------------------------------------------------------

	void scheduleStats()
	{
		std::weak_ptr<Impl> weak = weak_from_this();
		statsTimer_ = executor_.postDelayed(kStatsInterval, [weak] {
			auto self = weak.lock();
			if (!self || !self->running_)
				return;
			self->collectStats();
			self->scheduleStats();
		});
	}

	/** Closes connections to guests that are still "connected" at the transport level but have stopped sending. */
	void dropSilentGuests()
	{
		const int64_t nowUs = monotonicNowUs();
		std::vector<std::string> silent;
		for (const auto &[uuid, peer] : inbound_) {
			int64_t latest = 0;
			for (const auto &[mid, entry] : peer->tracks) {
				if (entry.receiver)
					latest = std::max(latest, entry.receiver->lastPacketUs());
			}
			// Only once media has flowed: negotiation legitimately takes a while before the first packet.
			if (latest > 0 && nowUs - latest > kMediaSilenceUs)
				silent.push_back(uuid);
		}
		for (const std::string &uuid : silent)
			closeInbound(uuid, "stopped sending");
	}

	void collectStats()
	{
		dropSilentGuests();
		const double seconds = std::chrono::duration<double>(kStatsInterval).count();
		for (auto &[id, seat] : seats_) {
			auto it = inbound_.find(seat->uuid);
			if (seat->uuid.empty() || it == inbound_.end())
				continue;

			GuestMediaStats result;
			uint64_t videoBytes = 0;
			uint64_t audioBytes = 0;
			for (const auto &[mid, entry] : it->second->tracks) {
				if (!entry.receiver)
					continue;
				const rtp::ReceiveStats stats = entry.receiver->stats();
				if (entry.video) {
					videoBytes += stats.bytesReceived;
					result.videoLoss = stats.recentLossFraction;
					result.jitterMs = stats.jitterMs;
					result.keyframeRequests = stats.plisSent;
				} else {
					audioBytes += stats.bytesReceived;
				}
			}
			if (videoBytes >= seat->lastVideoBytes)
				result.videoBitrateKbps = (videoBytes - seat->lastVideoBytes) * 8.0 / 1000.0 / seconds;
			if (audioBytes >= seat->lastAudioBytes)
				result.audioBitrateKbps = (audioBytes - seat->lastAudioBytes) * 8.0 / 1000.0 / seconds;
			seat->lastVideoBytes = videoBytes;
			seat->lastAudioBytes = audioBytes;

			std::lock_guard<std::mutex> lock(statsMutex_);
			stats_[id] = result;
		}
	}
};

// =====================================================================================================================

VdoService::VdoService(VdoServiceOptions options) : impl_(std::make_shared<Impl>(std::move(options)))
{
	installRtcLogger();
}

VdoService::~VdoService()
{
	stop();
	// Join the worker before the implementation can be destroyed so no task outlives it.
	impl_->executor_.shutdown();
}

void VdoService::setListener(GuestServiceListener *listener)
{
	impl_->listener_.store(listener);
}

void VdoService::start(const ShowConfig &config)
{
	impl_->post([config](Impl &self) { self.start(config); });
}

void VdoService::stop()
{
	auto impl = impl_;
	impl->executor_.runSync([impl] { impl->stopInternal(); });
}

void VdoService::addSeat(const GuestSeat &seat)
{
	impl_->post([seat](Impl &self) { self.ensureSeat(seat, false); });
}

void VdoService::renameSeat(const std::string &guestId, const std::string &name)
{
	impl_->post([guestId, name](Impl &self) {
		auto seat = self.findSeat(guestId);
		if (!seat)
			return;
		seat->seat.name = name;
		// As director we can also rename the guest for everyone else in the room.
		if (auto it = self.inbound_.find(seat->uuid); it != self.inbound_.end() && self.director_)
			sendJson(it->second->channel, buildChangeLabel(name));
	});
}

void VdoService::removeSeat(const std::string &guestId)
{
	disconnectGuest(guestId);
	impl_->post([guestId](Impl &self) {
		self.seats_.erase(guestId);
		std::lock_guard<std::mutex> lock(self.statsMutex_);
		self.stats_.erase(guestId);
	});
}

std::string VdoService::inviteLink(const ShowConfig &config, const GuestSeat &seat) const
{
	InviteOptions options;
	options.baseUrl = impl_->options_.guestAppUrl;
	options.room = config.room;
	options.password = config.password;
	options.streamId = seat.id;
	options.label = seat.name;
	return buildInviteLink(options);
}

std::string VdoService::openInviteLink(const ShowConfig &config) const
{
	InviteOptions options;
	options.baseUrl = impl_->options_.guestAppUrl;
	options.room = config.room;
	options.password = config.password;
	return buildInviteLink(options);
}

void VdoService::setMediaSink(const std::string &guestId, std::shared_ptr<GuestMediaSink> sink)
{
	auto impl = impl_;
	// Synchronous so the caller can rely on the old sink no longer being referenced by new work afterwards.
	impl->executor_.runSync([impl, guestId, sink] {
		auto seat = impl->findSeat(guestId);
		if (!seat) {
			if (!sink)
				return;
			seat = impl->ensureSeat(GuestSeat{guestId, "Guest"}, false);
		}
		seat->slot->set(sink);
		// A sink attached mid-stream needs a keyframe before it can show anything.
		if (sink) {
			if (auto it = impl->inbound_.find(seat->uuid); it != impl->inbound_.end()) {
				for (auto &[mid, entry] : it->second->tracks) {
					if (entry.video && entry.receiver && entry.track) {
						try {
							entry.track->requestKeyframe();
						} catch (const std::exception &) {
						}
					}
				}
			}
		}
	});
}

void VdoService::setGuestQuality(const std::string &guestId, const GuestQuality &quality)
{
	impl_->post([guestId, quality](Impl &self) {
		auto seat = self.findSeat(guestId);
		if (!seat)
			return;
		seat->quality = quality;
		if (auto it = self.inbound_.find(seat->uuid); it != self.inbound_.end())
			self.sendQuality(it->second, *seat);
	});
}

void VdoService::requestKeyframe(const std::string &guestId)
{
	impl_->post([guestId](Impl &self) {
		auto seat = self.findSeat(guestId);
		if (!seat)
			return;
		auto it = self.inbound_.find(seat->uuid);
		if (it == self.inbound_.end())
			return;
		for (auto &[mid, entry] : it->second->tracks) {
			if (entry.video && entry.receiver && entry.track) {
				try {
					entry.track->requestKeyframe();
				} catch (const std::exception &) {
				}
			}
		}
	});
}

void VdoService::disconnectGuest(const std::string &guestId)
{
	impl_->post([guestId](Impl &self) {
		auto seat = self.findSeat(guestId);
		if (!seat || seat->uuid.empty())
			return;
		const std::string uuid = seat->uuid;
		if (auto it = self.inbound_.find(uuid); it != self.inbound_.end())
			sendJson(it->second->channel, buildHangup());
		// Forget the stream so we do not immediately ask for it again.
		self.roomStreams_.erase(seat->wireStreamId);
		self.closeInbound(uuid, "removed by host", false);
		self.closeOutbound(Impl::outboundKey(0, uuid));
		self.closeOutbound(Impl::outboundKey(1, uuid));
	});
}

bool VdoService::guestStats(const std::string &guestId, GuestMediaStats &stats) const
{
	std::lock_guard<std::mutex> lock(impl_->statsMutex_);
	auto it = impl_->stats_.find(guestId);
	if (it == impl_->stats_.end())
		return false;
	stats = it->second;
	return true;
}

void VdoService::setReturnFeedFormat(ReturnFeedId feed, const ReturnFeedFormat &format)
{
	std::lock_guard<std::mutex> lock(impl_->returnMutex_);
	impl_->returnFormat_[static_cast<int>(feed)] = format;
}

void VdoService::sendReturnVideo(ReturnFeedId feed, const uint8_t *data, size_t size, int64_t timestampUs,
				 bool keyframe)
{
	const int feedIndex = static_cast<int>(feed);
	std::vector<std::shared_ptr<OutboundPeer>> targets;
	{
		std::lock_guard<std::mutex> lock(impl_->returnMutex_);
		targets = impl_->returnTargets_;
	}
	const rtc::FrameInfo info(std::chrono::duration<double>(static_cast<double>(timestampUs) / 1'000'000.0));
	for (const auto &peer : targets) {
		if (peer->feed != feedIndex || !peer->videoWanted.load() || !peer->videoTrack ||
		    !peer->videoTrack->isOpen())
			continue;
		if (peer->waitingForKeyframe.load()) {
			if (!keyframe)
				continue;
			peer->waitingForKeyframe = false;
		}
		try {
			peer->videoTrack->sendFrame(reinterpret_cast<const std::byte *>(data), size, info);
		} catch (const std::exception &) {
			// The connection is going away; its state callback will clean up.
		}
	}
}

void VdoService::sendReturnAudio(const uint8_t *data, size_t size, int64_t timestampUs)
{
	std::vector<std::shared_ptr<OutboundPeer>> targets;
	{
		std::lock_guard<std::mutex> lock(impl_->returnMutex_);
		targets = impl_->returnTargets_;
	}
	const rtc::FrameInfo info(std::chrono::duration<double>(static_cast<double>(timestampUs) / 1'000'000.0));
	for (const auto &peer : targets) {
		if (!peer->audioWanted.load() || !peer->audioTrack || !peer->audioTrack->isOpen())
			continue;
		try {
			peer->audioTrack->sendFrame(reinterpret_cast<const std::byte *>(data), size, info);
		} catch (const std::exception &) {
		}
	}
}

} // namespace qc::vdo
