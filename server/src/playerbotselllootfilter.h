#ifndef FS_PLAYERBOTSELLLOOTFILTER_H
#define FS_PLAYERBOTSELLLOOTFILTER_H

#include <algorithm>
#include <cstdint>

namespace playerbot {

// Cheap necessary conditions only. A lower bound must hold for EVERY feasible
// walking and NPC-travel alternative, including free travel. Do not substitute
// map distance, a preferred route's fare, or a walking-only estimate: none is
// necessarily a lower bound on the route ultimately selected. Unknown bounds
// are zero. Trip cost includes fare; revenue is the candidate's upper bound.
struct SellLootPrefilterInput {
	uint64_t revenue = 0;
	uint64_t unavoidableTripCost = 0;
	uint64_t unavoidableUpfrontFare = 0;
	uint64_t carriedGold = 0;
	uint64_t bankGold = 0;
	bool survivalFallback = false;
};

enum class SellLootPrefilterResult {
	NeedsRouteValidation,
	Unprofitable,
	UnaffordableFare,
};

inline SellLootPrefilterResult playerBotSellLootPrefilter(const SellLootPrefilterInput& input)
{
	// Compare without overflowing carriedGold + bankGold. Both are an upper
	// bound on funds available before sale, not a promise that either is usable.
	if (input.unavoidableUpfrontFare > input.carriedGold &&
	    input.unavoidableUpfrontFare - input.carriedGold > input.bankGold) {
		return SellLootPrefilterResult::UnaffordableFare;
	}
	// Recovery can accept a loss, but cannot fund an unavoidable fare.
	if (!input.survivalFallback && input.revenue <=
	    std::max(input.unavoidableTripCost, input.unavoidableUpfrontFare)) {
		return SellLootPrefilterResult::Unprofitable;
	}
	return SellLootPrefilterResult::NeedsRouteValidation;
}

} // namespace playerbot

#endif
