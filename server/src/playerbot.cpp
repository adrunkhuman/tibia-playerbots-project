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

PlayerBotManager::RecordPtr PlayerBotManager::byName(const std::string& name) const
{
	for (const auto& entry : records) {
		if (strcasecmp(entry.second->name.c_str(), name.c_str()) == 0) return entry.second;
	}
	return {};
}

PlayerBotManager::RecordPtr PlayerBotManager::byPlayerId(uint32_t id) const
{
	for (const auto& entry : records) {
		if (entry.second->state.managed() && entry.second->controller && entry.second->controller->playerId == id) return entry.second;
	}
	return {};
}

bool PlayerBotManager::owns(const std::string& name) const { return !!byName(name); }

bool PlayerBotManager::announcementsEnabled(uint32_t playerGuid) const
{
	return announcementSettings.enabled(playerGuid);
}

bool PlayerBotManager::handleLogControl(Player& sender, const std::string& receiver, const std::string& text)
{
	const PlayerBotLogControl control = parseLogControl(text);
	const RecordPtr record = byName(receiver);
	if (!record || !record->state.active() || control == PlayerBotLogControl::None) return false;
	Player* bot = g_game.getPlayerByGUID(record->guid);
	if (!bot || !bot->isPlayerBot() || bot->getAccount() != record->accountId ||
	    !record->controller || !record->controller->turnRouter.running()) return false;
	const bool enabled = control == PlayerBotLogControl::On;
	announcementSettings.set(record->guid, enabled);
	sender.sendPrivateMessage(bot, TALKTYPE_PRIVATE,
	    "Global announcements for " + record->name + " are now " + (enabled ? "on." : "off."));
	return true;
}

void PlayerBotManager::emitLifecycleFor(const std::string& name, uint32_t guid,
                                        const std::string& controllerId, const char* status,
                                        const Position& position, const std::string& fields) const
{
	std::string detail = "\"source\":\"manager\",\"status\":" + jsonString(status) +
	                     ",\"related_controller_id\":" +
	                     (controllerId.empty() ? std::string("null") : jsonString(controllerId));
	if (!fields.empty()) detail += ',' + fields;
	emitPlayerbotEvent(name, guid, {}, "lifecycle", position, detail);
}

void PlayerBotManager::emitLifecycle(const Record& record, const char* status, const Position& position,
                                     const std::string& fields) const
{
	emitLifecycleFor(record.name, record.guid, record.activeControllerId, status, position, fields);
}

bool PlayerBotManager::registered(const Record& record) const
{
	Database& db = Database::getInstance();
	DBResult_ptr result = db.storeQuery(
		"SELECT `players`.`id` FROM `player_bots` JOIN `players` ON `players`.`id` = `player_bots`.`player_id` "
		"WHERE `players`.`id` = " + std::to_string(record.guid) +
		" AND `players`.`account_id` = " + std::to_string(record.accountId) +
		" AND `players`.`name` = " + db.escapeString(record.name) +
		" AND `players`.`deletion` = 0 LIMIT 1");
	return result && result->getNumber<uint32_t>("id") == record.guid;
}

bool PlayerBotManager::start()
{
	Database& db = Database::getInstance();
	DBResult_ptr result = db.storeQuery(
		"SELECT `players`.`id`, `players`.`name`, `players`.`account_id` FROM `player_bots` "
		"JOIN `players` ON `players`.`id` = `player_bots`.`player_id` "
		"WHERE `players`.`deletion` = 0 ORDER BY `players`.`id`");
	if (!result) {
		std::cout << "[Warning - PlayerBotManager] Unable to read playerbot roster." << std::endl;
		return false;
	}
	std::vector<uint32_t> pending;
	do {
		const uint32_t guid = result->getNumber<uint32_t>("id");
		const uint32_t accountId = result->getNumber<uint32_t>("account_id");
		const std::string name = result->getString("name");
		if (guid == 0 || accountId == 0 || name.empty() || records.count(guid) || byName(name)) {
			emitLifecycleFor(name, guid, {}, "startup_failed", Position(), "\"reason\":\"invalid_roster_identity\"");
			continue;
		}
		auto record = std::make_shared<Record>();
		record->guid = guid;
		record->accountId = accountId;
		record->name = name;
		records.emplace(guid, std::move(record));
		pending.push_back(guid);
	} while (result->next());

	// The first activation remains synchronous for connectionless fixtures. Later
	// activations defer placement and start(), both of which run synchronous Lua/work.
	for (size_t index = 0; index < pending.size(); ++index) {
		Record& record = *records.at(pending[index]);
		const uint64_t generation = lifecycleClock.renew(record.lifecycleGeneration);
		if (index == 0) activate(record.guid, generation);
		else record.lifecycleEventId = g_scheduler.addEvent(createSchedulerTask(
			static_cast<uint32_t>(index * 1500), ([this, guid = record.guid, generation] { activate(guid, generation); })));
	}
	return true;
}

