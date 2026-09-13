/** Pure transit combat session. Navigation resets do not renew blocker attempts. */
#ifndef FS_PLAYERBOTTRANSITCOMBAT_H
#define FS_PLAYERBOTTRANSITCOMBAT_H

#include <chrono>
#include <cstdint>

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
		void finish()
		{
			travelling = false;
			defenseAttempted = false;
			breakoutAttempted = false;
			breakout = false;
		}
		bool active() const { return travelling; }
		bool allowsDefense(uint32_t, bool routeCritical) const
		{
			return !travelling || (routeCritical && (!defenseAttempted || breakout));
		}
		void beginDefense(uint32_t, std::chrono::steady_clock::time_point now)
		{
			if (!travelling) return;
			if (!defenseAttempted) defenseDeadline = now + std::chrono::seconds(5);
			defenseAttempted = true;
		}
		bool defenseExpired(std::chrono::steady_clock::time_point now) const
		{
			return travelling && defenseAttempted && !breakout && now >= defenseDeadline;
		}
		bool beginBreakout(bool routeExhausted, bool adjacentConfirmedHostileBlocker,
		                   [[maybe_unused]] bool routeUnavailable,
		                   std::chrono::steady_clock::time_point now,
		                   std::chrono::steady_clock::duration duration = std::chrono::seconds(30))
		{
			if (!travelling || breakoutAttempted || !routeExhausted || !adjacentConfirmedHostileBlocker) {
				return false;
			}
			breakoutAttempted = true;
			breakout = true;
			breakoutDeadline = now + duration;
			return true;
		}
		bool breakoutWasAttempted() const { return breakoutAttempted; }
		bool breakoutActive() const { return travelling && breakout; }
		bool breakoutExpired(std::chrono::steady_clock::time_point now) const
		{
			return breakoutActive() && now >= breakoutDeadline;
		}
		bool observeBreakoutNavigation(bool positionalProgress, bool routeAvailable)
		{
			if (!breakoutActive() || (!positionalProgress && !routeAvailable)) return false;
			breakout = false;
			return true;
		}
		void finishBreakout() { breakout = false; }

	private:
		bool travelling = false;
		bool defenseAttempted = false;
		bool breakoutAttempted = false;
		bool breakout = false;
		uint64_t goalId = 0;
		PlayerBotCyclePhase cyclePhase = PlayerBotCyclePhase::Idle;
		std::chrono::steady_clock::time_point defenseDeadline{};
		std::chrono::steady_clock::time_point breakoutDeadline{};
};

#endif
