/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTHUNTPLANNINGSESSION_H
#define FS_PLAYERBOTHUNTPLANNINGSESSION_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "playerbothuntregions.h"

struct PlayerBotHuntPlanningSnapshot {
	Position playerPosition;
	uint32_t playerLevel = 0;
	int32_t currentHealth = 0;
	uint16_t staminaMinutes = 0;
	uint64_t cacheRevision = 0;
	uint64_t topologyGeneration = 0;
	std::set<Position> excludedRegions;
	bool canUseRope = false;
	bool canUseShovel = false;
};

struct PlayerBotHuntPlanningStart {
	PlayerBotHuntRegionScan scan;
	PlayerBotHuntPlanningProfile profile;
	PlayerBotHuntPlanningSnapshot snapshot;
	std::shared_ptr<const PlayerBotTopologyDistances> topologyDistances;
	uint64_t topologyDistanceTimeUs = 0;
	std::string reason;
	std::chrono::steady_clock::time_point started;
};

struct PlayerBotHuntPlanningScoreWork {
	size_t candidateIndex = 0;
};

struct PlayerBotHuntPlanningRouteWork {
	size_t regionIndex = 0;
};

enum class PlayerBotHuntPlanningProgress : uint8_t {
	ScoringYield,
	Scored,
	ReachabilityYield,
	ReadyForSelection,
};

// A session retains its start snapshot through scoring and then reachability.
// For each batch, consume work until none remains and report each item exactly
// once before calling the matching complete method. completeScoring sorts the
// regions and advances the phase when all candidates are scored; otherwise it
// resets the scoring budget for the next turn. beginTurn resets reachability
// limits, and completeRouteValidation reports whether another turn is needed.
// Discard the session when invalidated() reports that its snapshot is stale.
class PlayerBotHuntPlanningSession
{
	public:
		explicit PlayerBotHuntPlanningSession(PlayerBotHuntPlanningStart start);

		bool invalidated(const PlayerBotHuntPlanningSnapshot& current) const;
		bool scoring() const { return phase == Phase::Scoring; }
		const PlayerBotHuntPlanningProfile& profile() const { return planningProfile; }
		const PlayerBotHuntPlanningSnapshot& snapshot() const { return planningSnapshot; }
		const std::shared_ptr<const PlayerBotTopologyDistances>& topology() const { return topologyDistances; }
		const std::string& reason() const { return planningReason; }
		std::chrono::steady_clock::time_point started() const { return planningStarted; }
		bool cacheHit() const { return scanCacheHit; }
		uint64_t snapshotTimeUs() const { return scanSnapshotTimeUs; }
		uint64_t clusteringTimeUs() const { return scanClusteringTimeUs; }
		uint64_t topologyTimeUs() const { return topologyDistanceTimeUs; }
		uint64_t scoringTimeUs() const { return totalScoringTimeUs; }
		uint32_t totalCandidates() const { return candidateCount; }
		uint32_t scoredCandidates() const { return scoredCandidateCount; }
		uint32_t suitableCandidates() const { return suitableCandidateCount; }
		uint32_t pathfindingCalls() const { return routeValidationCalls; }
		uint32_t batchPathfindingCalls() const { return routeValidationCallsThisTurn; }
		uint64_t expandedNodes() const { return routeValidationExpandedNodes; }
		uint32_t yields() const { return yieldCount; }
		uint32_t deferredRouteValidations() const { return deferredRouteValidationCount; }
		bool boundSatisfied() const { return routeBoundSatisfied; }
		bool topologySelection() const { return topologyDistances != nullptr; }

		void beginTurn();
		std::optional<PlayerBotHuntPlanningScoreWork> nextScoringWork(uint32_t maximumCandidates);
		void scoreCompleted(PlayerBotHuntRegion region);
		void addScoringTime(uint64_t elapsedUs) { totalScoringTimeUs += elapsedUs; }
		PlayerBotHuntPlanningProgress completeScoring();

		std::optional<PlayerBotHuntPlanningRouteWork> nextRouteValidationWork(uint32_t maximumCalls);
		void routeValidationCompleted(size_t regionIndex, bool pathfindingCalled, bool reachable, bool nodeLimit, uint64_t expandedNodes,
		                              uint32_t travelSteps, double estimatedTravelSeconds,
		                              double staminaExperienceMultiplier, const PlayerBotHuntCorridorDanger& corridorDanger,
		                              uint32_t huntDurationSeconds);
		PlayerBotHuntPlanningProgress completeRouteValidation();

		const std::vector<PlayerBotHuntRegion>& regions() const { return scoredRegions; }
		const PlayerBotHuntRegion& region(size_t index) const { return scoredRegions.at(index); }
	private:
		void refreshSuitableCandidates();
		bool canStopRouteValidation();

		enum class Phase : uint8_t {
			Scoring,
			Reachability,
		};

		std::vector<PlayerBotHuntRegion> scoredRegions;
		std::vector<size_t> candidateIndices;
		std::string planningReason;
		std::chrono::steady_clock::time_point planningStarted;
		PlayerBotHuntPlanningProfile planningProfile;
		PlayerBotHuntPlanningSnapshot planningSnapshot;
		std::shared_ptr<const PlayerBotTopologyDistances> topologyDistances;
		size_t nextCandidate = 0;
		size_t nextScoringCandidate = 0;
		uint32_t scoringCandidatesThisTurn = 0;
		uint32_t routeValidationsThisTurn = 0;
		uint32_t routeValidationCalls = 0;
		uint32_t routeValidationCallsThisTurn = 0;
		uint64_t routeValidationExpandedNodes = 0;
		uint32_t yieldCount = 0;
		uint32_t suitableCandidateCount = 0;
		uint32_t scoredCandidateCount = 0;
		uint32_t deferredRouteValidationCount = 0;
		uint32_t candidateCount = 0;
		bool scanCacheHit = false;
		bool routeBoundSatisfied = false;
		uint64_t scanSnapshotTimeUs = 0;
		uint64_t scanClusteringTimeUs = 0;
		uint64_t topologyDistanceTimeUs = 0;
		uint64_t totalScoringTimeUs = 0;
		Phase phase = Phase::Scoring;
};

#endif