void PlayerBotManager::cancelEvent(Record& record)
{
	lifecycleClock.renew(record.lifecycleGeneration);
	if (record.lifecycleEventId) g_scheduler.stopEvent(record.lifecycleEventId);
	record.lifecycleEventId = 0;
}

void PlayerBotManager::activate(uint32_t guid, uint64_t generation)
{
	const auto it = records.find(guid);
	if (it == records.end() || !it->second->state.managed() ||
	    !PlayerBotLifecycleClock::accepts(it->second->lifecycleGeneration, generation) || stopping) return;
	RecordPtr record = it->second;
	record->lifecycleEventId = 0;
	load(record, false);
}

bool PlayerBotManager::load(const RecordPtr& record, bool recovered)
{
	const char* failure = recovered ? "recovery_failed" : "startup_failed";
	if (!record->state.managed()) return false;
	if (record->controller || g_game.getPlayerByName(record->name) || g_game.getPlayerByGUID(record->guid)) {
		emitLifecycle(*record, failure, Position(), "\"reason\":\"character_or_controller_already_online\"");
		return false;
	}
	if (!registered(*record)) {
		emitLifecycle(*record, failure, Position(), "\"reason\":\"registration_lookup_failed\"");
		return false;
	}
	Player* player = new Player(nullptr);
	if (!IOLoginData::loadPlayerById(player, record->guid)) {
		delete player;
		emitLifecycle(*record, failure, Position(), "\"reason\":\"player_load_failed\"");
		return false;
	}
	if (player->getGUID() != record->guid || player->getAccount() != record->accountId ||
	    player->getName() != record->name) {
		delete player;
		emitLifecycle(*record, failure, Position(), "\"reason\":\"identity_mismatch\"");
		return false;
	}
	const uint64_t placementGeneration = record->lifecycleGeneration;
	player->setPlayerBot(true);
	player->setLastLoginSaved(std::max<time_t>(time(nullptr), player->getLastLoginSaved() + 1));
	if (!g_game.placeCreature(player, player->getLoginPosition()) &&
	    !g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
		const Position failedPosition = player->getLoginPosition();
		delete player;
		emitLifecycle(*record, failure, failedPosition, "\"reason\":\"placement_failed\"");
		return false;
	}
	// Login scripts can remove a player synchronously. Never install a controller
	// on a rejected placement or an explicitly terminated manager record.
	if (!record->state.managed() || record->lifecycleGeneration != placementGeneration || player->isRemoved() ||
	    g_game.getPlayerByID(player->getID()) != player) {
		const Position position = player->getPosition();
		if (!player->isRemoved() && g_game.getPlayerByID(player->getID()) == player) g_game.removeCreature(player);
		emitLifecycle(*record, failure, position, "\"reason\":\"placement_not_registered\"");
		return false;
	}
	const int32_t speedBonus = std::clamp<int32_t>(g_config.getNumber(ConfigManager::PLAYERBOT_SPEED_BONUS), 0, 1000);
	if (speedBonus) g_game.changeSpeed(player, speedBonus);
	if (!record->state.managed() || record->lifecycleGeneration != placementGeneration || player->isRemoved()) return false;
	record->activeControllerId = std::to_string(record->guid) + "-" + std::to_string(++nextControllerGeneration);
	record->controller = std::make_shared<PlayerBotController>(*player, record->activeControllerId, record->huntRegionCooldowns);
	record->lastSpawnedAt = std::chrono::steady_clock::now();
	const uint64_t generation = record->lifecycleGeneration;
	const auto startingController = record->controller;
	// Initialization can execute normal game actions and receive synchronous events.
	// Route those to the new controller too, including death during start().
	record->state.activate();
	startingController->start(player->getPosition(), recovered, record->consecutiveDeaths);
	if (!record->state.managed() || !PlayerBotLifecycleClock::accepts(record->lifecycleGeneration, generation) ||
	    record->controller != startingController || player->isRemoved() ||
	    g_game.getPlayerByID(player->getID()) != player) return false;
	// A focused fixture may pause or stop its controller during start(). That is
	// not a failed relog: placement and controller installation succeeded.
	return true;
}

bool PlayerBotManager::remove(uint32_t guid)
{
	const auto it = records.find(guid);
	if (it == records.end()) return false;
	RecordPtr record = it->second;
	if (!record->state.terminate()) return false;
	cancelEvent(*record);
	Player* player = g_game.getPlayerByGUID(guid);
	const Position position = player ? player->getPosition() : record->controller ? record->controller->lastPosition : Position();
	if (record->controller) record->controller->stop("manager_removed", position);
	record->controller.reset();
	if (!record->removingForRecovery && player && player->isPlayerBot() && player->getAccount() == record->accountId) {
		g_game.removeCreature(player);
	}
	return true;
}

