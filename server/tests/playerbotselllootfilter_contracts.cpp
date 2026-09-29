// Standalone sell-loot prefilter contracts; no world or route planner required.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>

#include "playerbotselllootfilter.h"

using playerbot::playerBotSellLootMinimumFare;
using playerbot::SellLootPrefilterInput;
using playerbot::SellLootTravelGraph;
using playerbot::SellLootPrefilterResult;
using playerbot::playerBotSellLootPrefilter;

int main()
{
	using Result = SellLootPrefilterResult;
	SellLootPrefilterInput input;
	input.revenue = 10;
	input.unavoidableTripCost = 10;
	assert(playerBotSellLootPrefilter(input) == Result::Unprofitable); // equality is not profit
	input.revenue = 11;
	assert(playerBotSellLootPrefilter(input) == Result::NeedsRouteValidation);
	input.revenue = 0;
	input.unavoidableTripCost = 0;
	assert(playerBotSellLootPrefilter(input) == Result::Unprofitable);
	input.survivalFallback = true;
	input.revenue = 0;
	assert(playerBotSellLootPrefilter(input) == Result::NeedsRouteValidation);

	// A costly preferred walk or a paid shortcut cannot justify a positive
	// lower bound if the other alternative may be free. Never use distance here.
	input = {};
	input.revenue = 1;
	assert(playerBotSellLootPrefilter(input) == Result::NeedsRouteValidation);
	input.unavoidableUpfrontFare = 3; // only if ALL feasible routes require >= 3
	input.unavoidableTripCost = 0; // fare itself is part of trip cost
	input.carriedGold = 1;
	input.bankGold = 1;
	assert(playerBotSellLootPrefilter(input) == Result::UnaffordableFare);
	input.bankGold = 2;
	assert(playerBotSellLootPrefilter(input) == Result::Unprofitable);
	input.survivalFallback = true;
	assert(playerBotSellLootPrefilter(input) == Result::NeedsRouteValidation);
	input.bankGold = 0;
	assert(playerBotSellLootPrefilter(input) == Result::UnaffordableFare);

	// Large funds must not wrap around to a false affordability rejection.
	input = {};
	input.revenue = 100;
	input.unavoidableUpfrontFare = 10;
	input.carriedGold = std::numeric_limits<uint64_t>::max();
	input.bankGold = std::numeric_limits<uint64_t>::max();
	assert(playerBotSellLootPrefilter(input) == Result::NeedsRouteValidation);
	input.carriedGold = 0;
	input.bankGold = 9;
	assert(playerBotSellLootPrefilter(input) == Result::UnaffordableFare);

	// Two possible routes: a bound from their minima must never discard a
	// fundable profitable route (or any fundable route during survival recovery).
	for (uint64_t fareA = 0; fareA <= 4; ++fareA) {
		for (uint64_t fareB = 0; fareB <= 4; ++fareB) {
			for (uint64_t extraA = 0; extraA <= 4; ++extraA) {
				for (uint64_t extraB = 0; extraB <= 4; ++extraB) {
					const uint64_t costA = fareA + extraA;
					const uint64_t costB = fareB + extraB;
					for (uint64_t revenue = 0; revenue <= 10; ++revenue) {
						for (uint64_t funds = 0; funds <= 8; ++funds) {
							for (bool survival : {false, true}) {
								const bool viableA = fareA <= funds && (survival || revenue > costA);
								const bool viableB = fareB <= funds && (survival || revenue > costB);
								if (!viableA && !viableB) continue;
								SellLootPrefilterInput candidate;
								candidate.revenue = revenue;
								candidate.unavoidableTripCost = std::min(costA, costB);
								candidate.unavoidableUpfrontFare = std::min(fareA, fareB);
								candidate.carriedGold = funds / 2;
								candidate.bankGold = funds - candidate.carriedGold;
								candidate.survivalFallback = survival;
								assert(playerBotSellLootPrefilter(candidate) == Result::NeedsRouteValidation);
							}
						}
					}
				}
			}
		}
	}

	// Offers: 0 = paid boat from the start (20), 1 = free ferry near the start
	// that lands on an unrelated island, 2 = onward boat to the seller (70),
	// 3 = free ferry near the seller that no chain from the start reaches.
	SellLootTravelGraph graph{{20, 0, 70, 0}, {
		{false, false, true, false},
		{false, false, false, false},
		{false, false, false, false},
		{false, false, false, false},
	}};
	const std::vector<bool> fromStart{true, true, false, false};
	const std::vector<bool> nearSeller{false, false, true, true};
	// A walkable leg costs nothing, even when every offer is expensive.
	assert(playerBotSellLootMinimumFare(graph, true, fromStart, nearSeller) == 0);
	// Free boats at both ends do not connect; the real chain costs 20 + 70.
	assert(playerBotSellLootMinimumFare(graph, false, fromStart, nearSeller) == 90);
	// A direct boat is both the first and last leg.
	assert(playerBotSellLootMinimumFare(graph, false, fromStart, {true, false, false, false}) == 20);
	// No boardable offer, or no chain to a landing near the seller: unreachable.
	assert(!playerBotSellLootMinimumFare(graph, false, {false, false, false, false}, nearSeller));
	assert(!playerBotSellLootMinimumFare(graph, false, {false, true, false, false}, nearSeller));
	// A free connected chain keeps the bound at zero.
	SellLootTravelGraph free{{0, 0}, {{false, true}, {false, false}}};
	assert(playerBotSellLootMinimumFare(free, false, {true, false}, {false, true}) == 0);
	// Cheaper multi-hop chains beat a dear direct boat.
	SellLootTravelGraph hops{{160, 40, 40}, {{false, false, false}, {false, false, true}, {false, false, false}}};
	assert(playerBotSellLootMinimumFare(hops, false, {true, true, false}, {true, false, true}) == 80);
	std::cout << "playerbotselllootfilter contracts passed\n";
}
