/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTRANGEDPOSITION_H
#define FS_PLAYERBOTRANGEDPOSITION_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "position.h"

// Position policy for a bot whose weapon outranges melee (#235). Attacks fire
// automatically while a target is set and in range, and walking never delays
// them (classicAttackSpeed), so this only decides where to stand: hold, step
// away from a closing monster, close in, or give up and fight in place.
//
// Decision record. The band is [1, weapon range], aimed at the far edge: a
// spear has a flat hit chance in this datapack, so distance costs no accuracy
// and only buys time before melee damage. Prefer holding against a target with
// a shorter current cardinal step duration. This is a local heuristic, not a
// prediction: terrain, diagonals and future target movement can change it.
// Range-aware danger estimates and multi-attacker kiting stay under #54, so a
// retreat tile only avoids other hostiles and the usual hazards.
enum class PlayerBotRangedAction : uint8_t {
	Hold,        // In the band with a clear shot; attacks continue.
	Retreat,     // Step to `tile`, which is farther from the target.
	Close,       // Out of range or no clear shot; walk toward the target.
	FightInPlace // Retreat is not useful or not possible; stay and fight.
};

// One walkable-or-not neighbour of the bot, supplied by the caller from live
// tiles. The caller lists cardinal tiles first: diagonal steps take longer.
struct PlayerBotRangedTile {
	Position position;
	bool open = false;        // Walkable and unoccupied.
	bool insideArea = true;   // Inside the active hunt region.
	bool hazardous = false;   // Above the current tile's danger.
	bool sightClear = false;  // A clear shot at the target from here.
	bool hostileNearby = false; // Another hostile monster is close to this tile.
};

struct PlayerBotRangedPositionInput {
	Position self;
	Position target;
	uint8_t range = 1;
	int64_t selfStepMs = 0;
	int64_t targetStepMs = 0;
	bool sightClear = false; // A clear shot from the current tile.
	std::vector<PlayerBotRangedTile> neighbours;
};

struct PlayerBotRangedPositionDecision {
	PlayerBotRangedAction action = PlayerBotRangedAction::Hold;
	Position tile;
	const char* reason = "in_band";
	uint32_t distance = 0;
};

inline uint32_t playerBotChebyshev(const Position& from, const Position& to)
{
	return std::max(Position::getDistanceX(from, to), Position::getDistanceY(from, to));
}

inline PlayerBotRangedPositionDecision playerBotRangedPosition(const PlayerBotRangedPositionInput& input)
{
	PlayerBotRangedPositionDecision decision;
	decision.distance = playerBotChebyshev(input.self, input.target);
	if (input.range <= 1) {
		decision.reason = "melee_style";
		return decision;
	}
	if (decision.distance > input.range || !input.sightClear) {
		decision.action = PlayerBotRangedAction::Close;
		decision.reason = decision.distance > input.range ? "out_of_range" : "no_line_of_sight";
		return decision;
	}
	if (decision.distance >= input.range) return decision;
	if (input.targetStepMs < input.selfStepMs) {
		decision.action = PlayerBotRangedAction::FightInPlace;
		decision.reason = "target_faster";
		return decision;
	}

	bool hostileBehind = false;
	bool hazardBehind = false;
	bool outsideBehind = false;
	const PlayerBotRangedTile* best = nullptr;
	uint32_t bestDistance = decision.distance;
	for (const PlayerBotRangedTile& tile : input.neighbours) {
		const uint32_t distance = playerBotChebyshev(tile.position, input.target);
		if (distance <= decision.distance || distance > input.range) continue;
		if (!tile.open) continue;
		if (tile.hostileNearby) { hostileBehind = true; continue; }
		if (tile.hazardous) { hazardBehind = true; continue; }
		if (!tile.insideArea) { outsideBehind = true; continue; }
		if (!tile.sightClear) continue;
		if (distance > bestDistance) {
			best = &tile;
			bestDistance = distance;
		}
	}
	if (best) {
		decision.action = PlayerBotRangedAction::Retreat;
		decision.tile = best->position;
		decision.reason = "target_too_close";
		return decision;
	}
	decision.action = PlayerBotRangedAction::FightInPlace;
	decision.reason = hostileBehind ? "retreat_hostile" : hazardBehind ? "retreat_hazard" :
	                  outsideBehind ? "retreat_outside_region" : "retreat_blocked";
	return decision;
}

#endif
