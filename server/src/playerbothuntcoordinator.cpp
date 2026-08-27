#include "otpch.h"

#include "playerbothuntcoordinator.h"
#include "playerbotequipmentpolicy.h"

#include <utility>

PlayerBotHuntCoordinator::PlayerBotHuntCoordinator(
	PlayerBotHuntCoordinatorConfig config, std::map<uint64_t, std::chrono::steady_clock::time_point>& sharedCooldowns) :
	combatRuntime(std::move(config.combat)), lootWorkflow(std::move(config.loot)),
	huntRuntime(std::move(config.fallbackPatrol)), huntRegionCooldowns(sharedCooldowns)
{}

std::optional<PlayerBotCombatDecision> PlayerBotHuntCoordinator::selectTraversalAttack(
	std::vector<PlayerBotTraversalCandidate> candidates, const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	return combatRuntime.selectTraversalAttack(std::move(candidates), currentPosition, now);
}

std::optional<PlayerBotCombatDecision> PlayerBotHuntCoordinator::selectDefensiveAttack(
	std::vector<PlayerBotDefensiveTarget> candidates, const Position& currentPosition) const
{
	return combatRuntime.selectDefensiveAttack(std::move(candidates), currentPosition);
}

PlayerBotCombatDecision PlayerBotHuntCoordinator::confirmCombatAttack(const PlayerBotCombatDecision& command, bool accepted,
	std::chrono::steady_clock::time_point now)
{
	return combatRuntime.confirmAttack(command, accepted, now);
}

PlayerBotCombatDecision PlayerBotHuntCoordinator::advanceCombat(const PlayerBotCombatSnapshot& snapshot) { return combatRuntime.advance(snapshot); }
PlayerBotCombatDecision PlayerBotHuntCoordinator::beginPursuit(const Position& currentPosition, const Position& destination,
	std::chrono::steady_clock::time_point now) { return combatRuntime.beginPursuit(currentPosition, destination, now); }
PlayerBotCombatDecision PlayerBotHuntCoordinator::abandonPursuit(std::chrono::steady_clock::time_point now) { return combatRuntime.abandonPursuit(now); }
std::optional<PlayerBotTraversalTarget> PlayerBotHuntCoordinator::clearTraversalTarget() { return combatRuntime.clearTraversalTarget(); }
std::optional<PlayerBotDefensiveTarget> PlayerBotHuntCoordinator::clearDefensiveTarget() { return combatRuntime.clearDefensiveTarget(); }
bool PlayerBotHuntCoordinator::hasDefensiveCombat() const { return combatRuntime.hasDefensiveCombat(); }
bool PlayerBotHuntCoordinator::hasActiveCombat() const { return combatRuntime.hasActiveCombat(); }
std::optional<PlayerBotTarget> PlayerBotHuntCoordinator::activeTarget() const { return combatRuntime.activeTarget(); }
std::optional<PlayerBotTraversalTarget> PlayerBotHuntCoordinator::traversalTarget() const { return combatRuntime.traversalTarget(); }
std::optional<PlayerBotDefensiveTarget> PlayerBotHuntCoordinator::defensiveTarget() const { return combatRuntime.defensiveTarget(); }

PlayerBotLootCommand PlayerBotHuntCoordinator::beginLoot(const PlayerBotCombatDecision& defeatedTarget,
	const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	if (huntRuntime.active()) huntRuntime.observeKill();
	return lootWorkflow.begin(defeatedTarget.target.id, defeatedTarget.target.position, defeatedTarget.expectedCorpse, currentPosition, now);
}

void PlayerBotHuntCoordinator::resetLoot() { lootWorkflow.reset(); }
PlayerBotLootDecision PlayerBotHuntCoordinator::advanceLoot(const PlayerBotLootWorkflowSnapshot& snapshot) { return lootWorkflow.advance(snapshot); }
PlayerBotLootNavigationTransition PlayerBotHuntCoordinator::observeLootNavigationFailure(const Position& currentPosition,
	std::chrono::steady_clock::time_point now) { return lootWorkflow.observeNavigationFailure(currentPosition, now); }
