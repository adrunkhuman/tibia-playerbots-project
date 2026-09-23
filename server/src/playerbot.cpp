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

#include "playerbotcontroller.h"

#include <mutex>

using namespace playerbot;

namespace playerbot {
	std::string jsonString(const std::string& value)
	{
		std::ostringstream escaped;
		escaped << '"';
		for (unsigned char character : value) {
			switch (character) {
				case '"': escaped << "\\\""; break;
				case '\\': escaped << "\\\\"; break;
				case '\b': escaped << "\\b"; break;
				case '\f': escaped << "\\f"; break;
				case '\n': escaped << "\\n"; break;
				case '\r': escaped << "\\r"; break;
				case '\t': escaped << "\\t"; break;
				default:
					if (character < 0x20) {
						escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<uint16_t>(character) << std::dec;
					} else {
						escaped << character;
					}
			}
		}
		escaped << '"';
		return escaped.str();
	}

	std::string utcTimestamp()
	{
		const auto now = std::chrono::system_clock::now();
		const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		const std::time_t time = std::chrono::system_clock::to_time_t(now);
		std::tm utcTime;
#ifdef _WIN32
		gmtime_s(&utcTime, &time);
#else
		gmtime_r(&time, &utcTime);
#endif

		std::ostringstream timestamp;
		timestamp << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S") << '.'
		          << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
		return timestamp.str();
	}

	void emitPlayerbotEvent(const std::string& playerName, uint32_t playerGuid, const std::string& controllerId,
	                        const char* event, const Position& position, const std::string& fields)
	{
		static const std::string serverRunId = [] {
			std::ostringstream id;
			id << std::hex << std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			return id.str();
		}();
		static std::mutex outputMutex;
		static uint64_t sequence = 0;
		std::lock_guard<std::mutex> lock(outputMutex);
		std::ostringstream output;
		output << "{\"schema\":1,\"ts\":" << jsonString(utcTimestamp())
		       << ",\"component\":\"playerbot\",\"event\":" << jsonString(event)
		       << ",\"server_run_id\":" << jsonString(serverRunId)
		       << ",\"controller_id\":" << (controllerId.empty() ? "null" : jsonString(controllerId))
		       << ",\"sequence\":" << ++sequence
		       << ",\"bot\":" << jsonString(playerName)
		       << ",\"player_id\":" << playerGuid
		       << ",\"position\":{\"x\":" << position.x << ",\"y\":" << position.y
		       << ",\"z\":" << static_cast<uint16_t>(position.z) << '}';
		if (!fields.empty()) {
			output << ',' << fields;
		}
		output << "}\n";
		std::cout << output.str() << std::flush;
	}
}

PlayerBotManager g_playerBots;

PlayerBotManager::~PlayerBotManager() = default;

bool PlayerBotManager::owns(const std::string& name) const
{
	return !controlledName.empty() && strcasecmp(controlledName.c_str(), name.c_str()) == 0;
}

bool PlayerBotManager::announcementsEnabled(uint32_t playerGuid) const
{
	return announcementSettings.enabled(playerGuid);
}

bool PlayerBotManager::handleLogControl(Player& sender, const std::string& receiver, const std::string& text)
{
	const PlayerBotLogControl control = parseLogControl(text);
	if (!owns(receiver) || control == PlayerBotLogControl::None) return false;
	Player* bot = g_game.getPlayerByName(controlledName);
	if (!bot || !bot->isPlayerBot() || bot->getGUID() != controlledGuid) return false;
	const bool enabled = control == PlayerBotLogControl::On;
	announcementSettings.set(controlledGuid, enabled);
	sender.sendPrivateMessage(bot, TALKTYPE_PRIVATE,
	    "Global announcements for " + controlledName + " are now " + (enabled ? "on." : "off."));
	return true;
}

void PlayerBotManager::emitLifecycleFor(const std::string& name, uint32_t playerGuid,
                                        const std::string& relatedControllerId, const char* status,
                                        const Position& position, const std::string& fields) const
{
	std::string detail = "\"source\":\"manager\",\"status\":" + jsonString(status) +
	                     ",\"related_controller_id\":" +
	                     (relatedControllerId.empty() ? std::string("null") : jsonString(relatedControllerId));
	if (!fields.empty()) detail += ',' + fields;
	emitPlayerbotEvent(name, playerGuid, {}, "lifecycle", position, detail);
}

void PlayerBotManager::emitLifecycle(const char* status, const Position& position, const std::string& fields) const
{
	emitLifecycleFor(controlledName, controlledGuid, activeControllerId, status, position, fields);
}

