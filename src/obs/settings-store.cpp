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

#include "settings-store.hpp"

#include "vdo/identity.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>

namespace qc {

namespace {

constexpr const char *kSettingsFile = "settings.json";

std::string configPath(const char *file)
{
	char *path = obs_module_config_path(file);
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

std::string getString(obs_data_t *data, const char *key, const std::string &fallback = {})
{
	const char *value = obs_data_get_string(data, key);
	return value && *value ? std::string(value) : fallback;
}

VideoCodec codecFromName(const std::string &name)
{
	if (name == "vp8")
		return VideoCodec::VP8;
	if (name == "vp9")
		return VideoCodec::VP9;
	return VideoCodec::H264;
}

const char *codecName(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::VP8:
		return "vp8";
	case VideoCodec::VP9:
		return "vp9";
	case VideoCodec::H264:
		break;
	}
	return "h264";
}

} // namespace

const GuestSeat *Settings::findSeat(const std::string &id) const
{
	auto it = std::find_if(seats.begin(), seats.end(), [&](const GuestSeat &seat) { return seat.id == id; });
	return it != seats.end() ? &*it : nullptr;
}

GuestSeat *Settings::findSeat(const std::string &id)
{
	auto it = std::find_if(seats.begin(), seats.end(), [&](const GuestSeat &seat) { return seat.id == id; });
	return it != seats.end() ? &*it : nullptr;
}

ShowConfig Settings::toShowConfig() const
{
	ShowConfig config;
	config.room = room;
	config.password = password;
	config.hostName = hostName.empty() ? "Host" : hostName;
	config.seats = seats;
	config.defaultQuality.maxVideoBitrateKbps = guestBitrateKbps;
	config.defaultQuality.preferredCodec = preferredCodec;
	config.returnFeedEnabled = returnFeedEnabled;
	config.secondFeedEnabled = returnFeedEnabled && secondFeedEnabled;
	if (!turnUrl.empty())
		config.iceServers.push_back(IceServer{turnUrl, turnUsername, turnPassword});
	return config;
}

Settings loadSettings()
{
	Settings settings;

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(configPath(kSettingsFile).c_str(), "bak");
	if (data) {
		obs_data_set_default_bool(data, "fit_to_canvas", settings.fitToCanvas);
		obs_data_set_default_bool(data, "hardware_decoding", settings.hardwareDecoding);
		obs_data_set_default_int(data, "guest_bitrate_kbps", settings.guestBitrateKbps);
		obs_data_set_default_bool(data, "return_feed_enabled", settings.returnFeedEnabled);
		obs_data_set_default_int(data, "return_audio_track", settings.returnAudioTrack);
		obs_data_set_default_int(data, "return_height", settings.returnHeight);
		obs_data_set_default_int(data, "return_video_bitrate_kbps", settings.returnVideoBitrateKbps);
		obs_data_set_default_int(data, "return_audio_bitrate_kbps", settings.returnAudioBitrateKbps);

		settings.room = getString(data, "room");
		settings.password = getString(data, "password");
		settings.hostName = getString(data, "host_name", "Host");
		settings.startWithObs = obs_data_get_bool(data, "start_with_obs");
		settings.dockIntroduced = obs_data_get_bool(data, "dock_introduced");
		settings.fitToCanvas = obs_data_get_bool(data, "fit_to_canvas");
		settings.guestBitrateKbps = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "guest_bitrate_kbps"), 300, 20000));
		settings.hardwareDecoding = obs_data_get_bool(data, "hardware_decoding");
		settings.preferredCodec = codecFromName(getString(data, "preferred_codec"));
		settings.returnFeedEnabled = obs_data_get_bool(data, "return_feed_enabled");
		settings.returnAudioTrack =
			static_cast<int>(std::clamp<long long>(obs_data_get_int(data, "return_audio_track"), 1, 6));
		settings.returnVideoSource = getString(data, "return_video_source");
		settings.secondFeedEnabled = obs_data_get_bool(data, "second_feed_enabled");
		settings.secondVideoSource = getString(data, "second_video_source");
		obs_data_set_default_int(data, "second_height", settings.secondHeight);
		obs_data_set_default_int(data, "second_video_bitrate_kbps", settings.secondVideoBitrateKbps);
		settings.secondHeight = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "second_height"), 180, 1080));
		settings.secondVideoBitrateKbps = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "second_video_bitrate_kbps"), 200, 8000));
		settings.returnHeight = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "return_height"), 180, 1080));
		settings.returnVideoBitrateKbps = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "return_video_bitrate_kbps"), 200, 8000));
		settings.returnAudioBitrateKbps = static_cast<uint32_t>(
			std::clamp<long long>(obs_data_get_int(data, "return_audio_bitrate_kbps"), 32, 256));
		settings.turnUrl = getString(data, "turn_url");
		settings.turnUsername = getString(data, "turn_username");
		settings.turnPassword = getString(data, "turn_password");

		OBSDataArrayAutoRelease seats = obs_data_get_array(data, "seats");
		const size_t count = seats ? obs_data_array_count(seats) : 0;
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(seats, i);
			GuestSeat seat;
			seat.id = getString(item, "id");
			seat.name = getString(item, "name", "Guest");
			if (!seat.id.empty() && !settings.findSeat(seat.id))
				settings.seats.push_back(seat);
		}
	}

	// First run: give the show its own private room so links work without any setup.
	bool generated = false;
	if (settings.room.empty()) {
		settings.room = vdo::generateRoomName();
		generated = true;
	}
	if (settings.password.empty()) {
		settings.password = vdo::generatePassword();
		generated = true;
	}
	if (generated)
		saveSettings(settings);
	return settings;
}