PlayerBotLootNavigationTransition PlayerBotHuntCoordinator::resumeLootNavigation(const Position& currentPosition,
	std::chrono::steady_clock::time_point now) { return lootWorkflow.resumeNavigation(currentPosition, now); }
bool PlayerBotHuntCoordinator::hasPendingLootMove() const { return lootWorkflow.hasPendingLootMove(); }
bool PlayerBotHuntCoordinator::lootNavigationSuspended() const { return lootWorkflow.navigationSuspended(); }
bool PlayerBotHuntCoordinator::lootTimedOut(std::chrono::steady_clock::time_point now) const { return lootWorkflow.timedOut(now); }
uint32_t PlayerBotHuntCoordinator::lootTargetId() const { return lootWorkflow.targetId(); }
const PlayerBotExpectedCorpse& PlayerBotHuntCoordinator::expectedCorpse() const { return lootWorkflow.expectedCorpse(); }
const Position& PlayerBotHuntCoordinator::lootDeathPosition() const { return lootWorkflow.deathPosition(); }
const Position& PlayerBotHuntCoordinator::corpsePosition() const { return lootWorkflow.corpsePosition(); }
bool PlayerBotHuntCoordinator::corpseObserved() const { return lootWorkflow.corpseObserved(); }
bool PlayerBotHuntCoordinator::lootedCurrentCorpse() const { return lootWorkflow.lootedCurrentCorpse(); }
uint32_t PlayerBotHuntCoordinator::lootSearchAttempts() const { return lootWorkflow.searchAttempts(); }
uint32_t PlayerBotHuntCoordinator::lootNavigationFailures() const { return lootWorkflow.navigationFailures(); }
uint32_t PlayerBotHuntCoordinator::lootNavigationSuspensions() const { return lootWorkflow.navigationSuspensions(); }
int64_t PlayerBotHuntCoordinator::lootElapsedMilliseconds(std::chrono::steady_clock::time_point now) const { return lootWorkflow.elapsedMilliseconds(now); }
std::chrono::steady_clock::time_point PlayerBotHuntCoordinator::lootNavigationRetryAt() const { return lootWorkflow.navigationRetryAt(); }

void PlayerBotHuntCoordinator::cancelPlanning() { huntRuntime.cancelPlanning(); }
bool PlayerBotHuntCoordinator::planningStartRequired(std::chrono::steady_clock::time_point now) const { return huntRuntime.planningStartRequired(now); }
bool PlayerBotHuntCoordinator::planningActive() const { return huntRuntime.planningActive(); }
PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::advancePlanning(const PlayerBotHuntRuntimePlanningInput& input,
	std::chrono::steady_clock::time_point now, const PlayerBotHuntPlanningObservation& observation)
{
	return huntRuntime.advancePlanning(input, now, observation);
}
PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
	uint64_t elapsedUs) { return huntRuntime.completeScoreWork(observations, elapsedUs); }
std::optional<PlayerBotHuntPlanningSession> PlayerBotHuntCoordinator::planningSession() const { return huntRuntime.planningSession(); }
void PlayerBotHuntCoordinator::completePlanningSelection() { huntRuntime.completePlanningSelection(); }
void PlayerBotHuntCoordinator::selectPlanningRegion(PlayerBotHuntRegion region,
	const PlayerBotHuntRuntimePlayerObservation& player, std::chrono::steady_clock::time_point now)
{
	huntRuntime.selectPlanningRegion(std::move(region), player, now);
}
void PlayerBotHuntCoordinator::rejectHuntVariant(uint64_t variantId, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration cooldown)
{
	const auto until = now + cooldown;
	auto& current = huntRegionCooldowns[variantId];
	if (current < until) current = until;
}

