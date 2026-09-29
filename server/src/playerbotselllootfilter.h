#ifndef FS_PLAYERBOTSELLLOOTFILTER_H
#define FS_PLAYERBOTSELLLOOTFILTER_H

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

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

// One NPC travel offer, seen from one trip leg. Pass only offers the route
// engine could use now: eligible, available, and affordable after reserves.
struct SellLootTravelOffer {
	uint64_t fare = 0;
	bool boardable = false; // The provider is reachable on foot from the leg start.
	bool landsNearTarget = false; // The destination reaches the leg target on foot.
};

// Lower bound on the fare of one leg. When walking and map portals cannot
// connect the leg, every route boards a boardable offer first and ends on a
// landing offer. One offer may be both, so the bound is the larger of the two
// minima, never their sum. nullopt means no usable route exists.
inline std::optional<uint64_t> playerBotSellLootLegMinimumFare(bool walkable,
                                                               const std::vector<SellLootTravelOffer>& offers)
{
	if (walkable) return 0;
	std::optional<uint64_t> boarding, landing;
	for (const SellLootTravelOffer& offer : offers) {
		if (offer.boardable) boarding = std::min(boarding.value_or(offer.fare), offer.fare);
		if (offer.landsNearTarget) landing = std::min(landing.value_or(offer.fare), offer.fare);
	}
	if (!boarding || !landing) return std::nullopt;
	return std::max(*boarding, *landing);
}

} // namespace playerbot

#endif