void PlayerBotManager::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	if (!controller || player.getID() != controller->playerId) {
		return;
	}

	controller->onDeath(player, killer, mostDamageKiller);
	if (recoveryEventId != 0) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (lastSpawnedAt.time_since_epoch().count() != 0 && now - lastSpawnedAt >= stableLifetimeReset) {
		consecutiveDeaths = 0;
	}
	++consecutiveDeaths;
	const uint32_t maximumDeaths = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_MAX_CONSECUTIVE_DEATHS));
	if (consecutiveDeaths > maximumDeaths) {
		controller->say(player, "Died; recovery abandoned after " +
		                std::to_string(consecutiveDeaths) + " consecutive deaths (limit " +
		                std::to_string(maximumDeaths) + ").");
		emitLifecycle("recovery_abandoned", player.getPosition(),
		              "\"reason\":\"death_loop_limit\",\"death_count\":" + std::to_string(consecutiveDeaths) +
		                  ",\"maximum_deaths\":" + std::to_string(maximumDeaths));
		const uint32_t generation = ++recoveryGeneration;
		recoveryEventId = g_scheduler.addEvent(createSchedulerTask(
			SCHEDULER_MINTICKS, std::bind(&PlayerBotManager::finalizeAbandonedDeath, this, generation)));
		return;
	}

	uint64_t delay = std::min<uint64_t>(static_cast<uint64_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_RELOG_DELAY_SECONDS))) * 1000, 60000);
	for (uint32_t death = 1; death < consecutiveDeaths && delay < 60000; ++death) {
		delay = std::min<uint64_t>(delay * 2, 60000);
	}
	const uint64_t delaySeconds = delay / 1000;
	controller->say(player, "Died; recovery scheduled in " + std::to_string(delaySeconds) +
	                (delaySeconds == 1 ? " second." : " seconds."));
	scheduleRecovery(static_cast<uint32_t>(delay), 1);
}

void PlayerBotManager::onHealthDrain(const Player& player, uint32_t damage)
{
	if (controller) {
		controller->onHealthDrain(player, damage);
	}
}

void PlayerBotManager::onLevelRestoration(const Player& player, uint32_t health, uint32_t mana)
{
	if (controller) {
		controller->onLevelRestoration(player, health, mana);
	}
}

void PlayerBotManager::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	if (controller) {
		controller->onCombatDamage(attacker, target, damage);
	}
}

void PlayerBotManager::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	if (controller) {
		controller->onHealthGain(healer, target, gain);
	}
}

void PlayerBotManager::onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text)
{
	if (controller) {
		controller->onNpcReply(playerId, npcId, type, text);
	}
}

bool PlayerBotManager::spawn(const std::string& name)
{
	if (controller) {
		emitLifecycleFor(name, 0, activeControllerId, "startup_failed", Position(),
		                 "\"reason\":\"controller_already_active\",\"active_bot\":" +
		                     jsonString(controlledName) + ",\"active_player_id\":" + std::to_string(controlledGuid));
		return false;
	}
	if (g_game.getPlayerByName(name)) {
		emitLifecycleFor(name, 0, {}, "startup_failed", Position(), "\"reason\":\"character_already_online\"");
		return false;
	}
	consecutiveDeaths = 0;
	return load(name, false);
}

bool PlayerBotManager::load(const std::string& name, bool recovered)
{
	if (controller || g_game.getPlayerByName(name)) {
		emitLifecycleFor(name, recovered ? controlledGuid : 0, recovered ? activeControllerId : std::string{},
		                 recovered ? "recovery_failed" : "startup_failed", Position(),
		                 "\"reason\":\"character_or_controller_already_online\"");
		return false;
	}

	Database& database = Database::getInstance();
	DBResult_ptr result = database.storeQuery(
		"SELECT `players`.`id` FROM `player_bots` "
		"JOIN `players` ON `players`.`id` = `player_bots`.`player_id` "
		"JOIN `accounts` ON `accounts`.`id` = `players`.`account_id` "
		"WHERE `players`.`name` = " + database.escapeString(name) +
		" AND `accounts`.`name` = " + database.escapeString(botAccountName) +
		" AND `players`.`deletion` = 0 LIMIT 1");
	if (!result) {
		emitLifecycleFor(name, recovered ? controlledGuid : 0, recovered ? activeControllerId : std::string{},
		                 recovered ? "recovery_failed" : "startup_failed", Position(),
		                 "\"reason\":\"registration_lookup_failed\"");
		return false;
	}
	controlledName = name;
	controlledGuid = result->getNumber<uint32_t>("id");

	Player* player = new Player(nullptr);
	if (!IOLoginData::loadPlayerById(player, controlledGuid)) {
		delete player;
		emitLifecycle(recovered ? "recovery_failed" : "startup_failed", Position(),
		              "\"reason\":\"player_load_failed\"");
		return false;
	}

	player->setPlayerBot(true);
	player->setLastLoginSaved(std::max<time_t>(time(nullptr), player->getLastLoginSaved() + 1));
	if (!g_game.placeCreature(player, player->getLoginPosition()) &&
	    !g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
		const Position failedPosition = player->getLoginPosition();
		delete player;
		emitLifecycle(recovered ? "recovery_failed" : "startup_failed", failedPosition,
		              "\"reason\":\"placement_failed\"");
		return false;
	}
	if (player->isRemoved() || g_game.getPlayerByID(player->getID()) != player) {
		emitLifecycle(recovered ? "recovery_failed" : "startup_failed", player->getPosition(),
		              "\"reason\":\"placement_not_registered\"");
		return false;
	}
	const int32_t speedBonus = std::clamp<int32_t>(g_config.getNumber(ConfigManager::PLAYERBOT_SPEED_BONUS), 0, 1000);
	if (speedBonus != 0) {
		g_game.changeSpeed(player, speedBonus);
	}

	activeControllerId = std::to_string(controlledGuid) + "-" + std::to_string(++controllerGeneration);
	controller = std::make_shared<PlayerBotController>(*player, activeControllerId, huntRegionCooldowns);
	lastSpawnedAt = std::chrono::steady_clock::now();
	controller->start(player->getPosition(), recovered, consecutiveDeaths);
	return true;
}

