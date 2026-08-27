/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotnavigation.h"

#include "actions.h"
#include "container.h"
#include "game.h"
#include "house.h"
#include "item.h"
#include "player.h"
#include "teleport.h"
#include "tile.h"

#include <array>
#include <limits>
#include <queue>
#include <unordered_map>

extern Game g_game;
extern Actions* g_actions;

namespace {
	constexpr uint32_t cardinalCost = 10;
	constexpr uint32_t diagonalCost = cardinalCost * 3;
	constexpr uint32_t transitionCost = 20;
	constexpr int32_t searchMargin = 1024;
	constexpr uint16_t ropeItemId = 2120;
	constexpr uint16_t shovelItemId = 2554;

	constexpr std::array<Direction, 8> directions = {
		DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
		DIRECTION_SOUTHWEST, DIRECTION_SOUTHEAST, DIRECTION_NORTHWEST, DIRECTION_NORTHEAST,
	};
	constexpr std::array<uint16_t, 3> ladderIds = {1386, 3678, 5543};
	constexpr std::array<uint16_t, 2> downUseIds = {430, 1369};
	constexpr std::array<uint16_t, 4> ropeSpotIds = {384, 418, 8278, 8592};
	constexpr std::array<uint16_t, 4> shovelHoleIds = {468, 481, 483, 7932};

