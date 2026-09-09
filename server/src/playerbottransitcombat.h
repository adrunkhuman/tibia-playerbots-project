/** Pure transit combat session. Navigation resets do not renew blocker attempts. */
#ifndef FS_PLAYERBOTTRANSITCOMBAT_H
#define FS_PLAYERBOTTRANSITCOMBAT_H

#include <chrono>
#include <cstdint>
#include <set>

#include "playerbotturnrouter.h"

class PlayerBotTransitCombat
{
	public:
		static bool required(PlayerBotCyclePhase phase, PlayerBotScenarioStage stage,
		                     bool huntReached, bool progressionActive)
		{
			return phase != PlayerBotCyclePhase::Hunt || !huntReached || progressionActive ||
			       stage == PlayerBotScenarioStage::LootCorpse;
		}
		// Goal decisions and phase transitions start new episodes, not route
		// replans or intermediate arrivals within the same goal.
		bool observe(bool transit, uint64_t goal, PlayerBotCyclePhase phase)
		{
			const bool changed = transit != active() || goal != goalId || phase != cyclePhase;
			if (changed) {
				finish();
				if (transit) begin();
				goalId = goal;
				cyclePhase = phase;
			}
			return changed;
		}
		void begin() { travelling = true; }
		void finish() { travelling = false; attempted.clear(); }
		bool active() const { return travelling; }
		bool allowsDefense(uint32_t id, bool routeCritical) const
		{
			return !travelling || (routeCritical && attempted.count(id) == 0);
		}
		void beginDefense(uint32_t id, std::chrono::steady_clock::time_point now)
		{
			if (!travelling) return;
			attempted.insert(id);
			deadline = now + std::chrono::seconds(5);
		}
		bool defenseExpired(std::chrono::steady_clock::time_point now) const
		{
			return travelling && !attempted.empty() && now >= deadline;
		}

	private:
		bool travelling = false;
		uint64_t goalId = 0;
		PlayerBotCyclePhase cyclePhase = PlayerBotCyclePhase::Idle;
		std::set<uint32_t> attempted;
		std::chrono::steady_clock::time_point deadline{};
};

#endif
