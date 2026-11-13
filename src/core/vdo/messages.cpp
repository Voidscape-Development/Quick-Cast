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

#include "messages.hpp"

#include <algorithm>
#include <cctype>

namespace qc::vdo {

namespace {

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

/** First string value found under any of `keys`. */
std::string firstString(const Json &object, std::initializer_list<const char *> keys)
{
	if (!object.is_object())
		return {};
	for (const char *key : keys) {
		auto it = object.find(key);
		if (it != object.end() && it->is_string())
			return it->get<std::string>();
	}
	return {};
}

std::string peerId(const Json &message)
{
	return firstString(message, {"UUID", "uuid", "from"});
}

std::string sessionOf(const Json &message)
{
	return firstString(message, {"session", "Session"});
}

std::string streamOf(const Json &message)
{
	return firstString(message, {"streamID", "streamId"});
}

std::string stripCandidatePrefix(std::string candidate)
{
	if (candidate.rfind("a=", 0) == 0)
		candidate.erase(0, 2);
	return candidate;
}

/**
 * Resolves a possibly-encrypted field to parsed JSON. A value is encrypted when it is a string and the envelope
 * carries a `vector`; anything else is taken as plaintext.
 */
std::optional<Json> resolvePayload(const Json &message, const char *key, const std::vector<const Credentials *> &keys)
{
	auto it = message.find(key);
	if (it == message.end())
		return std::nullopt;

	const std::string vector = firstString(message, {"vector"});
	if (!it->is_string() || vector.empty())
		return *it;

	const std::string ciphertext = it->get<std::string>();
	for (const Credentials *credentials : keys) {
		if (!credentials || !credentials->encryptionEnabled())
			continue;
		auto plaintext = decryptPayload(*credentials, ciphertext, vector);
		if (!plaintext)
			continue;
		Json parsed = Json::parse(*plaintext, nullptr, false);
		if (!parsed.is_discarded())
			return parsed;
	}
	return std::nullopt;
}

std::optional<IceCandidate> parseCandidate(const Json &value, const std::string &fallbackMid)
{
	IceCandidate candidate;
	if (value.is_string()) {
		candidate.candidate = value.get<std::string>();
		candidate.mid = fallbackMid;
	} else if (value.is_object()) {
		candidate.candidate = firstString(value, {"candidate"});
		candidate.mid = firstString(value, {"sdpMid", "mid", "smid", "rmid"});
		if (candidate.mid.empty())
			candidate.mid = fallbackMid;
	}
	candidate.candidate = stripCandidatePrefix(std::move(candidate.candidate));
	if (candidate.candidate.empty())
		return std::nullopt; // end-of-candidates marker or junk
	return candidate;
}

RoomMember parseMember(const Json &entry)
{
	RoomMember member;
	if (entry.is_string()) {
		member.streamId = entry.get<std::string>();
	} else if (entry.is_object()) {
		member.streamId = streamOf(entry);
		member.uuid = peerId(entry);
	}
	return member;
}

/** SDP occasionally arrives with literal "\r\n" text instead of real line breaks. */
std::string repairSdp(std::string sdp)
{
	if (sdp.find('\n') != std::string::npos)
		return sdp;
	std::string out;
	out.reserve(sdp.size());
	for (size_t i = 0; i < sdp.size(); ++i) {
		if (sdp[i] == '\\' && i + 1 < sdp.size() && (sdp[i + 1] == 'r' || sdp[i + 1] == 'n')) {
			if (sdp[i + 1] == 'n')
				out += "\r\n";
			++i;
		} else {
			out.push_back(sdp[i]);
		}
	}
	return out;
}

} // namespace

std::optional<EncryptedPayload> encryptPayload(const Credentials &credentials, const std::string &plaintext)
{
	if (!credentials.encryptionEnabled())
		return std::nullopt;
	crypto::AesIv iv{};
	crypto::randomBytes(iv.data(), iv.size());
	auto ciphertext = crypto::aes256CbcEncrypt(credentials.key(), iv, plaintext);
	if (!ciphertext)
		return std::nullopt;
	return EncryptedPayload{crypto::toHex(ciphertext->data(), ciphertext->size()),
				crypto::toHex(iv.data(), iv.size())};
}

std::optional<std::string> decryptPayload(const Credentials &credentials, const std::string &ciphertextHex,
					  const std::string &vectorHex)
{
	if (!credentials.encryptionEnabled())
		return std::nullopt;
	const auto ciphertext = crypto::fromHex(ciphertextHex);
	const auto vector = crypto::fromHex(vectorHex);
	if (!ciphertext || !vector || vector->size() != 16)
		return std::nullopt;
	crypto::AesIv iv{};
	std::copy(vector->begin(), vector->end(), iv.begin());
	return crypto::aes256CbcDecrypt(credentials.key(), iv, ciphertext->data(), ciphertext->size());
}

SignalingMessage parseSignalingMessage(const Json &message, const std::vector<const Credentials *> &keys,
				       const std::string &fallbackUuid)
{
	if (!message.is_object())
		return IgnoredMessage{"not an object"};

	std::string uuid = peerId(message);
	if (uuid.empty())
		uuid = fallbackUuid;
	const std::string session = sessionOf(message);
	const std::string request = lower(firstString(message, {"request", "Request"}));

	// Room listing (also sent when a room is transferred to us).
	if (request == "listing" || request == "transferred" || message.contains("list") ||
	    message.contains("listing")) {
		ListingMessage listing;
		const Json *array = nullptr;
		if (auto it = message.find("list"); it != message.end() && it->is_array())
			array = &*it;
		else if (auto it2 = message.find("listing"); it2 != message.end() && it2->is_array())
			array = &*it2;
		if (array) {
			for (const Json &entry : *array) {
				RoomMember member = parseMember(entry);
				if (!member.streamId.empty())
					listing.members.push_back(std::move(member));
			}
		}
		if (auto it = message.find("director"); it != message.end() && it->is_boolean())
			listing.director = it->get<bool>();
		if (auto it = message.find("claim"); it != message.end() && it->is_boolean())
			listing.director = it->get<bool>();
		return listing;
	}

	if (message.contains("description")) {
		auto payload = resolvePayload(message, "description", keys);
		if (!payload || !payload->is_object())
			return IgnoredMessage{"undecryptable description"};
		DescriptionMessage description;
		description.uuid = uuid;
		description.session = session;
		description.streamId = streamOf(message);
		description.isOffer = lower(firstString(*payload, {"type"})) == "offer";
		description.sdp = repairSdp(firstString(*payload, {"sdp"}));
		if (description.uuid.empty() || description.sdp.empty())
			return IgnoredMessage{"incomplete description"};
		return description;
	}

	if (message.contains("candidate") || message.contains("candidates")) {
		CandidatesMessage result;
		result.uuid = uuid;
		result.session = session;
		const std::string type = lower(firstString(message, {"type"}));
		if (type == "local")
			result.origin = CandidateOrigin::Offerer;
		else if (type == "remote")
			result.origin = CandidateOrigin::Answerer;

		const std::string fallbackMid = firstString(message, {"sdpMid", "mid"});
		if (message.contains("candidates")) {
			auto payload = resolvePayload(message, "candidates", keys);
			if (!payload)
				return IgnoredMessage{"undecryptable candidates"};
			if (payload->is_array()) {
				for (const Json &entry : *payload) {
					if (auto candidate = parseCandidate(entry, fallbackMid))
						result.candidates.push_back(std::move(*candidate));
				}
			} else if (auto candidate = parseCandidate(*payload, fallbackMid)) {
				result.candidates.push_back(std::move(*candidate));
			}
		} else {
			auto payload = resolvePayload(message, "candidate", keys);
			if (!payload)
				return IgnoredMessage{"undecryptable candidate"};
			if (auto candidate = parseCandidate(*payload, fallbackMid))
				result.candidates.push_back(std::move(*candidate));
		}
		if (result.uuid.empty())
			return IgnoredMessage{"candidate without peer"};
		return result;
	}

	if (auto it = message.find("iceRestartRequest"); it != message.end() && it->is_boolean() && it->get<bool>())
		return IceRestartRequestMessage{uuid, session};

	if (request == "alert" || request == "error") {
		std::string text = firstString(message, {"message", "alert", "error"});
		return AlertMessage{text.empty() ? std::string("Unknown server error") : text};
	}

	const bool byeTrue = [&] {
		auto it = message.find("bye");
		return it != message.end() && it->is_boolean() && it->get<bool>();
	}();
	if (request == "cleanup" || byeTrue) {
		if (uuid.empty())
			return IgnoredMessage{"cleanup without peer"};
		return PeerGoneMessage{uuid, session};
	}

	const std::string streamId = streamOf(message);
	if ((request == "someonejoined" && !streamId.empty()) || request == "videoaddedtoroom" ||
	    message.contains("videoAddedToRoom")) {
		if (streamId.empty())
			return IgnoredMessage{"stream added without id"};
		return StreamAddedMessage{RoomMember{streamId, uuid}};
	}
	if (request == "videoremovedfromroom" || message.contains("videoRemovedFromRoom"))
		return StreamRemovedMessage{RoomMember{streamId, uuid}};

	if (request == "offersdp" || request == "sendoffer" || request == "play") {
		if (uuid.empty())
			return IgnoredMessage{"offer request without peer"};
		return OfferRequestMessage{uuid, session};
	}

	if (message.contains("alert")) {
		const std::string text = firstString(message, {"alert"});
		if (!text.empty())
			return AlertMessage{text};
	}

	return IgnoredMessage{request.empty() ? "unrecognised" : "unhandled request: " + request};
}

Json buildJoinRoom(const std::string &wireRoomId, bool claimDirector)
{
	Json message = {{"request", "joinroom"}, {"roomid", wireRoomId}};
	if (claimDirector) {
		message["claim"] = true;
		message["claimSettings"] = {{"roomCap", nullptr}, {"bypassKey", ""}, {"requireApproval", false}};
	}
	return message;
}

Json buildSeed(const std::string &wireStreamId)
{
	return {{"request", "seed"}, {"streamID", wireStreamId}};
}

Json buildPlay(const std::string &wireStreamId)
{
	return {{"request", "play"}, {"streamID", wireStreamId}};
}

std::optional<Json> buildDescription(const Credentials &credentials, const std::string &uuid,
				     const std::string &session, bool isOffer, const std::string &sdp,
				     const std::string &wireStreamId)
{
	Json message = {{"UUID", uuid}, {"session", session}};
	if (!wireStreamId.empty())
		message["streamID"] = wireStreamId;

	Json description = {{"type", isOffer ? "offer" : "answer"}, {"sdp", sdp}};
	if (credentials.encryptionEnabled()) {
		auto encrypted = encryptPayload(credentials, description.dump());
		if (!encrypted)
			return std::nullopt;
		message["description"] = encrypted->ciphertextHex;
		message["vector"] = encrypted->vectorHex;
	} else {
		message["description"] = std::move(description);
	}
	return message;
}

std::optional<Json> buildCandidates(const Credentials &credentials, const std::string &uuid, const std::string &session,
				    bool weAreOfferer, const std::vector<IceCandidate> &candidates)
{
	Json list = Json::array();
	for (const IceCandidate &candidate : candidates) {
		list.push_back({{"candidate", stripCandidatePrefix(candidate.candidate)},
				{"sdpMid", candidate.mid},
				{"mid", candidate.mid}});
	}

	Json message = {{"UUID", uuid}, {"type", weAreOfferer ? "local" : "remote"}, {"session", session}};
	if (credentials.encryptionEnabled()) {
		auto encrypted = encryptPayload(credentials, list.dump());
		if (!encrypted)
			return std::nullopt;
		message["candidates"] = encrypted->ciphertextHex;
		message["vector"] = encrypted->vectorHex;
	} else {
		message["candidates"] = std::move(list);
	}
	return message;
}

} // namespace qc::vdo
