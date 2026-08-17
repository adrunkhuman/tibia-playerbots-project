/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTAREA_H
#define FS_PLAYERBOTAREA_H

#include <cstdint>
#include <iosfwd>

#include "position.h"

namespace playerbot {
	inline constexpr uint32_t maximumLocalPlanningDistance = 200;

	inline uint32_t localPlanningDistance(const Position& anchor, const Position& position)
	{
		return Position::getDistanceX(anchor, position) + Position::getDistanceY(anchor, position) +
		       Position::getDistanceZ(anchor, position) * 20;
	}

	inline bool isInsideLocalPlanningArea(const Position& anchor, const Position& position)
	{
		return localPlanningDistance(anchor, position) <= maximumLocalPlanningDistance;
	}
}

#endif
