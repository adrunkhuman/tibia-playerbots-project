/**
 * Owns combat, corpse-loot, and hunt transitions for one playerbot. Shared hunt
 * cooldowns outlive controllers; scheduling and world actions remain outside.
 */
#ifndef FS_PLAYERBOTHUNTCOORDINATOR_H
#define FS_PLAYERBOTHUNTCOORDINATOR_H

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "playerbotcombatruntime.h"
#include "playerbothuntruntime.h"
#include "playerbotlootworkflow.h"

struct PlayerBotHuntCoordinatorConfig {
	PlayerBotCombatRuntimeConfig combat;
	PlayerBotLootWorkflowConfig loot;
	std::vector<Position> fallbackPatrol;
};

struct PlayerBotHuntTurnObservation {
	bool regionSelectionRequired = false;
	bool planningActive = false;
	bool lootNavigationSuspended = false;
	bool cycleFinished = false;
};

class PlayerBotHuntCoordinator
{
	public:
		explicit PlayerBotHuntCoordinator(PlayerBotHuntCoordinatorConfig config,
		                                  std::map<uint64_t, std::chrono::steady_clock::time_point>& sharedCooldowns);

		std::optional<PlayerBotCombatDecision> selectTraversalAttack(std::vector<PlayerBotTraversalCandidate> candidates,
		                                                            const Position& currentPosition,
		                                                            std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotCombatDecision> selectDefensiveAttack(std::vector<PlayerBotDefensiveTarget> candidates,
		                                                           const Position& currentPosition) const;
		PlayerBotCombatDecision confirmCombatAttack(const PlayerBotCombatDecision& command, bool accepted,
		                                           std::chrono::steady_clock::time_point now);
		PlayerBotCombatDecision advanceCombat(const PlayerBotCombatSnapshot& snapshot);
		PlayerBotCombatDecision beginPursuit(const Position& currentPosition, const Position& destination,
		                                    std::chrono::steady_clock::time_point now);
		PlayerBotCombatDecision abandonPursuit(std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotTraversalTarget> clearTraversalTarget();
		std::optional<PlayerBotDefensiveTarget> clearDefensiveTarget();
		bool hasDefensiveCombat() const;
		bool hasActiveCombat() const;
		std::optional<PlayerBotTarget> activeTarget() const;
		std::optional<PlayerBotTraversalTarget> traversalTarget() const;
		std::optional<PlayerBotDefensiveTarget> defensiveTarget() const;

		// A defeated hunt target starts the loot workflow and counts as a hunt kill together.
		PlayerBotLootCommand beginLoot(const PlayerBotCombatDecision& defeatedTarget, const Position& currentPosition,
		                              std::chrono::steady_clock::time_point now);
		void resetLoot();
		PlayerBotLootDecision advanceLoot(const PlayerBotLootWorkflowSnapshot& snapshot);
		PlayerBotLootNavigationTransition observeLootNavigationFailure(const Position& currentPosition,
		                                                              std::chrono::steady_clock::time_point now);
		PlayerBotLootNavigationTransition resumeLootNavigation(const Position& currentPosition,
		                                                      std::chrono::steady_clock::time_point now);
		bool hasPendingLootMove() const;
		bool lootNavigationSuspended() const;
		bool lootTimedOut(std::chrono::steady_clock::time_point now) const;
		uint32_t lootTargetId() const;
		const PlayerBotExpectedCorpse& expectedCorpse() const;
		const Position& lootDeathPosition() const;
		const Position& corpsePosition() const;
		bool corpseObserved() const;
		bool lootedCurrentCorpse() const;
		uint32_t lootSearchAttempts() const;
		uint32_t lootNavigationFailures() const;
		uint32_t lootNavigationSuspensions() const;
		int64_t lootElapsedMilliseconds(std::chrono::steady_clock::time_point now) const;
		std::chrono::steady_clock::time_point lootNavigationRetryAt() const;

		void cancelPlanning();
		bool planningStartRequired(std::chrono::steady_clock::time_point now) const;
		bool planningActive() const;
		PlayerBotHuntRuntimeOutcome advancePlanning(const PlayerBotHuntRuntimePlanningInput& input,
		                                           std::chrono::steady_clock::time_point now,
		                                           const PlayerBotHuntPlanningObservation& observation = {});
		PlayerBotHuntRuntimeOutcome completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
		                                             uint64_t elapsedUs);
		std::optional<PlayerBotHuntPlanningSession> planningSession() const;
		void completePlanningSelection();
		void selectPlanningRegion(PlayerBotHuntRegion region, const PlayerBotHuntRuntimePlayerObservation& player,
		                         std::chrono::steady_clock::time_point now);
		void rejectHuntVariant(uint64_t variantId, std::chrono::steady_clock::time_point now,
		                       std::chrono::steady_clock::duration cooldown);

		void beginHuntCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds);
		bool huntDeadlineReached(std::chrono::steady_clock::time_point now) const;
		uint32_t completedHuntCycles() const;
		bool huntActive() const;
		PlayerBotHuntTurnObservation observeTurn(bool inHuntPhase, bool selectRegion, bool capacityLow,
		                                               std::chrono::steady_clock::time_point now) const;
		bool matchesHuntMonster(const std::string& name) const;
		void sampleHuntCombat(const PlayerBotHuntCombatSnapshot& snapshot);
		void observeHuntDamage(uint32_t damage);
		void observeHuntRecovery(bool potion);
		bool observeHuntDanger(int32_t maximumHealth, std::chrono::steady_clock::time_point now,
		                      std::chrono::steady_clock::duration cooldown);
		void observeHuntDeath(bool activeCombat, std::chrono::steady_clock::time_point now,
		                     std::chrono::steady_clock::duration cooldown);
		std::optional<PlayerBotHuntRuntimeCompletion> finishHunt(const PlayerBotHuntRuntimePlayerObservation& player,
		                                                        std::chrono::steady_clock::time_point now,
		                                                        uint32_t configuredDurationSeconds);
		PlayerBotHuntPlanningProfile huntPlanningProfile(PlayerBotHuntPlanningProfile profile) const;
		std::map<uint64_t, PlayerBotHuntRegionPerformance> huntRegionPerformance() const;
		PlayerBotEquipmentHuntSummary summarizeEquipmentHunts(const std::vector<PlayerBotHuntRegion>& regions, bool truncated) const;
		PlayerBotHuntPatrolOutcome huntPatrolTarget() const;
		PlayerBotHuntPatrolOutcome observeHuntPatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
		                                                      std::chrono::steady_clock::time_point now,
		                                                      uint32_t repeatedStepLimit, uint32_t routeFailureLimit);
		std::set<uint64_t> activeHuntCooldowns(std::chrono::steady_clock::time_point now);

	private:
		void applyCooldown(const std::optional<PlayerBotHuntRuntimeCooldownCommand>& command,
		                   std::chrono::steady_clock::time_point now);

		PlayerBotCombatRuntime combatRuntime;
		PlayerBotLootWorkflow lootWorkflow;
		PlayerBotHuntRuntime huntRuntime;
		std::map<uint64_t, std::chrono::steady_clock::time_point>& huntRegionCooldowns;
};

#endif
