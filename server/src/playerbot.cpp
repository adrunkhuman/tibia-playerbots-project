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

#include "container.h"
#include "condition.h"
#include "configmanager.h"
#include "database.h"
#include "game.h"
#include "iologindata.h"
#include "item.h"
#include "monster.h"
#include "player.h"
#include "scheduler.h"
#include "tile.h"

#include <array>
#include <ctime>
#include <set>

extern Game g_game;
extern ConfigManager g_config;

namespace {
	constexpr uint32_t navigationInterval = 1000;
	constexpr uint32_t blockedRouteRetryInterval = 500;
	constexpr std::chrono::seconds summaryInterval(60);
	constexpr std::chrono::seconds repeatedEventInterval(60);
	constexpr Position scenarioStart(32099, 32211, 7);
	constexpr Position sewerGratePosition(32097, 32205, 7);
	constexpr uint16_t sewerGrateItemId = 430;
	constexpr uint16_t ratCorpseItemId = 5964;
	constexpr uint16_t cheeseItemId = 2696;
	constexpr uint32_t cheeseLimit = 3;
	constexpr int32_t cheeseFoodTicks = 108000;
	constexpr int32_t maximumFoodSeconds = 1200;
	constexpr uint8_t corpseContainerId = 0;
	constexpr uint8_t backpackContainerId = 1;
	constexpr uint32_t maxCorpseSearchAttempts = 4;
	constexpr uint16_t ropeItemId = 2120;
	constexpr uint16_t ladderItemId = 1386;
	constexpr uint16_t ropeSpotItemId = 384;
	constexpr uint32_t maxBlockedTraversalSteps = 10;
	constexpr int32_t traversalCheckpointStorage = 45017;
	constexpr int32_t traversalTripStorage = 45018;
	constexpr size_t traversalLoopStart = 1;
	constexpr size_t traversalForwardEnd = 6;
	constexpr size_t traversalReverseEnd = 12;
	constexpr std::chrono::seconds traversalCombatTimeout(60);
	constexpr std::chrono::seconds traversalTargetSuppression(120);
	constexpr const char* botAccountName = "bot-one";

	enum class TraversalAction : uint8_t {
		Walk,
		UseRope,
		UseLadder,
	};

	struct TraversalCheckpoint {
		Position target;
		TraversalAction action;
		uint8_t expectedFloor;
		const char* name;
	};

	constexpr std::array<TraversalCheckpoint, 13> traversalCheckpoints = {{
		{Position(32091, 32179, 7), TraversalAction::Walk, 6, "temple_stairs_up"},
		{Position(32091, 32169, 6), TraversalAction::Walk, 7, "north_stairs_down"},
		{Position(32094, 32138, 7), TraversalAction::Walk, 8, "cave_stairs_down"},
		{Position(32101, 32130, 8), TraversalAction::UseRope, 7, "rope_up"},
		{Position(32096, 32119, 7), TraversalAction::UseLadder, 6, "first_ladder_up"},
		{Position(32092, 32127, 6), TraversalAction::UseLadder, 5, "second_ladder_up"},
		{Position(32101, 32128, 5), TraversalAction::UseLadder, 4, "third_ladder_up"},
		{Position(32101, 32129, 4), TraversalAction::Walk, 5, "third_ladder_down"},
		{Position(32092, 32128, 5), TraversalAction::Walk, 6, "second_ladder_down"},
		{Position(32096, 32120, 6), TraversalAction::Walk, 7, "first_ladder_down"},
		{Position(32101, 32131, 7), TraversalAction::Walk, 8, "rope_hole_down"},
		{Position(32094, 32138, 8), TraversalAction::Walk, 7, "cave_stairs_up"},
		{Position(32091, 32169, 7), TraversalAction::Walk, 6, "north_stairs_up"},
	}};

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
			playerId(player.getID()), playerGuid(player.getGUID()), playerName(player.getName())
		{
			int32_t storedCheckpoint;
			int32_t storedTrips;
			if (player.getStorageValue(traversalCheckpointStorage, storedCheckpoint) && storedCheckpoint >= 0 &&
			    static_cast<size_t>(storedCheckpoint) < traversalCheckpoints.size()) {
				traversalCheckpoint = static_cast<size_t>(storedCheckpoint);
				restoredTraversalState = true;
			}
			if (player.getStorageValue(traversalTripStorage, storedTrips) && storedTrips >= 0) {
				completedTrips = static_cast<uint32_t>(storedTrips);
			}
		}

