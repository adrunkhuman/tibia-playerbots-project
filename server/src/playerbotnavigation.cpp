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

#include "container.h"
#include "game.h"
#include "house.h"
#include "item.h"
#include "player.h"
#include "tile.h"

#include <array>
#include <limits>
#include <queue>
#include <unordered_map>

extern Game g_game;

namespace {
	constexpr uint32_t cardinalCost = 10;
	constexpr uint32_t diagonalCost = 30;
	constexpr uint32_t transitionCost = 20;
	constexpr int32_t searchMargin = 192;
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

	bool isInsideSearchBounds(const Position& position, const Position& start, const Position& destination)
	{
		const int32_t minimumX = std::min<int32_t>(start.x, destination.x) - searchMargin;
		const int32_t maximumX = std::max<int32_t>(start.x, destination.x) + searchMargin;
		const int32_t minimumY = std::min<int32_t>(start.y, destination.y) - searchMargin;
		const int32_t maximumY = std::max<int32_t>(start.y, destination.y) + searchMargin;
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
		Tile* fromTile = g_game.map.getTile(from);
		if (!fromTile) {
			return false;
		}

		destination = getNextPosition(direction, from);
		if (blockedPositions.find(destination) != blockedPositions.end()) {
			return false;
		}
		uint32_t flags = FLAG_IGNOREBLOCKCREATURE;
		const bool diagonal = (direction & DIRECTION_DIAGONAL_MASK) != 0;
		if (!diagonal) {
			if (from.z != 8 && fromTile->hasHeight(3)) {
				Tile* upperCurrent = g_game.map.getTile(from.x, from.y, from.z - 1);
				if (!upperCurrent || (!upperCurrent->getGround() && !upperCurrent->hasFlag(TILESTATE_BLOCKSOLID))) {
					Tile* upperDestination = g_game.map.getTile(destination.x, destination.y, destination.z - 1);
					if (upperDestination && upperDestination->getGround() &&
					    !upperDestination->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID) &&
					    !upperDestination->hasFlag(TILESTATE_FLOORCHANGE)) {
						destination.z--;
						flags |= FLAG_IGNOREBLOCKITEM;
					}
				}
			}

			if (from.z != 7 && from.z == destination.z) {
				Tile* sameFloor = g_game.map.getTile(destination);
				if (!sameFloor || (!sameFloor->getGround() && !sameFloor->hasFlag(TILESTATE_BLOCKSOLID))) {
					Tile* lowerDestination = g_game.map.getTile(destination.x, destination.y, destination.z + 1);
					if (lowerDestination && lowerDestination->hasHeight(3) &&
					    !lowerDestination->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID)) {
						destination.z++;
						flags |= FLAG_IGNOREBLOCKITEM;
					}
				}
			}
		}

		Tile* tile = g_game.map.getTile(destination);
		if (!canOccupy(player, tile, flags) || tile->getTeleportItem()) {
			return false;
		}

		int32_t index = 0;
		Item* destinationItem = nullptr;
		uint32_t destinationFlags = flags;
		for (uint32_t layer = 0; layer < MAP_MAX_LAYERS; ++layer) {
			Tile* nextTile = tile->queryDestination(index, player, &destinationItem, destinationFlags);
			if (!nextTile || nextTile == tile) {
				break;
			}
			if (nextTile->getTeleportItem()) {
				return false;
			}
			tile = nextTile;
		}
		destination = tile->getPosition();
		return g_game.map.getTile(destination) != nullptr && blockedPositions.find(destination) == blockedPositions.end();
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
		uint32_t cost;
		Position position;
		bool operator>(const QueueNode& other) const { return cost > other.cost; }
	};

	struct Parent {
		Position position;
		PlayerBotNavigationStep step;
	};
}

PlayerBotNavigationResult PlayerBotNavigator::plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
                                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
                                                   uint64_t maximumExpandedNodes) const
{
	steps.clear();
	expandedNodes = 0;
	const Position start = player.getPosition();
	if (start == destination) {
		return PlayerBotNavigationResult::Reached;
	}

	std::priority_queue<QueueNode, std::vector<QueueNode>, std::greater<QueueNode>> open;
	std::unordered_map<uint64_t, uint32_t> costs;
	std::unordered_map<uint64_t, Parent> parents;
	const uint64_t startKey = positionKey(start);
	costs[startKey] = 0;
	open.push({0, start});

	auto addCandidate = [&](const Position& from, uint32_t currentCost, const Position& to,
	                        uint32_t edgeCost, const PlayerBotNavigationStep& step) {
		if (!isInsideSearchBounds(to, start, destination)) {
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
		open.push({newCost, to});
	};

	while (!open.empty() && expandedNodes < maximumExpandedNodes) {
		QueueNode current = open.top();
		open.pop();
		const uint64_t currentKey = positionKey(current.position);
		auto knownCost = costs.find(currentKey);
		if (knownCost == costs.end() || knownCost->second != current.cost) {
			continue;
		}
		++expandedNodes;
		if (current.position == destination) {
			Position cursor = destination;
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
			addCandidate(current.position, current.cost, next,
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
				addCandidate(current.position, current.cost, expected, transitionCost, step);
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
				} else if (Item::items[itemId].isDoor() && Item::items[itemId].name.find("window") == std::string::npos &&
				           Item::items[itemId].blockSolid && item->getActionId() == 0 &&
				           (!item->getDoor() || item->getDoor()->canUse(&player))) {
					addDirectUse(itemId, PlayerBotNavigationAction::UseDoor, target);
				}
			}
		}
	}
	return open.empty() ? PlayerBotNavigationResult::Unreachable : PlayerBotNavigationResult::NodeLimit;
}
