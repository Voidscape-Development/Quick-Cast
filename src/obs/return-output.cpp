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

#include "return-output.hpp"

#include <obs-module.h>

#include <atomic>
#include <string>
#include <vector>

namespace qc {

namespace {

constexpr const char *kOutputId = "quick_cast_return_output";
/** Video-only variant used by the second feed (libobs fixes an output's media kinds per type). */
constexpr const char *kVideoOnlyOutputId = "quick_cast_return_output_video";

struct ReturnOutput {
	obs_output_t *output = nullptr;
	std::atomic<GuestService *> service{nullptr};
	ReturnFeedId feed = ReturnFeedId::Main;
	std::vector<uint8_t> scratch;
};

int64_t packetTimeUs(const encoder_packet *packet)
{
	if (packet->timebase_den == 0)
		return 0;
	return packet->pts * 1'000'000LL * packet->timebase_num / packet->timebase_den;
}

/** True when the Annex-B buffer contains a sequence parameter set. */
bool containsSps(const uint8_t *data, size_t size)
{
	for (size_t i = 0; i + 3 < size; ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			if ((data[i + 3] & 0x1F) == 7)
				return true;
			i += 2;
		}
	}
	return false;
}

const char *outputName(void *)
{
	return obs_module_text("ReturnFeed.OutputName");
}

void *outputCreate(obs_data_t *, obs_output_t *output)
{
	auto *state = new ReturnOutput();
	state->output = output;
	return state;
}

void outputDestroy(void *data)
{
	delete static_cast<ReturnOutput *>(data);
}

bool outputStart(void *data)
{
	auto *state = static_cast<ReturnOutput *>(data);
	if (!obs_output_can_begin_data_capture(state->output, 0))
		return false;
	if (!obs_output_initialize_encoders(state->output, 0))
		return false;
	return obs_output_begin_data_capture(state->output, 0);
}

void outputStop(void *data, uint64_t)
{
	auto *state = static_cast<ReturnOutput *>(data);
	obs_output_end_data_capture(state->output);
}

void outputEncodedPacket(void *data, encoder_packet *packet)
{
	auto *state = static_cast<ReturnOutput *>(data);
	GuestService *service = state->service.load();
	if (!service || !packet || !packet->data || packet->size == 0)
		return;

	const int64_t timestampUs = packetTimeUs(packet);

	if (packet->type == OBS_ENCODER_AUDIO) {
		service->sendReturnAudio(packet->data, packet->size, timestampUs);
		return;
	}
	if (packet->type != OBS_ENCODER_VIDEO)
		return;

	// A viewer that joins mid-stream needs the parameter sets with the keyframe. Not every encoder repeats
	// them in-band, so add them from the encoder's extra data when they are missing.
	if (packet->keyframe && !containsSps(packet->data, packet->size)) {
		uint8_t *extra = nullptr;
		size_t extraSize = 0;
		obs_encoder_t *encoder = obs_output_get_video_encoder(state->output);
		if (encoder && obs_encoder_get_extra_data(encoder, &extra, &extraSize) && extra && extraSize > 0) {
			state->scratch.assign(extra, extra + extraSize);
			state->scratch.insert(state->scratch.end(), packet->data, packet->data + packet->size);
			service->sendReturnVideo(state->feed, state->scratch.data(), state->scratch.size(), timestampUs,
						 true);
			return;
		}
	}
	service->sendReturnVideo(state->feed, packet->data, packet->size, timestampUs, packet->keyframe);
}

uint32_t evenSize(double value)
{
	const auto rounded = static_cast<uint32_t>(value + 0.5);
	return rounded < 2 ? 2 : (rounded & ~1u);
}

} // namespace

void registerReturnOutput()
{
	obs_output_info info = {};
	info.id = kOutputId;
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	info.encoded_video_codecs = "h264";
	info.encoded_audio_codecs = "opus";
	info.get_name = outputName;
	info.create = outputCreate;
	info.destroy = outputDestroy;
	info.start = outputStart;
	info.stop = outputStop;
	info.encoded_packet = outputEncodedPacket;
	obs_register_output(&info);

	obs_output_info videoOnly = info;
	videoOnly.id = kVideoOnlyOutputId;
	videoOnly.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	videoOnly.encoded_audio_codecs = nullptr;
	obs_register_output(&videoOnly);
}

ReturnFeed::ReturnFeed(GuestService *service, ReturnFeedId feed) : service_(service), feed_(feed) {}

bool ReturnFeed::enabled() const
{
	if (!settings_.returnFeedEnabled)
		return false;
	return feed_ == ReturnFeedId::Main || settings_.secondFeedEnabled;
}

