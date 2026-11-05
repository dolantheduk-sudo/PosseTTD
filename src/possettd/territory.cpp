/*
 * This file is part of PosseTTD, a fork of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file territory.cpp PosseTTD Phase 1: company territory overlay.
 *
 * IMPORTANT: this file only READS game state (stations, catchment areas, ratings) and keeps its
 * results in a private cache that is never saved and never sent over the network. Every client
 * computes its own copy, so it cannot cause a multiplayer desync.
 *
 * How influence is calculated:
 *   For every station that matches the filter, and every cargo it handles that a vehicle recently
 *   tried to load (so: actually in use), influence = station rating of that cargo (0..255).
 *   A station's influence is the sum over those cargos. Every tile in the station's catchment area
 *   receives that influence for the station's owner. On each tile the company with the highest
 *   total wins; if the runner-up has at least "contest %" of the winner's value, the tile is
 *   contested and shown striped.
 */

#include "../stdafx.h"

#include "territory.h"

#include "../company_base.h"
#include "../core/math_func.hpp"
#include "../gfx_func.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../palette_func.h"
#include "../settings_type.h"
#include "../station_base.h"
#include "../tile_map.h"
#include "../timer/timer.h"
#include "../timer/timer_game_tick.h"
#include "../timer/timer_window.h"
#include "../viewport_type.h"
#include "../zoom_func.h"

#include <array>
#include <unordered_map>
#include <vector>

#include "../safeguards.h"

/** Bits for TerritoryCell::edges. A set bit means: draw a border on that side of the tile. */
static constexpr uint8_t EDGE_Y_MINUS = 1 << 0; ///< Neighbour (x, y-1): edge between the top and left corner.
static constexpr uint8_t EDGE_X_PLUS  = 1 << 1; ///< Neighbour (x+1, y): edge between the left and bottom corner.
static constexpr uint8_t EDGE_Y_PLUS  = 1 << 2; ///< Neighbour (x, y+1): edge between the bottom and right corner.
static constexpr uint8_t EDGE_X_MINUS = 1 << 3; ///< Neighbour (x-1, y): edge between the right and top corner.

/** Stations whose cargo was not picked up for this many rating intervals (~2.5 days each) are considered unused. */
static constexpr uint8_t MAX_IDLE_INTERVALS = 20;

static TerritoryFilter _territory_filter = TerritoryFilter::All; ///< Active filter. Local to this client.
static std::vector<TerritoryCell> _territory_cells; ///< Cached result.
static TimerGameTick::TickCounter _territory_last_calc = 0; ///< Tick counter of the last calculation.
static bool _territory_have_calc = false; ///< Whether _territory_cells was ever calculated.

bool IsTerritoryOverlayEnabled() { return _settings_client.gui.posse_territory_enabled; }
TerritoryFilter GetTerritoryFilter() { return _territory_filter; }
const std::vector<TerritoryCell> &GetTerritoryCells() { return _territory_cells; }

void SetTerritoryFilter(TerritoryFilter filter)
{
	_territory_filter = filter;
	RecalculateTerritory();
	MarkWholeScreenDirty();
}

void ToggleTerritoryOverlay()
{
	_settings_client.gui.posse_territory_enabled = !_settings_client.gui.posse_territory_enabled;
	if (_settings_client.gui.posse_territory_enabled) RecalculateTerritory();
	MarkWholeScreenDirty();
}

/** Does this station count for the current filter? */
static bool StationMatchesFilter(const Station *st)
{
	switch (_territory_filter) {
		case TerritoryFilter::All:   return st->facilities.Any();
		case TerritoryFilter::Rail:  return st->facilities.Test(StationFacility::Train);
		case TerritoryFilter::Road:  return st->facilities.Test(StationFacility::TruckStop) || st->facilities.Test(StationFacility::BusStop);
		case TerritoryFilter::Air:   return st->facilities.Test(StationFacility::Airport);
		case TerritoryFilter::Water: return st->facilities.Test(StationFacility::Dock);
		default: return false; // Contracts: nothing until Phase 3.
	}
}

/** Sum of the ratings of all cargos that are actually being served at this station. */
static uint32_t StationInfluence(const Station *st)
{
	uint32_t total = 0;
	for (const GoodsEntry &ge : st->goods) {
		if (!ge.HasRating()) continue;
		if (ge.time_since_pickup > MAX_IDLE_INTERVALS) continue;
		total += ge.rating;
	}
	return total;
}

