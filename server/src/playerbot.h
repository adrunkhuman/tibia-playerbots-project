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

#include "position.h"

#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <string>

class PlayerBotController;
class Creature;
class Player;

namespace playerbot {
	std::string jsonString(const std::string& value);
	std::string utcTimestamp();
}

class PlayerBotManager
{
	public:
		~PlayerBotManager();

		bool spawn(const std::string& name);
		bool owns(const std::string& name) const;
		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller);
		void onHealthDrain(const Player& player, uint32_t damage);
		void onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage);
		void onHealthGain(Creature* healer, const Creature& target, uint32_t gain);
		void onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text);

	private:
		bool load(const std::string& name, bool recovered);
		void scheduleRecovery(uint32_t delay, uint32_t relogAttempt);
		void recover(uint32_t generation, uint32_t relogAttempt);
		void finalizeAbandonedDeath(uint32_t generation);

		std::shared_ptr<PlayerBotController> controller;
		std::string controlledName;
		uint32_t controlledGuid = 0;
		uint32_t recoveryEventId = 0;
		uint32_t recoveryGeneration = 0;
		uint32_t consecutiveDeaths = 0;
		std::map<Position, std::chrono::steady_clock::time_point> huntRegionCooldowns;
		std::chrono::steady_clock::time_point lastSpawnedAt;
};

extern PlayerBotManager g_playerBots;

#endif
