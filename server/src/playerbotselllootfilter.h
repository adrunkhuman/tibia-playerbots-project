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

// Eligible NPC travel offers as a graph. Pass only offers the route engine
// could use now: eligible, available, and affordable after reserves.
// boardableAfter[i][j] means offer j's provider is walkable from offer i's
// landing. Walking and map portals are free here; only fares count.
struct SellLootTravelGraph {
	std::vector<uint64_t> fares;
	std::vector<std::vector<bool>> boardableAfter;
};

// Cheapest total fare over every chain of offers from a leg start to a leg
// target, or nullopt when no chain exists. A walkable leg costs nothing.
inline std::optional<uint64_t> playerBotSellLootMinimumFare(const SellLootTravelGraph& graph, bool walkable,
                                                            const std::vector<bool>& boardableFromStart,
                                                            const std::vector<bool>& landsNearTarget)
{
	if (walkable) return 0;
	const size_t count = graph.fares.size();
	std::vector<std::optional<uint64_t>> best(count);
	std::vector<bool> done(count);
	for (size_t i = 0; i < count; ++i) {
		if (boardableFromStart[i]) best[i] = graph.fares[i];
	}
	// Dense Dijkstra: offer counts are small and edges are dense.
	for (;;) {
		std::optional<size_t> next;
		for (size_t i = 0; i < count; ++i) {
			if (!done[i] && best[i] && (!next || *best[i] < *best[*next])) next = i;
		}
		if (!next) return std::nullopt;
		if (landsNearTarget[*next]) return best[*next];
		done[*next] = true;
		for (size_t j = 0; j < count; ++j) {
			if (done[j] || !graph.boardableAfter[*next][j]) continue;
			const uint64_t fare = *best[*next] + graph.fares[j];
			if (!best[j] || fare < *best[j]) best[j] = fare;
		}
	}
}

} // namespace playerbot

#endif
