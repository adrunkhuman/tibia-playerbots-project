#ifndef FS_PLAYERBOTPREPARATION_H
#define FS_PLAYERBOTPREPARATION_H

#include "playerbotsupplystock.h"
#include "playerbotnavigation.h"

#include <algorithm>
#include <array>
#include <optional>

// Domain evaluators own the requirement. Pending means missing evidence, not
// failure or permission to execute a route. Preparation never carries a hunt route.
enum class PlayerBotReadiness { Pending, Executable, Blocked };
enum class PlayerBotRequirementKind { Stock, HealthRegeneration };
struct PlayerBotPreparationRequirement {
	PlayerBotRequirementKind kind = PlayerBotRequirementKind::Stock;
	PlayerBotSupplyRequirements stocks;
	uint32_t health = 0;
	bool observeRegeneration = false;
};
struct PlayerBotGoalReadiness {
	PlayerBotReadiness state = PlayerBotReadiness::Pending;
	std::optional<PlayerBotPreparationRequirement> requirement;
};
enum class PlayerBotPreparationOption { None, RestoreHealth, Regenerate, BuyFood, ServiceStock, RecoveryIncome };

inline bool playerBotPreparationProviderInRange(const Position& from, const Position& provider)
{
	return Position::getDistanceZ(from, provider) <= 2 &&
	       std::max(Position::getDistanceX(from, provider), Position::getDistanceY(from, provider)) <= 32;
}

template<class SafeRest>
std::vector<Position> playerBotPreparationRestPositions(const Position& from, SafeRest safe)
{
	std::vector<Position> candidates;
	// Geometry is only discovery, never reachability. One any-of route proof
	// can find a reachable upstairs endpoint instead of testing roofs one by one.
	for (int radius = 0; radius <= 32; ++radius) {
		for (int dz : {0, -1, 1, -2, 2}) {
			const int z = from.z + dz;
			if (z < 0 || z >= 16) continue; // Loaded engine map has 16 layers.
			for (int dx = -radius; dx <= radius; ++dx) for (int dy = -radius; dy <= radius; ++dy) {
				if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
				const int x = from.x + dx, y = from.y + dy;
				if (x < 0 || x > UINT16_MAX || y < 0 || y > UINT16_MAX) continue;
				const Position candidate(x, y, z);
				if (safe(candidate)) candidates.push_back(candidate);
				if (candidates.size() == 512) return candidates;
			}
		}
	}
	return candidates;
}

// All rest, provider, and post-purchase rest proofs share one finite allowance.
// Work is also admitted/charged by the dispatcher-wide planning budget.
class PlayerBotPreparationRouteBudget {
	public:
		uint64_t allowance() const { return proofs < 8 ? std::min<uint64_t>(4096, remaining) : 0; }
		void observed(uint64_t nodes) { remaining -= std::min(remaining, nodes); ++proofs; }
	private:
		uint64_t remaining = 16384;
		uint32_t proofs = 0;
};

inline bool playerBotPreparationRouteAccepted(PlayerBotNavigationResult result, uint32_t dangerCost,
    double peakDanger, uint64_t fare, bool endpointReached)
{
	return result == PlayerBotNavigationResult::Reached && endpointReached && fare == 0 &&
	       dangerCost == 0 && peakDanger == 0;
}

// A finite observation/action cycle, not a dependency-search engine. Improvement
// is measured against the best requirement gap, not currency/inventory identity.
class PlayerBotPreparationBudget {
	public:
		bool completed(uint64_t gap, bool regenerationObserved = false,
		               PlayerBotRequirementKind kind = PlayerBotRequirementKind::HealthRegeneration)
		{
			auto& bestGap = bestGaps[kind == PlayerBotRequirementKind::Stock ? 0 : 1];
			if (!bestGap || gap < *bestGap || (regenerationObserved && !observedRegeneration)) attempts = 0;
			bestGap = bestGap ? std::min(*bestGap, gap) : gap;
			observedRegeneration = observedRegeneration || regenerationObserved;
			return ++attempts >= 3;
		}
		void reset() { bestGaps = {}; attempts = 0; observedRegeneration = false; }
	private:
		std::array<std::optional<uint64_t>, 2> bestGaps;
		uint32_t attempts = 0;
		bool observedRegeneration = false;
};

#endif
