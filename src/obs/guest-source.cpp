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

#include "guest-source.hpp"

#include "scene-integration.hpp"
#include "session-controller.hpp"

#include <obs-module.h>

#include <algorithm>
#include <string>

namespace qc {

namespace {

/**
 * An OBS source that shows one guest. It holds no media logic of its own: it just tells the controller which guest
 * it is bound to, and the guest's player pushes decoded frames and audio into it.
 */
struct GuestSource {
	obs_source_t *source = nullptr;
	std::string guestId;
	/** Whether this source currently holds a quality override for its guest. */
	bool hasQualityOverride = false;
};

constexpr const char *kQualitySetting = "quality_kbps";
constexpr const char *kOfflineModeSetting = "offline_mode";
constexpr const char *kOfflineImageSetting = "offline_image";

void bind(GuestSource *self, const std::string &guestId)
{
	if (self->guestId == guestId)
		return;
	SessionController *controller = SessionController::instance();
	if (controller && !self->guestId.empty()) {
		// The override belongs to the pairing of this source and that guest; it goes when the pairing does.
		if (self->hasQualityOverride)
			controller->setGuestBitrateOverride(self->guestId, 0);
		self->hasQualityOverride = false;
		controller->detachSource(self->guestId, self->source);
		obs_source_output_video(self->source, nullptr);
	}
	self->guestId = guestId;
	if (controller && !guestId.empty())
		controller->attachSource(guestId, self->source);
}

const char *sourceName(void *)
{
	return obs_module_text("Source.TypeName");
}

void sourceUpdate(void *data, obs_data_t *settings)
{
	auto *self = static_cast<GuestSource *>(data);
	const char *guestId = obs_data_get_string(settings, kGuestIdSetting);
	bind(self, guestId ? guestId : "");

	SessionController *controller = SessionController::instance();
	if (!controller || self->guestId.empty())
		return;

	const auto kbps =
		static_cast<uint32_t>(std::clamp<long long>(obs_data_get_int(settings, kQualitySetting), 0, 20000));
	if (kbps != 0 || self->hasQualityOverride)
		controller->setGuestBitrateOverride(self->guestId, kbps);
	self->hasQualityOverride = kbps != 0;

	const long long mode = obs_data_get_int(settings, kOfflineModeSetting);
	const char *image = obs_data_get_string(settings, kOfflineImageSetting);
	controller->setSourceOfflineAppearance(self->guestId, self->source,
					       mode == 1   ? OfflineMode::LastFrame
					       : mode == 2 ? OfflineMode::Image
							   : OfflineMode::Nothing,
					       image ? image : "");
}

void sourceDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kQualitySetting, 0);
	obs_data_set_default_int(settings, kOfflineModeSetting, 0);
}

bool offlineModeChanged(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	// The image picker only matters for the one mode that uses it.
	obs_property_set_visible(obs_properties_get(properties, kOfflineImageSetting),
				 obs_data_get_int(settings, kOfflineModeSetting) == 2);
	return true;
}

void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
	auto *self = new GuestSource();
	self->source = source;
	sourceUpdate(self, settings);
	return self;
}

void sourceDestroy(void *data)
{
	auto *self = static_cast<GuestSource *>(data);
	bind(self, {});
	delete self;
}

obs_properties_t *sourceProperties(void *)
{
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(properties, kGuestIdSetting, obs_module_text("Source.Guest"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Source.Guest.None"), "");
	if (SessionController *controller = SessionController::instance()) {
		for (const GuestSeat &seat : controller->seatsSnapshot())
			obs_property_list_add_string(list, seat.name.c_str(), seat.id.c_str());
	}
	obs_properties_add_text(properties, "hint", obs_module_text("Source.Hint"), OBS_TEXT_INFO);

	obs_property_t *quality = obs_properties_add_list(properties, kQualitySetting,
							  obs_module_text("Source.Quality"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(quality, obs_module_text("Source.Quality.Default"), 0);
	obs_property_list_add_int(quality, obs_module_text("Settings.Quality.Low"), 1500);
	obs_property_list_add_int(quality, obs_module_text("Settings.Quality.Standard"), 2500);
	obs_property_list_add_int(quality, obs_module_text("Settings.Quality.High"), 4000);
	obs_property_list_add_int(quality, obs_module_text("Settings.Quality.VeryHigh"), 8000);
	obs_property_set_long_description(quality, obs_module_text("Source.Quality.Tooltip"));

	obs_property_t *offline = obs_properties_add_list(properties, kOfflineModeSetting,
							  obs_module_text("Source.Offline"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(offline, obs_module_text("Source.Offline.Nothing"), 0);
	obs_property_list_add_int(offline, obs_module_text("Source.Offline.LastFrame"), 1);
	obs_property_list_add_int(offline, obs_module_text("Source.Offline.Image"), 2);
	obs_property_set_long_description(offline, obs_module_text("Source.Offline.Tooltip"));
	obs_property_set_modified_callback(offline, offlineModeChanged);

	obs_properties_add_path(properties, kOfflineImageSetting, obs_module_text("Source.Offline.ImageFile"),
				OBS_PATH_FILE, obs_module_text("Source.Offline.ImageFilter"), nullptr);
	return properties;
}

} // namespace

void registerGuestSource()
{
	obs_source_info info = {};
	info.id = kGuestSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = sourceName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.update = sourceUpdate;
	info.get_properties = sourceProperties;
	info.get_defaults = sourceDefaults;
	obs_register_source(&info);
}

} // namespace qc
