/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTNAVIGATION_H
#define FS_PLAYERBOTNAVIGATION_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "position.h"

class Player;
class PlayerBotPathSearch;
class PlayerBotTopologyHeuristic;
class PlayerBotRouteCorridor;
class Item;
class Tile;

inline constexpr uint64_t playerBotNavigationMaximumExpandedNodes = 100000;

inline bool playerBotNpcTravelOfferEligible(uint32_t playerLevel, bool playerPremium, uint64_t availableMoney,
                                             uint32_t minimumLevel, bool premiumRequired, uint64_t fare,
                                             bool opaqueCondition, bool opaqueAction)
{
	return playerLevel >= minimumLevel && (!premiumRequired || playerPremium) && fare <= availableMoney &&
	       !opaqueCondition && !opaqueAction;
}

inline uint32_t playerBotNavigationDistance(const Position& from, const Position& destination)
{
	return Position::getDistanceX(from, destination) + Position::getDistanceY(from, destination) +
	       Position::getDistanceZ(from, destination) * 20;
}

// Exact interaction goals must leave the player on the tile. Occupied tiles
// remain eligible for the caller's bounded wait/retry policy, not for movement.
bool playerBotStableApproachTile(const Tile* tile, const Player& player);

bool playerBotIsTraversableDoor(const Item& item);
std::optional<uint16_t> playerBotPassageOpenItemId(const Item& item);
std::optional<uint32_t> playerBotPassageMinimumLevel(const Item& item);
bool playerBotCanTraverseDoor(const Player& player, const Item& item);

struct PlayerBotWalkTransition {
	Position target;
	Position entry;
	Position destination;
	bool ignoreBlockItem = false;
};

bool playerBotResolveWalkTransition(const Position& from, Direction direction, PlayerBotWalkTransition& transition,
                                    bool fixedHeightOnly = false, bool assumeOpenDoors = false, bool ignoreHeight = false);

enum class PlayerBotNavigationGoalType : uint8_t {
	Exact,
	WithinRange,
	AnyOf,
};

struct PlayerBotNavigationGoal {
	PlayerBotNavigationGoalType type = PlayerBotNavigationGoalType::Exact;
	Position position;
	uint8_t rangeX = 0;
	uint8_t rangeY = 0;
	uint8_t rangeZ = 0;
	std::vector<Position> positions;

	static PlayerBotNavigationGoal exact(const Position& position);
	static PlayerBotNavigationGoal withinRange(const Position& position, uint8_t rangeX, uint8_t rangeY, uint8_t rangeZ = 0);
	static PlayerBotNavigationGoal anyOf(std::vector<Position> positions);
	bool reached(const Position& candidate) const;
	uint32_t distance(const Position& candidate) const;
	Position representative() const { return position; }
	bool operator==(const PlayerBotNavigationGoal& other) const
	{
		return type == other.type && position == other.position && rangeX == other.rangeX && rangeY == other.rangeY &&
		       rangeZ == other.rangeZ && positions == other.positions;
	}
	bool operator!=(const PlayerBotNavigationGoal& other) const { return !(*this == other); }
};

// Lower bound only for same-floor ordinary walking (cardinal 10, diagonal 30,
// door 20, nonnegative danger). Never use it for unrestricted redirect edges.
inline uint32_t playerBotOrdinaryRemainingCost(const Position& from, const PlayerBotNavigationGoal& goal)
{
	if (goal.type == PlayerBotNavigationGoalType::AnyOf) {
		uint32_t lowest = UINT32_MAX;
		for (const Position& candidate : goal.positions) {
			if (candidate.z == from.z) {
				lowest = std::min(lowest, (Position::getDistanceX(from, candidate) +
				    Position::getDistanceY(from, candidate)) * 10u);
			}
		}
		return lowest == UINT32_MAX ? 0 : lowest;
	}
	if (Position::getDistanceZ(from, goal.position) > goal.rangeZ) return 0;
	const uint32_t dx = Position::getDistanceX(from, goal.position);
	const uint32_t dy = Position::getDistanceY(from, goal.position);
	return ((dx > goal.rangeX ? dx - goal.rangeX : 0) +
	        (dy > goal.rangeY ? dy - goal.rangeY : 0)) * 10u;
}

inline bool playerBotNpcTravelUsesLocalApproach(const Position& current, const Position& provider,
                                                 int32_t maximumDistance)
{
	return current.z == provider.z &&
	       std::max(Position::getDistanceX(current, provider), Position::getDistanceY(current, provider)) <= maximumDistance;
}

// Depot discovery can alternate between adjacent standable approach tiles of
// the same depot every decision tick. Treat such flapping as one fixed
// objective so failure accounting and confirmed route blockers survive it.
inline bool playerBotNavigationSameFixedObjective(const PlayerBotNavigationGoal& previous,
	const PlayerBotNavigationGoal& current)
{
	if (previous == current) return true;
	const Position previousRepresentative = previous.representative();
	const Position currentRepresentative = current.representative();
	return previousRepresentative.z == currentRepresentative.z &&
	       Position::areInRange<3, 3, 0>(previousRepresentative, currentRepresentative);
}

