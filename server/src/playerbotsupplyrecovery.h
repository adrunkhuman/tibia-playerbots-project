#ifndef FS_PLAYERBOTSUPPLYRECOVERY_H
#define FS_PLAYERBOTSUPPLYRECOVERY_H

#include <algorithm>
#include <cstdint>
#include <limits>

/** Degraded operation while a potion restock is unaffordable. The bot keeps
 *  hunting and selling instead of stopping; region selection clamps toward
 *  safe, coin-income hunts until funds cover a restock again. */
class PlayerBotSupplyRecoveryState
{
	public:
		bool active() const { return degraded || restockDeferred; }

		void deferRestock(uint64_t funds, uint32_t potions)
		{
			restockDeferred = true;
			deferredFunds = funds;
			deferredPotions = potions;
		}

		bool restockBlocked(uint64_t funds, uint32_t potions)
		{
			if (funds != deferredFunds || potions != deferredPotions) restockDeferred = false;
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
		uint32_t deferredPotions = 0;
};

inline uint32_t playerBotMandatoryPotionDeficit(uint32_t potions, uint32_t safetyTarget, bool recovery)
{
	return !recovery && potions < safetyTarget ? safetyTarget - potions : 0;
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
