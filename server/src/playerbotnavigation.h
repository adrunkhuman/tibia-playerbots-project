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

#include <cstdint>
#include <deque>
#include <functional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "position.h"

class Player;
class Item;

inline constexpr uint64_t playerBotNavigationMaximumExpandedNodes = 100000;

inline uint32_t playerBotNavigationDistance(const Position& from, const Position& destination)
{
	return Position::getDistanceX(from, destination) + Position::getDistanceY(from, destination) +
	       Position::getDistanceZ(from, destination) * 20;
}

bool playerBotIsTraversableDoor(const Item& item);

struct PlayerBotWalkTransition {
	Position target;
	Position entry;
	Position destination;
	bool ignoreBlockItem = false;
};

bool playerBotResolveWalkTransition(const Position& from, Direction direction, PlayerBotWalkTransition& transition);

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
	Position representative() const;
	bool operator==(const PlayerBotNavigationGoal& other) const;
	bool operator!=(const PlayerBotNavigationGoal& other) const { return !(*this == other); }
};

enum class PlayerBotNavigationAction : uint8_t {
	Move,
	Use,
	UseRope,
	UseShovel,
	UseDoor,
	NpcTravel,
};

enum class PlayerBotNavigationResult : uint8_t {
	Reached,
	Unreachable,
	NodeLimit,
};

struct PlayerBotNavigationStep {
	PlayerBotNavigationAction action = PlayerBotNavigationAction::Move;
	Direction direction = DIRECTION_NONE;
	Position target;
	Position expectedPosition;
	uint16_t itemId = 0;
	uint32_t npcId = 0;
	uint32_t price = 0;
	uint32_t minimumLevel = 0;
	bool premium = false;
	bool topologyPortal = false;
	std::vector<std::string> dialogue;
};

struct PlayerBotNavigationRiskProfile {
	double healthLossCost = 1000.0;
	double maximumHealthLossPerSecond = 0.08;
	double maximumRouteHealthLoss = 0.50;
};

inline bool playerBotNavigationRiskAccepts(const PlayerBotNavigationRiskProfile& risk, uint32_t dangerCost,
	                                         double maximumHealthLossPerSecond)
{
	return dangerCost <= static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost) &&
	       maximumHealthLossPerSecond <= risk.maximumHealthLossPerSecond;
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
	uint32_t dangerCost(const Position& position, uint32_t exposureMs) const;
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
		                               PlayerBotNavigationCostSummary* costSummary = nullptr) const;
		PlayerBotNavigationResult plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
		                               std::deque<PlayerBotNavigationStep>& steps,
		                               uint64_t& expandedNodes,
		                               uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                               Position* closestPosition = nullptr,
		                               const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                               PlayerBotNavigationCostSummary* costSummary = nullptr) const;
		PlayerBotNavigationResult planFrom(Player& player, const Position& start, const Position& destination,
		                                   const std::set<Position>& blockedPositions,
		                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
		                                   uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                                   Position* closestPosition = nullptr,
		                                   const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                                   PlayerBotNavigationCostSummary* costSummary = nullptr) const;
		PlayerBotNavigationResult planFrom(Player& player, const Position& start, const PlayerBotNavigationGoal& goal,
		                                   const std::set<Position>& blockedPositions,
		                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
		                                   uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                                   Position* closestPosition = nullptr,
		                                   const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                                   PlayerBotNavigationCostSummary* costSummary = nullptr) const;
		bool resolveMove(Player& player, const Position& from, Direction direction,
		                 const std::set<Position>& blockedPositions, PlayerBotNavigationStep& step) const;
};

#endif
