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

#include <cstddef>
#include <vector>

/**
 * Where to put a set of guests on the canvas. Pure geometry, so it can be tested without OBS; applying the result
 * to scene items is the job of obs/scene-integration.
 */
namespace qc {

enum class LayoutKind {
	/** Equal columns across the canvas, each the full height. */
	SideBySide,
	/** The most even grid that holds everyone; a short last row is centred. */
	Grid,
	/** Small tiles stacked up from the bottom-right corner, leaving the rest of the scene alone. */
	PictureInPicture,
};

struct TileRect {
	float x = 0.0f;
	float y = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
};

/**
 * One rectangle per tile, in the order the tiles should be filled. Tiles are boxes to fit a video into, not the
 * video's final size: a 16:9 camera in a tall side-by-side column is centred in it.
 *
 * Returns an empty list when there is nothing to lay out or the canvas has no area.
 */
std::vector<TileRect> computeLayout(LayoutKind kind, size_t count, float canvasWidth, float canvasHeight);

} // namespace qc
