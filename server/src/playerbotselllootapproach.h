#ifndef TFS_PLAYERBOTSELLLOOTAPPROACH_H
#define TFS_PLAYERBOTSELLLOOTAPPROACH_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <vector>

#include "position.h"

namespace playerbot {

struct SellLootApproach {
	Position position;
	bool coarseReachable;
	uint32_t coarseDistance;
	uint32_t geometricDistance;
	uint8_t direction; // Provider-relative 3x3 direction (centre excluded).
};

// Coarse reachability is a ranking hint, not a rejection criterion. The
// caller still validates the selected positions with normal pathfinding.
inline std::vector<Position> playerBotSellLootApproaches(std::vector<SellLootApproach> inputs,
                                                        size_t maxApproaches = 8)
{
	std::sort(inputs.begin(), inputs.end(), [](const SellLootApproach& left, const SellLootApproach& right) {
		if (left.coarseReachable != right.coarseReachable) return left.coarseReachable;
		if (left.coarseDistance != right.coarseDistance) return left.coarseDistance < right.coarseDistance;
		if (left.geometricDistance != right.geometricDistance) return left.geometricDistance < right.geometricDistance;
		if (left.position != right.position) return left.position < right.position;
		return left.direction < right.direction;
	});
	std::array<bool, 256> selectedDirections{};
	std::vector<Position> positions;
	positions.reserve(std::min<size_t>({inputs.size(), maxApproaches, 8}));
	for (const SellLootApproach& approach : inputs) {
		if (positions.size() >= maxApproaches || positions.size() >= 8) break;
		if (selectedDirections[approach.direction] ||
		    std::find(positions.begin(), positions.end(), approach.position) != positions.end()) continue;
		selectedDirections[approach.direction] = true;
		positions.push_back(approach.position);
	}
	return positions;
}

} // namespace playerbot

#endif