uint32_t ReturnFeed::targetHeight() const
{
	return feed_ == ReturnFeedId::Main ? settings_.returnHeight : settings_.secondHeight;
}

uint32_t ReturnFeed::videoBitrateKbps() const
{
	return feed_ == ReturnFeedId::Main ? settings_.returnVideoBitrateKbps : settings_.secondVideoBitrateKbps;
}

const std::string &ReturnFeed::sourceName() const
{
	return feed_ == ReturnFeedId::Main ? settings_.returnVideoSource : settings_.secondVideoSource;
}

ReturnFeed::~ReturnFeed()
{
	release();
}

void ReturnFeed::release()
{
	if (output_) {
		if (auto *state = static_cast<ReturnOutput *>(obs_obj_get_data(output_)))
			state->service = nullptr;
		obs_output_force_stop(output_);
	}
	output_ = nullptr;
	videoEncoder_ = nullptr;
	audioEncoder_ = nullptr;
	// The view's video output and the tapped audio output must outlive the encoders that read from them.
	releaseView();
	audioTap_.close();
}

void ReturnFeed::releaseView()
{
	if (!view_)
		return;
	obs_view_remove(view_);
	obs_view_set_source(view_, 0, nullptr);
	obs_view_destroy(view_);
	view_ = nullptr;
}

bool ReturnFeed::active() const
{
	return output_ && obs_output_active(output_);
}

void ReturnFeed::configure(const Settings &settings)
{
	settings_ = settings;
	const bool restart = wanted_;
	// Encoder parameters cannot change while encoding; rebuild from scratch.
	release();
	if (restart && enabled())
		start();
}

bool ReturnFeed::prepareSourceView(const obs_video_info &mainVideo, video_t *&videoOutput, uint32_t &width,
				   uint32_t &height)
{
	const std::string &name = sourceName();
	if (name.empty())
		return false;

	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (!source) {
		blog(LOG_WARNING, "[quick-cast] return feed source '%s' not found; sending the program instead",
		     name.c_str());
		return false;
	}

	// A camera that has not delivered a frame yet reports no size; use the canvas until it does.
	uint32_t sourceWidth = obs_source_get_width(source);
	uint32_t sourceHeight = obs_source_get_height(source);
	if (sourceWidth == 0 || sourceHeight == 0) {
		sourceWidth = mainVideo.base_width;
		sourceHeight = mainVideo.base_height;
	}

	obs_video_info viewVideo = mainVideo;
	viewVideo.base_width = sourceWidth;
	viewVideo.base_height = sourceHeight;
	viewVideo.output_width = evenSize(sourceWidth);
	viewVideo.output_height = evenSize(sourceHeight);
	if (targetHeight() > 0 && targetHeight() < sourceHeight) {
		viewVideo.output_width = evenSize(static_cast<double>(sourceWidth) * targetHeight() / sourceHeight);
		viewVideo.output_height = evenSize(targetHeight());
	}

	view_ = obs_view_create();
	obs_view_set_source(view_, 0, source);
	video_t *viewOutput = obs_view_add2(view_, &viewVideo);
	if (!viewOutput) {
		blog(LOG_WARNING, "[quick-cast] could not render '%s' for the return feed; sending the program instead",
		     name.c_str());
		releaseView();
		return false;
	}

	videoOutput = viewOutput;
	width = viewVideo.output_width;
	height = viewVideo.output_height;
	return true;
}

