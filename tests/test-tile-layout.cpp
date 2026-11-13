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

#include "layout/tile-layout.hpp"

#include <gtest/gtest.h>

using namespace qc;

namespace {

constexpr float kWidth = 1920.0f;
constexpr float kHeight = 1080.0f;

bool insideCanvas(const TileRect &tile)
{
	const float slack = 0.01f;
	return tile.x >= -slack && tile.y >= -slack && tile.x + tile.width <= kWidth + slack &&
	       tile.y + tile.height <= kHeight + slack;
}

bool overlap(const TileRect &a, const TileRect &b)
{
	const float slack = 0.01f;
	return a.x + a.width > b.x + slack && b.x + b.width > a.x + slack && a.y + a.height > b.y + slack &&
	       b.y + b.height > a.y + slack;
}

void expectTidy(const std::vector<TileRect> &tiles)
{
	for (size_t i = 0; i < tiles.size(); ++i) {
		EXPECT_TRUE(insideCanvas(tiles[i])) << "tile " << i;
		EXPECT_GT(tiles[i].width, 0.0f);
		EXPECT_GT(tiles[i].height, 0.0f);
		for (size_t j = i + 1; j < tiles.size(); ++j)
			EXPECT_FALSE(overlap(tiles[i], tiles[j])) << "tiles " << i << " and " << j;
	}
}

} // namespace

TEST(TileLayout, NothingToLayOut)
{
	EXPECT_TRUE(computeLayout(LayoutKind::Grid, 0, kWidth, kHeight).empty());
	EXPECT_TRUE(computeLayout(LayoutKind::SideBySide, 2, 0.0f, kHeight).empty());
}

TEST(TileLayout, OneTileFillsTheCanvas)
{
	for (const LayoutKind kind : {LayoutKind::SideBySide, LayoutKind::Grid}) {
		const auto tiles = computeLayout(kind, 1, kWidth, kHeight);
		ASSERT_EQ(tiles.size(), 1u);
		EXPECT_FLOAT_EQ(tiles[0].x, 0.0f);
		EXPECT_FLOAT_EQ(tiles[0].y, 0.0f);
		EXPECT_FLOAT_EQ(tiles[0].width, kWidth);
		EXPECT_FLOAT_EQ(tiles[0].height, kHeight);
	}
}

TEST(TileLayout, SideBySideSplitsTheWidth)
{
	const auto tiles = computeLayout(LayoutKind::SideBySide, 2, kWidth, kHeight);
	ASSERT_EQ(tiles.size(), 2u);
	expectTidy(tiles);
	EXPECT_FLOAT_EQ(tiles[0].x, 0.0f);
	EXPECT_FLOAT_EQ(tiles[0].width, tiles[1].width);
	EXPECT_FLOAT_EQ(tiles[0].height, kHeight);
	// Left to right, ending flush with the right edge.
	EXPECT_GT(tiles[1].x, tiles[0].x + tiles[0].width);
	EXPECT_NEAR(tiles[1].x + tiles[1].width, kWidth, 0.01f);
}

TEST(TileLayout, GridIsAsSquareAsPossible)
{
	// Four people make 2 x 2: every tile the same size, two distinct rows.
	const auto four = computeLayout(LayoutKind::Grid, 4, kWidth, kHeight);
	ASSERT_EQ(four.size(), 4u);
	expectTidy(four);
	EXPECT_FLOAT_EQ(four[0].y, four[1].y);
	EXPECT_FLOAT_EQ(four[2].y, four[3].y);
	EXPECT_GT(four[2].y, four[0].y);
	EXPECT_FLOAT_EQ(four[0].x, four[2].x);
}

TEST(TileLayout, GridCentresAShortLastRow)
{
	// Three people: two on top, one centred underneath.
	const auto tiles = computeLayout(LayoutKind::Grid, 3, kWidth, kHeight);
	ASSERT_EQ(tiles.size(), 3u);
	expectTidy(tiles);
	EXPECT_NEAR(tiles[2].x + tiles[2].width / 2.0f, kWidth / 2.0f, 0.01f);
	EXPECT_FLOAT_EQ(tiles[2].width, tiles[0].width);
}

TEST(TileLayout, GridHoldsALargeGroup)
{
	for (size_t count = 1; count <= 12; ++count) {
		const auto tiles = computeLayout(LayoutKind::Grid, count, kWidth, kHeight);
		ASSERT_EQ(tiles.size(), count);
		expectTidy(tiles);
	}
}

TEST(TileLayout, PictureInPictureStacksFromTheBottomRight)
{
	const auto tiles = computeLayout(LayoutKind::PictureInPicture, 2, kWidth, kHeight);
	ASSERT_EQ(tiles.size(), 2u);
	expectTidy(tiles);
	// Small, canvas-shaped, in the right-hand part of the picture, the second above the first.
	EXPECT_LT(tiles[0].width, kWidth / 3.0f);
	EXPECT_NEAR(tiles[0].width / tiles[0].height, kWidth / kHeight, 0.01f);
	EXPECT_GT(tiles[0].x, kWidth / 2.0f);
	EXPECT_GT(tiles[0].y, kHeight / 2.0f);
	EXPECT_FLOAT_EQ(tiles[1].x, tiles[0].x);
	EXPECT_LT(tiles[1].y, tiles[0].y);
}

TEST(TileLayout, PictureInPictureWrapsIntoANewColumn)
{
	const auto tiles = computeLayout(LayoutKind::PictureInPicture, 9, kWidth, kHeight);
	ASSERT_EQ(tiles.size(), 9u);
	expectTidy(tiles);
	EXPECT_LT(tiles.back().x, tiles.front().x);
}