void saveSettings(const Settings &settings)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "room", settings.room.c_str());
	obs_data_set_string(data, "password", settings.password.c_str());
	obs_data_set_string(data, "host_name", settings.hostName.c_str());
	obs_data_set_bool(data, "start_with_obs", settings.startWithObs);
	obs_data_set_bool(data, "dock_introduced", settings.dockIntroduced);
	obs_data_set_bool(data, "fit_to_canvas", settings.fitToCanvas);
	obs_data_set_int(data, "guest_bitrate_kbps", settings.guestBitrateKbps);
	obs_data_set_string(data, "preferred_codec", codecName(settings.preferredCodec));
	obs_data_set_bool(data, "hardware_decoding", settings.hardwareDecoding);
	obs_data_set_bool(data, "return_feed_enabled", settings.returnFeedEnabled);
	obs_data_set_int(data, "return_audio_track", settings.returnAudioTrack);
	obs_data_set_string(data, "return_video_source", settings.returnVideoSource.c_str());
	obs_data_set_bool(data, "second_feed_enabled", settings.secondFeedEnabled);
	obs_data_set_string(data, "second_video_source", settings.secondVideoSource.c_str());
	obs_data_set_int(data, "second_height", settings.secondHeight);
	obs_data_set_int(data, "second_video_bitrate_kbps", settings.secondVideoBitrateKbps);
	obs_data_set_int(data, "return_height", settings.returnHeight);
	obs_data_set_int(data, "return_video_bitrate_kbps", settings.returnVideoBitrateKbps);
	obs_data_set_int(data, "return_audio_bitrate_kbps", settings.returnAudioBitrateKbps);
	obs_data_set_string(data, "turn_url", settings.turnUrl.c_str());
	obs_data_set_string(data, "turn_username", settings.turnUsername.c_str());
	obs_data_set_string(data, "turn_password", settings.turnPassword.c_str());

	OBSDataArrayAutoRelease seats = obs_data_array_create();
	for (const GuestSeat &seat : settings.seats) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "id", seat.id.c_str());
		obs_data_set_string(item, "name", seat.name.c_str());
		obs_data_array_push_back(seats, item);
	}
	obs_data_set_array(data, "seats", seats);

	const std::string path = configPath(kSettingsFile);
	if (path.empty())
		return;
	const size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos)
		os_mkdirs(path.substr(0, slash).c_str());
	obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
}

} // namespace qc
