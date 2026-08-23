/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbothuntplanningsession.h"

#include <algorithm>
#include <utility>

PlayerBotHuntPlanningSession::PlayerBotHuntPlanningSession(PlayerBotHuntPlanningStart start) :
	candidateIndices(std::move(start.scan.candidateIndices)), planningReason(std::move(start.reason)),
	planningStarted(start.started), planningProfile(std::move(start.profile)), planningSnapshot(std::move(start.snapshot)),
	candidateCount(static_cast<uint32_t>(start.scan.candidateCount)), scanCacheHit(start.scan.cacheHit),
	scanSnapshotTimeUs(start.scan.snapshotTimeUs), scanClusteringTimeUs(start.scan.clusteringTimeUs)
{
	scoredRegions.reserve(candidateCount);
}

bool PlayerBotHuntPlanningSession::invalidated(const PlayerBotHuntPlanningSnapshot& current) const
{
	return current.playerPosition != planningSnapshot.playerPosition || current.playerLevel != planningSnapshot.playerLevel ||
	       current.currentHealth < planningSnapshot.currentHealth || current.staminaMinutes != planningSnapshot.staminaMinutes ||
	       current.excludedRegions != planningSnapshot.excludedRegions || current.cacheRevision != planningSnapshot.cacheRevision;
}

void PlayerBotHuntPlanningSession::beginTurn()
{
	routeValidationsThisTurn = 0;
	routeValidationCallsThisTurn = 0;
}

std::optional<PlayerBotHuntPlanningScoreWork> PlayerBotHuntPlanningSession::nextScoringWork(uint32_t maximumCandidates)
{
	if (phase != Phase::Scoring || scoringCandidatesThisTurn >= maximumCandidates || nextScoringCandidate >= candidateCount) {
		return std::nullopt;
	}
	++scoringCandidatesThisTurn;
	return PlayerBotHuntPlanningScoreWork{candidateIndices[nextScoringCandidate++]};
}

void PlayerBotHuntPlanningSession::scoreCompleted(PlayerBotHuntRegion region)
{
	scoredRegions.push_back(std::move(region));
	++scoredCandidateCount;
}

PlayerBotHuntPlanningProgress PlayerBotHuntPlanningSession::completeScoring()
{
	scoringCandidatesThisTurn = 0;
	if (nextScoringCandidate < candidateCount) {
		++yieldCount;
		return PlayerBotHuntPlanningProgress::ScoringYield;
	}
	std::stable_sort(scoredRegions.begin(), scoredRegions.end(), [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
		if (left.suitable != right.suitable) return left.suitable;
		if (left.inChallengeBand != right.inChallengeBand) return left.inChallengeBand;
		return left.optimisticProjectedExperience > right.optimisticProjectedExperience;
	});
	uint32_t regionId = 1;
	for (PlayerBotHuntRegion& region : scoredRegions) {
		region.id = regionId++;
	}
	refreshSuitableCandidates();
	phase = Phase::Reachability;
	return PlayerBotHuntPlanningProgress::Scored;
}

std::optional<PlayerBotHuntPlanningRouteWork> PlayerBotHuntPlanningSession::nextRouteValidationWork(uint32_t maximumCalls)
{
	if (phase != Phase::Reachability || routeValidationsThisTurn >= maximumCalls) {
		return std::nullopt;
	}
	if (canStopRouteValidation()) {
		return std::nullopt;
	}
	while (nextCandidate < scoredRegions.size()) {
		const size_t regionIndex = nextCandidate++;
		if (scoredRegions[regionIndex].suitable) {
			scoredRegions[regionIndex].routeValidationAttempted = true;
			++routeValidationsThisTurn;
			return PlayerBotHuntPlanningRouteWork{regionIndex};
		}
	}
	return std::nullopt;
}

void PlayerBotHuntPlanningSession::routeValidationCompleted(size_t regionIndex, bool pathfindingCalled, bool reachable, bool nodeLimit,
	                                                        uint64_t expandedNodes,
	                                                        uint32_t travelSteps, double estimatedTravelSeconds,
	                                                        double staminaExperienceMultiplier, uint32_t huntDurationSeconds)
{
	PlayerBotHuntRegion& region = scoredRegions[regionIndex];
	if (pathfindingCalled) {
		++routeValidationCalls;
		++routeValidationCallsThisTurn;
		routeValidationExpandedNodes += expandedNodes;
	}
	region.expandedNodes += expandedNodes;
	if (!reachable) {
		region.rejectionReason = nodeLimit ? "navigation_node_budget" : "unreachable";
		return;
	}
	region.reachable = true;
	region.travelSteps = travelSteps;
	region.estimatedTravelSeconds = estimatedTravelSeconds;
	region.availableHuntSeconds = std::max(0.0, huntDurationSeconds - estimatedTravelSeconds);
	region.staminaExperienceMultiplier = staminaExperienceMultiplier;
	region.projectedExperience = region.experiencePerMinute * region.observedCorrection *
	                           region.staminaExperienceMultiplier * region.availableHuntSeconds / 60.0;
	region.score = region.projectedExperience;
}

PlayerBotHuntPlanningProgress PlayerBotHuntPlanningSession::completeRouteValidation()
{
	if (routeValidationsThisTurn != 0) {
		++yieldCount;
		return PlayerBotHuntPlanningProgress::ReachabilityYield;
	}
	return PlayerBotHuntPlanningProgress::ReadyForSelection;
}

void PlayerBotHuntPlanningSession::refreshSuitableCandidates()
{
	suitableCandidateCount = static_cast<uint32_t>(std::count_if(scoredRegions.begin(), scoredRegions.end(),
		[](const PlayerBotHuntRegion& region) { return region.suitable; }));
}

bool PlayerBotHuntPlanningSession::canStopRouteValidation()
{
	if (routeBoundSatisfied) return true;
	const auto incumbent = std::max_element(scoredRegions.begin(), scoredRegions.end(), [](const auto& left, const auto& right) {
		return playerBotPreferHuntRegion(right, left);
	});
	if (incumbent == scoredRegions.end() || !incumbent->suitable || !incumbent->reachable) return false;

	for (const PlayerBotHuntRegion& candidate : scoredRegions) {
		if (!candidate.suitable || candidate.routeValidationAttempted) continue;
		if (candidate.inChallengeBand != incumbent->inChallengeBand) {
			if (candidate.inChallengeBand) return false;
			continue;
		}
		if (!std::isfinite(candidate.optimisticProjectedExperience) ||
		    candidate.optimisticProjectedExperience > incumbent->score) return false;
	}

	for (PlayerBotHuntRegion& candidate : scoredRegions) {
		if (candidate.suitable && !candidate.routeValidationAttempted) {
			candidate.routeValidationDeferredByBound = true;
			++deferredRouteValidationCount;
		}
	}
	routeBoundSatisfied = true;
	return true;
}
