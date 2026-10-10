#ifndef FS_PLAYERBOTSUPPLYRECOVERY_H
#define FS_PLAYERBOTSUPPLYRECOVERY_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <limits>
#include <vector>

// Shared departure prerequisite. Round up so integer health cannot fall below
// the planner's required fraction; ordinary emergency healing is independent.
inline uint32_t playerBotSupplyRecoveryRequiredHealth(uint32_t maximumHealth)
{
	return static_cast<uint32_t>((uint64_t(maximumHealth) * 4 + 4) / 5);
}

enum class PlayerBotDepartureHealthResult { Ready, Recovering, TimedOut };

// Preparation requests use existing healing actions but have their own finite
// bound. A pending request is not a new hunt mode or a combat-healing threshold.
class PlayerBotDepartureHealthPreparation
{
	public:
		bool active() const { return started.has_value(); }
		uint32_t target() const { return requiredHealth; }
		PlayerBotDepartureHealthResult advance(int32_t health, int32_t maximumHealth, uint32_t targetHealth,
		                                      std::chrono::steady_clock::time_point now)
		{
			requiredHealth = std::min<uint32_t>(std::max(0, maximumHealth), targetHealth);
			if (health >= static_cast<int64_t>(requiredHealth) || requiredHealth == 0) {
				started.reset();
				return PlayerBotDepartureHealthResult::Ready;
			}
			if (!started) started = now;
			return now - *started >= std::chrono::seconds(60) ? PlayerBotDepartureHealthResult::TimedOut :
			       PlayerBotDepartureHealthResult::Recovering;
		}
	private:
		uint32_t requiredHealth = 0;
		std::optional<std::chrono::steady_clock::time_point> started;
};

/** Degraded operation while a supply restock is unaffordable. The bot keeps
 *  hunting and selling instead of stopping; region selection clamps toward
 *  safe, coin-income hunts until funds cover a restock again. */
class PlayerBotSupplyRecoveryState
{
	public:
		bool active() const { return degraded || restockDeferred; }

		// The stock key is playerBotSupplyStockKey(): any count change retries.
		void deferRestock(uint64_t funds, uint64_t stockKey)
		{
			restockDeferred = true;
			deferredFunds = funds;
			deferredStockKey = stockKey;
		}

		void clearRestockDeferral() { restockDeferred = false; }

		bool restockBlocked(uint64_t funds, uint64_t stockKey)
		{
			if (funds != deferredFunds || stockKey != deferredStockKey) restockDeferred = false;
			return restockDeferred;
		}

		bool enter()
		{
			if (degraded) return false;
			degraded = true;
			return true;
		}

		// Returns true when the mode changed. An unknown budget cannot safely
		// clear recovery mode, but an observed shortfall may still enter it.
		bool update(uint64_t funds, uint64_t potionBudget)
		{
			if (potionBudget == std::numeric_limits<uint64_t>::max()) return false;
			const bool next = funds < potionBudget;
			if (next == degraded) return false;
			degraded = next;
			return true;
		}

	private:
		bool degraded = false;
		bool restockDeferred = false;
		uint64_t deferredFunds = 0;
		uint64_t deferredStockKey = 0;
};

struct PlayerBotSupplyFloorCost {
	uint32_t count = 0;
	uint32_t floor = 0;
	uint32_t price = std::numeric_limits<uint32_t>::max(); // max: no known offer
};

// Adds the floor cost of every non-health kind to the health recovery budget.
// An unknown price matters only for a kind that is actually below its floor.
inline uint64_t playerBotSupplyFloorSpendingReserve(uint64_t healthReserve, const std::vector<PlayerBotSupplyFloorCost>& others)
{
	uint64_t reserve = healthReserve;
	for (const PlayerBotSupplyFloorCost& cost : others) {
		if (reserve == std::numeric_limits<uint64_t>::max()) break;
		if (cost.count >= cost.floor) continue;
		if (cost.price == std::numeric_limits<uint32_t>::max()) return std::numeric_limits<uint64_t>::max();
		reserve += static_cast<uint64_t>(cost.floor - cost.count) * cost.price;
	}
	return reserve;
}

inline double playerBotSupplyRecoveryChallengeFrontier(double frontier, bool degraded)
{
	return degraded ? std::min(frontier, 0.20) : frontier;
}

struct PlayerBotSurvivalSellCandidate {
	bool normalPlanAccepted = false;
	bool sourceReachable = false;
	bool sellerReachable = false;
	bool routeSafe = false;
	uint64_t fare = 0;
	uint64_t funds = 0;
};

// Survival selling accepts a reachable, safe merchant even when the trip is
// not profitable; route availability, safety, and fare affordability still bind.
inline bool playerBotSurvivalSellAccepted(bool degraded, const PlayerBotSurvivalSellCandidate& candidate)
{
	return degraded && !candidate.normalPlanAccepted && candidate.sourceReachable &&
	       candidate.sellerReachable && candidate.routeSafe && candidate.fare <= candidate.funds;
}

#endif