inline bool playerBotNpcTravelApproachComplete(bool localApproach, bool hasSteps,
                                                bool exactDestinationReached)
{
	return localApproach || (hasSteps && exactDestinationReached);
}

enum class PlayerBotNavigationAction : uint8_t {
	Move,
	Use,
	UseRope,
	UseShovel,
	UseDoor,
	NpcTravel,
};

// UseShovel steps carry this stable passage identity until dispatch; the live
// tile then selects an ordinary move or normal shovel use.
struct PlayerBotShovelPassage {
	uint16_t closedItemId = 0;
	uint16_t openItemId = 0;
};

inline std::optional<PlayerBotShovelPassage> playerBotShovelPassage(uint16_t itemId)
{
	// These are the exact closed/open pairs used by data/actions/lib/actions.lua.
	switch (itemId) {
		case 468:
		case 469: return PlayerBotShovelPassage{468, 469};
		case 481:
		case 482: return PlayerBotShovelPassage{481, 482};
		case 483:
		case 484: return PlayerBotShovelPassage{483, 484};
		case 7932:
		case 7933: return PlayerBotShovelPassage{7932, 7933};
		default: return std::nullopt;
	}
}

const Item* playerBotShovelPassageItem(const Tile& tile, uint16_t passageItemId = 0);
Item* playerBotShovelPassageItem(Tile& tile, uint16_t passageItemId = 0);

inline std::optional<PlayerBotNavigationAction> playerBotResolveShovelPassageAction(
    uint16_t passageItemId, uint16_t liveItemId, bool canUseShovel)
{
	const auto passage = playerBotShovelPassage(passageItemId);
	const auto livePassage = playerBotShovelPassage(liveItemId);
	if (!passage || !livePassage || passage->closedItemId != livePassage->closedItemId) return std::nullopt;
	if (liveItemId == passage->openItemId) return PlayerBotNavigationAction::Move;
	return canUseShovel && liveItemId == passage->closedItemId ?
	    std::optional<PlayerBotNavigationAction>(PlayerBotNavigationAction::UseShovel) : std::nullopt;
}

enum class PlayerBotNavigationResult : uint8_t {
	Reached,
	Unreachable,
	NodeLimit,
	RiskRejected,
};

inline bool playerBotNavigationMayFallbackToNpcTravel(PlayerBotNavigationResult result)
{
	return result == PlayerBotNavigationResult::Unreachable;
}

inline bool playerBotNavigationExactGoalBlocked(const PlayerBotNavigationGoal& goal,
                                                 const std::set<Position>& blockedPositions)
{
	return goal.type == PlayerBotNavigationGoalType::Exact && blockedPositions.find(goal.position) != blockedPositions.end();
}

struct PlayerBotNavigationStep {
	PlayerBotNavigationAction action = PlayerBotNavigationAction::Move;
	Direction direction = DIRECTION_NONE;
	Position target;
	Position expectedPosition;
	uint16_t itemId = 0;
	uint16_t expectedItemId = 0;
	uint32_t npcId = 0;
	uint32_t price = 0;
	uint32_t minimumLevel = 0;
	bool premium = false;
	bool topologyPortal = false;
	std::vector<std::string> dialogue;
};

enum class PlayerBotNavigationDangerEvidence : uint8_t {
	Detailed,
	Coarse,
};

enum class PlayerBotNavigationRiskVerdict : uint8_t {
	Accepted,
	Rejected,
	Unknown,
};

struct PlayerBotNavigationRiskProfile {
	double healthLossCost = 1000.0;
	double maximumHealthLossPerSecond = 0.08;
	double maximumRouteHealthLoss = 0.50;
};

inline bool playerBotNavigationPeakAccepts(double maximum, double peak)
{
	return peak <= maximum;
}

// Necessary endpoint evidence only: safety does not establish reachability.
// Range goals and empty source-tree floods are left to the bounded search.
// Already being at a requested endpoint requires no exposure-producing arc.
template<class Sample>
std::optional<double> playerBotNavigationRejectedEndpointPeak(const Position& source,
    const PlayerBotNavigationGoal& goal, double maximum, Sample sample)
{
	if (!std::isfinite(maximum) || goal.type == PlayerBotNavigationGoalType::WithinRange) return std::nullopt;
	double minimum = std::numeric_limits<double>::infinity();
	auto rejected = [&](const Position& target) {
		if (target == source) return false;
		const double peak = sample(target);
		minimum = std::min(minimum, peak);
		return !playerBotNavigationPeakAccepts(maximum, peak);
	};
	if (goal.type == PlayerBotNavigationGoalType::Exact) {
		if (!rejected(goal.position)) return std::nullopt;
	} else {
		if (goal.positions.empty()) return std::nullopt;
		for (const Position& target : goal.positions) if (!rejected(target)) return std::nullopt;
	}
	return minimum;
}