void PlayerBotManager::scheduleRecovery(uint32_t delay, uint32_t relogAttempt)
{
	if (!controller || recoveryEventId != 0) {
		return;
	}

	emitLifecycle("recovery_scheduled", controller->lastPosition,
	              "\"reason\":\"death\",\"death_count\":" + std::to_string(consecutiveDeaths) +
	                  ",\"relog_attempt\":" + std::to_string(relogAttempt) +
	                  ",\"delay_ms\":" + std::to_string(delay));
	const uint32_t generation = ++recoveryGeneration;
	recoveryEventId = g_scheduler.addEvent(createSchedulerTask(
		delay, std::bind(&PlayerBotManager::recover, this, generation, relogAttempt)));
}

void PlayerBotManager::recover(uint32_t generation, uint32_t relogAttempt)
{
	if (generation != recoveryGeneration || controlledName.empty()) {
		return;
	}
	recoveryEventId = 0;
	Position recoveryPosition = controller ? controller->lastPosition : Position();

	if (Player* existing = g_game.getPlayerByName(controlledName)) {
		if (!existing->isPlayerBot() || existing->getGUID() != controlledGuid) {
			emitLifecycle("recovery_abandoned", recoveryPosition, "\"reason\":\"ownership_conflict\"");
			return;
		}
		if (controller) {
			controller->stop("controlled_player_dead", recoveryPosition);
		}
		if (!g_game.removeCreature(existing, false)) {
			emitLifecycle("recovery_failed", recoveryPosition, "\"reason\":\"player_removal_failed\"");
			return;
		}
	}

	if (controller) {
		controller->stop("controlled_player_dead", recoveryPosition);
		controller.reset();
	}
	if (load(controlledName, true)) {
		return;
	}

	emitLifecycle("recovery_failed", recoveryPosition,
	              "\"reason\":\"relog_failed\",\"relog_attempt\":" + std::to_string(relogAttempt));
	if (relogAttempt >= maximumRelogAttempts) {
		emitLifecycle("recovery_abandoned", recoveryPosition, "\"reason\":\"relog_attempt_limit\"");
		return;
	}

	const uint64_t configuredRetryDelay = static_cast<uint64_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_RELOG_DELAY_SECONDS))) * 1000;
	const uint32_t retryDelay = static_cast<uint32_t>(std::min<uint64_t>(configuredRetryDelay, 60000));
	emitLifecycle("recovery_scheduled", recoveryPosition,
	              "\"reason\":\"relog_retry\",\"death_count\":" + std::to_string(consecutiveDeaths) +
	                  ",\"relog_attempt\":" + std::to_string(relogAttempt + 1) +
	                  ",\"delay_ms\":" + std::to_string(retryDelay));
	const uint32_t retryGeneration = ++recoveryGeneration;
	recoveryEventId = g_scheduler.addEvent(createSchedulerTask(
		retryDelay, std::bind(&PlayerBotManager::recover, this, retryGeneration, relogAttempt + 1)));
}

void PlayerBotManager::finalizeAbandonedDeath(uint32_t generation)
{
	if (generation != recoveryGeneration) {
		return;
	}
	recoveryEventId = 0;
	const Position position = controller ? controller->lastPosition : Position();
	if (controller) {
		controller->stop("controlled_player_dead", position);
	}
	if (Player* existing = g_game.getPlayerByName(controlledName);
	    existing && existing->isPlayerBot() && existing->getGUID() == controlledGuid) {
		g_game.removeCreature(existing, false);
	}
	controller.reset();
}
