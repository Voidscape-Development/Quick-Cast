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

#include "sdp-utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace qc::vdo {

namespace {

std::string upper(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return value;
}

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string trim(const std::string &value)
{
	const size_t begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	const size_t end = value.find_last_not_of(" \t\r\n");
	return value.substr(begin, end - begin + 1);
}

int toInt(const std::string &value, int fallback = -1)
{
	if (value.empty())
		return fallback;
	int result = 0;
	for (const char c : value) {
		if (c < '0' || c > '9')
			return fallback;
		result = result * 10 + (c - '0');
		if (result > 1'000'000'000 / 10)
			return fallback;
	}
	return result;
}

SdpCodec *findCodec(SdpMediaSection &section, int payloadType)
{
	for (SdpCodec &codec : section.codecs) {
		if (codec.payloadType == payloadType)
			return &codec;
	}
	return nullptr;
}

} // namespace

std::string fmtpValue(const std::string &fmtp, const std::string &key)
{
	std::stringstream stream(fmtp);
	std::string entry;
	while (std::getline(stream, entry, ';')) {
		const size_t equals = entry.find('=');
		if (equals == std::string::npos)
			continue;
		if (lower(trim(entry.substr(0, equals))) == lower(key))
			return lower(trim(entry.substr(equals + 1)));
	}
	return {};
}

std::vector<SdpMediaSection> parseSdpMedia(const std::string &sdp)
{
	std::vector<SdpMediaSection> sections;
	SdpMediaSection *current = nullptr;
	bool sessionSendsDisabled = false;

	std::stringstream stream(sdp);
	std::string rawLine;
	while (std::getline(stream, rawLine)) {
		const std::string line = trim(rawLine);
		if (line.size() < 2 || line[1] != '=')
			continue;

		if (line[0] == 'm') {
			// m=<kind> <port> <proto> <fmt> ...
			std::stringstream fields(line.substr(2));
			std::string kind, port, proto, format;
			fields >> kind >> port >> proto;
			sections.emplace_back();
			current = &sections.back();
			current->kind = lower(kind);
			current->remoteSends = port != "0" && !sessionSendsDisabled;
			while (fields >> format) {
				const int payloadType = toInt(format);
				if (payloadType >= 0) {
					SdpCodec codec;
					codec.payloadType = payloadType;
					current->codecs.push_back(codec);
				}
			}
			continue;
		}

		if (line[0] != 'a')
			continue;
		const std::string attribute = line.substr(2);

		if (!current) {
			if (attribute == "recvonly" || attribute == "inactive")
				sessionSendsDisabled = true;
			continue;
		}

		if (attribute.rfind("mid:", 0) == 0) {
			current->mid = trim(attribute.substr(4));
		} else if (attribute == "recvonly" || attribute == "inactive") {
			current->remoteSends = false;
		} else if (attribute.rfind("rtpmap:", 0) == 0) {
			// rtpmap:<pt> <name>/<rate>[/<channels>]
			const size_t space = attribute.find(' ');
			if (space == std::string::npos)
				continue;
			SdpCodec *codec = findCodec(*current, toInt(attribute.substr(7, space - 7)));
			if (!codec)
				continue;
			std::stringstream parts(trim(attribute.substr(space + 1)));
			std::string name, rate, channels;
			std::getline(parts, name, '/');
			std::getline(parts, rate, '/');
			std::getline(parts, channels, '/');
			codec->name = upper(name);
			codec->clockRate = toInt(rate, 0);
			codec->channels = toInt(channels, 0);
		} else if (attribute.rfind("fmtp:", 0) == 0) {
			const size_t space = attribute.find(' ');
			if (space == std::string::npos)
				continue;
			if (SdpCodec *codec = findCodec(*current, toInt(attribute.substr(5, space - 5))))
				codec->fmtp = trim(attribute.substr(space + 1));
		}
	}

	// Static payload types without an rtpmap are of no interest to us.
	for (SdpMediaSection &section : sections) {
		section.codecs.erase(std::remove_if(section.codecs.begin(), section.codecs.end(),
						    [](const SdpCodec &codec) { return codec.name.empty(); }),
				     section.codecs.end());
	}
	return sections;
}

std::optional<SdpCodec> chooseVideoCodec(const SdpMediaSection &section, VideoCodecPreference preference)
{
	std::optional<SdpCodec> h264;
	int h264Score = -1;
	std::optional<SdpCodec> vp8;
	std::optional<SdpCodec> vp9;

	for (const SdpCodec &codec : section.codecs) {
		if (codec.name == "H264") {
			if (fmtpValue(codec.fmtp, "packetization-mode") != "1")
				continue;
			const std::string profile = fmtpValue(codec.fmtp, "profile-level-id");
			// profile_idc 0x42 = baseline family; constraint_set1 (0x40) marks constrained baseline.
			int score = 1;
			if (profile.size() == 6 && profile.compare(0, 2, "42") == 0) {
				score = 2;
				const int constraints = std::stoi(profile.substr(2, 2), nullptr, 16);
				if (constraints & 0x40)
					score = 3;
			}
			if (score > h264Score) {
				h264Score = score;
				h264 = codec;
			}
		} else if (codec.name == "VP8" && !vp8) {
			vp8 = codec;
		} else if (codec.name == "VP9" && !vp9) {
			const std::string profile = fmtpValue(codec.fmtp, "profile-id");
			if (profile.empty() || profile == "0")
				vp9 = codec;
		}
	}

	std::optional<SdpCodec> preferred;
	switch (preference) {
	case VideoCodecPreference::H264:
		preferred = h264;
		break;
	case VideoCodecPreference::VP8:
		preferred = vp8;
		break;
	case VideoCodecPreference::VP9:
		preferred = vp9;
		break;
	}
	if (preferred)
		return preferred;
	if (h264)
		return h264;
	return vp8 ? vp8 : vp9;
}

std::optional<SdpCodec> chooseAudioCodec(const SdpMediaSection &section)
{
	for (const SdpCodec &codec : section.codecs) {
		if (codec.name == "OPUS")
			return codec;
	}
	return std::nullopt;
}

} // namespace qc::vdo
