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
#include "combat.h"
#include "container.h"
#include "game.h"
#include "groups.h"
#include "house.h"
#include "item.h"
#include "player.h"
#include "teleport.h"
#include "tile.h"

#include <array>
#include <limits>
#include <queue>
#include <unordered_map>
#include "playerbotpathsearch.h"
#include "playerbotroutecorridor.h"
#include "playerbottopology.h"

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
	constexpr std::array<uint16_t, 1> downUseIds = {430};
	constexpr std::array<uint16_t, 4> ropeSpotIds = {384, 418, 8278, 8592};

	template<typename T, size_t N>
	bool contains(const std::array<T, N>& values, T value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
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
		if (transition.entry.z != from.z) {
			// Ignore climbs enabled only by movable height. Ordinary moves do
			// not need a second transition lookup.
			PlayerBotWalkTransition supported;
			if (!playerBotResolveWalkTransition(from, direction, supported, true) ||
			    transition.entry != supported.entry || transition.destination != supported.destination) return false;
		}
		uint32_t flags = FLAG_IGNOREBLOCKCREATURE;
		if (transition.ignoreBlockItem) flags |= FLAG_IGNOREBLOCKITEM;
		Tile* tile = g_game.map.getTile(transition.entry);
		if (!canOccupy(player, tile, flags)) {
			return false;
		}
		destination = transition.destination;
		return true;
	}

	// Tile.isWalkable in data/lib/core/tile.lua: ground that is not solid, and
	// no immovable solid item other than a magic field.
	bool scriptWalkable(const Tile* tile)
	{
		if (!tile) return false;
		const Item* ground = tile->getGround();
		if (!ground || ground->hasProperty(CONST_PROP_BLOCKSOLID)) return false;
		if (const TileItemVector* items = tile->getItemList()) {
			for (const Item* item : *items) {
				const ItemType& type = Item::items[item->getID()];
				if (!type.isMagicField() && !type.moveable && item->hasProperty(CONST_PROP_BLOCKSOLID)) return false;
			}
		}
		return true;
	}

	// Mirrors Position:moveUpstairs() in data/lib/core/position.lua, used by the
	// ladder and rope actions: the tile south of the upper position when it is
	// walkable, otherwise the first walkable neighbour in Direction order with
	// south replaced by west. The script teleports without an occupancy check.
	bool moveUpstairsDestination(Player&, const Position& target, Position& destination)
	{
		if (target.z == 0) {
			return false;
		}
		const Position upper(target.x, target.y, target.z - 1);
		const Position south = getNextPosition(DIRECTION_SOUTH, upper);
		if (scriptWalkable(g_game.map.getTile(south))) {
			destination = south;
			return true;
		}
		for (uint8_t value = DIRECTION_NORTH; value <= DIRECTION_NORTHEAST; ++value) {
			const Direction direction = value == DIRECTION_SOUTH ? DIRECTION_WEST : static_cast<Direction>(value);
			const Position candidate = getNextPosition(direction, upper);
			if (scriptWalkable(g_game.map.getTile(candidate))) {
				destination = candidate;
				return true;
			}
		}
		// The script would teleport onto an unwalkable south tile; never plan it.
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

}

bool playerBotStableApproachTile(const Tile* tile, const Player& player)
{
	// queryAdd alone permits tiles that redirect entry. Do not use PATHFINDING:
	// its extra blocker rules would change approach admission beyond stability.
	return tile && !tile->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT) &&
	       tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) == RETURNVALUE_NOERROR;
}

const Item* playerBotShovelPassageItem(const Tile& tile, uint16_t passageItemId)
{
	const auto expected = passageItemId == 0 ? std::nullopt : playerBotShovelPassage(passageItemId);
	if (passageItemId != 0 && !expected) return nullptr;
	auto matches = [&expected](const Item* item) {
		if (!item) return false;
		const auto passage = playerBotShovelPassage(item->getID());
		return passage && (!expected || passage->closedItemId == expected->closedItemId);
	};
	if (const Item* ground = tile.getGround(); matches(ground)) return ground;
	if (const TileItemVector* items = tile.getItemList()) {
		const auto found = std::find_if(items->begin(), items->end(), matches);
		if (found != items->end()) return *found;
	}
	return nullptr;
}