void PlayerBotManager::shutdown()
{
	stopping = true;
	for (const auto& entry : records) remove(entry.first);
}

void PlayerBotManager::onPlayerRemoved(const Player& player)
{
	const auto it = records.find(player.getGUID());
	if (it == records.end() || !it->second->state.managed() || !player.isPlayerBot() ||
	    player.getAccount() != it->second->accountId) return;
	RecordPtr record = it->second;
	if (!record->controller || record->controller->playerId != player.getID()) {
		// A login script rejected placement before the controller existed.
		if (!record->state.recovering()) cancelEvent(*record);
		return;
	}
	if (record->awaitingDeathRemoval) {
		record->awaitingDeathRemoval = false;
		return;
	}
	if (record->removingForRecovery) return;
	record->state.terminate();
	cancelEvent(*record);
	const Position position = player.getPosition();
	record->controller->stop("controlled_player_removed", position);
	record->controller.reset();
	if (!stopping) emitLifecycle(*record, "removed", position);
}

void PlayerBotManager::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	RecordPtr record = byPlayerId(player.getID());
	if (!record || !record->state.active()) return;
	// A previously stopped controller is terminal, even if its Player later dies.
	if (record->controller->telemetry.terminalLogged()) return;
	const auto dyingController = record->controller;
	dyingController->onDeath(player, killer, mostDamageKiller);
	if (record->controller != dyingController || dyingController->telemetry.terminalLogged()) return;
	const auto now = std::chrono::steady_clock::now();
	if (record->lastSpawnedAt.time_since_epoch().count() && now - record->lastSpawnedAt >= stableLifetimeReset)
		record->consecutiveDeaths = 0;
	++record->consecutiveDeaths;
	if (!record->state.beginRecovery()) return;
	record->awaitingDeathRemoval = true;
	const uint32_t limit = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_MAX_CONSECUTIVE_DEATHS));
	if (record->consecutiveDeaths > limit) {
		dyingController->say(player, "Died; recovery abandoned after " + std::to_string(record->consecutiveDeaths) +
		                     " consecutive deaths (limit " + std::to_string(limit) + ").");
		if (!record->state.recovering() || record->controller != dyingController) return;
		emitLifecycle(*record, "recovery_abandoned", player.getPosition(),
		              "\"reason\":\"death_loop_limit\",\"death_count\":" + std::to_string(record->consecutiveDeaths) +
		                  ",\"maximum_deaths\":" + std::to_string(limit));
		const uint64_t generation = lifecycleClock.renew(record->lifecycleGeneration);
		record->lifecycleEventId = g_scheduler.addEvent(createSchedulerTask(SCHEDULER_MINTICKS,
			([this, guid = record->guid, generation] { finalizeAbandonedDeath(guid, generation); })));
		dyingController->stop("controlled_player_dead", player.getPosition());
		return;
	}
	uint64_t delay = std::min<uint64_t>(static_cast<uint64_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_RELOG_DELAY_SECONDS))) * 1000, 60000);
	for (uint32_t death = 1; death < record->consecutiveDeaths && delay < 60000; ++death)
		delay = std::min<uint64_t>(delay * 2, 60000);
	const uint64_t seconds = delay / 1000;
	dyingController->say(player, "Died; recovery scheduled in " + std::to_string(seconds) +
	                     (seconds == 1 ? " second." : " seconds."));
	scheduleRecovery(record, static_cast<uint32_t>(delay), 1);
	dyingController->stop("controlled_player_dead", player.getPosition());
}

void PlayerBotManager::onDeathComplete(const Player& player)
{
	const RecordPtr record = byPlayerId(player.getID());
	if (record && record->state.recovering() && record->guid == player.getGUID()) {
		record->awaitingDeathRemoval = false;
	}
}

void PlayerBotManager::onHealthDrain(const Player& player, uint32_t damage)
{
	if (RecordPtr record = byPlayerId(player.getID()); record && record->controller->turnRouter.running())
		record->controller->onHealthDrain(player, damage);
}

void PlayerBotManager::onLevelRestoration(const Player& player, uint32_t health, uint32_t mana)
{
	if (RecordPtr record = byPlayerId(player.getID()); record && record->controller->turnRouter.running())
		record->controller->onLevelRestoration(player, health, mana);
}

void PlayerBotManager::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	for (const auto& entry : records) {
		auto& controller = entry.second->controller;
		if (entry.second->state.active() && controller && controller->turnRouter.running())
			controller->onCombatDamage(attacker, target, damage);
	}
}

