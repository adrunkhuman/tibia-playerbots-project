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
	topologyDistances(std::move(start.topologyDistances)), topologyDistanceTimeUs(start.topologyDistanceTimeUs),
	candidateCount(static_cast<uint32_t>(start.scan.candidateCount)), scanCacheHit(start.scan.cacheHit),
	scanSnapshotTimeUs(start.scan.snapshotTimeUs), scanClusteringTimeUs(start.scan.clusteringTimeUs)
{
	scoredRegions.reserve(candidateCount);
}

bool PlayerBotHuntPlanningSession::invalidated(const PlayerBotHuntPlanningSnapshot& current) const
{
	return current.playerPosition != planningSnapshot.playerPosition || current.playerLevel != planningSnapshot.playerLevel ||
	       current.currentHealth < planningSnapshot.currentHealth || current.staminaMinutes != planningSnapshot.staminaMinutes ||
	       current.topologyGeneration != planningSnapshot.topologyGeneration ||
	       current.canUseRope != planningSnapshot.canUseRope || current.canUseShovel != planningSnapshot.canUseShovel ||
	       current.excludedVariants != planningSnapshot.excludedVariants || current.cacheRevision != planningSnapshot.cacheRevision;
}

void PlayerBotHuntPlanningSession::beginTurn()
{
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
		return left.score > right.score;
	});
	uint32_t regionId = 1;
	for (PlayerBotHuntRegion& region : scoredRegions) {
		region.id = regionId++;
	}
	refreshSuitableCandidates();
	phase = Phase::Ready;
	return PlayerBotHuntPlanningProgress::Scored;
}

void PlayerBotHuntPlanningSession::refreshSuitableCandidates()
{
	suitableCandidateCount = static_cast<uint32_t>(std::count_if(scoredRegions.begin(), scoredRegions.end(),
		[](const PlayerBotHuntRegion& region) { return region.suitable; }));
}
