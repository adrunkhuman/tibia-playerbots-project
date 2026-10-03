/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTAPPROACH_H
#define FS_PLAYERBOTAPPROACH_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

#include "position.h"

// Shared approach to a set of acceptable tiles: a depot locker, a service NPC,
// a sell-loot seller, or an NPC-travel captain. Callers keep their own route
// validation, safety, and economics; this owns tile choice and retry bounds.
// Patrol and hunt waypoints deliberately stay separate: their failures feed
// hunt-level decisions, not a choice between tiles beside one provider.
//
// Decision record (#219). One rule set replaced per-caller variants. If a
// caller regresses, change the value here; every outcome reports a stable
// `reason` in the `approach_result` event.
// - Tile choice: free tiles before occupied ones, then caller cost. Occupied
//   tiles stay eligible as a last resort (was depot-only).
// - Creature on the path: walking sidesteps first (#217). Callers then detour
//   around the blocked tiles; otherwise wait up to `blockedWaits` retries
//   (1 s apart for service, one navigation retry for depot; was service-only).
// - Provider moves: retarget to its live tiles up to `providerMoves` times
//   (was sell-loot-only; boarding now follows a wandered captain too).
// - Failed tile: reject it and try the next; give up after `rejectedTiles`
//   rejections (was the depot discovery attempt limit). Depot exhaustion only
//   defers and rescans, because a depot failure stops the controller.
// - Provider out of range at the chosen tile: wait up to `providerWaits`,
//   then release the tile; reject it after `providerReleases` releases (was
//   service-only).
struct PlayerBotApproachLimits {
	static constexpr uint32_t blockedWaits = 30;
	static constexpr uint32_t providerMoves = 2;
	static constexpr uint32_t rejectedTiles = 4;
	static constexpr uint32_t providerWaits = 15;
	static constexpr uint32_t providerReleases = 5;
};

struct PlayerBotApproachTile {
	Position position;
	uint32_t cost = 0;
	bool occupied = false;
};

// Equal tiles keep the caller's order, which may encode provider-specific
// preferences such as speech range or direction diversity.
inline bool playerBotApproachTileBefore(const PlayerBotApproachTile& left, const PlayerBotApproachTile& right)
{
	return std::tie(left.occupied, left.cost) < std::tie(right.occupied, right.cost);
}

enum class PlayerBotApproachVerdict : uint8_t {
	Continue, // Keep going: retry, retarget, or try the next tile after a rejection.
	Wait,     // Wait one interval and observe again.
	Release,  // Drop the selected tile and choose again from live tiles.
	Exhausted // The bound is spent; `reason()` explains the last failure.
};

class PlayerBotApproach
{
	public:
		// A new target starts with no tiles, rejections, or spent bounds.
		void reset() { *this = {}; }

		// Replace the acceptable tiles, for example around a provider's live
		// position. Rejections and spent bounds are kept.
		void offer(std::vector<PlayerBotApproachTile> candidates)
		{
			std::stable_sort(candidates.begin(), candidates.end(), playerBotApproachTileBefore);
			tiles = std::move(candidates);
		}
		const std::vector<PlayerBotApproachTile>& offered() const { return tiles; }

		// The best tile not yet rejected, or none once the rejection bound is spent.
		std::optional<Position> next() const
		{
			if (rejections >= PlayerBotApproachLimits::rejectedTiles) return std::nullopt;
			for (const PlayerBotApproachTile& tile : tiles) {
				if (!rejectedTiles.count(tile.position)) return tile.position;
			}
			return std::nullopt;
		}

		void select(const Position& tile) { chosen = tile; }
		void release() { chosen.reset(); }
		const std::optional<Position>& selected() const { return chosen; }
		bool rejected(const Position& tile) const { return rejectedTiles.count(tile) != 0; }

		PlayerBotApproachVerdict reject(const Position& tile, const char* cause)
		{
			lastReason = cause;
			if (chosen == tile) chosen.reset();
			if (rejectedTiles.insert(tile).second) ++rejections;
			blockedCount = 0;
			outOfRangeWaits = 0;
			return !next() ?
			    PlayerBotApproachVerdict::Exhausted : PlayerBotApproachVerdict::Continue;
		}

		// A creature blocks the path and no detour exists.
		PlayerBotApproachVerdict blocked()
		{
			lastReason = "route_blocked";
			return ++blockedCount <= PlayerBotApproachLimits::blockedWaits ?
			    PlayerBotApproachVerdict::Wait : PlayerBotApproachVerdict::Exhausted;
		}
		void unblocked() { blockedCount = 0; }
		uint32_t blockedWaits() const { return blockedCount; }

		// The provider left the tiles chosen around its earlier position.
		PlayerBotApproachVerdict providerMoved()
		{
			lastReason = "provider_moved";
			chosen.reset();
			return ++moves <= PlayerBotApproachLimits::providerMoves ?
			    PlayerBotApproachVerdict::Continue : PlayerBotApproachVerdict::Exhausted;
		}

		// At the selected tile while the provider is out of range. Wait when no
		// other tile is in range; release otherwise, rejecting after repeats.
		PlayerBotApproachVerdict providerOutOfRange(bool alternativeInRange)
		{
			lastReason = "provider_out_of_range";
			if (!chosen) return PlayerBotApproachVerdict::Release;
			if (!alternativeInRange && ++outOfRangeWaits <= PlayerBotApproachLimits::providerWaits)
				return PlayerBotApproachVerdict::Wait;
			outOfRangeWaits = 0;
			if (++releases > PlayerBotApproachLimits::providerReleases) {
				const PlayerBotApproachVerdict verdict = reject(*chosen, "provider_out_of_range");
				return verdict == PlayerBotApproachVerdict::Exhausted ? verdict : PlayerBotApproachVerdict::Continue;
			}
			chosen.reset();
			return PlayerBotApproachVerdict::Release;
		}

		// The provider is in range: waits for it no longer count.
		void providerInRange() { outOfRangeWaits = 0; }

		const char* reason() const { return lastReason; }

	private:
		std::vector<PlayerBotApproachTile> tiles;
		std::set<Position> rejectedTiles;
		std::optional<Position> chosen;
		const char* lastReason = "none";
		uint32_t rejections = 0;
		uint32_t blockedCount = 0;
		uint32_t moves = 0;
		uint32_t outOfRangeWaits = 0;
		uint32_t releases = 0;
};

inline const char* playerBotApproachVerdictName(PlayerBotApproachVerdict verdict)
{
	switch (verdict) {
		case PlayerBotApproachVerdict::Continue: return "retry";
		case PlayerBotApproachVerdict::Wait: return "waiting";
		case PlayerBotApproachVerdict::Release: return "released";
		case PlayerBotApproachVerdict::Exhausted: return "failed";
	}
	return "unknown";
}

#endif