	template<typename T, size_t N>
	bool contains(const std::array<T, N>& values, T value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	uint64_t positionKey(const Position& position)
	{
		return (static_cast<uint64_t>(position.z) << 32) |
		       (static_cast<uint64_t>(position.x) << 16) | position.y;
	}

	bool isInsideSearchBounds(const Position& position, const Position& start, const PlayerBotNavigationGoal& goal)
	{
		int32_t minimumX = start.x;
		int32_t maximumX = start.x;
		int32_t minimumY = start.y;
		int32_t maximumY = start.y;
		auto include = [&](const Position& candidate, uint8_t rangeX = 0, uint8_t rangeY = 0) {
			minimumX = std::min(minimumX, static_cast<int32_t>(candidate.x) - rangeX);
			maximumX = std::max(maximumX, static_cast<int32_t>(candidate.x) + rangeX);
			minimumY = std::min(minimumY, static_cast<int32_t>(candidate.y) - rangeY);
			maximumY = std::max(maximumY, static_cast<int32_t>(candidate.y) + rangeY);
		};
		if (goal.type == PlayerBotNavigationGoalType::AnyOf) {
			for (const Position& candidate : goal.positions) include(candidate);
		} else {
			include(goal.position, goal.rangeX, goal.rangeY);
		}
		minimumX -= searchMargin;
		maximumX += searchMargin;
		minimumY -= searchMargin;
		maximumY += searchMargin;
		return position.x >= minimumX && position.x <= maximumX &&
		       position.y >= minimumY && position.y <= maximumY;
	}

	bool canOccupy(Player& player, Tile* tile, uint32_t flags = FLAG_IGNOREBLOCKCREATURE)
	{
		return tile && tile->queryAdd(0, player, 1, flags) == RETURNVALUE_NOERROR;
	}

	bool resolveWalk(Player& player, const Position& from, Direction direction,
	                 const std::set<Position>& blockedPositions, Position& destination)
	{
		PlayerBotWalkTransition transition;
		if (!playerBotResolveWalkTransition(from, direction, transition) ||
		    blockedPositions.find(transition.target) != blockedPositions.end() ||
		    blockedPositions.find(transition.destination) != blockedPositions.end()) return false;
		uint32_t flags = FLAG_IGNOREBLOCKCREATURE;
		if (transition.ignoreBlockItem) flags |= FLAG_IGNOREBLOCKITEM;
		Tile* tile = g_game.map.getTile(transition.entry);
		if (!canOccupy(player, tile, flags)) {
			return false;
		}
		destination = transition.destination;
		return true;
	}

	bool moveUpstairsDestination(Player& player, const Position& target, Position& destination)
	{
		if (target.z == 0) {
			return false;
		}
		const Position upper(target.x, target.y, target.z - 1);
		constexpr std::array<Direction, 8> preference = {
			DIRECTION_SOUTH, DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_WEST,
			DIRECTION_SOUTHWEST, DIRECTION_SOUTHEAST, DIRECTION_NORTHWEST, DIRECTION_NORTHEAST,
		};
		for (Direction direction : preference) {
			Position candidate = getNextPosition(direction, upper);
			if (canOccupy(player, g_game.map.getTile(candidate))) {
				destination = candidate;
				return true;
			}
		}
		return false;
	}

	Item* findItem(Tile* tile, uint16_t itemId)
	{
		if (!tile) {
			return nullptr;
		}
		if (Item* ground = tile->getGround(); ground && ground->getID() == itemId) {
			return ground;
		}
		TileItemVector* items = tile->getItemList();
		if (!items) {
			return nullptr;
		}
		for (Item* item : *items) {
			if (item->getID() == itemId) {
				return item;
			}
		}
		return nullptr;
	}

	struct QueueNode {
		uint32_t estimatedCost;
		uint32_t pathCost;
		Position position;
		bool operator>(const QueueNode& other) const {
			return estimatedCost != other.estimatedCost ? estimatedCost > other.estimatedCost : pathCost < other.pathCost;
		}
	};

	uint32_t exactRemainingCost(const Position& position, const Position& destination)
	{
		return (Position::getDistanceX(position, destination) + Position::getDistanceY(position, destination)) * cardinalCost +
		       Position::getDistanceZ(position, destination) * transitionCost;
	}

	uint32_t remainingCost(const Position& position, const PlayerBotNavigationGoal& goal)
	{
		if (goal.type == PlayerBotNavigationGoalType::AnyOf) {
			uint32_t result = std::numeric_limits<uint32_t>::max();
			for (const Position& candidate : goal.positions) result = std::min(result, exactRemainingCost(position, candidate));
			return result;
		}
		const uint32_t distanceX = Position::getDistanceX(position, goal.position);
		const uint32_t distanceY = Position::getDistanceY(position, goal.position);
		const uint32_t distanceZ = Position::getDistanceZ(position, goal.position);
		return (distanceX > goal.rangeX ? distanceX - goal.rangeX : 0) * cardinalCost +
		       (distanceY > goal.rangeY ? distanceY - goal.rangeY : 0) * cardinalCost +
		       (distanceZ > goal.rangeZ ? distanceZ - goal.rangeZ : 0) * transitionCost;
	}

	uint32_t searchHeuristic(const Position& position, const PlayerBotNavigationGoal& goal)
	{
		// A walk edge can redirect through a floor change or teleport. One cardinal
		// step is therefore the strongest universal lower bound on remaining cost.
		return goal.reached(position) ? 0 : cardinalCost;
	}

	struct Parent {
		Position position;
		PlayerBotNavigationStep step;
	};
}

bool playerBotResolveWalkTransition(const Position& from, Direction direction, PlayerBotWalkTransition& transition)
{
	Tile* fromTile = g_game.map.getTile(from);
	if (!fromTile) return false;
	transition = {};
	transition.target = getNextPosition(direction, from);
	transition.entry = transition.target;
	const bool diagonal = (direction & DIRECTION_DIAGONAL_MASK) != 0;
	if (!diagonal) {
		if (from.z != 8 && fromTile->hasHeight(3)) {
			Tile* upperCurrent = g_game.map.getTile(from.x, from.y, from.z - 1);
			if (!upperCurrent || (!upperCurrent->getGround() && !upperCurrent->hasFlag(TILESTATE_BLOCKSOLID))) {
				Tile* upperDestination = g_game.map.getTile(transition.entry.x, transition.entry.y, transition.entry.z - 1);
				if (upperDestination && upperDestination->getGround() &&
				    !upperDestination->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID) &&
				    !upperDestination->hasFlag(TILESTATE_FLOORCHANGE)) {
					transition.entry.z--;
					transition.ignoreBlockItem = true;
				}
			}
		}
		if (from.z != 7 && from.z == transition.entry.z) {
			Tile* sameFloor = g_game.map.getTile(transition.entry);
			if (!sameFloor || (!sameFloor->getGround() && !sameFloor->hasFlag(TILESTATE_BLOCKSOLID))) {
				Tile* lowerDestination = g_game.map.getTile(transition.entry.x, transition.entry.y, transition.entry.z + 1);
				if (lowerDestination && lowerDestination->hasHeight(3) &&
				    !lowerDestination->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID)) {
					transition.entry.z++;
					transition.ignoreBlockItem = true;
				}
			}
		}
	}
	Tile* tile = g_game.map.getTile(transition.entry);
	if (!tile) return false;
	if (Teleport* teleport = tile->getTeleportItem()) {
		transition.destination = teleport->getDestPos();
		return g_game.map.getTile(transition.destination) != nullptr;
	}
	for (uint32_t layer = 0; layer < MAP_MAX_LAYERS; ++layer) {
		Tile* nextTile = tile->getFloorChangeDestination();
		if (!nextTile || nextTile == tile) break;
		tile = nextTile;
	}
	transition.destination = tile->getPosition();
	return true;
}

PlayerBotNavigationGoal PlayerBotNavigationGoal::exact(const Position& position)
{
	PlayerBotNavigationGoal goal;
	goal.position = position;
	return goal;
}

