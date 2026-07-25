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

#include "playerbot.h"

#include "database.h"
#include "game.h"
#include "iologindata.h"
#include "item.h"
#include "monster.h"
#include "player.h"
#include "scheduler.h"
#include "tile.h"

#include <ctime>
#include <set>

extern Game g_game;

namespace {
	constexpr uint32_t navigationInterval = 1000;
	constexpr uint32_t blockedRouteRetryInterval = 500;
	constexpr std::chrono::seconds summaryInterval(60);
	constexpr std::chrono::seconds repeatedEventInterval(60);
	constexpr Position scenarioStart(32099, 32211, 7);
	constexpr Position sewerGratePosition(32097, 32205, 7);
	constexpr uint16_t sewerGrateItemId = 430;
	constexpr const char* botAccountName = "bot-one";

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
}

class PlayerBotController
{
	public:
		explicit PlayerBotController(const Player& player) :
			playerId(player.getID()), playerGuid(player.getGUID()), playerName(player.getName()) {}

		void start(const Position& position)
		{
			previousPosition = position;
			lastPosition = position;
			emit("lifecycle", position, "\"status\":\"online\",\"message\":\"Playerbot online\"");
			if (position.z == sewerGratePosition.z + 1) {
				setStage(ScenarioStage::FindRat, position);
			} else if (position.z != scenarioStart.z) {
				stop("unsupported_saved_floor", position);
				return;
			}
			schedule(navigationInterval);
		}

	private:
		enum class ScenarioStage : uint8_t {
			ToStart,
			ToGrate,
			UseGrate,
			FindRat,
			ApproachRat,
			Combat,
			Explore,
			Stopped,
		};

		struct RatCandidate {
			uint32_t id;
			Position position;
			int32_t chebyshevDistance;
		};

		struct Counters {
			uint64_t decisions = 0;
			uint64_t decisionTimeUs = 0;
			uint64_t pathfindingCalls = 0;
			uint64_t pathfindingFailures = 0;
			uint64_t pathfindingTimeUs = 0;
			uint64_t actionsAttempted = 0;
			uint64_t actionsFailed = 0;
			uint64_t stuckEvents = 0;
			uint64_t suppressedEvents = 0;
		};

		class DecisionTimer
		{
			public:
				explicit DecisionTimer(PlayerBotController& controller) : controller(controller)
				{
					controller.decisionStarted = std::chrono::steady_clock::now();
					controller.decisionActive = true;
					++controller.counters.decisions;
				}
				~DecisionTimer()
				{
					controller.counters.decisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - controller.decisionStarted).count();
					controller.decisionActive = false;
				}