bool ReturnFeed::createObjects()
{
	obs_video_info video = {};
	if (!obs_get_video_info(&video) || video.output_height == 0)
		return false;

	// What guests see: the program by default, or one chosen source (usually the host's camera) rendered on its
	// own so the host can show their face to a co-streamer whatever is on the program.
	video_t *videoOutput = obs_get_video();
	uint32_t nativeWidth = video.output_width;
	uint32_t nativeHeight = video.output_height;
	uint32_t width = nativeWidth;
	uint32_t height = nativeHeight;
	const bool customSource = prepareSourceView(video, videoOutput, width, height);
	if (!customSource && targetHeight() > 0 && targetHeight() < height) {
		width = evenSize(static_cast<double>(width) * targetHeight() / height);
		height = evenSize(targetHeight());
	}
	const uint32_t fps = video.fps_den > 0 ? (video.fps_num + video.fps_den / 2) / video.fps_den : 30;

	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", videoBitrateKbps());
	// libobs cannot force a keyframe on demand, so a short interval is what bounds how long a newly joined
	// guest (or one recovering from packet loss) waits for a picture.
	obs_data_set_int(videoSettings, "keyint_sec", 1);
	obs_data_set_string(videoSettings, "preset", "veryfast");
	obs_data_set_string(videoSettings, "profile", "baseline");
	obs_data_set_string(videoSettings, "tune", "zerolatency");
	obs_data_set_int(videoSettings, "bf", 0);
	obs_data_set_bool(videoSettings, "repeat_headers", true);
	const bool main = feed_ == ReturnFeedId::Main;
	videoEncoder_ = obs_video_encoder_create(
		"obs_x264", main ? "Quick Cast Return Video" : "Quick Cast Second Video", videoSettings, nullptr);
	if (!videoEncoder_) {
		blog(LOG_ERROR, "[quick-cast] could not create the return feed video encoder (x264)");
		return false;
	}
	obs_encoder_set_video(videoEncoder_, videoOutput);
	// A source view already renders at the return feed's size; only the program needs scaling by the encoder.
	if (!customSource && (width != nativeWidth || height != nativeHeight))
		obs_encoder_set_scaled_size(videoEncoder_, width, height);

	if (main) {
		OBSDataAutoRelease audioSettings = obs_data_create();
		obs_data_set_int(audioSettings, "bitrate", settings_.returnAudioBitrateKbps);
		// Guests hear either one of OBS' mix tracks or a single chosen source on its own.
		audio_t *audioSource = obs_get_audio();
		size_t mixer = static_cast<size_t>(settings_.returnAudioTrack - 1);
		if (!settings_.returnAudioSource.empty()) {
			if (audioTap_.open(settings_.returnAudioSource)) {
				audioSource = audioTap_.audio();
				mixer = 0;
			} else {
				blog(LOG_WARNING,
				     "[quick-cast] return audio source '%s' not found; sending audio track %d instead",
				     settings_.returnAudioSource.c_str(), settings_.returnAudioTrack);
			}
		}
		audioEncoder_ = obs_audio_encoder_create("ffmpeg_opus", "Quick Cast Return Audio", audioSettings, mixer,
							 nullptr);
		if (!audioEncoder_) {
			blog(LOG_ERROR, "[quick-cast] could not create the return feed audio encoder (Opus)");
			videoEncoder_ = nullptr;
			releaseView();
			return false;
		}
		obs_encoder_set_audio(audioEncoder_, audioSource);
	}

	output_ = obs_output_create(main ? kOutputId : kVideoOnlyOutputId,
				    main ? "Quick Cast Return Feed" : "Quick Cast Second Feed", nullptr, nullptr);
	if (!output_) {
		videoEncoder_ = nullptr;
		audioEncoder_ = nullptr;
		releaseView();
		return false;
	}
	if (auto *state = static_cast<ReturnOutput *>(obs_obj_get_data(output_))) {
		state->feed = feed_;
		state->service = service_;
	}
	obs_output_set_video_encoder(output_, videoEncoder_);
	if (audioEncoder_)
		obs_output_set_audio_encoder(output_, audioEncoder_, 0);

	ReturnFeedFormat format;
	format.width = width;
	format.height = height;
	format.framesPerSecond = fps;
	format.videoBitrateKbps = videoBitrateKbps();
	format.audioBitrateKbps = settings_.returnAudioBitrateKbps;
	service_->setReturnFeedFormat(feed_, format);
	return true;
}

void ReturnFeed::start()
{
	wanted_ = true;
	if (!enabled() || active())
		return;
	if (!output_ && !createObjects())
		return;
	if (!obs_output_start(output_)) {
		const char *error = obs_output_get_last_error(output_);
		blog(LOG_ERROR, "[quick-cast] could not start the return feed: %s", error ? error : "unknown error");
		return;
	}
	if (feed_ == ReturnFeedId::Main)
		blog(LOG_INFO, "[quick-cast] return feed started (video: %s, audio: %s)",
		     view_ ? sourceName().c_str() : "program",
		     audioTap_.audio() ? settings_.returnAudioSource.c_str()
				       : ("track " + std::to_string(settings_.returnAudioTrack)).c_str());
	else
		blog(LOG_INFO, "[quick-cast] second feed started (video: %s)",
		     view_ ? sourceName().c_str() : "program");
}

void ReturnFeed::stop()
{
	wanted_ = false;
	if (!output_)
		return;
	if (obs_output_active(output_)) {
		obs_output_stop(output_);
		blog(LOG_INFO, "[quick-cast] %s feed stopped", feed_ == ReturnFeedId::Main ? "return" : "second");
	}
}

} // namespace qc
