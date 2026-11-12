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

#include <obs.hpp>

#include <string>

namespace qc {

inline constexpr const char *kGuestSourceId = "quick_cast_guest";
inline constexpr const char *kGuestIdSetting = "guest_id";

/** Finds an existing guest source bound to `guestId`. Returns a new reference or nullptr. */
OBSSourceAutoRelease findGuestSource(const std::string &guestId);

/** Finds or creates the source for a guest. `name` is used (made unique if needed) only when creating. */
OBSSourceAutoRelease getOrCreateGuestSource(const std::string &guestId, const std::string &name);

/** Default OBS source name for a guest. */
std::string guestSourceName(const std::string &guestName);

bool isInCurrentScene(obs_source_t *source);
/** Adds the source to the scene currently being edited, optionally scaled to fill the canvas. */
bool addToCurrentScene(obs_source_t *source, bool fitToCanvas);
bool removeFromCurrentScene(obs_source_t *source);

} // namespace qc
