/** Pure transit movement-fallback state. Route replans do not change its chosen target. */
#ifndef FS_PLAYERBOTTRANSITCOMBAT_H
#define FS_PLAYERBOTTRANSITCOMBAT_H

#include <cstdint>
#include <optional>

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
		static bool lootDeadlineRequiresRelease(bool lootCorpseStage, bool lootTimedOut)
		{
			return lootCorpseStage && lootTimedOut;
		}
		// Goal decisions and phase transitions start new episodes. Replans and
		// intermediate arrivals within one goal do not.
		bool observe(bool transit, uint64_t goal, PlayerBotCyclePhase phase)
		{
			const bool changed = transit != active() || goal != goalId || phase != cyclePhase;
			if (changed) {
				finish();
				if (transit) travelling = true;
				goalId = goal;
				cyclePhase = phase;
			}
			return changed;
		}
		void finish()
		{
			travelling = false;
			clearFallback();
		}
		bool active() const { return travelling; }

		void observeMovementFailure(const Position& currentPosition,
		                           std::optional<Position> intendedStep = std::nullopt)
		{
			stalled = true;
			stalledPosition = currentPosition;
			if (intendedStep) intendedMovementStep = intendedStep;
		}
		void observePosition(const Position& currentPosition)
		{
			if (stalled && currentPosition != stalledPosition) clearFallback();
		}
		void observeViableMovement() { clearFallback(); }
		bool movementFallbackRequired() const { return stalled; }
		std::optional<Position> intendedStep() const { return intendedMovementStep; }

		bool allowsDefense(uint32_t blockerId, bool routeCritical) const
		{
			return (!travelling && !routeCritical) || (routeCritical && movementFallbackRequired() &&
			       (defensiveBlockerId == 0 || defensiveBlockerId == blockerId));
		}
		void beginDefense(uint32_t blockerId, bool selectedIntendedStep)
		{
			if (!movementFallbackRequired()) return;
			defensiveBlockerId = blockerId;
			defensiveTargetWasIntendedStep = selectedIntendedStep;
		}
		bool retainsDefense(uint32_t blockerId, const Position& currentPosition,
		                    const Position& blockerPosition, bool safe) const
		{
			return movementFallbackRequired() && defensiveBlockerId == blockerId && safe &&
			       currentPosition == stalledPosition &&
			       (!defensiveTargetWasIntendedStep || !intendedMovementStep ||
			        blockerPosition == *intendedMovementStep);
		}
		void clearFallback()
		{
			stalled = false;
			intendedMovementStep.reset();
			defensiveBlockerId = 0;
			defensiveTargetWasIntendedStep = false;
		}

	private:
		bool travelling = false;
		bool stalled = false;
		bool defensiveTargetWasIntendedStep = false;
		uint32_t defensiveBlockerId = 0;
		uint64_t goalId = 0;
		PlayerBotCyclePhase cyclePhase = PlayerBotCyclePhase::Idle;
		Position stalledPosition;
		std::optional<Position> intendedMovementStep;
};

#endif