void PlayerBotManager::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	for (const auto& entry : records) {
		auto& controller = entry.second->controller;
		if (entry.second->state.active() && controller && controller->turnRouter.running())
			controller->onHealthGain(healer, target, gain);
	}
}

void PlayerBotManager::onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text)
{
	if (RecordPtr record = byPlayerId(playerId); record && record->controller->turnRouter.running())
		record->controller->onNpcReply(playerId, npcId, type, text);
}

void PlayerBotManager::scheduleRecovery(const RecordPtr& record, uint32_t delay, uint32_t attempt)
{
	if (!record->state.recovering() || !record->controller || record->lifecycleEventId) return;
	emitLifecycle(*record, "recovery_scheduled", record->controller->lastPosition,
	              "\"reason\":\"death\",\"death_count\":" + std::to_string(record->consecutiveDeaths) +
	                  ",\"relog_attempt\":" + std::to_string(attempt) + ",\"delay_ms\":" + std::to_string(delay));
	const uint64_t generation = lifecycleClock.renew(record->lifecycleGeneration);
	record->lifecycleEventId = g_scheduler.addEvent(createSchedulerTask(delay,
		([this, guid = record->guid, generation, attempt] { recover(guid, generation, attempt); })));
}

void PlayerBotManager::recover(uint32_t guid, uint64_t generation, uint32_t attempt)
{
	const auto it = records.find(guid);
	if (it == records.end() || !it->second->state.recovering() ||
	    !PlayerBotLifecycleClock::accepts(it->second->lifecycleGeneration, generation) || stopping) return;
	RecordPtr record = it->second;
	record->lifecycleEventId = 0;
	const Position position = record->controller ? record->controller->lastPosition : Position();
	auto abandonOwnershipConflict = [&] {
		emitLifecycle(*record, "recovery_abandoned", position, "\"reason\":\"ownership_conflict\"");
		if (record->controller) record->controller->stop("ownership_conflict", position);
		record->controller.reset();
		record->state.suspend();
	};
	if (!registered(*record)) {
		abandonOwnershipConflict();
		return;
	}
	if (Player* existing = g_game.getPlayerByGUID(guid)) {
		if (!existing->isPlayerBot() || existing->getAccount() != record->accountId || existing->getName() != record->name) {
			abandonOwnershipConflict();
			return;
		}
		if (record->controller) record->controller->stop("controlled_player_dead", position);
		record->removingForRecovery = true;
		const bool removed = g_game.removeCreature(existing, false);
		record->removingForRecovery = false;
		record->awaitingDeathRemoval = false;
		if (!removed) emitLifecycle(*record, "recovery_failed", position, "\"reason\":\"player_removal_failed\"");
	}
	if (record->controller) record->controller->stop("controlled_player_dead", position);
	record->controller.reset();
	if (records.find(guid) == records.end() || record->lifecycleGeneration != generation) return;
	if (load(record, true)) return;
	if (records.find(guid) == records.end() || record->lifecycleGeneration != generation) return;
	emitLifecycle(*record, "recovery_failed", position,
	              "\"reason\":\"relog_failed\",\"relog_attempt\":" + std::to_string(attempt));
	if (attempt >= maximumRelogAttempts) {
		emitLifecycle(*record, "recovery_abandoned", position, "\"reason\":\"relog_attempt_limit\"");
		record->state.suspend();
		return;
	}
	const uint32_t delay = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_RELOG_DELAY_SECONDS))) * 1000, 60000));
	emitLifecycle(*record, "recovery_scheduled", position,
	              "\"reason\":\"relog_retry\",\"death_count\":" + std::to_string(record->consecutiveDeaths) +
	                  ",\"relog_attempt\":" + std::to_string(attempt + 1) + ",\"delay_ms\":" + std::to_string(delay));
	const uint64_t next = lifecycleClock.renew(record->lifecycleGeneration);
	record->lifecycleEventId = g_scheduler.addEvent(createSchedulerTask(delay,
		([this, guid, next, attempt] { recover(guid, next, attempt + 1); })));
}

void PlayerBotManager::finalizeAbandonedDeath(uint32_t guid, uint64_t generation)
{
	const auto it = records.find(guid);
	if (it == records.end() || !it->second->state.recovering() ||
	    !PlayerBotLifecycleClock::accepts(it->second->lifecycleGeneration, generation) || stopping) return;
	RecordPtr record = it->second;
	record->lifecycleEventId = 0;
	const Position position = record->controller ? record->controller->lastPosition : Position();
	if (record->controller) record->controller->stop("controlled_player_dead", position);
	if (Player* existing = g_game.getPlayerByGUID(guid);
	    existing && existing->isPlayerBot() && existing->getAccount() == record->accountId)
	{
		record->removingForRecovery = true;
		g_game.removeCreature(existing, false);
		record->removingForRecovery = false;
	}
	record->controller.reset();
	record->state.suspend();
}
