/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "playerbothuntplanningsession.h"

#include <algorithm>
#include <utility>

PlayerBotHuntPlanningSession::PlayerBotHuntPlanningSession(PlayerBotHuntPlanningStart start) :
	candidateIndices(std::move(start.scan.candidateIndices)), transportOffers(std::move(start.transportOffers)),
	planningReason(std::move(start.reason)), planningStarted(start.started), planningProfile(std::move(start.profile)),
	planningSnapshot(std::move(start.snapshot)), topologyDistances(std::move(start.topologyDistances)),
	candidateCount(static_cast<uint32_t>(start.scan.candidateCount)), scanCacheHit(start.scan.cacheHit),
	scanSnapshotTimeUs(start.scan.snapshotTimeUs), scanClusteringTimeUs(start.scan.clusteringTimeUs),
	topologyDistanceTimeUs(start.topologyDistanceTimeUs)
{
	scoredRegions.reserve(candidateCount);
	if (start.originReachability && transportOffers && !transportOffers->empty()) {
		transportArrivals.push_back({planningSnapshot.playerPosition, 0, 0, std::move(start.originReachability)});
	} else {
		phase = Phase::Scoring;
	}
}

bool PlayerBotHuntPlanningSession::invalidated(const PlayerBotHuntPlanningSnapshot& current) const
{
	return current.playerPosition != planningSnapshot.playerPosition || current.playerLevel != planningSnapshot.playerLevel ||
	       current.currentHealth < planningSnapshot.currentHealth || current.staminaMinutes != planningSnapshot.staminaMinutes ||
	       current.potions != planningSnapshot.potions || current.mana < planningSnapshot.mana ||
	       current.funds != planningSnapshot.funds ||
	       current.topologyGeneration != planningSnapshot.topologyGeneration ||
	       current.npcGeneration != planningSnapshot.npcGeneration ||
	       current.canUseRope != planningSnapshot.canUseRope || current.canUseShovel != planningSnapshot.canUseShovel ||
	       current.premium != planningSnapshot.premium || current.excludedVariants != planningSnapshot.excludedVariants || current.cacheRevision != planningSnapshot.cacheRevision;
}

void PlayerBotHuntPlanningSession::beginTurn()
{
	if (phase == Phase::Transport) {
		transportTurnArrivals = std::make_shared<const std::vector<PlayerBotHuntTransportArrival>>(transportArrivals);
	}
}

std::optional<PlayerBotHuntPlanningTransportWork> PlayerBotHuntPlanningSession::nextTransportWork(
	uint32_t maximumOffers)
{
	if (phase != Phase::Transport || !transportOffers || transportOffersThisTurn >= maximumOffers ||
	    nextTransportOffer >= transportOffers->size()) return std::nullopt;
	++transportOffersThisTurn;
	const size_t index = nextTransportOffer++;
	return PlayerBotHuntPlanningTransportWork{index, transportOffers->at(index), transportTurnArrivals};
}

void PlayerBotHuntPlanningSession::transportCompleted(
	size_t offerIndex, std::optional<PlayerBotHuntTransportArrival> arrival)
{
	if (phase != Phase::Transport || offerIndex >= (transportOffers ? transportOffers->size() : 0) || !arrival) return;
	auto found = std::find_if(transportArrivals.begin(), transportArrivals.end(), [&arrival](const auto& current) {
		return current.position == arrival->position;
	});
	if (found == transportArrivals.end()) {
		transportArrivals.push_back(std::move(*arrival));
		transportPassChanged = true;
	} else if (std::tie(arrival->fare, arrival->estimatedSteps) <
	           std::tie(found->fare, found->estimatedSteps)) {
		*found = std::move(*arrival);
		transportPassChanged = true;
	}
}

PlayerBotHuntPlanningProgress PlayerBotHuntPlanningSession::completeTransport()
{
	transportOffersThisTurn = 0;
	transportTurnArrivals.reset();
	if (phase != Phase::Transport) return PlayerBotHuntPlanningProgress::Scored;
	if (transportOffers && nextTransportOffer < transportOffers->size()) {
		++yieldCount;
		return PlayerBotHuntPlanningProgress::ScoringYield;
	}
	if (transportPassChanged) {
		transportPassChanged = false;
		nextTransportOffer = 0;
		++yieldCount;
		return PlayerBotHuntPlanningProgress::ScoringYield;
	}
	planningProfile.transportArrivals.clear();
	for (const auto& arrival : transportArrivals) {
		if (arrival.position != planningSnapshot.playerPosition) planningProfile.transportArrivals.push_back(arrival);
	}
	phase = Phase::Scoring;
	return PlayerBotHuntPlanningProgress::Scored;
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
	std::stable_sort(scoredRegions.begin(), scoredRegions.end(), playerBotPreferHuntRegion);
	uint32_t regionId = 1;
	for (PlayerBotHuntRegion& region : scoredRegions) {
		region.id = regionId++;
	}
	refreshSuitableCandidates();
	routeShortlist = playerBotHuntRouteCandidates(scoredRegions);
	phase = Phase::Ready;
	return PlayerBotHuntPlanningProgress::Scored;
}

void PlayerBotHuntPlanningSession::refreshSuitableCandidates()
{
	suitableCandidateCount = static_cast<uint32_t>(std::count_if(scoredRegions.begin(), scoredRegions.end(),
		[](const PlayerBotHuntRegion& region) { return region.suitable; }));
}
