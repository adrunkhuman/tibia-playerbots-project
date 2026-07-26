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
#include <ostream>
#include <set>

#include "position.h"

class Player;

enum class PlayerBotNavigationAction : uint8_t {
	Move,
	Use,
	UseRope,
	UseShovel,
	UseDoor,
};

struct PlayerBotNavigationStep {
	PlayerBotNavigationAction action = PlayerBotNavigationAction::Move;
	Direction direction = DIRECTION_NONE;
	Position target;
	Position expectedPosition;
	uint16_t itemId = 0;
};

class PlayerBotNavigator
{
	public:
		bool plan(Player& player, const Position& destination, const std::set<Position>& blockedPositions,
		          std::deque<PlayerBotNavigationStep>& steps,
		          uint64_t& expandedNodes) const;
};

#endif
