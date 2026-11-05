/*
 * This file is part of PosseTTD, a fork of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file territory.h PosseTTD Phase 1: company territory overlay (display only, never touches game state). */

#ifndef POSSETTD_TERRITORY_H
#define POSSETTD_TERRITORY_H

#include "../company_type.h"
#include "../gfx_type.h"
#include "../tile_type.h"

struct Viewport;

/** Which stations count towards territory. */
enum class TerritoryFilter : uint8_t {
	All,       ///< Every station.
	Rail,      ///< Only stations with train facilities.
	Road,      ///< Only stations with bus/truck stops.
	Air,       ///< Only stations with airports.
	Water,     ///< Only stations with docks.
	Contracts, ///< Reserved for Phase 3 (city contracts); currently shows nothing.
	End,
};

/** One tile that belongs to somebody's territory. Result of the (cached) calculation. */
struct TerritoryCell {
	TileIndex tile;     ///< The tile.
	CompanyID owner;    ///< Company with the strongest influence.
	CompanyID rival;    ///< Second strongest company, only meaningful if \c contested.
	bool contested;     ///< Whether the second company is nearly as strong as the owner.
	uint8_t edges;      ///< Bit set = neighbour on that side is not the same territory (draw a border). See territory.cpp.
};

void ToggleTerritoryOverlay();
bool IsTerritoryOverlayEnabled();
TerritoryFilter GetTerritoryFilter();
void SetTerritoryFilter(TerritoryFilter filter);

/** Recalculate now (ignores the interval). */
void RecalculateTerritory();
const std::vector<TerritoryCell> &GetTerritoryCells();

/** Draw the territory over a viewport. \a dpi uses screen coordinates, like the link graph overlay. */
void DrawTerritoryOverlay(const Viewport &vp, const DrawPixelInfo *dpi);

#endif /* POSSETTD_TERRITORY_H */
