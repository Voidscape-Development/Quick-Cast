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

#include "scene-integration.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>
#include <cstring>

#include <vector>

namespace qc {

namespace {

/** The scene the user is editing: the preview scene in studio mode, otherwise the program scene. */
OBSSourceAutoRelease currentSceneSource()
{
	if (obs_frontend_preview_program_mode_active()) {
		if (obs_source_t *preview = obs_frontend_get_current_preview_scene())
			return OBSSourceAutoRelease(preview);
	}
	return OBSSourceAutoRelease(obs_frontend_get_current_scene());
}

struct FindContext {
	const std::string *guestId;
	obs_source_t *found;
};

struct ItemSearch {
	obs_source_t *source;
	std::vector<obs_sceneitem_t *> items;
};

bool collectItems(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<ItemSearch *>(param);
	if (obs_sceneitem_get_source(item) == search->source)
		search->items.push_back(item);
	return true;
}

} // namespace

std::string guestSourceName(const std::string &guestName)
{
	std::string name = obs_module_text("Source.NamePrefix");
	name += guestName.empty() ? "Guest" : guestName;
	return name;
}

OBSSourceAutoRelease findGuestSource(const std::string &guestId)
{
	FindContext context{&guestId, nullptr};
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *ctx = static_cast<FindContext *>(param);
			const char *id = obs_source_get_unversioned_id(source);
			if (!id || strcmp(id, kGuestSourceId) != 0)
				return true;
			OBSDataAutoRelease settings = obs_source_get_settings(source);
			const char *bound = obs_data_get_string(settings, kGuestIdSetting);
			if (bound && *ctx->guestId == bound) {
				ctx->found = obs_source_get_ref(source);
				return false;
			}
			return true;
		},
		&context);
	return OBSSourceAutoRelease(context.found);
}

OBSSourceAutoRelease getOrCreateGuestSource(const std::string &guestId, const std::string &name)
{
	if (OBSSourceAutoRelease existing = findGuestSource(guestId))
		return existing;

	// Source names are unique in OBS.
	std::string unique = name;
	for (int suffix = 2; suffix < 1000; ++suffix) {
		OBSSourceAutoRelease clash = obs_get_source_by_name(unique.c_str());
		if (!clash)
			break;
		unique = name + " " + std::to_string(suffix);
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, kGuestIdSetting, guestId.c_str());
	return OBSSourceAutoRelease(obs_source_create(kGuestSourceId, unique.c_str(), settings, nullptr));
}

bool isInCurrentScene(obs_source_t *source)
{
	OBSSourceAutoRelease sceneSource = currentSceneSource();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene || !source)
		return false;
	ItemSearch search{source, {}};
	obs_scene_enum_items(scene, collectItems, &search);
	return !search.items.empty();
}

bool addToCurrentScene(obs_source_t *source, bool fitToCanvas)
{
	OBSSourceAutoRelease sceneSource = currentSceneSource();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene || !source)
		return false;

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (!item)
		return false;

	if (fitToCanvas) {
		// Bounds keep the guest filling the canvas whatever resolution their camera ends up sending.
		obs_video_info video = {};
		if (obs_get_video_info(&video)) {
			vec2 bounds;
			vec2_set(&bounds, static_cast<float>(video.base_width), static_cast<float>(video.base_height));
			vec2 position;
			vec2_set(&position, 0.0f, 0.0f);
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			obs_sceneitem_set_bounds(item, &bounds);
			obs_sceneitem_set_pos(item, &position);
		}
	}
	return true;
}

size_t arrangeInCurrentScene(const std::vector<obs_source_t *> &sources, LayoutKind kind)
{
	OBSSourceAutoRelease sceneSource = currentSceneSource();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_video_info video = {};
	if (!scene || !obs_get_video_info(&video))
		return 0;

	std::vector<obs_sceneitem_t *> items;
	for (obs_source_t *source : sources) {
		ItemSearch search{source, {}};
		obs_scene_enum_items(scene, collectItems, &search);
		// A locked item is the user saying "leave this where it is".
		if (!search.items.empty() && !obs_sceneitem_locked(search.items.front()))
			items.push_back(search.items.front());
	}

	const std::vector<TileRect> tiles = computeLayout(kind, items.size(), static_cast<float>(video.base_width),
							  static_cast<float>(video.base_height));
	for (size_t i = 0; i < items.size() && i < tiles.size(); ++i) {
		obs_sceneitem_t *item = items[i];
		const TileRect &tile = tiles[i];
		vec2 position;
		vec2_set(&position, tile.x, tile.y);
		vec2 bounds;
		vec2_set(&bounds, tile.width, tile.height);

		// A bounding box rather than a scale, so the tile holds whatever resolution the guest ends up sending.
		// Crop and filters are the user's and stay as they are.
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_rot(item, 0.0f);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_pos(item, &position);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		obs_sceneitem_set_bounds(item, &bounds);
		obs_sceneitem_defer_update_end(item);
		// Small tiles are only useful on top of whatever fills the rest of the scene.
		if (kind == LayoutKind::PictureInPicture)
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
	}
	return std::min(items.size(), tiles.size());
}

namespace {

struct DeviceSearch {
	const char *const *ids;
	std::string firstName;
	std::string inSceneName;
};

/** First source of one of the given types, preferring one that is in the scene being edited. */
std::string findDeviceSource(const char *const *ids)
{
	DeviceSearch search{ids, {}, {}};
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *ctx = static_cast<DeviceSearch *>(param);
			const char *id = obs_source_get_unversioned_id(source);
			const char *name = obs_source_get_name(source);
			if (!id || !name || !*name)
				return true;
			for (const char *const *candidate = ctx->ids; *candidate; ++candidate) {
				if (strcmp(id, *candidate) != 0)
					continue;
				if (ctx->firstName.empty())
					ctx->firstName = name;
				if (ctx->inSceneName.empty() && isInCurrentScene(source))
					ctx->inSceneName = name;
				break;
			}
			return ctx->inSceneName.empty();
		},
		&search);
	return search.inSceneName.empty() ? search.firstName : search.inSceneName;
}

} // namespace

std::string detectCameraSourceName()
{
	static const char *const ids[] = {"dshow_input",
					  "av_capture_input",
					  "macos-avcapture",
					  "macos-avcapture-fast",
					  "v4l2_input",
					  "pipewire-camera-source",
					  nullptr};
	return findDeviceSource(ids);
}

std::string detectMicrophoneSourceName()
{
	// OBS keeps the first "Mic/Aux" device chosen under Settings -> Audio on output channel 3.
	if (OBSSourceAutoRelease global = obs_get_output_source(3)) {
		const char *name = obs_source_get_name(global);
		if (name && *name)
			return name;
	}
	static const char *const ids[] = {"wasapi_input_capture", "coreaudio_input_capture", "pulse_input_capture",
					  "alsa_input_capture", nullptr};
	return findDeviceSource(ids);
}

bool removeFromCurrentScene(obs_source_t *source)
{
	OBSSourceAutoRelease sceneSource = currentSceneSource();
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene || !source)
		return false;
	ItemSearch search{source, {}};
	obs_scene_enum_items(scene, collectItems, &search);
	for (obs_sceneitem_t *item : search.items)
		obs_sceneitem_remove(item);
	return !search.items.empty();
}

} // namespace qc
