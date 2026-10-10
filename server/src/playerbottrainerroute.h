#ifndef FS_PLAYERBOTTRAINERROUTE_H
#define FS_PLAYERBOTTRAINERROUTE_H

#include "playerbotnavigationruntime.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

// The saved boarding walk remains executable while its endpoint is in the
// selected captain's live talking range. Never substitute a generic follow path.
inline std::optional<Position> playerBotTrainerBoardingApproach(const PlayerBotNavigationRoutePlan& route, Position source)
{
	for (const auto& step : route.steps) {
		if (step.action == PlayerBotNavigationAction::NpcTravel) return source;
		source = step.expectedPosition;
	}
	return std::nullopt;
}

inline bool playerBotTrainerDestinationRetained(const PlayerBotNavigationRoutePlan& route,
    const std::vector<Position>& quoted, const std::vector<Position>& live)
{
	if (route.metrics.firstNpcTravelOffer) {
		// This leg ends at boarding. Common trainer goals remain destinations;
		// the next leg obtains a fresh goal/itinerary proof after landing.
		return std::any_of(quoted.begin(), quoted.end(), [&](Position p) {
			return std::find(live.begin(), live.end(), p) != live.end();
		});
	}
	const Position endpoint = route.steps.empty() ? route.metrics.waypoint : route.steps.back().expectedPosition;
	return std::find(live.begin(), live.end(), endpoint) != live.end();
}

inline bool playerBotTrainerDiscoveryDestinationRetained(const PlayerBotNavigationRoutePlan& route,
    const std::vector<Position>& quoted, const std::vector<Position>& live)
{
	// Paid discovery retains a whole-trip quote but not the final walking
	// endpoint. Keep its destination-set validation conservative until landing.
	return route.metrics.firstNpcTravelOffer ? quoted == live : playerBotTrainerDestinationRetained(route, quoted, live);
}

inline uint32_t playerBotTrainerMinimumSteps(Position source, const std::vector<Position>& liveApproaches)
{
	// Walking, item-use and NPC travel all count as steps. If the source is
	// not a live goal, no complete route can have zero steps (including boats).
	return std::find(liveApproaches.begin(), liveApproaches.end(), source) != liveApproaches.end() ? 0 : 1;
}

// Ranking before route-dependent steps is also the discovery work order.
// A strictly worse prefix cannot win even with a shorter or cheaper journey.
template<class Offer> auto playerBotTrainerPriority(const Offer& offer)
{
	return std::tie(offer.learningPriority, offer.level, offer.price);
}

template<class Offer> bool playerBotTrainerOptimisticCanBeat(const Offer& candidate, uint32_t minimumSteps,
                                                            const Offer& incumbent)
{
	return std::tuple_cat(playerBotTrainerPriority(candidate),
	           std::tie(minimumSteps, candidate.spellName, candidate.npcId)) <
	       std::tuple_cat(playerBotTrainerPriority(incumbent),
	           std::tie(incumbent.route.steps, incumbent.spellName, incumbent.npcId));
}

// The potion requirement belongs to the discarded proof, not the offer.
// Retained completed candidates must keep theirs until that proof is replaced.
template<class Offer> void playerBotTrainerDiscardRouteProof(Offer& offer)
{
	offer.route = {};
	offer.potionReserve = 0;
}

template<class Offers> void playerBotTrainerEndPendingProofs(Offers& offers, size_t next)
{
	for (size_t i = next; i < offers.size(); ++i)
		if (!offers[i].route.reachable) offers[i].routeRejection = "route_proof_exhausted";
}

// If final live validation rejects the incumbent, deferred offers can compete
// again. Their old rejection was ranking, never a reachability proof.
template<class Offers, class MinimumSteps> std::optional<size_t> playerBotTrainerDeferredNeeded(
    const Offers& offers, std::optional<size_t> selected, MinimumSteps minimumSteps)
{
	for (size_t i = 0; i < offers.size(); ++i) {
		if (offers[i].routeRejection == "lower_learning_priority" &&
		    (!selected || playerBotTrainerPriority(offers[i]) <= playerBotTrainerPriority(offers[*selected]))) return i;
		if (offers[i].routeRejection == "route_rank_dominated" &&
		    (!selected || playerBotTrainerOptimisticCanBeat(offers[i], minimumSteps(i), offers[*selected]))) return i;
	}
	return std::nullopt;
}

template<class Offers> std::optional<size_t> playerBotTrainerDeferredNeeded(
    const Offers& offers, std::optional<size_t> selected)
{
	return playerBotTrainerDeferredNeeded(offers, selected, [](size_t) { return uint32_t(0); });
}

struct PlayerBotTrainerScanProgress {
	using Clock = std::chrono::steady_clock;
	uint64_t continuations = 0; // Includes admission waits; never a route-proof limit.
	uint32_t restarts = 0;
	Clock::time_point deadline;

	const char* exhaustion(Clock::time_point now, uint32_t maximumRestarts) const {
		if (restarts > maximumRestarts) return "scan_restart_limit";
		if (now >= deadline) return "scan_elapsed_limit";
		return nullptr;
	}
};

// Subtraction, not addition: quotes must remain safe even at UINT64_MAX.
inline bool playerBotTrainerTripAffordable(uint64_t funds, uint64_t reserve, uint64_t spell, uint64_t fare)
{
	return reserve != UINT64_MAX && funds >= reserve && funds - reserve >= spell &&
	       funds - reserve - spell >= fare;
}

inline bool playerBotTrainerFareAffordable(uint64_t funds, uint64_t reserve, uint64_t spell,
                                           uint64_t remainingFare, uint64_t fare)
{
	return fare <= remainingFare && playerBotTrainerTripAffordable(funds, reserve, spell, remainingFare);
}

inline bool playerBotTrainerTravelReceipt(bool landed, uint64_t before, uint64_t after, uint64_t fare)
{
	return landed && before >= fare && after == before - fare;
}

// One arbitration episode has two admitted phases. A completed negative
// trainer decision is just as durable as a positive one while preparation
// waits for admission. The controller supplies semantic facts and route proof.
template<class Plan> class PlayerBotTrainerArbitration {
public:
	struct Result {
		bool pending = false;
		std::optional<Plan> selected;
	};
	enum class Phase { Trainer, Preparation };
	Phase phase() const { return currentPhase; }
	bool completed() const { return currentPhase == Phase::Preparation; }
	void reset() { *this = {}; }

	template<class Discover>
	Result advanceTrainer(const std::string& liveFacts, bool proofValid, Discover discover) {
		if (completed() && facts == liveFacts && proofValid) return decision;
		currentPhase = Phase::Trainer;
		decision = discover();
		if (!decision.pending) {
			facts = liveFacts;
			currentPhase = Phase::Preparation;
		}
		return decision;
	}
private:
	Phase currentPhase = Phase::Trainer;
	std::string facts;
	Result decision;
};

// The route engine uses one active slot. Scoped substitution keeps independent
// owners' frontiers intact even when a slice yields or throws.
template<class Work> class PlayerBotRouteWorkSlot {
public:
	PlayerBotRouteWorkSlot(Work& active, Work& owned) : active(active), owned(owned) { active.swap(owned); }
	~PlayerBotRouteWorkSlot() { active.swap(owned); }
	PlayerBotRouteWorkSlot(const PlayerBotRouteWorkSlot&) = delete;
	PlayerBotRouteWorkSlot& operator=(const PlayerBotRouteWorkSlot&) = delete;
private:
	Work& active;
	Work& owned;
};

#endif