			private:
				PlayerBotController& controller;
		};

		void schedule(uint32_t interval)
		{
			g_scheduler.addEvent(createSchedulerTask(interval, std::bind(&PlayerBotController::navigate, this)));
		}

		static const char* stageName(ScenarioStage stage)
		{
			switch (stage) {
				case ScenarioStage::ToStart: return "to_start";
				case ScenarioStage::ToGrate: return "to_grate";
				case ScenarioStage::UseGrate: return "use_grate";
				case ScenarioStage::FindRat: return "find_rat";
				case ScenarioStage::ApproachRat: return "approach_rat";
				case ScenarioStage::Combat: return "combat";
				case ScenarioStage::Explore: return "explore";
				case ScenarioStage::Stopped: return "stopped";
			}
			return "unknown";
		}

		void emit(const char* event, const Position& position, const std::string& fields = {}) const
		{
			std::ostringstream output;
			output << "{\"schema\":1,\"ts\":" << jsonString(utcTimestamp())
			       << ",\"component\":\"playerbot\",\"event\":" << jsonString(event)
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

		bool shouldEmitRepeated(const std::string& key)
		{
			const auto now = std::chrono::steady_clock::now();
			auto it = repeatedEventTimes.find(key);
			if (it != repeatedEventTimes.end() && now - it->second < repeatedEventInterval) {
				++counters.suppressedEvents;
				return false;
			}

			repeatedEventTimes[key] = now;
			return true;
		}

		void setStage(ScenarioStage stage, const Position& position)
		{
			if (scenarioStage == stage) {
				return;
			}

			const ScenarioStage previousStage = scenarioStage;
			scenarioStage = stage;
			const std::string repeatKey = std::string("state:") + stageName(previousStage) + ':' + stageName(stage);
			if (!shouldEmitRepeated(repeatKey)) {
				return;
			}
			emit("state_transition", position, std::string("\"from\":") + jsonString(stageName(previousStage)) +
			     ",\"to\":" + jsonString(stageName(stage)));
		}

		void setRatTarget(uint32_t targetId, const Position& targetPosition, const Position& position, const char* reason)
		{
			if (ratId == targetId) {
				ratPosition = targetPosition;
				return;
			}

			const uint32_t previousTargetId = ratId;
			ratId = targetId;
			ratPosition = targetPosition;
			if (!shouldEmitRepeated(std::string("target:set:") + reason)) {
				return;
			}
			std::ostringstream fields;
			fields << "\"previous_target_id\":";
			if (previousTargetId == 0) {
				fields << "null";
			} else {
				fields << previousTargetId;
			}
			fields << ",\"target_id\":" << targetId
			       << ",\"target_type\":\"rat\",\"target_position\":{\"x\":" << targetPosition.x
			       << ",\"y\":" << targetPosition.y << ",\"z\":" << static_cast<uint16_t>(targetPosition.z) << '}'
			       << ",\"reason\":" << jsonString(reason);
			emit("target_changed", position, fields.str());
		}

		void clearRatTarget(const Position& position, const char* reason)
		{
			if (ratId == 0) {
				return;
			}

			const uint32_t previousTargetId = ratId;
			ratId = 0;
			if (!shouldEmitRepeated(std::string("target:clear:") + reason)) {
				return;
			}
			emit("target_changed", position, "\"previous_target_id\":" + std::to_string(previousTargetId) +
			     ",\"target_id\":null,\"reason\":" + jsonString(reason));
		}

		void logActionFailure(const char* action, const char* reason, const Position& position)
		{
			++counters.actionsFailed;
			if (!shouldEmitRepeated(std::string("action:") + action + ':' + reason)) {
				return;
			}
			emit("action_result", position, std::string("\"action\":") + jsonString(action) +
			     ",\"result\":\"failed\",\"reason\":" + jsonString(reason));
		}

		void logSummary(const Position& position, bool final)
		{
			const auto uptimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			uint64_t decisionTimeUs = counters.decisionTimeUs;
			if (decisionActive) {
				decisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - decisionStarted).count();
			}
			std::ostringstream fields;
			fields << "\"final\":" << (final ? "true" : "false")
			       << ",\"uptime_ms\":" << uptimeMs
			       << ",\"state\":" << jsonString(stageName(scenarioStage))
			       << ",\"target_id\":";
			if (ratId == 0) {
				fields << "null";
			} else {
				fields << ratId
				       << ",\"target_position\":{\"x\":" << ratPosition.x << ",\"y\":" << ratPosition.y
				       << ",\"z\":" << static_cast<uint16_t>(ratPosition.z) << '}';
			}
			fields << ",\"decisions\":" << counters.decisions
			       << ",\"decision_time_us\":" << decisionTimeUs
			       << ",\"pathfinding_calls\":" << counters.pathfindingCalls
			       << ",\"pathfinding_failures\":" << counters.pathfindingFailures
			       << ",\"pathfinding_time_us\":" << counters.pathfindingTimeUs
			       << ",\"actions_attempted\":" << counters.actionsAttempted
			       << ",\"actions_failed\":" << counters.actionsFailed
			       << ",\"stuck_events\":" << counters.stuckEvents
			       << ",\"suppressed_events\":" << counters.suppressedEvents;
			emit("summary", position, fields.str());
		}

		void maybeLogSummary(const Position& position)
		{
			const auto now = std::chrono::steady_clock::now();
			if (now - lastSummary < summaryInterval) {
				return;
			}

			logSummary(position, false);
			lastSummary = now;
		}

		void stop(const char* reason, const Position& position)
		{
			if (terminalLogged) {
				return;
			}

			setStage(ScenarioStage::Stopped, position);
			logSummary(position, true);
			emit("terminal", position, std::string("\"reason\":") + jsonString(reason));
			terminalLogged = true;
		}

		bool findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams)
		{
			++counters.pathfindingCalls;
			const auto startedAt = std::chrono::steady_clock::now();
			const bool found = player->getPathTo(target, result, pathParams);
			counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - startedAt).count();
			if (!found) {
				++counters.pathfindingFailures;
			}
			return found;
		}

		void navigate()
		{
			DecisionTimer decisionTimer(*this);
			Player* player = g_game.getPlayerByID(playerId);
			if (!player) {
				stop("controlled_player_not_found", lastPosition);
				return;
			}
			if (!player->isPlayerBot()) {
				stop("controlled_player_ownership_lost", lastPosition);
				return;
			}
			if (player->isRemoved()) {
				emit("lifecycle", lastPosition, "\"status\":\"removed\"");
				stop("controlled_player_removed", lastPosition);
				return;
			}
			if (player->isDead()) {
				emit("lifecycle", player->getPosition(), "\"status\":\"dead\"");
				stop("controlled_player_dead", player->getPosition());
				return;
			}

			const Position currentPosition = player->getPosition();
			lastPosition = currentPosition;
			maybeLogSummary(currentPosition);
			if (currentPosition.z == sewerGratePosition.z + 1) {
				visitedPositions.insert(currentPosition);
				frontierPositions.erase(currentPosition);
				constexpr int32_t discoveryRadius = 6;
				for (int32_t offsetX = -discoveryRadius; offsetX <= discoveryRadius; ++offsetX) {
					for (int32_t offsetY = -discoveryRadius; offsetY <= discoveryRadius; ++offsetY) {
						Position position(currentPosition.x + offsetX, currentPosition.y + offsetY, currentPosition.z);
						if (g_game.map.getTile(position) && visitedPositions.find(position) == visitedPositions.end()) {
							frontierPositions.insert(position);
						}
					}
				}
			}
			if (stepPending) {
				if (currentPosition == previousPosition) {
					const int32_t walkDelay = player->getWalkDelay();
					if (walkDelay > 0) {
						schedule(static_cast<uint32_t>(walkDelay) + SCHEDULER_MINTICKS);
						return;
					}
					route.clear();
					logActionFailure("move", "position_unchanged", currentPosition);
					if (++blockedStepCount == 3) {
						++counters.stuckEvents;
						emit("stuck", currentPosition, "\"reason\":\"repeated_blocked_movement\",\"blocked_steps\":3");
					}
				} else {
					blockedStepCount = 0;
				}
				stepPending = false;
			}

			switch (scenarioStage) {
				case ScenarioStage::ToStart:
					if (currentPosition == scenarioStart) {
						setStage(ScenarioStage::ToGrate, currentPosition);
						route.clear();
					} else if (route.empty()) {
						planRoute(player, scenarioStart, 0);
					}
					break;
				case ScenarioStage::ToGrate:
					if (Position::areInRange<1, 1, 0>(currentPosition, sewerGratePosition)) {
						setStage(ScenarioStage::UseGrate, currentPosition);
						route.clear();
					} else if (route.empty()) {
						planRoute(player, sewerGratePosition, 1);
					}
					break;
				case ScenarioStage::UseGrate:
					if (currentPosition.z == sewerGratePosition.z + 1) {
						grateUseAttempts = 0;
						setStage(ScenarioStage::FindRat, currentPosition);
						route.clear();
					} else {
						++counters.actionsAttempted;
						const bool grateAvailable = useSewerGrate(player);
						if (!grateAvailable) {
							logActionFailure("use_item", "sewer_grate_unavailable", currentPosition);
						}
						if (++grateUseAttempts >= 20) {
							if (grateAvailable) {
								logActionFailure("use_item", "no_floor_transition", currentPosition);
							}
							stop("sewer_grate_transition_failed", currentPosition);
						}
					}
					break;
				case ScenarioStage::FindRat:
				case ScenarioStage::ApproachRat: {
					Creature* rat = g_game.getCreatureByID(ratId);
					if (!rat || rat->isRemoved() || rat->isDead() || rat->getName() != "Rat" ||
					    !player->canSee(rat->getPosition())) {
						clearRatTarget(currentPosition, "target_invalid");
						route.clear();
					} else if (rat->getPosition() != ratPosition) {
						ratPosition = rat->getPosition();
						route.clear();
					}

					if (planRatRoute(player)) {
						if (scenarioStage != ScenarioStage::Combat) {
							setStage(ScenarioStage::ApproachRat, currentPosition);
						}
					} else {
						setStage(ScenarioStage::Explore, currentPosition);
					}
					break;
				}
				case ScenarioStage::Combat: {
					Creature* rat = g_game.getCreatureByID(ratId);
					if (!rat || rat->isRemoved() || rat->isDead()) {
						g_game.playerSetAttackedCreature(playerId, 0);
						clearRatTarget(currentPosition, "target_unavailable");
						route.clear();
						setStage(ScenarioStage::FindRat, currentPosition);
					} else if (player->getAttackedCreature() != rat || !player->canSee(rat->getPosition())) {
						g_game.playerSetAttackedCreature(playerId, 0);
						clearRatTarget(currentPosition, "target_lost");
						route.clear();
						setStage(ScenarioStage::FindRat, currentPosition);
					}
					break;
				}
				case ScenarioStage::Explore:
					if (planRatRoute(player)) {
						if (scenarioStage != ScenarioStage::Combat) {
							setStage(ScenarioStage::ApproachRat, currentPosition);
						}
					} else if (route.empty() && !planExplorationRoute(player)) {
						stop("no_reachable_exploration_frontier", currentPosition);
					}
					break;
				case ScenarioStage::Stopped:
					return;
			}

			if (scenarioStage == ScenarioStage::Stopped) {
				return;
			}

			if (scenarioStage == ScenarioStage::UseGrate || scenarioStage == ScenarioStage::Combat) {
				schedule(navigationInterval);
				return;
			}

			if (route.empty()) {
				schedule(blockedRouteRetryInterval);
				return;
			}

			previousPosition = currentPosition;
			stepPending = true;
			++counters.actionsAttempted;
			g_game.playerMove(playerId, route.back());
			route.pop_back();
			schedule(navigationInterval);
		}

		bool planRoute(Player* player, const Position& target, int32_t targetDistance)
		{
			FindPathParams pathParams;
			pathParams.maxSearchDist = 64;
			pathParams.minTargetDist = targetDistance;
			pathParams.maxTargetDist = targetDistance;
			if (findPath(player, target, route, pathParams) && !route.empty()) {
				fixedTargetRouteFailureCount = 0;
				return true;
			}

			if (++fixedTargetRouteFailureCount >= 20) {
				stop("repeated_route_planning_failures", player->getPosition());
			}
			return false;
		}

		bool planExplorationRoute(Player* player)
		{
			struct ExplorationCandidate {
				Position position;
				int32_t chebyshevDistance;
			};

			const Position currentPosition = player->getPosition();
			std::vector<ExplorationCandidate> candidates;
			for (const Position& position : frontierPositions) {
				candidates.push_back({position, std::max(Position::getDistanceX(currentPosition, position),
				                                                 Position::getDistanceY(currentPosition, position))});
			}

			std::sort(candidates.begin(), candidates.end(), [](const ExplorationCandidate& left, const ExplorationCandidate& right) {
				if (left.chebyshevDistance != right.chebyshevDistance) {
				return left.chebyshevDistance < right.chebyshevDistance;
				}
				return left.position < right.position;
			});

			FindPathParams pathParams;
			pathParams.maxSearchDist = 256;
			pathParams.minTargetDist = 0;
			pathParams.maxTargetDist = 0;
			for (const ExplorationCandidate& candidate : candidates) {
				std::vector<Direction> candidateRoute;
				if (!findPath(player, candidate.position, candidateRoute, pathParams) || candidateRoute.empty()) {
					continue;
				}

				route = std::move(candidateRoute);
				return true;
			}
			return false;
		}

		bool useSewerGrate(Player* player)
		{
			Tile* tile = g_game.map.getTile(sewerGratePosition);
			if (!tile) {
				return false;
			}

			Item* grate = nullptr;
			if (Item* ground = tile->getGround(); ground && ground->getID() == sewerGrateItemId) {
				grate = ground;
			} else if (TileItemVector* items = tile->getItemList()) {
				for (Item* item : *items) {
					if (item->getID() == sewerGrateItemId) {
						grate = item;
						break;
					}
				}
			}

			if (!grate) {
				return false;
			}

			const int32_t stackPosition = tile->getThingIndex(grate);
			if (stackPosition < 0 || stackPosition > UINT8_MAX) {
				return false;
			}

			g_game.playerUseItem(playerId, sewerGratePosition, static_cast<uint8_t>(stackPosition), 0, grate->getClientID());
			return true;
		}

		bool planRatRoute(Player* player)
		{
			SpectatorVec spectators;
			g_game.map.getSpectators(spectators, player->getPosition());

			std::vector<RatCandidate> candidates;
			uint32_t adjacentRatId = 0;
			for (Creature* creature : spectators) {
				if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || creature->getName() != "Rat" || !player->canSee(creature->getPosition())) {
					continue;
				}

				const Position& position = creature->getPosition();
				if (Position::areInRange<1, 1, 0>(player->getPosition(), position)) {
					if (creature->getAttackedCreature() == player) {
						adjacentRatId = creature->getID();
						break;
					}
					if (adjacentRatId == 0) {
						adjacentRatId = creature->getID();
					}
				}

				candidates.push_back({creature->getID(), position, std::max(Position::getDistanceX(player->getPosition(), position), Position::getDistanceY(player->getPosition(), position))});
			}

			if (adjacentRatId != 0) {
				Creature* adjacentRat = g_game.getCreatureByID(adjacentRatId);
				if (adjacentRat) {
					setRatTarget(adjacentRatId, adjacentRat->getPosition(), player->getPosition(), "adjacent_target");
					route.clear();
					++counters.actionsAttempted;
					g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
					g_game.playerSetAttackedCreature(playerId, ratId);
					if (player->getAttackedCreature() == adjacentRat) {
						setStage(ScenarioStage::Combat, player->getPosition());
						return true;
					}
					logActionFailure("attack", "target_rejected", player->getPosition());
				}
				return false;
			}

			if (ratId != 0 && !route.empty()) {
				return true;
			}

			std::sort(candidates.begin(), candidates.end(), [](const RatCandidate& left, const RatCandidate& right) {
				return left.chebyshevDistance == right.chebyshevDistance ? left.id < right.id : left.chebyshevDistance < right.chebyshevDistance;
			});

			FindPathParams pathParams;
			pathParams.maxSearchDist = 64;
			pathParams.minTargetDist = 1;
			pathParams.maxTargetDist = 1;
			for (const RatCandidate& candidate : candidates) {
				std::vector<Direction> candidateRoute;
				if (!findPath(player, candidate.position, candidateRoute, pathParams) || candidateRoute.empty()) {
					continue;
				}

				setRatTarget(candidate.id, candidate.position, player->getPosition(), "reachable_target");
				route = std::move(candidateRoute);
				return true;
			}

			clearRatTarget(player->getPosition(), "no_reachable_target");
			return false;
		}

		uint32_t playerId;
		uint32_t playerGuid;
		std::string playerName;
		uint32_t ratId = 0;
		Position previousPosition;
		Position lastPosition;
		Position ratPosition;
		std::vector<Direction> route;
		std::set<Position> visitedPositions;
		std::set<Position> frontierPositions;
		ScenarioStage scenarioStage = ScenarioStage::ToStart;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t grateUseAttempts = 0;
		uint32_t blockedStepCount = 0;
		Counters counters;
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> repeatedEventTimes;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point lastSummary = started;
		std::chrono::steady_clock::time_point decisionStarted;
		bool stepPending = false;
		bool decisionActive = false;
		bool terminalLogged = false;
};

PlayerBotManager g_playerBots;

PlayerBotManager::~PlayerBotManager() = default;

bool PlayerBotManager::spawn(const std::string& name)
{
	if (g_game.getPlayerByName(name)) {
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
		return false;
	}

	Player* player = new Player(nullptr);
	if (!IOLoginData::loadPlayerById(player, result->getNumber<uint32_t>("id"))) {
		delete player;
		return false;
	}

	player->setPlayerBot(true);
	player->setLastLoginSaved(std::max<time_t>(time(nullptr), player->getLastLoginSaved() + 1));
	if (!g_game.placeCreature(player, player->getLoginPosition()) &&
	    !g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
		delete player;
		return false;
	}

	controller = std::make_unique<PlayerBotController>(*player);
	controller->start(player->getPosition());
	return true;
}