PlayerBotNavigationGoal PlayerBotNavigationGoal::withinRange(const Position& position, uint8_t rangeX, uint8_t rangeY, uint8_t rangeZ)
{
	PlayerBotNavigationGoal goal;
	goal.type = PlayerBotNavigationGoalType::WithinRange;
	goal.position = position;
	goal.rangeX = rangeX;
	goal.rangeY = rangeY;
	goal.rangeZ = rangeZ;
	return goal;
}

PlayerBotNavigationGoal PlayerBotNavigationGoal::anyOf(std::vector<Position> positions)
{
	PlayerBotNavigationGoal goal;
	goal.type = PlayerBotNavigationGoalType::AnyOf;
	goal.positions = std::move(positions);
	if (!goal.positions.empty()) goal.position = goal.positions.front();
	return goal;
}

bool PlayerBotNavigationGoal::reached(const Position& candidate) const
{
	if (type == PlayerBotNavigationGoalType::AnyOf) {
		return std::find(positions.begin(), positions.end(), candidate) != positions.end();
	}
	return Position::getDistanceX(candidate, position) <= rangeX &&
	       Position::getDistanceY(candidate, position) <= rangeY &&
	       Position::getDistanceZ(candidate, position) <= rangeZ;
}

uint32_t PlayerBotNavigationGoal::distance(const Position& candidate) const
{
	return remainingCost(candidate, *this) / cardinalCost;
}

Position PlayerBotNavigationGoal::representative() const
{
	return position;
}

bool PlayerBotNavigationGoal::operator==(const PlayerBotNavigationGoal& other) const
{
	return type == other.type && position == other.position && rangeX == other.rangeX && rangeY == other.rangeY &&
	       rangeZ == other.rangeZ && positions == other.positions;
}

bool playerBotIsTraversableDoor(const Item& item)
{
	const ItemType& type = Item::items[item.getID()];
	return type.isDoor() && type.blockSolid && (item.getActionId() == 0 || type.levelDoor != 0) &&
	       g_actions && g_actions->hasAction(&item);
}

PlayerBotNavigationResult PlayerBotNavigator::plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
                                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
                                                   uint64_t maximumExpandedNodes, Position* closestPosition) const

{
	return plan(player, PlayerBotNavigationGoal::exact(destination), blockedPositions, steps, expandedNodes,
	            maximumExpandedNodes, closestPosition);
}

PlayerBotNavigationResult PlayerBotNavigator::plan(Player& player, const PlayerBotNavigationGoal& goal,
	const std::set<Position>& blockedPositions, std::deque<PlayerBotNavigationStep>& steps,
	uint64_t& expandedNodes, uint64_t maximumExpandedNodes, Position* closestPosition) const
{
	return planFrom(player, player.getPosition(), goal, blockedPositions, steps, expandedNodes, maximumExpandedNodes,
	                closestPosition);
}

bool PlayerBotNavigator::resolveMove(Player& player, const Position& from, Direction direction,
	                                 const std::set<Position>& blockedPositions,
	                                 PlayerBotNavigationStep& step) const
{
	Position expectedPosition;
	if (!resolveWalk(player, from, direction, blockedPositions, expectedPosition)) return false;
	step.action = PlayerBotNavigationAction::Move;
	step.direction = direction;
	step.target = getNextPosition(direction, from);
	step.expectedPosition = expectedPosition;
	return true;
}

PlayerBotNavigationResult PlayerBotNavigator::planFrom(Player& player, const Position& start, const Position& destination,
	                                                    const std::set<Position>& blockedPositions,
	                                                    std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
	                                                    uint64_t maximumExpandedNodes, Position* closestPosition) const
{
	return planFrom(player, start, PlayerBotNavigationGoal::exact(destination), blockedPositions, steps, expandedNodes,
	                maximumExpandedNodes, closestPosition);
}

