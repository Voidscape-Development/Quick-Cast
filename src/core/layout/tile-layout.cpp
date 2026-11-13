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

#include "tile-layout.hpp"

#include <algorithm>
#include <cmath>

namespace qc {

namespace {

/** Space between tiles and, for picture-in-picture, to the canvas edge, as a fraction of the canvas height. */
constexpr float kGapFraction = 0.015f;
constexpr float kPipMarginFraction = 0.03f;
/** Width of a picture-in-picture tile as a fraction of the canvas width. */
constexpr float kPipWidthFraction = 0.22f;

std::vector<TileRect> sideBySide(size_t count, float width, float height, float gap)
{
	std::vector<TileRect> tiles;
	const float tileWidth = (width - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
	for (size_t i = 0; i < count; ++i)
		tiles.push_back(TileRect{static_cast<float>(i) * (tileWidth + gap), 0.0f, tileWidth, height});
	return tiles;
}

std::vector<TileRect> grid(size_t count, float width, float height, float gap)
{
	const auto columns = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
	const size_t rows = (count + columns - 1) / columns;
	const float tileWidth = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
	const float tileHeight = (height - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);

	std::vector<TileRect> tiles;
	for (size_t i = 0; i < count; ++i) {
		const size_t row = i / columns;
		const size_t column = i % columns;
		// A last row that is not full sits in the middle rather than hugging the left edge.
		const size_t inRow = row == rows - 1 ? count - row * columns : columns;
		const float rowWidth = static_cast<float>(inRow) * tileWidth + static_cast<float>(inRow - 1) * gap;
		const float left = (width - rowWidth) / 2.0f;
		tiles.push_back(TileRect{left + static_cast<float>(column) * (tileWidth + gap),
					 static_cast<float>(row) * (tileHeight + gap), tileWidth, tileHeight});
	}
	return tiles;
}

std::vector<TileRect> pictureInPicture(size_t count, float width, float height, float gap)
{
	const float margin = height * kPipMarginFraction;
	const float tileWidth = width * kPipWidthFraction;
	// Tiles keep the canvas's shape, which is also the shape of most cameras.
	const float tileHeight = tileWidth * height / width;
	const auto perColumn = std::max<size_t>(
		1, static_cast<size_t>(std::floor((height - 2.0f * margin + gap) / (tileHeight + gap))));

	std::vector<TileRect> tiles;
	for (size_t i = 0; i < count; ++i) {
		// Up from the bottom-right corner; a full column continues in a new one to its left.
		const size_t column = i / perColumn;
		const size_t row = i % perColumn;
		tiles.push_back(TileRect{width - margin - tileWidth - static_cast<float>(column) * (tileWidth + gap),
					 height - margin - tileHeight - static_cast<float>(row) * (tileHeight + gap),
					 tileWidth, tileHeight});
	}
	return tiles;
}

} // namespace

std::vector<TileRect> computeLayout(LayoutKind kind, size_t count, float canvasWidth, float canvasHeight)
{
	if (count == 0 || canvasWidth <= 0.0f || canvasHeight <= 0.0f)
		return {};

	const float gap = std::round(canvasHeight * kGapFraction);
	switch (kind) {
	case LayoutKind::SideBySide:
		return sideBySide(count, canvasWidth, canvasHeight, gap);
	case LayoutKind::Grid:
		return grid(count, canvasWidth, canvasHeight, gap);
	case LayoutKind::PictureInPicture:
		return pictureInPicture(count, canvasWidth, canvasHeight, gap);
	}
	return {};
}

} // namespace qc
