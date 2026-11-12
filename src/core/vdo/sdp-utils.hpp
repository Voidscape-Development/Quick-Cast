#pragma once

#include <optional>
#include <string>
#include <vector>

namespace qc::vdo {

struct SdpCodec {
	int payloadType = -1;
	std::string name; // upper-case encoding name, e.g. "H264", "VP8", "OPUS"
	int clockRate = 0;
	int channels = 0;
	std::string fmtp; // raw format parameters, may be empty
};

struct SdpMediaSection {
	std::string kind; // "audio", "video", "application"
	std::string mid;
	/** Whether the remote side will send media on this section. */
	bool remoteSends = false;
	std::vector<SdpCodec> codecs; // in the offerer's preference order
};

/** Extracts the media sections of an SDP offer. Tolerant of unknown lines; never throws. */
std::vector<SdpMediaSection> parseSdpMedia(const std::string &sdp);

enum class VideoCodecPreference { H264, VP8 };

/**
 * Picks the video codec to accept from an offered section: the preferred codec when offered, otherwise the other
 * supported one. For H.264 only packetization-mode=1 variants are considered, favouring constrained baseline since
 * every browser can encode it.
 */
std::optional<SdpCodec> chooseVideoCodec(const SdpMediaSection &section, VideoCodecPreference preference);

std::optional<SdpCodec> chooseAudioCodec(const SdpMediaSection &section);

/** Value of one `key=value` entry in a semicolon separated fmtp string, lower-cased. */
std::string fmtpValue(const std::string &fmtp, const std::string &key);

} // namespace qc::vdo