void PlayerBotHuntCoordinator::beginHuntCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds)
{
	huntRuntime.beginCycle(now, durationSeconds);
}
bool PlayerBotHuntCoordinator::huntDeadlineReached(std::chrono::steady_clock::time_point now) const { return huntRuntime.deadlineReached(now); }
uint32_t PlayerBotHuntCoordinator::completedHuntCycles() const { return huntRuntime.completedCycles(); }
bool PlayerBotHuntCoordinator::huntActive() const { return huntRuntime.active(); }
PlayerBotHuntTurnObservation PlayerBotHuntCoordinator::observeTurn(bool inHuntPhase, bool selectRegion, bool capacityLow,
	std::chrono::steady_clock::time_point now) const
{
	return {inHuntPhase && selectRegion && !huntRuntime.active() && !huntRuntime.planningActive(),
	        huntRuntime.planningActive(), lootWorkflow.navigationSuspended(),
	        inHuntPhase && (huntRuntime.deadlineReached(now) || capacityLow)};
}
bool PlayerBotHuntCoordinator::matchesHuntMonster(const std::string& name) const { return huntRuntime.matchesMonster(name); }
void PlayerBotHuntCoordinator::sampleHuntCombat(const PlayerBotHuntCombatSnapshot& snapshot) { huntRuntime.sampleCombat(snapshot); }
void PlayerBotHuntCoordinator::observeHuntDamage(uint32_t damage) { huntRuntime.observeDamage(damage); }
void PlayerBotHuntCoordinator::observeHuntRecovery(bool potion) { huntRuntime.observeRecovery(potion); }
bool PlayerBotHuntCoordinator::observeHuntDanger(int32_t maximumHealth, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration cooldown)
{
	const auto command = huntRuntime.dangerObserved(maximumHealth, now, cooldown);
	applyCooldown(command, now);
	return command.has_value();
}
void PlayerBotHuntCoordinator::observeHuntDeath(bool activeCombat, std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration cooldown)
{
	applyCooldown(huntRuntime.observeDeath(activeCombat, cooldown), now);
}
std::optional<PlayerBotHuntRuntimeCompletion> PlayerBotHuntCoordinator::finishHunt(const PlayerBotHuntRuntimePlayerObservation& player,
	std::chrono::steady_clock::time_point now, uint32_t configuredDurationSeconds)
{
	return huntRuntime.complete(player, now, configuredDurationSeconds);
}
PlayerBotHuntPlanningProfile PlayerBotHuntCoordinator::huntPlanningProfile(PlayerBotHuntPlanningProfile profile) const
{
	return huntRuntime.planningProfile(std::move(profile));
}
std::map<uint64_t, PlayerBotHuntRegionPerformance> PlayerBotHuntCoordinator::huntRegionPerformance() const { return huntRuntime.regionPerformance(); }
PlayerBotEquipmentHuntSummary PlayerBotHuntCoordinator::summarizeEquipmentHunts(const std::vector<PlayerBotHuntRegion>& regions,
	bool truncated) const { return huntRuntime.summarizeEquipmentHunts(regions, truncated); }
PlayerBotHuntPatrolOutcome PlayerBotHuntCoordinator::huntPatrolTarget() const { return huntRuntime.patrolTarget(); }
PlayerBotHuntPatrolOutcome PlayerBotHuntCoordinator::observeHuntPatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
	std::chrono::steady_clock::time_point now, uint32_t repeatedStepLimit, uint32_t routeFailureLimit)
{
	PlayerBotHuntPatrolOutcome outcome = huntRuntime.observePatrolNavigation(navigation, now, repeatedStepLimit, routeFailureLimit);
	applyCooldown(outcome.cooldown, now);
	return outcome;
}

std::set<uint64_t> PlayerBotHuntCoordinator::activeHuntCooldowns(std::chrono::steady_clock::time_point now)
{
	std::set<uint64_t> excluded;
	for (auto it = huntRegionCooldowns.begin(); it != huntRegionCooldowns.end();) {
		if (now >= it->second) it = huntRegionCooldowns.erase(it);
		else { excluded.insert(it->first); ++it; }
	}
	return excluded;
}

void PlayerBotHuntCoordinator::applyCooldown(const std::optional<PlayerBotHuntRuntimeCooldownCommand>& command,
	std::chrono::steady_clock::time_point now)
{
	if (command) huntRegionCooldowns[command->variantId] = now + command->duration;
}
