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

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qc::vdo {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------------------------------------------
// Inbound signaling
// ---------------------------------------------------------------------------------------------------------------

struct IceCandidate {
	std::string candidate; // "candidate:..." without the "a=" prefix
	std::string mid;
};

/** Which end of a negotiation sent a candidate; VDO.Ninja uses separate connections for each media direction. */
enum class CandidateOrigin { Unknown, Offerer, Answerer };

struct RoomMember {
	std::string streamId; // wire form
	std::string uuid;     // may be empty
};

struct ListingMessage {
	std::vector<RoomMember> members;
	/** True when the server confirmed that we hold the director role. */
	std::optional<bool> director;
};

struct StreamAddedMessage {
	RoomMember member;
};

struct StreamRemovedMessage {
	RoomMember member;
};

/** A viewer wants our stream: we must create the offer. */
struct OfferRequestMessage {
	std::string uuid;
	std::string session;
};

struct DescriptionMessage {
	std::string uuid;
	std::string session;
	std::string streamId;
	bool isOffer = false;
	std::string sdp;
};

struct CandidatesMessage {
	std::string uuid;
	std::string session;
	CandidateOrigin origin = CandidateOrigin::Unknown;
	std::vector<IceCandidate> candidates;
};

struct AlertMessage {
	std::string text;
};

struct PeerGoneMessage {
	std::string uuid;
	std::string session;
};

struct IceRestartRequestMessage {
	std::string uuid;
	std::string session;
};

struct IgnoredMessage {
	std::string reason;
};

using SignalingMessage =
	std::variant<IgnoredMessage, ListingMessage, StreamAddedMessage, StreamRemovedMessage, OfferRequestMessage,
		     DescriptionMessage, CandidatesMessage, AlertMessage, PeerGoneMessage, IceRestartRequestMessage>;

/**
 * Decodes one signaling envelope (from the WebSocket or relayed over a data channel).
 *
 * Encrypted SDP/ICE payloads are decrypted with the first of `keys` that yields valid padding. `fallbackUuid` is
 * used for data-channel-relayed messages that omit the sender.
 */
SignalingMessage parseSignalingMessage(const Json &message, const std::vector<const Credentials *> &keys,
				       const std::string &fallbackUuid = {});

// ---------------------------------------------------------------------------------------------------------------
// Outbound signaling
// ---------------------------------------------------------------------------------------------------------------

Json buildJoinRoom(const std::string &wireRoomId, bool claimDirector);
Json buildSeed(const std::string &wireStreamId);
Json buildPlay(const std::string &wireStreamId);

/** Returns nullopt if encryption is required but failed; SDP is never sent in the clear in that case. */
std::optional<Json> buildDescription(const Credentials &credentials, const std::string &uuid,
				     const std::string &session, bool isOffer, const std::string &sdp,
				     const std::string &wireStreamId = {});

std::optional<Json> buildCandidates(const Credentials &credentials, const std::string &uuid, const std::string &session,
				    bool weAreOfferer, const std::vector<IceCandidate> &candidates);

// ---------------------------------------------------------------------------------------------------------------
// Payload encryption (AES-256-CBC, hex ciphertext + hex "vector")
// ---------------------------------------------------------------------------------------------------------------

struct EncryptedPayload {
	std::string ciphertextHex;
	std::string vectorHex;
};

std::optional<EncryptedPayload> encryptPayload(const Credentials &credentials, const std::string &plaintext);
std::optional<std::string> decryptPayload(const Credentials &credentials, const std::string &ciphertextHex,
					  const std::string &vectorHex);

} // namespace qc::vdo
