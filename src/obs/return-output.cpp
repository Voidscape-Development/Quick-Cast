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
#include <vector>

namespace qc {

namespace {

constexpr const char *kOutputId = "quick_cast_return_output";

struct ReturnOutput {
	obs_output_t *output = nullptr;
	std::atomic<GuestService *> service{nullptr};
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
			service->sendReturnVideo(state->scratch.data(), state->scratch.size(), timestampUs, true);
			return;
		}
	}
	service->sendReturnVideo(packet->data, packet->size, timestampUs, packet->keyframe);
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
}

ReturnFeed::ReturnFeed(GuestService *service) : service_(service) {}

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
	if (restart && settings_.returnFeedEnabled)
		start();
}

bool ReturnFeed::createObjects()
{
	obs_video_info video = {};
	if (!obs_get_video_info(&video) || video.output_height == 0)
		return false;

	uint32_t width = video.output_width;
	uint32_t height = video.output_height;
	if (settings_.returnHeight > 0 && settings_.returnHeight < height) {
		width = evenSize(static_cast<double>(width) * settings_.returnHeight / height);
		height = evenSize(settings_.returnHeight);
	}
	const uint32_t fps = video.fps_den > 0 ? (video.fps_num + video.fps_den / 2) / video.fps_den : 30;

	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", settings_.returnVideoBitrateKbps);
	// libobs cannot force a keyframe on demand, so a short interval is what bounds how long a newly joined
	// guest (or one recovering from packet loss) waits for a picture.
	obs_data_set_int(videoSettings, "keyint_sec", 1);
	obs_data_set_string(videoSettings, "preset", "veryfast");
	obs_data_set_string(videoSettings, "profile", "baseline");
	obs_data_set_string(videoSettings, "tune", "zerolatency");
	obs_data_set_int(videoSettings, "bf", 0);
	obs_data_set_bool(videoSettings, "repeat_headers", true);
	videoEncoder_ = obs_video_encoder_create("obs_x264", "Quick Cast Return Video", videoSettings, nullptr);
	if (!videoEncoder_) {
		blog(LOG_ERROR, "[quick-cast] could not create the return feed video encoder (x264)");
		return false;
	}
	obs_encoder_set_video(videoEncoder_, obs_get_video());
	if (width != video.output_width || height != video.output_height)
		obs_encoder_set_scaled_size(videoEncoder_, width, height);

	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", settings_.returnAudioBitrateKbps);
	const size_t mixer = static_cast<size_t>(settings_.returnAudioTrack - 1);
	audioEncoder_ =
		obs_audio_encoder_create("ffmpeg_opus", "Quick Cast Return Audio", audioSettings, mixer, nullptr);
	if (!audioEncoder_) {
		blog(LOG_ERROR, "[quick-cast] could not create the return feed audio encoder (Opus)");
		videoEncoder_ = nullptr;
		return false;
	}
	obs_encoder_set_audio(audioEncoder_, obs_get_audio());

	output_ = obs_output_create(kOutputId, "Quick Cast Return Feed", nullptr, nullptr);
	if (!output_) {
		videoEncoder_ = nullptr;
		audioEncoder_ = nullptr;
		return false;
	}
	if (auto *state = static_cast<ReturnOutput *>(obs_obj_get_data(output_)))
		state->service = service_;
	obs_output_set_video_encoder(output_, videoEncoder_);
	obs_output_set_audio_encoder(output_, audioEncoder_, 0);

	ReturnFeedFormat format;
	format.width = width;
	format.height = height;
	format.framesPerSecond = fps;
	format.videoBitrateKbps = settings_.returnVideoBitrateKbps;
	format.audioBitrateKbps = settings_.returnAudioBitrateKbps;
	service_->setReturnFeedFormat(format);
	return true;
}

void ReturnFeed::start()
{
	wanted_ = true;
	if (!settings_.returnFeedEnabled || active())
		return;
	if (!output_ && !createObjects())
		return;
	if (!obs_output_start(output_)) {
		const char *error = obs_output_get_last_error(output_);
		blog(LOG_ERROR, "[quick-cast] could not start the return feed: %s", error ? error : "unknown error");
		return;
	}
	blog(LOG_INFO, "[quick-cast] return feed started (audio track %d)", settings_.returnAudioTrack);
}

void ReturnFeed::stop()
{
	wanted_ = false;
	if (!output_)
		return;
	if (obs_output_active(output_)) {
		obs_output_stop(output_);
		blog(LOG_INFO, "[quick-cast] return feed stopped");
	}
}

} // namespace qc
