#include "otpch.h"

#include "playerbothuntruntime.h"

#include "playerbotequipmentpolicy.h"

PlayerBotHuntRuntime::PlayerBotHuntRuntime(std::vector<Position> fallbackPatrol) :
	fallbackPatrol(std::move(fallbackPatrol))
{}

PlayerBotHuntPlanningSnapshot PlayerBotHuntRuntime::snapshot(const PlayerBotHuntRuntimePlayerObservation& player, uint64_t revision)
{
	return {player.position, player.level, player.health, player.staminaMinutes, revision, player.topologyGeneration, player.excludedVariants,
	        player.canUseRope, player.canUseShovel};
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
		planning.reset();
		pendingScoreCandidates.clear();
		outcome.staleRevision = true;
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
		outcome.staleRevision = planning->snapshot().cacheRevision != input.cacheRevision;
		planning.reset();
		pendingScoreCandidates.clear();
	}
	if (planning && observation.cancelAtScoreBarrier && !planning->scoring()) {
		planning.reset();
		pendingScoreCandidates.clear();
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningCancelled;
		return outcome;
	}
	if (!planning) {
		if (!input.start) return outcome;
		PlayerBotHuntPlanningProfile profile = planningProfile(input.start->profile);
		planning.emplace(PlayerBotHuntPlanningStart{input.start->scan, std::move(profile),
		                                             snapshot(input.player, input.start->scan.revision),
		                                             input.start->topologyDistances, input.start->topologyDistanceTimeUs,
		                                             input.reason, now});
		plannedHuntDurationSeconds = input.huntDurationSeconds;
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningStarted;
		return outcome;
	}

	planning->beginTurn();
	if (planning->scoring()) {
		while (const auto work = planning->nextScoringWork(256)) {
			pendingScoreCandidates.push_back(work->candidateIndex);
			outcome.scoreWork.push_back({work->candidateIndex, planning->profile(), planning->snapshot().cacheRevision,
				planning->snapshot().excludedVariants, policy.regionPerformance(), planning->topology(),
				plannedHuntDurationSeconds});
		}
		if (outcome.scoreWork.empty()) {
			outcome.command = planning->completeScoring() == PlayerBotHuntPlanningProgress::ScoringYield ?
			                         PlayerBotHuntRuntimeCommand::PlanningYield : PlayerBotHuntRuntimeCommand::PlanningScored;
		}
		return outcome;
	}
	const auto& regions = planning->regions();
	outcome.candidates = regions;
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

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
	                                                                  uint64_t elapsedUs)
{
	PlayerBotHuntRuntimeOutcome outcome;
	if (!planning || !planning->scoring() || observations.size() != pendingScoreCandidates.size()) return outcome;
	for (size_t index = 0; index < observations.size(); ++index) {
		if (!observations[index].valid || observations[index].candidateIndex != pendingScoreCandidates[index]) {
			outcome.staleRevision = true;
			planning.reset();
			pendingScoreCandidates.clear();
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
	outcome.command = PlayerBotHuntRuntimeCommand::PlanningScored;
	return outcome;
}

void PlayerBotHuntRuntime::applyCandidateSuitability(PlayerBotHuntRegion& region,
	const PlayerBotHuntRuntimeScoreObservation& observation) const
{
	region.challengeFrontier = policy.challengeFrontier();
	region.challengeBandMinimum = 0;
	region.challengeBandMaximum = region.challengeFrontier + 0.05;
	region.inChallengeBand = region.threatRatio <= region.challengeBandMaximum;
	region.suitable = observation.candidateFactsAvailable && !region.predictedLethal &&
	                  region.threatRatio <= region.challengeBandMaximum && observation.withinPlanningScope;
	if (planning->snapshot().excludedVariants.find(region.atlasVariantId) != planning->snapshot().excludedVariants.end()) {
		region.suitable = false;
		region.rejectionReason = "observed_danger_cooldown";
	} else if (!observation.withinPlanningScope) {
		region.rejectionReason = "travel_distance";
	} else if (region.predictedLethal) {
		region.rejectionReason = "predicted_lethal";
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
	outcome.candidates = planning->regions();
	++scopeExhaustions;
	outcome.command = PlayerBotHuntRuntimeCommand::ScopeExhausted;
	outcome.scopeExhaustionAttempt = scopeExhaustions;
	outcome.stopForScopeExhaustion = scopeExhaustions >= 3;
	outcome.retryAfter = retryAfter;
	scopeReevaluationAfter = now + retryAfter;
	planning.reset();
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
	                                                std::chrono::steady_clock::duration grace) const
{
	if (!activeRegion || capacityPressureStarted == std::chrono::steady_clock::time_point{}) return false;
	return now - capacityPressureStarted >= grace;
}

bool PlayerBotHuntRuntime::matchesMonster(const std::string& name) const
{
	return !activeRegion || std::any_of(activeRegion->monsters.begin(), activeRegion->monsters.end(), [&name](const auto& monster) {
		return strcasecmp(monster.name.c_str(), name.c_str()) == 0;
	});
}

PlayerBotHuntPlanningProfile PlayerBotHuntRuntime::planningProfile(PlayerBotHuntPlanningProfile profile) const
{
	profile.challengeFrontier = policy.challengeFrontier();
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
	result.performance = policy.observePerformance(activeRegion->atlasVariantId, {result.durationSeconds, result.combat.kills, result.experienceGained,
		activeRegion->projectedExperience, activeRegion->observedCorrection, configuredDurationSeconds});
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
	outcome.command = activeRegion ? PlayerBotHuntPatrolCommand::SkipWaypoint : PlayerBotHuntPatrolCommand::SkipWaypoint;
	outcome.reason = oscillating ? "position_oscillation" : repeatedSteps ? "repeated_step_failure" : "route_unavailable";
	outcome.stepFailures = navigation.stepFailureCount;
	outcome.routeFailures = patrolRouteFailures;
	outcome.expandedNodes = patrolFailureExpandedNodes;
	outcome.elapsedMs = patrolFailureStarted == std::chrono::steady_clock::time_point{} ? 0 : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - patrolFailureStarted).count());
	if (activeRegion) {
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
