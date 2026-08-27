/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTCOMBATTARGET_H
#define FS_PLAYERBOTCOMBATTARGET_H

#include <cstdint>
#include <string>

#include "position.h"

struct PlayerBotExpectedCorpse {
	uint16_t itemId = 0;
	bool lootable = false;
};

struct PlayerBotTarget {
	uint32_t id = 0;
	Position position;
	std::string name;
};

struct PlayerBotTraversalTarget : PlayerBotTarget {
	PlayerBotExpectedCorpse expectedCorpse;
};

struct PlayerBotDefensiveTarget : PlayerBotTarget {
	bool routeCritical = false;
};

struct PlayerBotTraversalCandidate : PlayerBotTarget {
	PlayerBotExpectedCorpse expectedCorpse;
	bool attacksPlayer = false;
};

#endif