		void start(const Position& position)
		{
			previousPosition = position;
			lastPosition = position;
			if (!restoredTraversalState) {
				for (size_t index = 0; index < traversalCheckpoints.size(); ++index) {
					if (position == traversalCheckpoints[index].target) {
						traversalCheckpoint = index;
						break;
					}
				}
				if (position == Position(32091, 32169, 7)) {
					traversalCheckpoint = 2;
				}
			}
			emit("lifecycle", position, "\"status\":\"online\",\"message\":\"Playerbot online\"");
			const uint32_t requiredTrips = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_TRAVERSAL_TRIPS));
			if (completedTrips >= requiredTrips && position == Position(32101, 32129, 4)) {
				setStage(ScenarioStage::Complete, position);
				schedule(navigationInterval);
				return;
			}
			if (restoredTraversalState && position.z == traversalCheckpoints[traversalCheckpoint].expectedFloor) {
				if (Player* player = g_game.getPlayerByID(playerId)) {
					advanceTraversal(player, position);
					if (scenarioStage == ScenarioStage::Complete || scenarioStage == ScenarioStage::Stopped) {
						return;
					}
				}
			}
			setStage(ScenarioStage::Traverse, position);
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
			LootCorpse,
			Explore,
			Traverse,
			TraversalCombat,
			Complete,
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
				case ScenarioStage::LootCorpse: return "loot_corpse";
				case ScenarioStage::Explore: return "explore";
				case ScenarioStage::Traverse: return "traverse";
				case ScenarioStage::TraversalCombat: return "traversal_combat";
				case ScenarioStage::Complete: return "complete";
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

		void logLootSuccess(uint16_t itemId, uint32_t count, uint32_t inventoryCount, const Position& position)
		{
			std::ostringstream fields;
			fields << "\"action\":\"loot\",\"result\":\"success\",\"item_id\":" << itemId
			       << ",\"count\":" << count << ",\"inventory_count\":" << inventoryCount;
			emit("action_result", position, fields.str());
		}

		uint32_t getInventoryItemCount(const Player& player, uint16_t itemId) const
		{
			return static_cast<const Cylinder&>(player).getItemTypeCount(itemId);
		}

		int32_t getFoodTicks(const Player& player) const
		{
			Condition* condition = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT, 0);
			return condition ? condition->getTicks() : 0;
		}

		bool canEatCheese(const Player& player) const
		{
			return getFoodTicks(player) / 1000 + cheeseFoodTicks / 1000 < maximumFoodSeconds;
		}

		void logEatSuccess(uint32_t inventoryCount, int32_t foodTicks, const Position& position)
		{
			std::ostringstream fields;
			fields << "\"action\":\"eat\",\"result\":\"success\",\"item_id\":" << cheeseItemId
			       << ",\"count\":1,\"inventory_count\":" << inventoryCount << ",\"food_ticks\":" << foodTicks;
			emit("action_result", position, fields.str());
		}

		bool handleFood(Player* player, const Position& currentPosition)
		{
			if (pendingLootItemId != 0) {
				return false;
			}

			const auto now = std::chrono::steady_clock::now();
			if (pendingEat) {
				const uint32_t inventoryCount = getInventoryItemCount(*player, cheeseItemId);
				const int32_t foodTicks = getFoodTicks(*player);
				if (inventoryCount + 1 == pendingEatInventoryCount && foodTicks > pendingEatFoodTicks) {
					logEatSuccess(inventoryCount, foodTicks, currentPosition);
				} else if (inventoryCount == pendingEatInventoryCount && !canEatCheese(*player)) {
					// The normal food action leaves the item untouched when the player is full.
				} else {
					logActionFailure("eat", "consumption_not_verified", currentPosition);
					eatRetryAfter = now + std::chrono::seconds(5);
				}
				pendingEat = false;
			}

			if (now < eatRetryAfter || !canEatCheese(*player)) {
				return false;
			}

			Item* cheese = g_game.findItemOfType(player, cheeseItemId, true);
			if (!cheese) {
				return false;
			}
			if (!player->canDoAction()) {
				return true;
			}

			pendingEatInventoryCount = getInventoryItemCount(*player, cheeseItemId);
			pendingEatFoodTicks = getFoodTicks(*player);
			pendingEat = true;
			++counters.actionsAttempted;
			g_game.playerUseItem(playerId, Position(0xFFFF, 0, 0), 0, 0, cheese->getClientID());
			return true;
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

		void logTraversalSuccess(const TraversalCheckpoint& checkpoint, const Position& position)
		{
			std::ostringstream fields;
			fields << "\"action\":\"transition\",\"result\":\"success\",\"checkpoint\":"
			       << jsonString(checkpoint.name) << ",\"trip\":" << completedTrips;
			emit("action_result", position, fields.str());
		}

		void setTraversalTarget(Creature* target, const Position& position)
		{
			ratId = target->getID();
			ratPosition = target->getPosition();
			std::ostringstream fields;
			fields << "\"previous_target_id\":null,\"target_id\":" << ratId
			       << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(target->getName())
			       << ",\"target_position\":{\"x\":" << ratPosition.x << ",\"y\":" << ratPosition.y
			       << ",\"z\":" << static_cast<uint16_t>(ratPosition.z) << "},\"reason\":\"visible_monster\"";
			emit("target_changed", position, fields.str());
		}

		bool attackVisibleMonster(Player* player, const Position& currentPosition)
		{
			SpectatorVec spectators;
			g_game.map.getSpectators(spectators, currentPosition);
			std::sort(spectators.begin(), spectators.end(), [&currentPosition](Creature* left, Creature* right) {
				const int32_t leftDistance = std::max(std::abs(currentPosition.getX() - left->getPosition().getX()),
				                                             std::abs(currentPosition.getY() - left->getPosition().getY()));
				const int32_t rightDistance = std::max(std::abs(currentPosition.getX() - right->getPosition().getX()),
				                                              std::abs(currentPosition.getY() - right->getPosition().getY()));
				return leftDistance == rightDistance ? left->getID() < right->getID() : leftDistance < rightDistance;
			});

			for (Creature* creature : spectators) {
				if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || !player->canSee(creature->getPosition())) {
					continue;
				}
				auto suppressed = suppressedTraversalTargets.find(creature->getID());
				if (suppressed != suppressedTraversalTargets.end()) {
					if (std::chrono::steady_clock::now() < suppressed->second) {
						continue;
					}
					suppressedTraversalTargets.erase(suppressed);
				}
				++counters.actionsAttempted;
				g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
				g_game.playerSetAttackedCreature(playerId, creature->getID());
				if (player->getAttackedCreature() != creature) {
					continue;
				}

				setTraversalTarget(creature, currentPosition);
				combatStarted = std::chrono::steady_clock::now();
				route.clear();
				setStage(ScenarioStage::TraversalCombat, currentPosition);
				return true;
			}
			return false;
		}

		void finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
		{
			g_game.playerSetAttackedCreature(playerId, 0);
			clearRatTarget(currentPosition, reason);
			route.clear();
			setStage(ScenarioStage::Traverse, currentPosition);
		}

		void processTraversalCombat(Player* player, const Position& currentPosition)
		{
			Creature* target = g_game.getCreatureByID(ratId);
			if (!target || target->isRemoved() || target->isDead()) {
				finishTraversalCombat(player, currentPosition, "target_defeated");
			} else if (!player->canSee(target->getPosition()) || player->getAttackedCreature() != target) {
				finishTraversalCombat(player, currentPosition, "target_lost");
			} else if (std::chrono::steady_clock::now() - combatStarted >= traversalCombatTimeout) {
				logActionFailure("attack", "combat_timeout", currentPosition);
				suppressedTraversalTargets[target->getID()] = std::chrono::steady_clock::now() + traversalTargetSuppression;
				finishTraversalCombat(player, currentPosition, "combat_timeout");
			} else {
				ratPosition = target->getPosition();
			}
			schedule(navigationInterval);
		}

		bool planTraversalRoute(Player* player, const TraversalCheckpoint& checkpoint)
		{
			const int32_t targetDistance = checkpoint.action == TraversalAction::Walk ? 0 : 1;
			if (targetDistance == 1 && Position::areInRange<1, 1, 0>(player->getPosition(), checkpoint.target)) {
				return true;
			}
			if (!route.empty()) {
				return true;
			}

			FindPathParams pathParams;
			pathParams.maxSearchDist = 128;
			pathParams.clearSight = false;
			pathParams.minTargetDist = targetDistance;
			pathParams.maxTargetDist = targetDistance;
			std::vector<Direction> plannedRoute;
			if (findPath(player, checkpoint.target, plannedRoute, pathParams) && !plannedRoute.empty()) {
				route = std::move(plannedRoute);
				fixedTargetRouteFailureCount = 0;
				return true;
			}

			const Position currentPosition = player->getPosition();
			const int32_t offsetX = checkpoint.target.x - currentPosition.x;
			const int32_t offsetY = checkpoint.target.y - currentPosition.y;
			const int32_t distance = std::max(std::abs(offsetX), std::abs(offsetY));
			if (distance > 8) {
				const int32_t stepX = offsetX * 8 / distance;
				const int32_t stepY = offsetY * 8 / distance;
				const bool mostlyVertical = std::abs(offsetY) >= std::abs(offsetX);
				for (int32_t lateral = 0; lateral <= 8; ++lateral) {
					for (int32_t sign : {-1, 1}) {
						if (lateral == 0 && sign == 1) {
							continue;
						}
						Position intermediate(currentPosition.x + stepX + (mostlyVertical ? lateral * sign : 0),
						                      currentPosition.y + stepY + (mostlyVertical ? 0 : lateral * sign),
						                      currentPosition.z);
						std::vector<Direction> intermediateRoute;
						FindPathParams intermediateParams = pathParams;
						intermediateParams.maxSearchDist = 32;
						if (findPath(player, intermediate, intermediateRoute, intermediateParams) && !intermediateRoute.empty()) {
							route = std::move(intermediateRoute);
							fixedTargetRouteFailureCount = 0;
							return true;
						}
					}
				}
			}

			if (++fixedTargetRouteFailureCount >= 20) {
				stop("traversal_route_unavailable", player->getPosition());
			}
			return false;
		}

		Item* getCheckpointItem(const TraversalCheckpoint& checkpoint) const
		{
			Tile* tile = g_game.map.getTile(checkpoint.target);
			if (!tile) {
				return nullptr;
			}
			if (checkpoint.action == TraversalAction::UseRope) {
				Item* ground = tile->getGround();
				return ground && ground->getID() == ropeSpotItemId ? ground : nullptr;
			}

			TileItemVector* items = tile->getItemList();
			if (items) {
				for (Item* item : *items) {
					if (item->getID() == ladderItemId) {
						return item;
					}
				}
			}
			return nullptr;
		}

		bool useTraversalCheckpoint(Player* player, const TraversalCheckpoint& checkpoint)
		{
			if (!player->canDoAction()) {
				return false;
			}

			Item* target = getCheckpointItem(checkpoint);
			Tile* tile = g_game.map.getTile(checkpoint.target);
			const int32_t stackPosition = target && tile ? tile->getThingIndex(target) : -1;
			if (!target || stackPosition < 0 || stackPosition > UINT8_MAX) {
				logActionFailure("transition", "target_item_unavailable", player->getPosition());
				return false;
			}

			++counters.actionsAttempted;
			if (checkpoint.action == TraversalAction::UseLadder) {
				g_game.playerUseItem(playerId, checkpoint.target, static_cast<uint8_t>(stackPosition), 0, target->getClientID());
				return true;
			}

			Item* rope = g_game.findItemOfType(player, ropeItemId, true);
			if (!rope) {
				logActionFailure("transition", "rope_unavailable", player->getPosition());
				return false;
			}
			g_game.playerUseItemEx(playerId, Position(0xFFFF, 0, 0), 0, rope->getClientID(), checkpoint.target,
			                         static_cast<uint8_t>(stackPosition), target->getClientID());
			return true;
		}

		void completeTraversal(Player* player, const Position& currentPosition)
		{
			setStage(ScenarioStage::Complete, currentPosition);
			g_game.internalCreatureSay(player, TALKTYPE_SAY, "trip completed", false);
			logSummary(currentPosition, true);
			std::ostringstream fields;
			fields << "\"reason\":\"trips_completed\",\"trips\":" << completedTrips;
			emit("terminal", currentPosition, fields.str());
			terminalLogged = true;
		}

		void persistTraversalState(Player* player)
		{
			player->addStorageValue(traversalCheckpointStorage, static_cast<int32_t>(traversalCheckpoint));
			player->addStorageValue(traversalTripStorage, static_cast<int32_t>(completedTrips));
		}

		void advanceTraversal(Player* player, const Position& currentPosition)
		{
			const TraversalCheckpoint& checkpoint = traversalCheckpoints[traversalCheckpoint];
			transitionPending = false;
			transitionAttempts = 0;
			fixedTargetRouteFailureCount = 0;
			route.clear();

			if (traversalCheckpoint == traversalForwardEnd) {
				if (currentPosition != Position(32101, 32129, 4)) {
					stop("unexpected_trip_end_position", currentPosition);
					return;
				}
				++completedTrips;
				logTraversalSuccess(checkpoint, currentPosition);
				traversalCheckpoint = traversalForwardEnd + 1;
				const uint32_t requiredTrips = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_TRAVERSAL_TRIPS));
				if (completedTrips >= requiredTrips) {
					persistTraversalState(player);
					completeTraversal(player, currentPosition);
					return;
				}
				persistTraversalState(player);
				return;
			}

			logTraversalSuccess(checkpoint, currentPosition);
			if (traversalCheckpoint == traversalReverseEnd) {
				traversalCheckpoint = traversalLoopStart;
			} else {
				++traversalCheckpoint;
			}
			persistTraversalState(player);
		}

		void processTraversal(Player* player, const Position& currentPosition)
		{
			if (scenarioStage == ScenarioStage::Complete) {
				return;
			}
			if (scenarioStage == ScenarioStage::TraversalCombat) {
				processTraversalCombat(player, currentPosition);
				return;
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
					++blockedStepCount;
					if (blockedStepCount == 3) {
						++counters.stuckEvents;
						emit("stuck", currentPosition, "\"reason\":\"repeated_blocked_movement\",\"blocked_steps\":3");
					}
					if (blockedStepCount >= maxBlockedTraversalSteps) {
						stop("traversal_movement_blocked", currentPosition);
						return;
					}
				} else {
					blockedStepCount = 0;
				}
				stepPending = false;
			}

			const TraversalCheckpoint& checkpoint = traversalCheckpoints[traversalCheckpoint];
			if (transitionPending) {
				if (currentPosition.z == checkpoint.expectedFloor) {
					advanceTraversal(player, currentPosition);
					if (scenarioStage == ScenarioStage::Complete || scenarioStage == ScenarioStage::Stopped) {
						return;
					}
				} else if (player->getWalkDelay() > 0 || !player->canDoAction()) {
					schedule(blockedRouteRetryInterval);
					return;
				} else {
					transitionPending = false;
					route.clear();
					logActionFailure("transition", "floor_change_not_verified", currentPosition);
					if (++transitionAttempts >= 5) {
						stop("traversal_transition_failed", currentPosition);
						return;
					}
				}
			}

			if (attackVisibleMonster(player, currentPosition)) {
				schedule(navigationInterval);
				return;
			}

			const TraversalCheckpoint& currentCheckpoint = traversalCheckpoints[traversalCheckpoint];
			if (currentCheckpoint.action == TraversalAction::Walk && currentPosition == currentCheckpoint.target) {
				if (player->getWalkDelay() > 0) {
					schedule(blockedRouteRetryInterval);
					return;
				}
				previousPosition = currentPosition;
				stepPending = true;
				transitionPending = true;
				++counters.actionsAttempted;
				g_game.playerMove(playerId, DIRECTION_NORTH);
				schedule(navigationInterval);
				return;
			}
			if (!planTraversalRoute(player, currentCheckpoint)) {
				if (scenarioStage != ScenarioStage::Stopped) {
					schedule(blockedRouteRetryInterval);
				}
				return;
			}

			if (currentCheckpoint.action != TraversalAction::Walk &&
			    Position::areInRange<1, 1, 0>(currentPosition, currentCheckpoint.target)) {
				if (!player->canDoAction()) {
					schedule(blockedRouteRetryInterval);
					return;
				}
				if (useTraversalCheckpoint(player, currentCheckpoint)) {
					transitionPending = true;
					schedule(blockedRouteRetryInterval);
				} else {
					if (++transitionAttempts >= 5) {
						stop("traversal_transition_unavailable", currentPosition);
						return;
					}
					schedule(blockedRouteRetryInterval);
				}
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
			if (handleFood(player, currentPosition)) {
				schedule(blockedRouteRetryInterval);
				return;
			}
			if (scenarioStage == ScenarioStage::Traverse || scenarioStage == ScenarioStage::TraversalCombat ||
			    scenarioStage == ScenarioStage::Complete) {
				processTraversal(player, currentPosition);
				return;
			}
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
					if (!rat || rat->isRemoved() || rat->isDead()) {
						if (ratId != 0) {
							beginLoot(currentPosition);
							break;
						}
					} else if (rat->getName() != "Rat" || !player->canSee(rat->getPosition())) {
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
						if (rat) {
							ratPosition = rat->getPosition();
						}
						g_game.playerSetAttackedCreature(playerId, 0);
						beginLoot(currentPosition);
					} else if (player->getAttackedCreature() != rat || !player->canSee(rat->getPosition())) {
						g_game.playerSetAttackedCreature(playerId, 0);
						clearRatTarget(currentPosition, "target_lost");
						route.clear();
						setStage(ScenarioStage::FindRat, currentPosition);
					} else {
						ratPosition = rat->getPosition();
					}
					break;
				}
				case ScenarioStage::LootCorpse:
					lootCorpse(player, currentPosition);
					break;
				case ScenarioStage::Explore:
					if (planRatRoute(player)) {
						if (scenarioStage != ScenarioStage::Combat) {
							setStage(ScenarioStage::ApproachRat, currentPosition);
						}
					} else if (route.empty() && !planExplorationRoute(player)) {
						stop("no_reachable_exploration_frontier", currentPosition);
					}
					break;
				case ScenarioStage::Traverse:
				case ScenarioStage::TraversalCombat:
				case ScenarioStage::Complete:
					return;
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

		void beginLoot(const Position& currentPosition)
		{
			clearRatTarget(currentPosition, "target_defeated");
			route.clear();
			lootPosition = ratPosition;
			corpseSearchAttempts = 0;
			corpseOpenAttempts = 0;
			pendingLootItemId = 0;
			unavailableLootItemIds.clear();
			setStage(ScenarioStage::LootCorpse, currentPosition);
		}

		void finishLoot(Player* player, const Position& currentPosition)
		{
			player->closeContainer(corpseContainerId);
			route.clear();
			pendingLootItemId = 0;
			setStage(ScenarioStage::FindRat, currentPosition);
		}

		bool hasDesiredLoot(const Player& player, const Container& corpse) const
		{
			for (Item* item : corpse.getItemList()) {
				if (item->getID() == ITEM_GOLD_COIN ||
				    (item->getID() == cheeseItemId && getInventoryItemCount(player, cheeseItemId) < cheeseLimit)) {
					return true;
				}
			}
			return false;
		}

		Container* findRatCorpse(Player* player, const Position& currentPosition)
		{
			Container* fallback = nullptr;
			Position fallbackPosition;
			for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
				for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
					Position position(currentPosition.x + offsetX, currentPosition.y + offsetY, currentPosition.z);
					Tile* tile = g_game.map.getTile(position);
					TileItemVector* items = tile ? tile->getItemList() : nullptr;
					if (!items) {
						continue;
					}

					for (auto it = items->rbegin(); it != items->rend(); ++it) {
						Container* corpse = (*it)->getContainer();
						if (!corpse || corpse->getID() != ratCorpseItemId || !player->canOpenCorpse(corpse->getCorpseOwner())) {
							continue;
						}
						if (hasDesiredLoot(*player, *corpse)) {
							lootPosition = position;
							return corpse;
						}
						if (!fallback) {
							fallback = corpse;
							fallbackPosition = position;
						}
					}
				}
			}
			if (fallback) {
				lootPosition = fallbackPosition;
			}
			return fallback;
		}

		uint8_t backpackDestinationIndex(const Container& backpack, const Item& item) const
		{
			if (item.isStackable()) {
				const ItemDeque& items = backpack.getItemList();
				for (size_t index = 0; index < items.size(); ++index) {
					if (items[index]->getID() == item.getID() && items[index]->getItemCount() < 100) {
						return static_cast<uint8_t>(index);
					}
				}
			}
			return static_cast<uint8_t>(backpack.size());
		}

		void lootCorpse(Player* player, const Position& currentPosition)
		{
			if (!Position::areInRange<1, 1, 0>(currentPosition, lootPosition)) {
				if (route.empty()) {
					planRoute(player, lootPosition, 1);
				}
				return;
			}

			Container* corpse = findRatCorpse(player, currentPosition);
			if (!corpse) {
				if (++corpseSearchAttempts >= maxCorpseSearchAttempts) {
					logActionFailure("loot", "owned_rat_corpse_unavailable", currentPosition);
					finishLoot(player, currentPosition);
				}
				return;
			}

			if (pendingLootItemId != 0) {
				const uint32_t inventoryCount = getInventoryItemCount(*player, pendingLootItemId);
				if (inventoryCount > pendingLootInventoryCount) {
					logLootSuccess(pendingLootItemId, inventoryCount - pendingLootInventoryCount, inventoryCount, currentPosition);
				} else {
					unavailableLootItemIds.insert(pendingLootItemId);
					logActionFailure("loot", "item_move_failed", currentPosition);
				}
				pendingLootItemId = 0;
			}

			Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			if (!backpack) {
				logActionFailure("loot", "backpack_unavailable", currentPosition);
				finishLoot(player, currentPosition);
				return;
			}

			if (player->getContainerByID(corpseContainerId) != corpse) {
				if (!player->canDoAction()) {
					return;
				}
				if (++corpseOpenAttempts > 2) {
					logActionFailure("loot", "corpse_open_failed", currentPosition);
					finishLoot(player, currentPosition);
					return;
				}
				player->closeContainer(corpseContainerId);
				Tile* tile = g_game.map.getTile(lootPosition);
				const int32_t stackPosition = tile ? tile->getThingIndex(corpse) : -1;
				if (stackPosition < 0 || stackPosition > UINT8_MAX) {
					return;
				}
				++counters.actionsAttempted;
				g_game.playerUseItem(playerId, lootPosition, static_cast<uint8_t>(stackPosition), corpseContainerId, corpse->getClientID());
				return;
			}

			if (player->getContainerByID(backpackContainerId) != backpack) {
				if (!player->canDoAction()) {
					return;
				}
				const int8_t existingContainerId = player->getContainerID(backpack);
				if (existingContainerId >= 0) {
					player->closeContainer(static_cast<uint8_t>(existingContainerId));
				}
				const Position backpackPosition(0xFFFF, CONST_SLOT_BACKPACK, 0);
				++counters.actionsAttempted;
				g_game.playerUseItem(playerId, backpackPosition, 0, backpackContainerId, backpack->getClientID());
				return;
			}

			Item* lootItem = nullptr;
			uint8_t lootIndex = 0;
			const ItemDeque& corpseItems = corpse->getItemList();
			for (size_t index = 0; index < corpseItems.size(); ++index) {
				Item* candidate = corpseItems[index];
				if (unavailableLootItemIds.find(candidate->getID()) != unavailableLootItemIds.end()) {
					continue;
				}
				if (candidate->getID() == ITEM_GOLD_COIN ||
				    (candidate->getID() == cheeseItemId && getInventoryItemCount(*player, cheeseItemId) < cheeseLimit)) {
					lootItem = candidate;
					lootIndex = static_cast<uint8_t>(index);
					break;
				}
			}

			if (!lootItem) {
				finishLoot(player, currentPosition);
				return;
			}

			const uint32_t inventoryCount = getInventoryItemCount(*player, lootItem->getID());
			const uint8_t moveCount = static_cast<uint8_t>(lootItem->getItemCount());
			const Position fromPosition(0xFFFF, 0x40 | corpseContainerId, lootIndex);
			const Position toPosition(0xFFFF, 0x40 | backpackContainerId, backpackDestinationIndex(*backpack, *lootItem));
			if (!player->canDoAction()) {
				return;
			}
			pendingLootItemId = lootItem->getID();
			pendingLootInventoryCount = inventoryCount;
			++counters.actionsAttempted;
			g_game.playerMoveItem(player, fromPosition, lootItem->getClientID(), lootIndex, toPosition, moveCount, lootItem, backpack);
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
		Position lootPosition;
		std::vector<Direction> route;
		std::set<Position> visitedPositions;
		std::set<Position> frontierPositions;
		ScenarioStage scenarioStage = ScenarioStage::ToStart;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t grateUseAttempts = 0;
		uint32_t blockedStepCount = 0;
		uint32_t corpseSearchAttempts = 0;
		uint32_t corpseOpenAttempts = 0;
		uint16_t pendingLootItemId = 0;
		uint32_t pendingLootInventoryCount = 0;
		std::set<uint16_t> unavailableLootItemIds;
		bool pendingEat = false;
		uint32_t pendingEatInventoryCount = 0;
		int32_t pendingEatFoodTicks = 0;
		std::chrono::steady_clock::time_point eatRetryAfter;
		size_t traversalCheckpoint = 0;
		uint32_t completedTrips = 0;
		uint32_t transitionAttempts = 0;
		bool transitionPending = false;
		std::chrono::steady_clock::time_point combatStarted;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> suppressedTraversalTargets;
		bool restoredTraversalState = false;
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
	if (player->isRemoved() || g_game.getPlayerByID(player->getID()) != player) {
		return false;
	}

	controller = std::make_unique<PlayerBotController>(*player);
	controller->start(player->getPosition());
	return true;
}