void RecalculateTerritory()
{
	_territory_last_calc = TimerGameTick::counter;
	_territory_have_calc = true;
	_territory_cells.clear();

	using Influence = std::array<uint32_t, MAX_COMPANIES>;
	std::unordered_map<TileIndex, Influence> tiles;

	for (const Station *st : Station::Iterate()) {
		if (!Company::IsValidID(st->owner)) continue;
		if (!StationMatchesFilter(st)) continue;
		uint32_t influence = StationInfluence(st);
		if (influence == 0) continue;

		BitmapTileIterator it(st->catchment_tiles);
		for (TileIndex tile = it; tile != INVALID_TILE; tile = ++it) {
			auto [pos, inserted] = tiles.try_emplace(tile);
			if (inserted) pos->second.fill(0);
			pos->second[st->owner.base()] += influence;
		}
	}

	/* Resolve the winner of every tile. */
	struct Resolved { CompanyID owner; CompanyID rival; bool contested; };
	std::unordered_map<TileIndex, Resolved> resolved;
	resolved.reserve(tiles.size());
	const uint64_t contest_pct = _settings_client.gui.posse_territory_contest_pct;

	for (const auto &[tile, infl] : tiles) {
		uint32_t best = 0, second = 0;
		uint best_c = MAX_COMPANIES, second_c = MAX_COMPANIES;
		for (uint c = 0; c < MAX_COMPANIES; c++) {
			if (infl[c] == 0) continue;
			if (infl[c] > best) {
				second = best; second_c = best_c;
				best = infl[c]; best_c = c;
			} else if (infl[c] > second) {
				second = infl[c]; second_c = c;
			}
		}
		if (best_c == MAX_COMPANIES) continue;
		bool contested = second_c != MAX_COMPANIES && (uint64_t)second * 100 >= (uint64_t)best * contest_pct;
		resolved[tile] = {CompanyID(best_c), contested ? CompanyID(second_c) : CompanyID::Invalid(), contested};
	}

	/* Work out the borders and fill the cache. */
	auto differs = [&](const Resolved &me, int x, int y) {
		if (x < 0 || y < 0 || x >= (int)Map::SizeX() || y >= (int)Map::SizeY()) return true;
		auto n = resolved.find(TileXY(x, y));
		if (n == resolved.end()) return true;
		return n->second.owner != me.owner || n->second.contested != me.contested;
	};

	_territory_cells.reserve(resolved.size());
	for (const auto &[tile, r] : resolved) {
		int x = TileX(tile), y = TileY(tile);
		uint8_t edges = 0;
		if (differs(r, x, y - 1)) edges |= EDGE_Y_MINUS;
		if (differs(r, x + 1, y)) edges |= EDGE_X_PLUS;
		if (differs(r, x, y + 1)) edges |= EDGE_Y_PLUS;
		if (differs(r, x - 1, y)) edges |= EDGE_X_MINUS;
		_territory_cells.push_back({tile, r.owner, r.rival, r.contested, edges});
	}
}

/** Periodic refresh (real time, client side). Only does work while the overlay is on. */
static const IntervalTimer<TimerWindow> _territory_refresh_timer(std::chrono::seconds(1), [](auto) {
	if (!_settings_client.gui.posse_territory_enabled) return;
	TimerGameTick::TickCounter interval = (TimerGameTick::TickCounter)_settings_client.gui.posse_territory_recalc_days * Ticks::DAY_TICKS;
	bool due = !_territory_have_calc || TimerGameTick::counter < _territory_last_calc /* a game was (re)loaded */
			|| TimerGameTick::counter - _territory_last_calc >= interval;
	if (!due) return;
	RecalculateTerritory();
	MarkWholeScreenDirty();
});

static PixelColour CompanyTerritoryColour(CompanyID c)
{
	const Company *comp = Company::GetIfValid(c);
	return GetColourGradient(comp != nullptr ? comp->colour : COLOUR_WHITE, SHADE_NORMAL);
}

/** Convert a world position to screen coordinates of the viewport (same maths as GetViewportStationMiddle). */
static Point WorldToScreen(const Viewport &vp, int x, int y, int z)
{
	Point p = RemapCoords(x, y, z);
	p.x = UnScaleByZoom(p.x - vp.virtual_left, vp.zoom) + vp.left;
	p.y = UnScaleByZoom(p.y - vp.virtual_top, vp.zoom) + vp.top;
	return p;
}

void DrawTerritoryOverlay(const Viewport &vp, const DrawPixelInfo *dpi)
{
	if (!_settings_client.gui.posse_territory_enabled) return;

	const int line_width = ScaleGUITrad(2);
	for (const TerritoryCell &cell : _territory_cells) {
		int wx = TileX(cell.tile) * TILE_SIZE;
		int wy = TileY(cell.tile) * TILE_SIZE;
		int z = TileHeight(cell.tile) * TILE_HEIGHT;

		/* The four corners of the (flat) tile diamond on screen. */
		Point top    = WorldToScreen(vp, wx, wy, z);
		Point left   = WorldToScreen(vp, wx + TILE_SIZE, wy, z);
		Point right  = WorldToScreen(vp, wx, wy + TILE_SIZE, z);
		Point bottom = WorldToScreen(vp, wx + TILE_SIZE, wy + TILE_SIZE, z);

		/* Skip tiles outside the area being drawn. */
		if (right.x < dpi->left || left.x >= dpi->left + dpi->width) continue;
		if (bottom.y < dpi->top || top.y >= dpi->top + dpi->height) continue;

		PixelColour colour = CompanyTerritoryColour(cell.owner);
		PixelColour colour2 = cell.contested ? CompanyTerritoryColour(cell.rival) : colour;

		/* Fill the diamond row by row with a dithered (half see-through) colour. Contested tiles alternate in bands. */
		int half_h = (bottom.y - top.y) / 2;
		int half_w = (right.x - left.x) / 2;
		if (half_h < 1) {
			GfxFillRect(left.x, top.y, right.x, bottom.y, colour);
		} else {
			for (int i = 0; i < 2 * half_h; i++) {
				int dy = i < half_h ? i : 2 * half_h - 1 - i;
				int hw = half_w * (dy + 1) / half_h;
				int y = top.y + i;
				PixelColour c = (cell.contested && ((y / 2) & 1)) ? colour2 : colour;
				GfxFillRect(top.x - hw, y, top.x + hw, y, c, FILLRECT_CHECKER);
			}
		}

		/* Borders where the neighbouring tile belongs to somebody else. */
		if (cell.edges & EDGE_Y_MINUS) GfxDrawLine(top.x, top.y, left.x, left.y, colour, line_width);
		if (cell.edges & EDGE_X_PLUS)  GfxDrawLine(left.x, left.y, bottom.x, bottom.y, colour, line_width);
		if (cell.edges & EDGE_Y_PLUS)  GfxDrawLine(bottom.x, bottom.y, right.x, right.y, colour, line_width);
		if (cell.edges & EDGE_X_MINUS) GfxDrawLine(right.x, right.y, top.x, top.y, colour, line_width);
	}
}
