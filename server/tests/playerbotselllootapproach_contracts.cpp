// Standalone conversation-approach ranking; no NPC, map or route planner needed.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "playerbotselllootapproach.h"

using playerbot::SellLootApproach;
using playerbot::playerBotSellLootApproaches;
using playerbot::playerBotNextSellLootApproach;

int main()
{
	const Position customerSide(105, 100, 7);
	const Position behindCounter(101, 100, 7);
	const Position otherCounterTile(102, 100, 7);
	const std::vector<SellLootApproach> counter{
		{behindCounter, false, std::numeric_limits<uint32_t>::max(), 1, 0},
		{otherCounterTile, true, 90, 2, 1},
		{customerSide, true, 4, 5, 2},
	};
	// The farther, reachable customer side beats nearer tiles behind the counter.
	assert((playerBotSellLootApproaches(counter) ==
	        std::vector<Position>{customerSide, otherCounterTile, behindCounter}));

	// Unknown topology is not a negative path result. Retain an approach for
	// detailed validation even when all coarse queries are inconclusive.
	const std::vector<SellLootApproach> unknown{
		{Position(200, 200, 7), false, 20, 1, 0},
		{Position(201, 200, 7), false, 10, 4, 1},
	};
	assert((playerBotSellLootApproaches(unknown) ==
	        std::vector<Position>{Position(201, 200, 7), Position(200, 200, 7)}));

	std::vector<SellLootApproach> candidates;
	for (uint8_t direction = 0; direction < 9; ++direction) {
		if (direction == 4) continue; // Centre is the NPC's own tile.
		candidates.push_back({Position(static_cast<uint16_t>(300 + direction), 300, 7),
		                      true, direction, direction, direction});
		// The more distant tile from the same direction must not crowd out others.
		candidates.push_back({Position(static_cast<uint16_t>(400 + direction), 300, 7),
		                      true, direction + 10u, 0, direction});
	}
	const auto expected = playerBotSellLootApproaches(candidates);
	assert(expected.size() == 8);
	for (const Position& position : expected) assert(position.x < 400);
	assert(playerBotSellLootApproaches(candidates, 100) == expected);
	assert(playerBotSellLootApproaches(candidates, 3).size() == 3);
	assert(playerBotSellLootApproaches(candidates, 0).empty());
	std::reverse(candidates.begin(), candidates.end());
	assert(playerBotSellLootApproaches(candidates) == expected);

	// Equal scores are resolved by position, independent of input order.
	const std::vector<SellLootApproach> ties{
		{Position(502, 500, 7), true, 3, 1, 1},
		{Position(501, 500, 7), true, 3, 1, 0},
	};
	assert((playerBotSellLootApproaches(ties) ==
	        std::vector<Position>{Position(501, 500, 7), Position(502, 500, 7)}));
	assert(playerBotSellLootApproaches({}).empty());
	// When the seller moves east, an old western-edge approach is out of
	// speech range. Skip it even if it was valid in the original snapshot.
	const Position failed(103, 100, 7);
	const std::vector<Position> moving{failed, Position(97, 100, 7), Position(104, 101, 7)};
	assert(playerBotNextSellLootApproach(moving, 1, Position(101, 100, 7), failed) == 2);
	assert(playerBotNextSellLootApproach(moving, 2, Position(90, 100, 7), failed) == moving.size());
	std::cout << "playerbotselllootapproach contracts passed\n";
}