Item* playerBotShovelPassageItem(Tile& tile, uint16_t passageItemId)
{
	return const_cast<Item*>(playerBotShovelPassageItem(
	    static_cast<const Tile&>(tile), passageItemId));
}

uint32_t PlayerBotNavigationCostPolicy::dangerCost(const Position& position, uint32_t exposureMs) const
{
	if (!enabled()) return 0;
	const double expectedHealthLoss = std::max(0.0, dangerAt(position)) * exposureMs / 1000.0;
	return static_cast<uint32_t>(std::min<double>(std::numeric_limits<uint32_t>::max(),
	                                                std::ceil(expectedHealthLoss * risk.healthLossCost)));
}

bool playerBotResolveWalkTransition(const Position& from, Direction direction, PlayerBotWalkTransition& transition,
                                    bool fixedHeightOnly, bool assumeOpenDoors, bool ignoreHeight)
{
	auto hasHeight = [fixedHeightOnly](const Tile* tile) {
		if (!tile) return false;
		if (!fixedHeightOnly) return tile->hasHeight(3);
		uint32_t height = 0;
		if (const Item* ground = tile->getGround(); ground && ground->hasProperty(CONST_PROP_HASHEIGHT)) ++height;
		if (const TileItemVector* items = tile->getItemList()) {
			for (const Item* item : *items) {
				if (!item->isMoveable() && item->hasProperty(CONST_PROP_HASHEIGHT)) ++height;
			}
		}
		return height >= 3;
	};
	auto canLand = [assumeOpenDoors](const Tile* tile) {
		if (!tile->hasFlag(TILESTATE_IMMOVABLEBLOCKSOLID)) return true;
		if (!assumeOpenDoors) return false;
		const TileItemVector* items = tile->getItemList();
		return items && std::any_of(items->begin(), items->end(), [](const Item* item) {
			return item && playerBotIsTraversableDoor(*item);
		});
	};
	Tile* fromTile = g_game.map.getTile(from);
	if (!fromTile) return false;
	transition = {};
	transition.target = getNextPosition(direction, from);
	transition.entry = transition.target;
	const bool diagonal = (direction & DIRECTION_DIAGONAL_MASK) != 0;
	if (!diagonal && !ignoreHeight) {
		if (from.z != 8 && hasHeight(fromTile)) {
			Tile* upperCurrent = g_game.map.getTile(from.x, from.y, from.z - 1);
			if (!upperCurrent || (!upperCurrent->getGround() && !upperCurrent->hasFlag(TILESTATE_BLOCKSOLID))) {
				Tile* upperDestination = g_game.map.getTile(transition.entry.x, transition.entry.y, transition.entry.z - 1);
				if (upperDestination && upperDestination->getGround() &&
				    canLand(upperDestination) && !upperDestination->hasFlag(TILESTATE_FLOORCHANGE)) {
					transition.entry.z--;
					transition.ignoreBlockItem = true;
				}
			}
		}
		if (from.z != 7 && from.z == transition.entry.z) {
			Tile* sameFloor = g_game.map.getTile(transition.entry);
			if (!sameFloor || (!sameFloor->getGround() && !sameFloor->hasFlag(TILESTATE_BLOCKSOLID))) {
				Tile* lowerDestination = g_game.map.getTile(transition.entry.x, transition.entry.y, transition.entry.z + 1);
				if (lowerDestination && hasHeight(lowerDestination) &&
				    canLand(lowerDestination)) {
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

bool playerBotIsTraversableDoor(const Item& item)
{
	if (!g_actions) return false;
	const auto passage = g_actions->getPassageDescriptor(&item);
	// doors.lua treats any nonzero AID on an ordinary door as locked.
	return passage && (passage->access != ActionPassageAccess::Ordinary || item.getActionId() == 0);
}

std::optional<uint16_t> playerBotPassageOpenItemId(const Item& item)
{
	if (!g_actions) return std::nullopt;
	const auto passage = g_actions->getPassageDescriptor(&item);
	return passage ? std::optional<uint16_t>(passage->openItemId) : std::nullopt;
}

std::optional<uint32_t> playerBotPassageMinimumLevel(const Item& item)
{
	if (!g_actions) return std::nullopt;
	const auto passage = g_actions->getPassageDescriptor(&item);
	if (!passage) return std::nullopt;
	if (passage->access != ActionPassageAccess::Level) return 0;

	const uint32_t actionId = item.getActionId();
	if (actionId == 0) return std::numeric_limits<uint32_t>::max();
	return actionId > passage->levelActionIdOffset ? actionId - passage->levelActionIdOffset : 0;
}

bool playerBotCanTraverseDoor(const Player& player, const Item& item)
{
	if (!g_actions) return false;
	const auto passage = g_actions->getPassageDescriptor(&item);
	if (!passage) return false;
	if (const Door* door = item.getDoor(); door && !door->canUse(&player)) return false;
	if (passage->access == ActionPassageAccess::Ordinary) return item.getActionId() == 0;
	if (passage->access == ActionPassageAccess::House) return true;

	const Group* group = player.getGroup();
	if (group && group->access) return true;
	const uint32_t actionId = item.getActionId();
	return actionId != 0 && player.getLevel() >=
	       (actionId > passage->levelActionIdOffset ? actionId - passage->levelActionIdOffset : 0);
}

PlayerBotNavigationResult PlayerBotNavigator::plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
                                                   std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
                                                   uint64_t maximumExpandedNodes, Position* closestPosition,
	                                               const PlayerBotNavigationCostPolicy* costPolicy,
	                                               PlayerBotNavigationCostSummary* costSummary, bool sameFloorOnly) const

{
	return plan(player, PlayerBotNavigationGoal::exact(destination), blockedPositions, steps, expandedNodes,
	            maximumExpandedNodes, closestPosition, costPolicy, costSummary, sameFloorOnly);
}

PlayerBotNavigationResult PlayerBotNavigator::plan(Player& player, const PlayerBotNavigationGoal& goal,
	const std::set<Position>& blockedPositions, std::deque<PlayerBotNavigationStep>& steps,
	uint64_t& expandedNodes, uint64_t maximumExpandedNodes, Position* closestPosition,
	const PlayerBotNavigationCostPolicy* costPolicy, PlayerBotNavigationCostSummary* costSummary, bool sameFloorOnly) const
{
	return planFrom(player, player.getPosition(), goal, blockedPositions, steps, expandedNodes, maximumExpandedNodes,
	                closestPosition, costPolicy, costSummary, sameFloorOnly);
}

bool PlayerBotNavigator::validateStep(Player& player, Position from, const PlayerBotNavigationStep& step,
                                      const std::set<Position>& blockedPositions) const
{
	if (step.action == PlayerBotNavigationAction::NpcTravel) return false;
	// Even an absent tile is a dependency of the retained route's change watch.
	Tile* target = g_game.map.getTile(step.target);
	Tile* destination = g_game.map.getTile(step.expectedPosition);
	if (!target || !destination || blockedPositions.count(step.target) ||
	    blockedPositions.count(step.expectedPosition)) return false;

	const Direction adjacent = getDirectionTo(from, step.target);
	if (getNextPosition(adjacent, from) != step.target) return false;
	if (step.action == PlayerBotNavigationAction::Move) {
		PlayerBotNavigationStep live;
		return step.direction == adjacent &&
		       resolveMove(player, from, step.direction, blockedPositions, live) &&
		       live.target == step.target && live.expectedPosition == step.expectedPosition;
	}
	if (step.action == PlayerBotNavigationAction::UseShovel) {
		PlayerBotNavigationStep live;
		const auto passage = playerBotShovelPassage(step.itemId);
		if (!passage || step.itemId != passage->closedItemId ||
		    step.expectedItemId != passage->openItemId ||
		    !resolveShovelPassage(player, from, step, blockedPositions, live) ||
		    live.target != step.target || live.expectedPosition != step.expectedPosition) return false;
		if (live.action == PlayerBotNavigationAction::Move) return true;
		return live.action == PlayerBotNavigationAction::UseShovel &&
		       live.itemId == step.itemId && live.expectedItemId == step.expectedItemId &&
		       canOccupy(player, destination);
	}
	if (step.action == PlayerBotNavigationAction::UseDoor) {
		Item* door = findItem(target, step.itemId);
		PlayerBotWalkTransition transition;
		if (!door || !playerBotCanTraverseDoor(player, *door) ||
		    playerBotPassageOpenItemId(*door) != std::optional<uint16_t>(step.expectedItemId) ||
		    !playerBotResolveWalkTransition(from, adjacent, transition) ||
		    transition.entry != step.target || transition.destination != step.expectedPosition) return false;
		// The closed door itself blocks queryAdd. Check the tile as it would be
		// after opening, without mutating the item or ignoring other blockers.
		if (!target->getGround() || (!player.getParent() && target->hasFlag(TILESTATE_NOLOGOUT)) ||
		    Item::items[target->getGround()->getID()].blockSolid) return false;
		if (const TileItemVector* items = target->getItemList()) {
			for (const Item* item : *items) {
				if (item != door && Item::items[item->getID()].blockSolid) return false;
			}
		}
		if (destination != target) return canOccupy(player, destination);
		if (canOccupy(player, target)) return true;
		if (const MagicField* field = target->getFieldItem(); field && field->getDamage() != 0) return false;
		if (const Tile* source = player.getTile(); source && player.isPzLocked()) {
			if (source->hasFlag(TILESTATE_PVPZONE) != target->hasFlag(TILESTATE_PVPZONE) ||
			    (!source->hasFlag(TILESTATE_NOPVPZONE) && target->hasFlag(TILESTATE_NOPVPZONE)) ||
			    (!source->hasFlag(TILESTATE_PROTECTIONZONE) && target->hasFlag(TILESTATE_PROTECTIONZONE))) return false;
		}
		return true;
	}
	if (step.action == PlayerBotNavigationAction::UseRope) {
		Item* ground = target->getGround();
		Position upstairs;
		return ground && ground->getID() == step.itemId && contains(ropeSpotIds, step.itemId) &&
		       g_game.findItemOfType(&player, ropeItemId, true) &&
		       moveUpstairsDestination(player, step.target, upstairs) && upstairs == step.expectedPosition;
	}
	if (step.action == PlayerBotNavigationAction::Use) {
		if (!findItem(target, step.itemId) || !canOccupy(player, destination)) return false;
		if (contains(ladderIds, step.itemId)) {
			Position upstairs;
			return moveUpstairsDestination(player, step.target, upstairs) && upstairs == step.expectedPosition;
		}
		return contains(downUseIds, step.itemId) && step.target.z < MAP_MAX_LAYERS - 1 &&
		       step.expectedPosition == Position(step.target.x, step.target.y, step.target.z + 1);
	}
	return false;
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

bool PlayerBotNavigator::resolveShovelPassage(Player& player, const Position& from,
	                                           const PlayerBotNavigationStep& passage,
	                                           const std::set<Position>& blockedPositions,
	                                           PlayerBotNavigationStep& step) const
{
	if (passage.action != PlayerBotNavigationAction::UseShovel ||
	    passage.target.z >= MAP_MAX_LAYERS - 1 ||
	    passage.expectedPosition != Position(passage.target.x, passage.target.y, passage.target.z + 1) ||
	    blockedPositions.find(passage.target) != blockedPositions.end() ||
	    blockedPositions.find(passage.expectedPosition) != blockedPositions.end()) return false;
	const Direction direction = getDirectionTo(from, passage.target);
	if (getNextPosition(direction, from) != passage.target) return false;
	Tile* tile = g_game.map.getTile(passage.target);
	const Item* passageItem = tile ? playerBotShovelPassageItem(*tile, passage.itemId) : nullptr;
	if (!passageItem) return false;
	const bool canUseShovel = g_game.findItemOfType(&player, shovelItemId, true) != nullptr;
	const auto action = playerBotResolveShovelPassageAction(passage.itemId, passageItem->getID(), canUseShovel);
	if (!action) return false;
	if (*action == PlayerBotNavigationAction::Move) {
		if (!resolveMove(player, from, direction, blockedPositions, step) ||
		    step.expectedPosition != passage.expectedPosition) return false;
		step.topologyPortal = passage.topologyPortal;
		return true;
	}
	const auto descriptor = playerBotShovelPassage(passageItem->getID());
	if (!descriptor) return false;
	step = passage;
	step.itemId = descriptor->closedItemId;
	step.expectedItemId = descriptor->openItemId;
	return true;
}

PlayerBotNavigationResult PlayerBotNavigator::planFrom(Player& player, const Position& start, const Position& destination,
	                                                    const std::set<Position>& blockedPositions,
	                                                    std::deque<PlayerBotNavigationStep>& steps, uint64_t& expandedNodes,
	                                                    uint64_t maximumExpandedNodes, Position* closestPosition,
	                                                    const PlayerBotNavigationCostPolicy* costPolicy,
	                                                    PlayerBotNavigationCostSummary* costSummary, bool sameFloorOnly) const
{
	return planFrom(player, start, PlayerBotNavigationGoal::exact(destination), blockedPositions, steps, expandedNodes,
	                maximumExpandedNodes, closestPosition, costPolicy, costSummary, sameFloorOnly);
}

PlayerBotNavigationResult PlayerBotNavigator::planFrom(Player& player, const Position& start, const PlayerBotNavigationGoal& goal,
	const std::set<Position>& blockedPositions, std::deque<PlayerBotNavigationStep>& steps,
	uint64_t& expandedNodes, uint64_t maximumExpandedNodes, Position* closestPosition,
	const PlayerBotNavigationCostPolicy* costPolicy, PlayerBotNavigationCostSummary* costSummary, bool sameFloorOnly) const
{
	PlayerBotPathSearch search(start, goal, maximumExpandedNodes);
	// Synchronous execution callers retain the same total allowance. Queue-pop
	// slices may yield on stale labels; only hunt selection returns to dispatcher.
	while (!advance(player, search, blockedPositions, std::max<uint64_t>(maximumExpandedNodes, 1), costPolicy, sameFloorOnly)) {}
	steps = std::move(search.steps);
	expandedNodes = search.expanded;
	if (closestPosition) *closestPosition = search.closest;
	if (costSummary) *costSummary = search.summary;
	return *search.result;
}

std::optional<PlayerBotNavigationResult> PlayerBotNavigator::advance(
	Player& player, PlayerBotPathSearch& search, const std::set<Position>& blockedPositions,
	uint64_t slice, const PlayerBotNavigationCostPolicy* costPolicy, bool sameFloorOnly,
	const PlayerBotTopologyHeuristic* heuristic, const PlayerBotRouteCorridor* corridor) const
{
	const auto& start = search.start;
	const auto& goal = search.goal;
	if (!search.sourceTree && goal.reached(start)) return search.result = PlayerBotNavigationResult::Reached;
	if (!search.sourceTree && playerBotNavigationExactGoalBlocked(goal, blockedPositions)) {
		return search.result = PlayerBotNavigationResult::Unreachable;
	}
	auto expand = [&](const Position& position) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		arcs.reserve(16);
		auto addCandidate = [&](const Position& to,
		                        uint32_t movementCost, uint32_t exposureMs, const PlayerBotNavigationStep& step) {
			if (!search.sourceTree && !search.unbounded && !isInsideSearchBounds(to, start, goal)) return;
			if (corridor && !corridor->contains(PlayerBotTopology::instance().walkNode(to))) return;
			arcs.push_back({step, movementCost, costPolicy ? costPolicy->dangerCost(to, exposureMs) : 0,
			               costPolicy ? costPolicy->dangerAt(to) : 0});
		};
		for (Direction direction : directions) {
			Position next;
			if (!resolveWalk(player, position, direction, blockedPositions, next)) {
				continue;
			}
			if (sameFloorOnly && next.z != start.z) continue;
			PlayerBotNavigationStep step;
			step.action = PlayerBotNavigationAction::Move;
			step.direction = direction;
			step.target = getNextPosition(direction, position);
			if (search.ordinaryOnly && next != step.target) continue;
			step.expectedPosition = next;
			if (!search.ordinaryOnly && next != step.target) {
				Tile* entryTile = g_game.map.getTile(step.target);
				const Item* entryPassage = entryTile ? playerBotShovelPassageItem(*entryTile) : nullptr;
				const auto shovelPassage = entryPassage ? playerBotShovelPassage(entryPassage->getID()) : std::nullopt;
				if (shovelPassage && entryPassage->getID() == shovelPassage->openItemId) {
					// Keep a live-open local move recognizable if the hole closes before dispatch.
					step.action = PlayerBotNavigationAction::UseShovel;
					step.itemId = shovelPassage->closedItemId;
					step.expectedItemId = shovelPassage->openItemId;
					step.topologyPortal = true;
				}
			}
			addCandidate(next,
			             (direction & DIRECTION_DIAGONAL_MASK) ? diagonalCost : cardinalCost,
			             player.getStepDuration(direction), step);
		}

		for (Direction direction : directions) {
			const Position target = getNextPosition(direction, position);
			Tile* tile = g_game.map.getTile(target);
			if (!tile) {
				continue;
			}

			auto addDirectUse = [&](uint16_t itemId, PlayerBotNavigationAction action, const Position& expected,
			                        uint16_t expectedItemId = 0) {
				if (blockedPositions.find(target) != blockedPositions.end() ||
				    blockedPositions.find(expected) != blockedPositions.end() ||
				    (sameFloorOnly && expected.z != start.z) ||
				    (search.ordinaryOnly && (action != PlayerBotNavigationAction::UseDoor || expected != target ||
				                             target.z != start.z))) {
					return;
				}
				PlayerBotNavigationStep step;
				step.action = action;
				step.target = target;
				step.expectedPosition = expected;
				step.itemId = itemId;
				step.expectedItemId = expectedItemId;
				step.topologyPortal = action == PlayerBotNavigationAction::UseShovel;
				if (!validateStep(player, position, step, blockedPositions)) return;
				addCandidate(expected, transitionCost, 1000, step);
			};

			Item* ground = tile->getGround();
			if (!search.ordinaryOnly && ground && contains(ropeSpotIds, ground->getID()) &&
			    g_game.findItemOfType(&player, ropeItemId, true)) {
				Position expected;
				if (moveUpstairsDestination(player, target, expected)) {
					addDirectUse(ground->getID(), PlayerBotNavigationAction::UseRope, expected);
				}
			}
			const Item* passageItem = playerBotShovelPassageItem(*tile);
			const auto shovelPassage = passageItem ? playerBotShovelPassage(passageItem->getID()) : std::nullopt;
			if (!search.ordinaryOnly && shovelPassage && passageItem->getID() == shovelPassage->closedItemId &&
			    g_game.findItemOfType(&player, shovelItemId, true) && target.z < MAP_MAX_LAYERS - 1) {
				addDirectUse(shovelPassage->closedItemId, PlayerBotNavigationAction::UseShovel,
				             Position(target.x, target.y, target.z + 1), shovelPassage->openItemId);
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
				if (!search.ordinaryOnly && contains(ladderIds, itemId)) {
					Position expected;
					if (moveUpstairsDestination(player, target, expected)) {
						addDirectUse(itemId, PlayerBotNavigationAction::Use, expected);
					}
				} else if (!search.ordinaryOnly && contains(downUseIds, itemId) && target.z < MAP_MAX_LAYERS - 1) {
					addDirectUse(itemId, PlayerBotNavigationAction::Use,
					             Position(target.x, target.y, target.z + 1));
				} else if (const auto openItemId = playerBotPassageOpenItemId(*item);
				           openItemId && playerBotCanTraverseDoor(player, *item)) {
					addDirectUse(itemId, PlayerBotNavigationAction::UseDoor, target, *openItemId);
				}
			}
		}
		return arcs;
	};
	return search.advance(slice, expand, [&](const Position& p) { return goal.reached(p); },
	                      [&](const Position& p) { return remainingCost(p, goal); }, cardinalCost,
	                      [&](const Position& p) {
		                      if (search.ordinaryOnly) return playerBotOrdinaryRemainingCost(p, goal);
		                      if (heuristic) return heuristic->estimate(p);
		                      return goal.reached(p) ? 0u : cardinalCost;
	                      });
}
