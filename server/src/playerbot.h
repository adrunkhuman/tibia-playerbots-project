/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOT_H
#define FS_PLAYERBOT_H

#include <cstdint>
#include <memory>
#include <string>

class PlayerBotController;

class PlayerBotManager
{
	public:
		~PlayerBotManager();

		bool spawn(const std::string& name);
		void onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text);

	private:
		std::unique_ptr<PlayerBotController> controller;
};

extern PlayerBotManager g_playerBots;

#endif
