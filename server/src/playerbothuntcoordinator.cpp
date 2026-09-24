#include "otpch.h"

#include "playerbothuntcoordinator.h"
#include "playerbotequipmentpolicy.h"

#include <algorithm>
#include <utility>

PlayerBotHuntCoordinator::PlayerBotHuntCoordinator(
	PlayerBotHuntCoordinatorConfig config, std::map<uint64_t, std::chrono::steady_clock::time_point>& sharedCooldowns) :
	combatRuntime(std::move(config.combat)), lootWorkflow(std::move(config.loot)),
	huntRuntime(std::move(config.fallbackPatrol)), huntRegionCooldowns(sharedCooldowns)
{}

std::optional<PlayerBotCombatDecision> PlayerBotHuntCoordinator::selectTraversalAttack(
	std::vector<PlayerBotTraversalCandidate> candidates, const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	if (transitCombat.active()) return std::nullopt;
	return combatRuntime.selectTraversalAttack(std::move(candidates), currentPosition, now);
}

std::optional<PlayerBotCombatDecision> PlayerBotHuntCoordinator::selectDefensiveAttack(
	std::vector<PlayerBotDefensiveTarget> candidates, const Position& currentPosition) const
{
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [this](const PlayerBotDefensiveTarget& target) {
		return !transitCombat.allowsDefense(target.id, target.routeCritical);
	}), candidates.end());
	return combatRuntime.selectDefensiveAttack(std::move(candidates), currentPosition);
}

PlayerBotCombatDecision PlayerBotHuntCoordinator::confirmCombatAttack(const PlayerBotCombatDecision& command, bool accepted,
	std::chrono::steady_clock::time_point now)
{
	const auto result = combatRuntime.confirmAttack(command, accepted, now);
	if (accepted && result.command == PlayerBotCombatCommand::AttackDefensive) {
		transitCombat.beginDefense(command.target.id, command.intendedStep);
	}
	return result;
}

PlayerBotCombatDecision PlayerBotHuntCoordinator::advanceCombat(const PlayerBotCombatSnapshot& snapshot) { return combatRuntime.advance(snapshot); }
void PlayerBotHuntCoordinator::suppressTraversalTarget(uint32_t id, std::chrono::steady_clock::time_point now,
	                                                    std::chrono::steady_clock::duration suppression)
{
	combatRuntime.suppressTraversalTarget(id, now, suppression);
}
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
void PlayerBotHuntCoordinator::cancelPendingLootMove() { lootWorkflow.cancelPendingLootMove(); }
void PlayerBotHuntCoordinator::cancelPendingDiscardMove() { lootWorkflow.cancelPendingDiscardMove(); }
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

PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::cancelPlanning() { return huntRuntime.cancelPlanning(); }
bool PlayerBotHuntCoordinator::planningStartRequired(std::chrono::steady_clock::time_point now) const { return huntRuntime.planningStartRequired(now); }
bool PlayerBotHuntCoordinator::planningActive() const { return huntRuntime.planningActive(); }
PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::advancePlanning(const PlayerBotHuntRuntimePlanningInput& input,
	std::chrono::steady_clock::time_point now, const PlayerBotHuntPlanningObservation& observation)
{
	return huntRuntime.advancePlanning(input, now, observation);
}
PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::completeTransportWork(
	const std::vector<PlayerBotHuntRuntimeTransportObservation>& observations)
{
	return huntRuntime.completeTransportWork(observations);
}
PlayerBotHuntRuntimeOutcome PlayerBotHuntCoordinator::completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
	uint64_t elapsedUs) { return huntRuntime.completeScoreWork(observations, elapsedUs); }
std::optional<PlayerBotHuntPlanningSession> PlayerBotHuntCoordinator::planningSession() const { return huntRuntime.planningSession(); }
void PlayerBotHuntCoordinator::completePlanningSelection() { huntRuntime.completePlanningSelection(); }
bool PlayerBotHuntCoordinator::beginRouteSelection(uint64_t pass, uint64_t revision,
    const std::vector<PlayerBotHuntRegion>& candidates)
{
	return huntRuntime.beginRouteSelection(pass, revision, candidates);
}
std::optional<PlayerBotHuntRouteRequest> PlayerBotHuntCoordinator::nextRouteRequest()
{
	return huntRuntime.nextRouteRequest();
}
PlayerBotHuntRouteResult PlayerBotHuntCoordinator::observeRoute(const PlayerBotHuntRouteRequest& request,
    const PlayerBotHuntRouteObservation& observation)
{
	return huntRuntime.observeRoute(request, observation);
}
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
void PlayerBotHuntCoordinator::enterHuntArea(const PlayerBotHuntRuntimePlayerObservation& player,
	const PlayerBotSupplyProfile& supplyProfile, std::chrono::steady_clock::time_point now)
{
	transitCombat.finish();
	huntRuntime.enterHuntArea(player, supplyProfile, now);
}
bool PlayerBotHuntCoordinator::insideHuntArea(const Position& position, uint32_t westRange, uint32_t eastRange,
	uint32_t northRange, uint32_t southRange) const
{
	return huntRuntime.insideHuntArea(position, westRange, eastRange, northRange, southRange);
}
PlayerBotHuntTurnObservation PlayerBotHuntCoordinator::observeTurn(bool inHuntPhase, bool selectRegion,
	std::chrono::steady_clock::time_point now) const
{
	return {inHuntPhase && selectRegion && !huntRuntime.active() && !huntRuntime.planningActive(),
	        huntRuntime.planningActive(), lootWorkflow.navigationSuspended(),
	        inHuntPhase && huntRuntime.deadlineReached(now)};
}
bool PlayerBotHuntCoordinator::matchesHuntMonster(const std::string& name) const { return huntRuntime.matchesMonster(name); }
void PlayerBotHuntCoordinator::sampleHuntCombat(const PlayerBotHuntCombatSnapshot& snapshot) { huntRuntime.sampleCombat(snapshot); }
void PlayerBotHuntCoordinator::observeHuntDamage(uint32_t damage) { huntRuntime.observeDamage(damage); }
void PlayerBotHuntCoordinator::observeHuntRecovery(bool potion) { huntRuntime.observeRecovery(potion); }
void PlayerBotHuntCoordinator::observeHuntLevelRestoration(uint32_t health, uint32_t mana)
{
	huntRuntime.observeLevelRestoration(health, mana);
}
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
