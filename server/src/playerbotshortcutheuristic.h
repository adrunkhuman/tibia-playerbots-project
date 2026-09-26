#ifndef FS_PLAYERBOTSHORTCUTHEURISTIC_H
#define FS_PLAYERBOTSHORTCUTHEURISTIC_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "position.h"

struct PlayerBotShortcut {
	Position source;
	Position destination;
	bool operator==(const PlayerBotShortcut& other) const {
		return source == other.source && destination == other.destination;
	}
};

// A down-floor redirect depends on flags/tiles below it. Retain every possible
// XY outcome even when those tiles are absent, so enabling an exit later needs
// no reverse-dependency scan. Tile::getFloorChangeDestination combines at most
// -1, -2, +1 per axis (the alternate early exits are included). Up redirects
// depend only on the source flags and use their exact summed offsets.
inline std::vector<PlayerBotShortcut> playerBotFloorShortcuts(
    Position source, bool down, int32_t xOffset, int32_t yOffset)
{
	std::vector<PlayerBotShortcut> result;
	if (down) {
		result.reserve(25);
		for (int32_t dx = -3; dx <= 1; ++dx) {
			for (int32_t dy = -3; dy <= 1; ++dy) {
				result.push_back({source, Position(uint16_t(source.x + dx), uint16_t(source.y + dy),
				                                  uint8_t(source.z + 1))});
			}
		}
	} else {
		result.push_back({source, Position(uint16_t(source.x + xOffset), uint16_t(source.y + yOffset),
		                                  uint8_t(source.z - 1))});
	}
	return result;
}

// Dispatcher-owned shortcut descriptions and an independent dirty set. Ordinary
// item churn must neither exhaust the route journal nor invalidate potentials.
// The callbacks return value descriptions only; no tile/player pointers survive.
class PlayerBotShortcutIndex {
public:
	static constexpr size_t maximumDirtyTiles = 65536;
	void changed(Position p) {
		if (rescan) return;
		dirty.insert(key(p));
		if (dirty.size() > maximumDirtyTiles) invalidate();
	}
	void invalidate() { rescan = true; dirty.clear(); }
	uint64_t revision() const { return version; }
	const std::vector<PlayerBotShortcut>& shortcuts() const { return flattened; }

	// scanAll calls its visitor once per map tile; describe also accepts an
	// absent coordinate and returns an empty vector for it.
	template<class ScanAll, class Describe>
	void refresh(ScanAll scanAll, Describe describe) {
		if (!rescan && dirty.empty()) return;
		bool different = false;
		if (rescan) {
			decltype(entries) replacement;
			scanAll([&](Position p) {
				auto value = describe(p);
				if (!value.empty()) replacement.emplace(key(p), std::move(value));
			});
			different = replacement != entries;
			entries = std::move(replacement);
			rescan = false;
		} else {
			for (uint64_t tile : dirty) {
				const Position p(uint16_t(tile >> 16), uint16_t(tile), uint8_t(tile >> 32));
				auto value = describe(p);
				auto it = entries.find(tile);
				if (it == entries.end() && value.empty()) continue;
				if (it != entries.end() && it->second == value) continue;
				different = true;
				if (value.empty()) entries.erase(tile);
				else entries[tile] = std::move(value);
			}
		}
		dirty.clear();
		if (!different) return;
		++version;
		flattened.clear();
		for (const auto& entry : entries) {
			flattened.insert(flattened.end(), entry.second.begin(), entry.second.end());
		}
	}

private:
	static uint64_t key(Position p) { return (uint64_t(p.z) << 32) | (uint64_t(p.x) << 16) | p.y; }
	bool rescan = true;
	uint64_t version = 0;
	std::unordered_set<uint64_t> dirty;
	std::unordered_map<uint64_t, std::vector<PlayerBotShortcut>> entries;
	std::vector<PlayerBotShortcut> flattened;
};