PlayerBotNavigationResult PlayerBotNavigator::planFrom(Player& player, const Position& start, const PlayerBotNavigationGoal& goal,
	const std::set<Position>& blockedPositions, std::deque<PlayerBotNavigationStep>& steps,
	uint64_t& expandedNodes, uint64_t maximumExpandedNodes, Position* closestPosition) const
{
	steps.clear();
	expandedNodes = 0;
	Position closest = start;
	uint32_t closestCost = remainingCost(start, goal);
	if (closestPosition) *closestPosition = closest;
	if (goal.reached(start)) {
		return PlayerBotNavigationResult::Reached;
	}

	std::priority_queue<QueueNode, std::vector<QueueNode>, std::greater<QueueNode>> open;
	std::unordered_map<uint64_t, uint32_t> costs;
	std::unordered_map<uint64_t, Parent> parents;
	const uint64_t startKey = positionKey(start);
	costs[startKey] = 0;
	open.push({searchHeuristic(start, goal), 0, start});

	auto addCandidate = [&](const Position& from, uint32_t currentCost, const Position& to,
	                        uint32_t edgeCost, const PlayerBotNavigationStep& step) {
		if (!isInsideSearchBounds(to, start, goal)) {
			return;
		}
		const uint32_t newCost = currentCost + edgeCost;
		const uint64_t key = positionKey(to);
		auto existing = costs.find(key);
		if (existing != costs.end() && existing->second <= newCost) {
			return;
		}
		costs[key] = newCost;
		parents[key] = {from, step};
		open.push({newCost + searchHeuristic(to, goal), newCost, to});
	};

	while (!open.empty() && expandedNodes < maximumExpandedNodes) {
		QueueNode current = open.top();
		open.pop();
		const uint64_t currentKey = positionKey(current.position);
		auto knownCost = costs.find(currentKey);
		if (knownCost == costs.end() || knownCost->second != current.pathCost) {
			continue;
		}
		++expandedNodes;
		const uint32_t distance = remainingCost(current.position, goal);
		if (distance < closestCost) {
			closest = current.position;
			closestCost = distance;
			if (closestPosition) *closestPosition = closest;
		}
		if (goal.reached(current.position)) {
			Position cursor = current.position;
			while (cursor != start) {
				auto parent = parents.find(positionKey(cursor));
				if (parent == parents.end()) {
					steps.clear();
					return PlayerBotNavigationResult::Unreachable;
				}
				steps.push_front(parent->second.step);
				cursor = parent->second.position;
			}
			return PlayerBotNavigationResult::Reached;
		}

		for (Direction direction : directions) {
			Position next;
			if (!resolveWalk(player, current.position, direction, blockedPositions, next)) {
				continue;
			}
			PlayerBotNavigationStep step;
			step.action = PlayerBotNavigationAction::Move;
			step.direction = direction;
			step.target = getNextPosition(direction, current.position);
			step.expectedPosition = next;
			addCandidate(current.position, current.pathCost, next,
			             (direction & DIRECTION_DIAGONAL_MASK) ? diagonalCost : cardinalCost, step);
		}

		for (Direction direction : directions) {
			const Position target = getNextPosition(direction, current.position);
			Tile* tile = g_game.map.getTile(target);
			if (!tile) {
				continue;
			}

			auto addDirectUse = [&](uint16_t itemId, PlayerBotNavigationAction action, const Position& expected) {
				if (blockedPositions.find(target) != blockedPositions.end() ||
				    blockedPositions.find(expected) != blockedPositions.end()) {
					return;
				}
				PlayerBotNavigationStep step;
				step.action = action;
				step.target = target;
				step.expectedPosition = expected;
				step.itemId = itemId;
				addCandidate(current.position, current.pathCost, expected, transitionCost, step);
			};

			Item* ground = tile->getGround();
			if (ground && contains(ropeSpotIds, ground->getID()) &&
			    g_game.findItemOfType(&player, ropeItemId, true)) {
				Position expected;
				if (moveUpstairsDestination(player, target, expected)) {
					addDirectUse(ground->getID(), PlayerBotNavigationAction::UseRope, expected);
				}
			}
			if (ground && contains(shovelHoleIds, ground->getID()) &&
			    g_game.findItemOfType(&player, shovelItemId, true) && target.z < MAP_MAX_LAYERS - 1) {
				addDirectUse(ground->getID(), PlayerBotNavigationAction::UseShovel,
				             Position(target.x, target.y, target.z + 1));
			}

			std::vector<Item*> tileItems;
			if (ground) {
				tileItems.push_back(ground);
			}
			if (TileItemVector* items = tile->getItemList()) {
				tileItems.insert(tileItems.end(), items->begin(), items->end());
			}
			for (Item* item : tileItems) {
				const uint16_t itemId = item->getID();
				if (contains(ladderIds, itemId)) {
					Position expected;
					if (moveUpstairsDestination(player, target, expected)) {
						addDirectUse(itemId, PlayerBotNavigationAction::Use, expected);
					}
				} else if (contains(downUseIds, itemId) && target.z < MAP_MAX_LAYERS - 1) {
					addDirectUse(itemId, PlayerBotNavigationAction::Use,
					             Position(target.x, target.y, target.z + 1));
				} else if (playerBotIsTraversableDoor(*item) &&
				           Item::items[itemId].description != "It is locked." &&
				           (!item->getDoor() || item->getDoor()->canUse(&player))) {
					addDirectUse(itemId, PlayerBotNavigationAction::UseDoor, target);
				}
			}
		}
	}
	return open.empty() ? PlayerBotNavigationResult::Unreachable : PlayerBotNavigationResult::NodeLimit;
}
