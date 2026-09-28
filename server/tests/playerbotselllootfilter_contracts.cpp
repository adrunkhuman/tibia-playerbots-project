// Standalone sell-loot prefilter contracts; no world or route planner required.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>

#include "playerbotselllootfilter.h"

using playerbot::SellLootPrefilterInput;
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
	std::cout << "playerbotselllootfilter contracts passed\n";
}
