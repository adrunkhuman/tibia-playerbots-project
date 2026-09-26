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
#include "playerbotlifecycle.h"

#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <string>

class PlayerBotController;
class Creature;
class Player;

namespace playerbot {
	enum class PlayerBotLogControl : uint8_t { None, On, Off };

	inline PlayerBotLogControl parseLogControl(const std::string& text)
	{
		if (text == "logs on") return PlayerBotLogControl::On;
		if (text == "logs off") return PlayerBotLogControl::Off;
		return PlayerBotLogControl::None;
	}

	class PlayerBotAnnouncementSettings
	{
		public:
			bool enabled(uint32_t playerGuid) const
			{
				const auto setting = settings.find(playerGuid);
				return setting == settings.end() || setting->second;
			}

			void set(uint32_t playerGuid, bool enabled)
			{
				settings[playerGuid] = enabled;
			}

		private:
			std::map<uint32_t, bool> settings;
	};

	std::string jsonString(const std::string& value);
	std::string utcTimestamp();
}

class PlayerBotManager
{
	public:
		~PlayerBotManager();

		// Called on the dispatcher. Reserve the entire database roster before activating anyone.
		bool start();
		bool remove(uint32_t guid);
		void shutdown();
		void onPlayerRemoved(const Player& player);
		bool owns(const std::string& name) const;
		bool handleLogControl(Player& sender, const std::string& receiver, const std::string& text);
		bool announcementsEnabled(uint32_t playerGuid) const;
		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller);
		void onDeathComplete(const Player& player);
		void onHealthDrain(const Player& player, uint32_t damage);
		void onLevelRestoration(const Player& player, uint32_t health, uint32_t mana);
		void onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage);
		void onHealthGain(Creature* healer, const Creature& target, uint32_t gain);
		void onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text);

	private:
		struct Record {
			uint32_t guid = 0;
			uint32_t accountId = 0;
			std::string name;
			std::shared_ptr<PlayerBotController> controller;
			uint32_t lifecycleEventId = 0;
			uint64_t lifecycleGeneration = 0;
			uint32_t consecutiveDeaths = 0;
			std::string activeControllerId;
			PlayerBotLifecycleState state;
			bool awaitingDeathRemoval = false;
			bool removingForRecovery = false;
			std::map<uint64_t, std::chrono::steady_clock::time_point> huntRegionCooldowns;
			std::chrono::steady_clock::time_point lastSpawnedAt;
		};
		using RecordPtr = std::shared_ptr<Record>;
		RecordPtr byName(const std::string& name) const;
		RecordPtr byPlayerId(uint32_t id) const;
		bool registered(const Record& record) const;
		bool load(const RecordPtr& record, bool recovered);
		void activate(uint32_t guid, uint64_t generation);
		void cancelEvent(Record& record);
		void emitLifecycleFor(const std::string& name, uint32_t playerGuid, const std::string& relatedControllerId,
		                      const char* status, const Position& position, const std::string& fields = {}) const;
		void scheduleRecovery(const RecordPtr& record, uint32_t delay, uint32_t relogAttempt);
		void recover(uint32_t guid, uint64_t generation, uint32_t relogAttempt);
		void finalizeAbandonedDeath(uint32_t guid, uint64_t generation);
		void emitLifecycle(const Record& record, const char* status, const Position& position,
		                   const std::string& fields = {}) const;

		std::map<uint32_t, RecordPtr> records;
		playerbot::PlayerBotAnnouncementSettings announcementSettings;
		PlayerBotLifecycleClock lifecycleClock;
		uint64_t nextControllerGeneration = 0;
		bool stopping = false;
};

extern PlayerBotManager g_playerBots;

#endif
