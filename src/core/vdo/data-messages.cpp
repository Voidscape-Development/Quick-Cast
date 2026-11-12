#include "data-messages.hpp"

namespace qc::vdo {

namespace {

/** Advertised to peers so they can tell which client they are talking to. */
constexpr const char *kClientVersion = "quick-cast";

std::optional<bool> asBool(const Json &object, const char *key)
{
	auto it = object.find(key);
	if (it == object.end() || !it->is_boolean())
		return std::nullopt;
	return it->get<bool>();
}

std::optional<std::string> labelFrom(const Json &value)
{
	if (value.is_string())
		return value.get<std::string>();
	if (value.is_null() || (value.is_boolean() && !value.get<bool>()))
		return std::string();
	return std::nullopt;
}

} // namespace

PeerDataMessage parsePeerDataMessage(const Json &message)
{
	PeerDataMessage result;
	if (!message.is_object())
		return result;

	result.carriesSignaling = message.contains("description") || message.contains("candidate") ||
				  message.contains("candidates");

	if (auto it = message.find("ping"); it != message.end())
		result.pingToken = *it;

	// On a data channel `bye` is acted on by presence; the official client sends e.g. {"videoMuted":true,"bye":true}.
	result.bye = message.contains("bye");
	result.iceRestartRequested = message.contains("iceRestartRequest");
	result.keyframeRequested = message.contains("keyframe") || message.contains("requestKeyframe");

	if (auto info = message.find("info"); info != message.end() && info->is_object()) {
		if (auto label = info->find("label"); label != info->end())
			result.label = labelFrom(*label);
		if (auto muted = asBool(*info, "muted"))
			result.microphoneMuted = muted;
		if (auto videoMuted = asBool(*info, "video_muted_init"))
			result.cameraMuted = videoMuted;
	}

	if (message.contains("changeLabel")) {
		if (auto value = message.find("value"); value != message.end())
			result.label = labelFrom(*value);
	}

	if (auto muted = asBool(message, "muteState"))
		result.microphoneMuted = muted;
	if (auto muted = asBool(message, "videoMuted"))
		result.cameraMuted = muted;

	if (auto audio = asBool(message, "audio"))
		result.wantsAudio = audio;
	if (auto video = asBool(message, "video"))
		result.wantsVideo = video;

	if (auto it = message.find("bitrate"); it != message.end() && it->is_number())
		result.videoBitrateKbps = static_cast<int>(it->get<double>());

	if (auto it = message.find("rejected"); it != message.end() && it->is_string())
		result.rejected = it->get<std::string>();

	return result;
}

Json buildViewerHello(const std::string &hostLabel, bool director)
{
	// `guest:false` matters: publishers cap what they send to peers that identify as fellow room guests.
	// `allowchunked` / `allowmeshcast` false keep the media on plain peer-to-peer RTP, the only transport we speak.
	return {{"audio", true},
		{"video", true},
		{"allowscreenaudio", true},
		{"allowscreenvideo", true},
		{"broadcast", false},
		{"iframe", false},
		{"widget", false},
		{"downloads", false},
		{"allowmidi", false},
		{"allowdrawing", false},
		{"allowwebp", false},
		{"allowchunked", false},
		{"allowresources", false},
		{"allowmeshcast", false},
		{"guest", false},
		{"scene", false},
		{"director", director},
		{"limitaudio", false},
		{"forceios", false},
		{"degrade", "maintain-resolution"},
		{"info", {{"label", hostLabel}, {"version", kClientVersion}, {"platform", "OBS"}}}};
}

Json buildBitrateRequest(uint32_t kbps)
{
	// `targetBitrate` raises the publisher's ceiling, `bitrate` applies the value.
	return {{"targetBitrate", kbps}, {"bitrate", kbps}};
}

Json buildResolutionRequest(uint32_t width, uint32_t height)
{
	return {{"requestResolution", {{"w", width}, {"h", height}}}};
}

Json buildPublisherInfo(const std::string &hostLabel, uint32_t width, uint32_t height, uint32_t framesPerSecond)
{
	Json info = {{"label", hostLabel},  {"version", kClientVersion}, {"muted", false},
		     {"video_muted_init", false}, {"room_init", true},       {"codec_url", "h264"},
		     {"audio_codec_url", "opus"}, {"stereo_url", true},      {"remote", false},
		     {"obs_control", false}};
	if (width > 0 && height > 0) {
		info["video_init_width"] = width;
		info["video_init_height"] = height;
	}
	if (framesPerSecond > 0)
		info["video_init_frameRate"] = framesPerSecond;
	return {{"info", std::move(info)}};
}

Json buildPong(const Json &token)
{
	return {{"pong", token}};
}

Json buildBye()
{
	return {{"bye", true}};
}

Json buildHangup()
{
	return {{"hangup", true}};
}

Json buildChangeLabel(const std::string &label)
{
	return {{"changeLabel", true}, {"value", label}};
}

} // namespace qc::vdo
