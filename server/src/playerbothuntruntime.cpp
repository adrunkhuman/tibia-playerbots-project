#include "definitions.h"
#include "playerbothuntruntime.h"
#include "playerbotequipmentpolicy.h"

#include <cstring>
#ifndef _WIN32
#include <strings.h>
#endif

PlayerBotHuntRuntime::PlayerBotHuntRuntime(std::vector<Position> fallbackPatrol) :
	fallbackPatrol(std::move(fallbackPatrol))
{}

PlayerBotHuntPlanningSnapshot PlayerBotHuntRuntime::snapshot(const PlayerBotHuntRuntimePlayerObservation& player, uint64_t revision)
{
	return {player.position, player.level, player.health, player.staminaMinutes, revision, player.topologyGeneration,
	        player.npcGeneration, player.excludedVariants,
	        player.canUseRope, player.canUseShovel, player.premium, player.potions, player.mana, player.funds};
}

bool PlayerBotHuntRuntime::planningStartRequired(std::chrono::steady_clock::time_point now) const
{
	return !planning && now >= scopeReevaluationAfter;
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::advancePlanning(const PlayerBotHuntRuntimePlanningInput& input,
	                                                                std::chrono::steady_clock::time_point now,
	                                                                const PlayerBotHuntPlanningObservation& observation)
{
	PlayerBotHuntRuntimeOutcome outcome;
	if (observation.invalidateCacheRevision) {
		invalidatePlanning(outcome, "cache_revision_invalidated");
		outcome.invalidateCache = true;
		return outcome;
	}
	const PlayerBotHuntPlanningSnapshot current = snapshot(input.player, input.cacheRevision);
	if (!planning && now < scopeReevaluationAfter) {
		outcome.command = PlayerBotHuntRuntimeCommand::ScopeReevaluationPending;
		outcome.retryAfter = scopeReevaluationAfter - now;
		return outcome;
	}
	if (planning && planning->invalidated(current)) {
		const char* cancellationReason = planningInvalidationReason(current);
		invalidatePlanning(outcome, cancellationReason,
		                   planning->snapshot().cacheRevision != input.cacheRevision);
	}
	if (planning && observation.cancelAtScoreBarrier && pendingTransportOffers.empty() &&
	    pendingScoreCandidates.empty()) {
		outcome = cancelPlanning();
		outcome.cancellationReason = "fixture_cancelled";
		return outcome;
	}
	if (!planning) {
		if (!input.start) return outcome;
		PlayerBotHuntPlanningProfile profile = planningProfile(input.start->profile);
		planning.emplace(PlayerBotHuntPlanningStart{input.start->scan, std::move(profile),
		                                             snapshot(input.player, input.start->scan.revision),
		                                             input.start->topologyDistances, input.start->originReachability,
		                                             input.start->transportOffers, input.start->topologyDistanceTimeUs,
		                                             input.reason, now});
		plannedHuntDurationSeconds = input.huntDurationSeconds;
		currentPlanningPass = ++nextPlanningPass;
		attributeOutcome(outcome);
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningStarted;
		return outcome;
	}

	attributeOutcome(outcome);
	planning->beginTurn();
	if (planning->transportPlanning()) {
		while (const auto work = planning->nextTransportWork(8)) {
			pendingTransportOffers.push_back(work->offerIndex);
			outcome.transportWork.push_back({work->offerIndex, work->offer, std::move(work->arrivals),
			                                 planning->snapshot().playerLevel, planning->snapshot().funds,
			                                 planning->snapshot().premium, planning->snapshot().canUseRope,
			                                 planning->snapshot().canUseShovel});
		}
		if (outcome.transportWork.empty()) {
			outcome.command = planning->completeTransport() == PlayerBotHuntPlanningProgress::ScoringYield ?
			                         PlayerBotHuntRuntimeCommand::PlanningYield : PlayerBotHuntRuntimeCommand::PlanningScored;
		}
		return outcome;
	}
	if (planning->scoring()) {
		while (const auto work = planning->nextScoringWork(256)) {
			pendingScoreCandidates.push_back(work->candidateIndex);
			outcome.scoreWork.push_back({work->candidateIndex, planning->profile(), planning->snapshot().cacheRevision,
				planning->snapshot().excludedVariants, policy.regionPerformance(), planning->topology(),
				plannedHuntDurationSeconds});
		}
		if (outcome.scoreWork.empty()) {
			if (planning->completeScoring() == PlayerBotHuntPlanningProgress::ScoringYield) {
				outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
			} else {
				outcome.candidateSnapshot = true;
				outcome.candidates = planning->regions();
				outcome.routeCandidates = planning->routeCandidates();
				outcome.command = PlayerBotHuntRuntimeCommand::PlanningScored;
			}
		}
		return outcome;
	}
	const auto& regions = planning->regions();
	if (!observation.candidatesAvailable) return exhaustScope(now, std::chrono::seconds(1));
	if (playerBotHuntScopeExhausted(regions)) {
		return exhaustScope(now, std::chrono::seconds(30));
	}
	auto selected = std::max_element(regions.begin(), regions.end(), [](const auto& left, const auto& right) {
		return playerBotPreferHuntRegion(right, left);
	});
	outcome.command = PlayerBotHuntRuntimeCommand::RegionSelected;
	outcome.selectedRegion = *selected;
	scopeExhaustions = 0;
	scopeReevaluationAfter = {};
	return outcome;
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::completeTransportWork(
	const std::vector<PlayerBotHuntRuntimeTransportObservation>& observations)
{
	PlayerBotHuntRuntimeOutcome outcome;
	attributeOutcome(outcome);
	if (!planning || !planning->transportPlanning() || observations.size() != pendingTransportOffers.size()) return outcome;
	for (size_t index = 0; index < observations.size(); ++index) {
		if (observations[index].offerIndex != pendingTransportOffers[index]) {
			invalidatePlanning(outcome, "transport_observation_changed");
			return outcome;
		}
		planning->transportCompleted(observations[index].offerIndex, observations[index].arrival);
	}
	pendingTransportOffers.clear();
	if (planning->completeTransport() == PlayerBotHuntPlanningProgress::ScoringYield) {
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
		return outcome;
	}
	// Scoring starts on the next scheduler turn; do not combine the transport
	// barrier with a 256-candidate scoring batch.
	outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
	return outcome;
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
	                                                                  uint64_t elapsedUs)
{
	PlayerBotHuntRuntimeOutcome outcome;
	attributeOutcome(outcome);
	if (!planning || !planning->scoring() || observations.size() != pendingScoreCandidates.size()) return outcome;
	for (size_t index = 0; index < observations.size(); ++index) {
		if (!observations[index].valid || observations[index].candidateIndex != pendingScoreCandidates[index]) {
			invalidatePlanning(outcome, "score_observation_changed");
			return outcome;
		}
		PlayerBotHuntRegion region = observations[index].region;
		applyCandidateSuitability(region, observations[index]);
		planning->scoreCompleted(std::move(region));
	}
	pendingScoreCandidates.clear();
	planning->addScoringTime(elapsedUs);
	if (planning->completeScoring() == PlayerBotHuntPlanningProgress::ScoringYield) {
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
		return outcome;
	}
	outcome.candidateSnapshot = true;
	outcome.candidates = planning->regions();
	outcome.routeCandidates = planning->routeCandidates();
	outcome.command = PlayerBotHuntRuntimeCommand::PlanningScored;
	return outcome;
}

void PlayerBotHuntRuntime::attributeOutcome(PlayerBotHuntRuntimeOutcome& outcome) const
{
	if (!planning) return;
	outcome.planningPass = currentPlanningPass;
	outcome.scoringRevision = planning->snapshot().cacheRevision;
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::cancelPlanning()
{
	PlayerBotHuntRuntimeOutcome outcome;
	attributeOutcome(outcome);
	if (outcome.planningPass == 0) return outcome;
	outcome.command = PlayerBotHuntRuntimeCommand::PlanningCancelled;
	outcome.planningCancelled = true;
	outcome.cancellationReason = "planning_cancelled";
	planning.reset();
	currentPlanningPass = 0;
	pendingTransportOffers.clear();
	pendingScoreCandidates.clear();
	return outcome;
}

const char* PlayerBotHuntRuntime::planningInvalidationReason(const PlayerBotHuntPlanningSnapshot& current) const
{
	const PlayerBotHuntPlanningSnapshot& previous = planning->snapshot();
	if (current.cacheRevision != previous.cacheRevision) return "cache_revision_changed";
	if (current.topologyGeneration != previous.topologyGeneration) return "topology_changed";
	if (current.npcGeneration != previous.npcGeneration) return "npc_topology_changed";
	if (current.playerPosition != previous.playerPosition) return "position_changed";
	if (current.playerLevel != previous.playerLevel) return "level_changed";
	if (current.currentHealth < previous.currentHealth) return "health_decreased";
	if (current.staminaMinutes != previous.staminaMinutes) return "stamina_changed";
	if (current.potions != previous.potions) return "potions_changed";
	if (current.mana < previous.mana) return "mana_decreased";
	if (current.funds != previous.funds) return "funds_changed";
	if (current.canUseRope != previous.canUseRope || current.canUseShovel != previous.canUseShovel ||
	    current.premium != previous.premium) return "travel_capability_changed";
	return "hunt_cooldowns_changed";
}

void PlayerBotHuntRuntime::invalidatePlanning(PlayerBotHuntRuntimeOutcome& outcome, const char* cancellationReason,
	bool staleRevision)
{
	attributeOutcome(outcome);
	if (outcome.planningPass == 0) return;
	outcome.invalidatedPlanningPass = outcome.planningPass;
	outcome.invalidatedScoringRevision = outcome.scoringRevision;
	outcome.planningCancelled = true;
	outcome.cancellationReason = cancellationReason;
	outcome.staleRevision = staleRevision;
	planning.reset();
	currentPlanningPass = 0;
	pendingTransportOffers.clear();
	pendingScoreCandidates.clear();
}

void PlayerBotHuntRuntime::applyCandidateSuitability(PlayerBotHuntRegion& region,
	const PlayerBotHuntRuntimeScoreObservation& observation) const
{
	const double frontier = playerBotSupplyRecoveryChallengeFrontier(policy.challengeFrontier(), supplyRecoveryDegraded);
	region.challengeFrontier = frontier;
	region.challengeBandMinimum = 0;
	region.challengeBandMaximum = frontier + 0.05;
	region.inChallengeBand = region.threatRatio <= region.challengeBandMaximum;
	region.suitable = observation.candidateFactsAvailable && region.sustainedEligible && !region.predictedLethal &&
	                  region.threatRatio <= region.challengeBandMaximum && observation.withinPlanningScope &&
	                  region.transportPlausible && region.recoverySustainable();
	if (planning->snapshot().excludedVariants.find(region.atlasVariantId) != planning->snapshot().excludedVariants.end()) {
		region.suitable = false;
		region.rejectionReason = "observed_danger_cooldown";
	} else if (!observation.withinPlanningScope) {
		region.rejectionReason = "travel_distance";
	} else if (!region.transportPlausible) {
		region.rejectionReason = "transport_requirements_unavailable";
	} else if (region.predictedLethal) {
		region.rejectionReason = "predicted_lethal";
	} else if (!region.sustainedEligible) {
		region.rejectionReason = playerBotHuntViabilityRejection(region.viability);
	} else if (!region.recoverySustainable()) {
		region.rejectionReason = "recovery_hunt_not_sustainable";
	} else if (!region.suitable) {
		region.rejectionReason = "challenge_frontier";
	} else {
		region.rejectionReason.clear();
	}
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::exhaustScope(std::chrono::steady_clock::time_point now,
	std::chrono::steady_clock::duration retryAfter)
{
	PlayerBotHuntRuntimeOutcome outcome;
	attributeOutcome(outcome);
	scopeExhaustions = std::min<uint32_t>(scopeExhaustions + 1, 3);
	outcome.command = PlayerBotHuntRuntimeCommand::ScopeExhausted;
	outcome.scopeExhaustionAttempt = scopeExhaustions;
	outcome.stopForScopeExhaustion = !supplyRecoveryDegraded && scopeExhaustions >= 3;
	outcome.retryAfter = retryAfter;
	scopeReevaluationAfter = now + retryAfter;
	planning.reset();
	currentPlanningPass = 0;
	pendingTransportOffers.clear();
	pendingScoreCandidates.clear();
	return outcome;
}

void PlayerBotHuntRuntime::activate(PlayerBotHuntRegion region, const PlayerBotHuntRuntimePlayerObservation& player,
	                                 std::chrono::steady_clock::time_point now)
{
	auto first = std::find(region.patrolPoints.begin(), region.patrolPoints.end(), region.destination);
	if (first != region.patrolPoints.end()) std::rotate(region.patrolPoints.begin(), first, region.patrolPoints.end());
	activeRegion = std::move(region);
	patrolIndex = 0;
	singleWaypointReached = false;
	huntStarted = now;
	huntStartExperience = player.experience;
	coinGoldAcquired = 0;
	huntStartLevel = player.level;
	policy.resetCombatEvidence();
	resetPatrolFailures();
}

void PlayerBotHuntRuntime::beginCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds)
{
	singleWaypointReached = false;
	huntDeadline = now + std::chrono::seconds(durationSeconds);
	capacityPressureStarted = {};
	++cycles;
}

void PlayerBotHuntRuntime::observeCapacityPressure(std::chrono::steady_clock::time_point now)
{
	if (activeRegion && capacityPressureStarted == std::chrono::steady_clock::time_point{}) {
		capacityPressureStarted = now;
	}
}

bool PlayerBotHuntRuntime::capacityPressureElapsed(std::chrono::steady_clock::time_point now,
	                                                std::chrono::steady_clock::duration grace,
	                                                std::chrono::steady_clock::duration minimumHunt) const
{
	if (!activeRegion || capacityPressureStarted == std::chrono::steady_clock::time_point{}) return false;
	if (minimumHunt > std::chrono::steady_clock::duration{} && huntStarted != std::chrono::steady_clock::time_point{} &&
	    now - huntStarted < minimumHunt) {
		return false;
	}
	return now - capacityPressureStarted >= grace;
}

bool PlayerBotHuntRuntime::matchesMonster(const std::string& name) const
{
	return !activeRegion || std::any_of(activeRegion->monsters.begin(), activeRegion->monsters.end(), [&name](const auto& monster) {
		return strcasecmp(monster.name.c_str(), name.c_str()) == 0;
	});
}

bool PlayerBotHuntRuntime::insideHuntArea(const Position& position, uint32_t westRange, uint32_t eastRange,
	uint32_t northRange, uint32_t southRange) const
{
	return activeRegion && std::any_of(activeRegion->patrolPoints.begin(), activeRegion->patrolPoints.end(),
		[&position, westRange, eastRange, northRange, southRange](const Position& patrolPoint) {
			const int32_t deltaX = static_cast<int32_t>(patrolPoint.x) - position.x;
			const int32_t deltaY = static_cast<int32_t>(patrolPoint.y) - position.y;
			return position.z == patrolPoint.z && deltaX >= -static_cast<int32_t>(westRange) &&
			       deltaX <= static_cast<int32_t>(eastRange) && deltaY >= -static_cast<int32_t>(northRange) &&
			       deltaY <= static_cast<int32_t>(southRange);
		});
}

PlayerBotHuntPlanningProfile PlayerBotHuntRuntime::planningProfile(PlayerBotHuntPlanningProfile profile) const
{
	profile.challengeFrontier = playerBotSupplyRecoveryChallengeFrontier(policy.challengeFrontier(), supplyRecoveryDegraded);
	profile.supplyRecovery = supplyRecoveryDegraded;
	return profile;
}

PlayerBotEquipmentHuntSummary PlayerBotHuntRuntime::summarizeEquipmentHunts(const std::vector<PlayerBotHuntRegion>& regions,
	bool truncated) const
{
	PlayerBotEquipmentHuntSummary summary;
	summary.lowestThreatRatio = std::numeric_limits<double>::max();
	for (const PlayerBotHuntRegion& region : regions) {
		++summary.evaluatedRegions;
		summary.lowestThreatRatio = std::min(summary.lowestThreatRatio, region.threatRatio);
		if (region.suitable) {
			++summary.suitableRegions;
			summary.bestProjectedExperience = std::max(summary.bestProjectedExperience, region.projectedExperience);
		}
	}
	summary.truncated = truncated;
	if (summary.lowestThreatRatio == std::numeric_limits<double>::max()) summary.lowestThreatRatio = 0;
	return summary;
}

std::optional<PlayerBotHuntRuntimeCooldownCommand> PlayerBotHuntRuntime::dangerObserved(int32_t maximumHealth,
	std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown)
{
	if (!activeRegion || !policy.observeDanger(maximumHealth, now - huntStarted)) return std::nullopt;
	return {{activeRegion->atlasVariantId, cooldown}};
}

std::optional<PlayerBotHuntRuntimeCompletion> PlayerBotHuntRuntime::complete(const PlayerBotHuntRuntimePlayerObservation& player,
	                                                                           std::chrono::steady_clock::time_point now,
	                                                                           uint32_t configuredDurationSeconds)
{
	if (!activeRegion) return std::nullopt;
	PlayerBotHuntRuntimeCompletion result;
	result.region = *activeRegion;
	result.durationSeconds = static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(now - huntStarted).count()));
	result.experienceGained = player.experience >= huntStartExperience ? player.experience - huntStartExperience : 0;
	result.levelBefore = huntStartLevel;
	result.combat = policy.combatSummary();
	result.coinGoldAcquired = coinGoldAcquired;
	result.performance = policy.observePerformance(activeRegion->atlasVariantId, activeRegion->atlasRevision,
	    {result.durationSeconds, result.combat.activeSeconds, result.combat.kills, result.experienceGained,
	     activeRegion->projectedExperience, activeRegion->observedCorrection,
	     activeRegion->supplyRecovery ? std::min<uint32_t>(configuredDurationSeconds, 120) : configuredDurationSeconds,
	     result.combat.dangerObserved, result.combat.deathObserved});
	result.supplyObservation = policy.observeSupplies(*activeRegion, player.supplyCapability,
	    result.durationSeconds, player.health, player.maximumHealth, player.mana, player.potions,
	    player.supplyInterrupted);
	result.region.supplyCalibration = result.supplyObservation.calibration;
	result.challenge = policy.updateChallengeFrontier({result.durationSeconds, player.maximumHealth});
	activeRegion.reset();
	capacityPressureStarted = {};
	policy.resetCombatEvidence();
	return result;
}

std::optional<PlayerBotHuntRuntimeCooldownCommand> PlayerBotHuntRuntime::observeDeath(bool activeCombat,
	std::chrono::steady_clock::duration cooldown)
{
	if (activeCombat) policy.observeDeath();
	if (!activeRegion) return std::nullopt;
	return {{activeRegion->atlasVariantId, cooldown}};
}

PlayerBotHuntPatrolOutcome PlayerBotHuntRuntime::patrolTarget() const
{
	PlayerBotHuntPatrolOutcome outcome;
	const auto& points = activeRegion && !activeRegion->patrolPoints.empty() ? activeRegion->patrolPoints : fallbackPatrol;
	if (points.empty()) return outcome;
	outcome.destination = points[patrolIndex % points.size()];
	outcome.waypoint = static_cast<uint32_t>(patrolIndex);
	if (activeRegion) outcome.regionId = activeRegion->id;
	return outcome;
}

PlayerBotHuntPatrolOutcome PlayerBotHuntRuntime::observePatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
	                                                                      std::chrono::steady_clock::time_point now,
	                                                                      uint32_t repeatedStepLimit, uint32_t routeFailureLimit)
{
	PlayerBotHuntPatrolOutcome outcome = patrolTarget();
	if (navigation.destinationReached) {
		resetPatrolFailures();
		const auto& points = activeRegion && !activeRegion->patrolPoints.empty() ? activeRegion->patrolPoints : fallbackPatrol;
		if (points.size() == 1) {
			outcome.command = singleWaypointReached ? PlayerBotHuntPatrolCommand::WaitAtWaypoint :
			                  PlayerBotHuntPatrolCommand::WaypointReached;
			singleWaypointReached = true;
			return outcome;
		}
		if (!points.empty()) patrolIndex = (patrolIndex + 1) % points.size();
		outcome.command = PlayerBotHuntPatrolCommand::WaypointReached;
		outcome.waypoint = static_cast<uint32_t>(patrolIndex);
		return outcome;
	}
	if (navigation.plan.attempted && navigation.routeUnavailable) {
		if (patrolFailureTarget != outcome.destination) resetPatrolFailures();
		patrolFailureTarget = outcome.destination;
		if (patrolRouteFailures++ == 0) patrolFailureStarted = now;
		patrolFailureExpandedNodes += navigation.plan.expandedNodes;
	}
	const bool oscillating = navigation.oscillation.has_value();
	const bool repeatedSteps = navigation.stepFailureCount >= repeatedStepLimit;
	const bool repeatedRoutes = patrolRouteFailures >= routeFailureLimit;
	if (!oscillating && !repeatedSteps && !repeatedRoutes) return outcome;
	outcome.command = PlayerBotHuntPatrolCommand::SkipWaypoint;
	outcome.reason = oscillating ? "position_oscillation" : repeatedSteps ? "repeated_step_failure" : "route_unavailable";
	outcome.stepFailures = navigation.stepFailureCount;
	outcome.routeFailures = patrolRouteFailures;
	outcome.expandedNodes = patrolFailureExpandedNodes;
	outcome.elapsedMs = patrolFailureStarted == std::chrono::steady_clock::time_point{} ? 0 : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - patrolFailureStarted).count());
	if (activeRegion && activeRegion->viability.reachableSpawns != 0) {
		// Eligibility describes the original circuit. Removing even one point
		// invalidates its absence windows; return for service and replan rather
		// than reuse that eligibility for a reduced, potentially blocked patrol.
		outcome.command = PlayerBotHuntPatrolCommand::RegionExhausted;
		outcome.cooldown = {{activeRegion->atlasVariantId, std::chrono::minutes(10)}};
	} else if (activeRegion) {
		activeRegion->patrolPoints.erase(activeRegion->patrolPoints.begin() + patrolIndex);
		if (activeRegion->patrolPoints.empty()) {
			outcome.command = PlayerBotHuntPatrolCommand::RegionExhausted;
			outcome.cooldown = {{activeRegion->atlasVariantId, std::chrono::minutes(10)}};
		} else patrolIndex %= activeRegion->patrolPoints.size();
	} else if (!fallbackPatrol.empty()) patrolIndex = (patrolIndex + 1) % fallbackPatrol.size();
	resetPatrolFailures();
	return outcome;
}

void PlayerBotHuntRuntime::resetPatrolFailures()
{
	patrolRouteFailures = 0;
	patrolFailureExpandedNodes = 0;
	patrolFailureTarget = Position();
	patrolFailureStarted = {};
}