inline bool playerBotNavigationRiskAccepts(const PlayerBotNavigationRiskProfile& risk, uint32_t dangerCost,
	                                         double maximumHealthLossPerSecond)
{
	return dangerCost <= static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost) &&
	       playerBotNavigationPeakAccepts(risk.maximumHealthLossPerSecond, maximumHealthLossPerSecond);
}

inline PlayerBotNavigationRiskVerdict playerBotNavigationRiskVerdict(
    const PlayerBotNavigationRiskProfile& risk, PlayerBotNavigationDangerEvidence evidence,
    uint32_t dangerCost, double maximumHealthLossPerSecond)
{
	if (evidence == PlayerBotNavigationDangerEvidence::Coarse) return PlayerBotNavigationRiskVerdict::Unknown;
	return playerBotNavigationRiskAccepts(risk, dangerCost, maximumHealthLossPerSecond) ?
	    PlayerBotNavigationRiskVerdict::Accepted : PlayerBotNavigationRiskVerdict::Rejected;
}

struct PlayerBotNavigationCostPolicy {
	PlayerBotNavigationRiskProfile risk;
	std::function<double(const Position&)> expectedHealthLossPerSecond;
	uint32_t topologyExposureMs = 16000;

	bool enabled() const { return static_cast<bool>(expectedHealthLossPerSecond) && risk.healthLossCost > 0; }
	double dangerAt(const Position& position) const
	{
		return expectedHealthLossPerSecond ? expectedHealthLossPerSecond(position) : 0;
	}
	uint32_t dangerCost(const Position& position, uint32_t exposureMs) const
	{
		return enabled() ? dangerCostForSample(dangerAt(position), exposureMs) : 0;
	}
	// Reuse a sample within synchronous work; dangerAt still records its dependencies.
	uint32_t dangerCostForSample(double healthLossPerSecond, uint32_t exposureMs) const
	{
		if (!enabled()) return 0;
		const double expectedHealthLoss = std::max(0.0, healthLossPerSecond) * exposureMs / 1000.0;
		return static_cast<uint32_t>(std::min<double>(std::numeric_limits<uint32_t>::max(),
		                                                std::ceil(expectedHealthLoss * risk.healthLossCost)));
	}
};

struct PlayerBotNavigationCostSummary {
	uint32_t movementCost = 0;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
};

class PlayerBotNavigator
{
	public:
		PlayerBotNavigationResult plan(Player& player, const PlayerBotNavigationGoal& goal,
		                               const std::set<Position>& blockedPositions,
		                               std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
		                               uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                               Position* closestPosition = nullptr,
		                               const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                               PlayerBotNavigationCostSummary* costSummary = nullptr,
		                               bool sameFloorOnly = false) const;
		PlayerBotNavigationResult plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
		                               std::deque<PlayerBotNavigationStep>& steps,
		                               uint64_t& expandedNodes,
		                               uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                               Position* closestPosition = nullptr,
		                               const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                               PlayerBotNavigationCostSummary* costSummary = nullptr,
		                               bool sameFloorOnly = false) const;
		PlayerBotNavigationResult planFrom(Player& player, const Position& start, const Position& destination,
		                                   const std::set<Position>& blockedPositions,
		                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
		                                   uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                                   Position* closestPosition = nullptr,
		                                   const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                                   PlayerBotNavigationCostSummary* costSummary = nullptr,
		                                   bool sameFloorOnly = false) const;
		PlayerBotNavigationResult planFrom(Player& player, const Position& start, const PlayerBotNavigationGoal& goal,
		                                   const std::set<Position>& blockedPositions,
		                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
		                                   uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                                   Position* closestPosition = nullptr,
		                                   const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                                   PlayerBotNavigationCostSummary* costSummary = nullptr,
		                                   bool sameFloorOnly = false) const;
		std::optional<PlayerBotNavigationResult> advance(Player& player, PlayerBotPathSearch& search,
		    const std::set<Position>& blockedPositions, uint64_t slice,
		    const PlayerBotNavigationCostPolicy* costPolicy = nullptr, bool sameFloorOnly = false,
		    const PlayerBotTopologyHeuristic* heuristic = nullptr,
		    const PlayerBotRouteCorridor* corridor = nullptr) const;
		// Recheck a retained portal against live tiles without changing the recorded step.
		bool validateStep(Player& player, Position from, const PlayerBotNavigationStep& step,
		                  const std::set<Position>& blockedPositions) const;
		bool resolveMove(Player& player, const Position& from, Direction direction,
		                 const std::set<Position>& blockedPositions, PlayerBotNavigationStep& step) const;
		bool resolveShovelPassage(Player& player, const Position& from,
		                          const PlayerBotNavigationStep& passage,
		                          const std::set<Position>& blockedPositions,
		                          PlayerBotNavigationStep& step) const;
};

#endif