// Pure, immutable reverse potential on an obstacle-free, floor-collapsed grid.
// Every grid hop costs 10, NOT cellSize * 10. Projection of a real cardinal
// move takes at most one hop, a diagonal at most one (real cost 30), and a
// direct rope/ladder/shovel use at most two (real cost 20). Floor redirects
// and teleports are separate zero-cost directed edges, including each link
// of a chained floor redirect. Thus every real arc has a no-more-expensive
// relaxed path; reverse distances to any projected goal are lower bounds.
//
// Clamping outside the rectangle is nonexpansive. Opposite boundary cells
// are adjacent to accommodate uint16 coordinate wraparound in getNextPosition.
// These extra wrap edges are deliberately optimistic even on an interior box.
class PlayerBotShortcutHeuristic {
public:
	static constexpr uint32_t maximumCells = 1024 * 1024;

	PlayerBotShortcutHeuristic(const std::vector<Position>& goals,
	                          const std::vector<PlayerBotShortcut>& shortcuts,
	                          Position minimum, Position maximum) {
		if (goals.empty()) return;
		auto include = [&](Position p) {
			minimum.x = std::min(minimum.x, p.x);
			minimum.y = std::min(minimum.y, p.y);
			maximum.x = std::max(maximum.x, p.x);
			maximum.y = std::max(maximum.y, p.y);
		};
		for (Position goal : goals) include(goal);
		for (const auto& shortcut : shortcuts) {
			include(shortcut.source);
			include(shortcut.destination);
		}
		for (;;) {
			minX = minimum.x / cellSize;
			minY = minimum.y / cellSize;
			width = maximum.x / cellSize - minX + 1;
			height = maximum.y / cellSize - minY + 1;
			if (uint64_t(width) * height <= maximumCells) break;
			cellSize *= 2;
		}
		costs.assign(width * height, UINT32_MAX);
		std::unordered_map<uint32_t, std::vector<uint32_t>> incoming;
		for (const auto& shortcut : shortcuts) {
			const uint32_t from = node(shortcut.source), to = node(shortcut.destination);
			if (from != to) incoming[to].push_back(from);
		}
		// 0-1 BFS; queue entries retain their label so obsolete entries do not
		// rescan outgoing edges. Distances are stored in units of ten.
		std::deque<std::pair<uint32_t, uint32_t>> open;
		for (Position goal : goals) {
			const uint32_t index = node(goal);
			if (costs[index] == 0) continue;
			costs[index] = 0;
			open.emplace_back(index, 0);
		}
		while (!open.empty()) {
			const auto [current, cost] = open.front();
			open.pop_front();
			if (cost != costs[current]) continue;
			auto relax = [&](uint32_t next, uint32_t weight) {
				if (cost + weight >= costs[next]) return;
				costs[next] = cost + weight;
				if (weight == 0) open.emplace_front(next, cost);
				else open.emplace_back(next, cost + weight);
			};
			if (auto it = incoming.find(current); it != incoming.end()) {
				for (uint32_t source : it->second) relax(source, 0);
			}
			const uint32_t x = current % width, y = current / width;
			const uint32_t xs[] = {x == 0 ? width - 1 : x - 1, x, x + 1 == width ? 0 : x + 1};
			const uint32_t ys[] = {y == 0 ? height - 1 : y - 1, y, y + 1 == height ? 0 : y + 1};
			for (uint32_t ny : ys) for (uint32_t nx : xs) relax(ny * width + nx, 1);
		}
	}

	uint32_t estimate(Position position) const {
		return costs.empty() ? 0 : costs[node(position)] * 10;
	}
	size_t cellCount() const { return costs.size(); }
	uint32_t resolution() const { return cellSize; }

private:
	uint32_t node(Position p) const {
		const uint32_t x = std::clamp(uint32_t(p.x) / cellSize, minX, minX + width - 1) - minX;
		const uint32_t y = std::clamp(uint32_t(p.y) / cellSize, minY, minY + height - 1) - minY;
		return y * width + x;
	}
	uint32_t minX = 0, minY = 0, width = 0, height = 0, cellSize = 1;
	std::vector<uint32_t> costs;
};

#endif
