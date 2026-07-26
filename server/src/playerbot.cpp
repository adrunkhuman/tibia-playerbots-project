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
#include "playerbotnavigation.h"

#include "container.h"
#include "condition.h"
#include "configmanager.h"
#include "database.h"
#include "game.h"
#include "iologindata.h"
#include "item.h"
#include "monster.h"
#include "npc.h"
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
	constexpr uint16_t meatItemId = 2666;
	constexpr uint16_t smallHealthPotionItemId = 8704;
	constexpr uint32_t minimumSmallHealthPotions = 5;
	constexpr uint32_t minimumMeat = 1;
	constexpr int32_t meatFoodTicks = 108000;
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
	constexpr std::chrono::seconds navigationBlockSuppression(10);
	constexpr std::chrono::seconds navigationStepTimeout(2);
	constexpr uint32_t returnCapacityThreshold = 30 * 100;
	constexpr uint32_t carriedGoldReserve = 100;
	constexpr uint32_t maximumServiceAttempts = 3;
	constexpr Position fakeDepotPosition(32105, 32195, 8);
	constexpr Position fakeDepotTilePosition(32105, 32196, 8);
	constexpr std::array<Position, 4> huntingLoop = {{
		Position(32084, 32144, 5),
		Position(32103, 32124, 8),
		Position(32117, 32090, 9),
		Position(32103, 32124, 8),
	}};
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
	friend class PlayerBotManager;

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
			emit("lifecycle", position, "\"status\":\"online\",\"message\":\"Playerbot online\"");
			const char* gameplayMode = std::getenv("PLAYERBOT_GAMEPLAY_MODE");
			if (gameplayMode && (std::strcmp(gameplayMode, "navigation") == 0 || std::strcmp(gameplayMode, "corpse") == 0)) {
				startHunt(position);
			} else {
				cyclePhase = CyclePhase::Service;
			}
			setStage(ScenarioStage::Traverse, position);
			schedule(navigationInterval);
		}

	private:
		enum class CyclePhase : uint8_t {
			Service,
			ReturnToDepot,
			DepositLoot,
			Hunt,
		};

		enum class ServiceStage : uint8_t {
			Discover,
			SellLoot,
			BuyPotions,
			BuyMeat,
			Bank,
			Complete,
		};

		enum class ConversationStep : uint8_t {
			Greet,
			Request,
			Ready,
			Confirm,
			Verify,
		};

		struct ServiceNpc {
			uint32_t id;
			Position position;
		};

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
			return getFoodTicks(player) / 1000 + meatFoodTicks / 1000 < maximumFoodSeconds;
		}

		void logEatSuccess(uint32_t inventoryCount, int32_t foodTicks, const Position& position)
		{
			std::ostringstream fields;
			fields << "\"action\":\"eat\",\"result\":\"success\",\"item_id\":" << meatItemId
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
				const uint32_t inventoryCount = getInventoryItemCount(*player, meatItemId);
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

			if (getInventoryItemCount(*player, meatItemId) <= minimumMeat) {
				return false;
			}
			Item* meat = g_game.findItemOfType(player, meatItemId, true);
			if (!meat) {
				return false;
			}
			if (!player->canDoAction()) {
				return true;
			}

			pendingEatInventoryCount = getInventoryItemCount(*player, meatItemId);
			pendingEatFoodTicks = getFoodTicks(*player);
			pendingEat = true;
			++counters.actionsAttempted;
			g_game.playerUseItem(playerId, Position(0xFFFF, 0, 0), 0, 0, meat->getClientID());
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
				if (!Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) {
					FindPathParams pathParams;
					pathParams.maxSearchDist = 32;
					pathParams.minTargetDist = 1;
					pathParams.maxTargetDist = 1;
					std::vector<Direction> targetRoute;
					if (!findPath(player, creature->getPosition(), targetRoute, pathParams)) {
						continue;
					}
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
				clearNavigation();
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
				beginLoot(currentPosition);
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

		const char* cyclePhaseName() const
		{
			switch (cyclePhase) {
				case CyclePhase::Service: return "service";
				case CyclePhase::ReturnToDepot: return "return_to_depot";
				case CyclePhase::DepositLoot: return "deposit_loot";
				case CyclePhase::Hunt: return "hunt";
			}
			return "unknown";
		}

		void setCyclePhase(CyclePhase phase, const Position& position, const char* reason)
		{
			if (cyclePhase == phase) {
				return;
			}
			const char* previous = cyclePhaseName();
			cyclePhase = phase;
			std::ostringstream fields;
			fields << "\"from\":" << jsonString(previous) << ",\"to\":" << jsonString(cyclePhaseName())
			       << ",\"reason\":" << jsonString(reason);
			emit("objective_transition", position, fields.str());
		}

		void clearNavigation()
		{
			navigationSteps.clear();
			navigationPending = false;
			worldChangePending = false;
			navigationTarget = Position();
		}

		void beginReturn(Player* player, const Position& position, const char* reason)
		{
			const uint32_t previousTarget = ratId;
			g_game.playerCancelAttackAndFollow(playerId);
			clearRatTarget(position, reason);
			route.clear();
			clearNavigation();
			pendingLootItemId = 0;
			player->closeContainer(corpseContainerId);
			setStage(ScenarioStage::Traverse, position);
			setCyclePhase(CyclePhase::ReturnToDepot, position, reason);
			std::ostringstream fields;
			fields << "\"action\":\"return\",\"result\":\"started\",\"reason\":" << jsonString(reason)
			       << ",\"previous_target_id\":" << (previousTarget == 0 ? "null" : std::to_string(previousTarget))
			       << ",\"destination\":{\"x\":" << fakeDepotPosition.x << ",\"y\":" << fakeDepotPosition.y
			       << ",\"z\":" << static_cast<uint16_t>(fakeDepotPosition.z) << '}';
			emit("action_result", position, fields.str());
			schedule(navigationInterval);
		}

		void onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text)
		{
			if (replyingPlayerId != playerId || npcId != serviceTargetId || type != TALKTYPE_PRIVATE_NP) {
				return;
			}
			serviceGreetingAcknowledged = true;
			Npc* npc = g_game.getNpcByID(npcId);
			emit("npc_reply", lastPosition, "\"npc_id\":" + std::to_string(npcId) +
			     ",\"npc_name\":" + jsonString(npc ? npc->getName() : "") + ",\"text\":" + jsonString(text));
		}

		void beginService(Player* player, const Position& position, const char* reason)
		{
			g_game.playerCancelAttackAndFollow(playerId);
			clearRatTarget(position, reason);
			route.clear();
			clearNavigation();
			pendingLootItemId = 0;
			player->closeContainer(corpseContainerId);
			serviceShops.clear();
			serviceBankers.clear();
			serviceTargetId = 0;
			serviceApproachTarget = Position();
			serviceStage = ServiceStage::Discover;
			conversationStep = ConversationStep::Greet;
			serviceAttempts = 0;
			setCyclePhase(CyclePhase::Service, position, reason);
		}

		void discoverServices(const Position& position)
		{
			for (const auto& entry : g_game.getNpcs()) {
				Npc* npc = entry.second;
				const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
				if (!capability) {
					continue;
				}
				std::vector<ServiceNpc>* services = *capability == "shop" ? &serviceShops :
				                                    (*capability == "banker" ? &serviceBankers : nullptr);
				if (!services) {
					continue;
				}
				services->push_back({npc->getID(), npc->getPosition()});
				emit("service_discovered", position, "\"capability\":" + jsonString(*capability) +
				     ",\"npc_id\":" + std::to_string(npc->getID()) + ",\"npc_name\":" + jsonString(npc->getName()) +
				     ",\"offers\":" + std::to_string(npc->getShopOffers().size()));
			}
			if (serviceShops.empty() || serviceBankers.empty()) {
				stop("service_npc_unavailable", position);
				return;
			}
			std::sort(serviceShops.begin(), serviceShops.end(), [](const ServiceNpc& left, const ServiceNpc& right) {
				return left.id < right.id;
			});
			serviceStage = ServiceStage::SellLoot;
		}

		bool approachServiceNpc(Player* player, ServiceNpc& service, const Position& currentPosition)
		{
			Npc* npc = g_game.getNpcByID(service.id);
			if (!npc || npc->isRemoved()) {
				stop("service_npc_unavailable", currentPosition);
				return false;
			}
			service.position = npc->getPosition();
			if (Position::areInRange<3, 3, 0>(currentPosition, service.position)) {
				serviceApproachTarget = Position();
				return true;
			}
			if (serviceApproachTarget != Position()) {
				if (currentPosition == serviceApproachTarget) {
					serviceApproachTarget = Position();
					clearNavigation();
					schedule(SCHEDULER_MINTICKS);
					return false;
				}
				return processNavigation(player, currentPosition, serviceApproachTarget);
			}

			std::vector<Position> candidates;
			candidates.reserve(48);
			for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) {
				for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
					if (xOffset != 0 || yOffset != 0) {
						candidates.emplace_back(service.position.x + xOffset, service.position.y + yOffset, service.position.z);
					}
				}
			}
			std::sort(candidates.begin(), candidates.end(), [&currentPosition](const Position& left, const Position& right) {
				const int32_t leftDistance = std::max(Position::getDistanceX(currentPosition, left), Position::getDistanceY(currentPosition, left));
				const int32_t rightDistance = std::max(Position::getDistanceX(currentPosition, right), Position::getDistanceY(currentPosition, right));
				return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
			});
			for (const Position& candidate : candidates) {
				if (serviceRejectedApproaches.find(candidate) != serviceRejectedApproaches.end()) {
					continue;
				}
				Tile* tile = g_game.map.getTile(candidate);
				if (!tile || tile->queryAdd(0, *player, 1, 0) != RETURNVALUE_NOERROR) {
					continue;
				}
				std::deque<PlayerBotNavigationStep> candidateSteps;
				uint64_t expandedNodes = 0;
				++counters.pathfindingCalls;
				const auto startedAt = std::chrono::steady_clock::now();
				const bool planned = navigator.plan(*player, candidate, {}, candidateSteps, expandedNodes);
				counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - startedAt).count();
				if (!planned || candidateSteps.empty()) {
					++counters.pathfindingFailures;
					serviceRejectedApproaches.insert(candidate);
					schedule(SCHEDULER_MINTICKS);
					return false;
				}
				serviceApproachTarget = candidate;
				navigationTarget = candidate;
				navigationSteps = std::move(candidateSteps);
				std::ostringstream fields;
				fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << navigationSteps.size()
				       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << candidate.x
				       << ",\"y\":" << candidate.y << ",\"z\":" << static_cast<uint16_t>(candidate.z) << '}';
				emit("action_result", currentPosition, fields.str());
				return processNavigation(player, currentPosition, candidate);
			}
			stop("service_approach_unavailable", currentPosition);
			return false;
		}

		void resetConversation(uint32_t targetId)
		{
			serviceTargetId = targetId;
			conversationStep = ConversationStep::Greet;
			serviceAttempts = 0;
			serviceApproachTarget = Position();
			serviceRejectedApproaches.clear();
			clearNavigation();
		}

		bool openServiceShop(Player* player, ServiceNpc& service, const Position& position)
		{
			Npc* npc = g_game.getNpcByID(service.id);
			if (!npc || npc->isRemoved()) {
				stop("service_npc_unavailable", position);
				return false;
			}
			if (conversationStep == ConversationStep::Greet) {
				serviceGreetingAcknowledged = false;
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
				conversationStep = ConversationStep::Request;
				schedule(1000);
				return false;
			}
			if (conversationStep == ConversationStep::Request) {
				if (!serviceGreetingAcknowledged) {
					if (++serviceAttempts >= maximumServiceAttempts) {
						logActionFailure("shop", "npc_focus_unconfirmed", position);
						return false;
					}
					conversationStep = ConversationStep::Greet;
					schedule(1000);
					return false;
				}
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "trade");
				conversationStep = ConversationStep::Ready;
				schedule(1000);
				return false;
			}
			int32_t onBuy;
			int32_t onSell;
			if (player->getShopOwner(onBuy, onSell) == npc && !player->getShopItemList().empty()) {
				return true;
			}
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure("shop", "shop_window_unavailable", position);
				return false;
			}
			conversationStep = ConversationStep::Greet;
			schedule(SCHEDULER_MINTICKS);
			return false;
		}

		bool isApprovedSaleItem(uint16_t itemId) const
		{
			return itemId == 2813 || itemId == 5896 || itemId == 5897 || itemId == 5878;
		}

		const ShopInfo* findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const
		{
			Npc* npc = g_game.getNpcByID(service.id);
			if (!npc || npc->isRemoved()) {
				return nullptr;
			}
			const std::vector<ShopInfo>& offers = npc->getShopOffers();
			auto it = std::find_if(offers.begin(), offers.end(), [itemId, buying](const ShopInfo& offer) {
				return offer.itemId == itemId && (buying ? offer.buyPrice != 0 : offer.sellPrice != 0);
			});
			return it == offers.end() ? nullptr : &*it;
		}

		uint32_t serviceDistance(const Position& from, const ServiceNpc& service) const
		{
			return std::max(Position::getDistanceX(from, service.position), Position::getDistanceY(from, service.position)) +
			       (from.z == service.position.z ? 0 : 32 * Position::getDistanceZ(from, service.position));
		}

		ServiceNpc* findNearestService(std::vector<ServiceNpc>& services, const Position& position)
		{
			auto it = std::min_element(services.begin(), services.end(), [this, &position](const ServiceNpc& left, const ServiceNpc& right) {
				return serviceDistance(position, left) < serviceDistance(position, right);
			});
			return it == services.end() ? nullptr : &*it;
		}

		ServiceNpc* findShopFor(uint16_t itemId, bool buying, const Position& position)
		{
			ServiceNpc* nearest = nullptr;
			for (ServiceNpc& service : serviceShops) {
				if (findOffer(service, itemId, buying) && (!nearest || serviceDistance(position, service) < serviceDistance(position, *nearest))) {
					nearest = &service;
				}
			}
			return nearest;
		}

		ServiceNpc* findLootSeller(Player* player, const Position& position, uint16_t& itemId)
		{
			ServiceNpc* nearest = nullptr;
			for (ServiceNpc& service : serviceShops) {
				Npc* npc = g_game.getNpcByID(service.id);
				if (!npc || npc->isRemoved()) {
					continue;
				}
				for (const ShopInfo& offer : npc->getShopOffers()) {
					if (offer.sellPrice != 0 && isApprovedSaleItem(offer.itemId) && getInventoryItemCount(*player, offer.itemId) > 0 &&
					    (!nearest || serviceDistance(position, service) < serviceDistance(position, *nearest))) {
						itemId = offer.itemId;
						nearest = &service;
					}
				}
			}
			return nearest;
		}

		void completeServiceAction(Player* player, const char* action, uint16_t itemId, uint32_t amount, const Position& position)
		{
			std::ostringstream fields;
			fields << "\"action\":" << jsonString(action) << ",\"result\":\"success\",\"item_id\":" << itemId
			       << ",\"count\":" << amount << ",\"carried_before\":" << serviceBeforeMoney
			       << ",\"carried_after\":" << player->getMoney() << ",\"bank_before\":" << serviceBeforeBalance
			       << ",\"bank_after\":" << player->getBankBalance();
			emit("action_result", position, fields.str());
			conversationStep = ConversationStep::Greet;
			serviceAttempts = 0;
		}

		void processServiceShop(Player* player, const Position& currentPosition, ServiceNpc& service, const char* action,
		                        uint16_t itemId, uint32_t amount, bool purchase)
		{
			if (!approachServiceNpc(player, service, currentPosition)) {
				return;
			}
			if (!openServiceShop(player, service, currentPosition)) {
				if (serviceAttempts >= maximumServiceAttempts) {
					stop("shop_transaction_unavailable", currentPosition);
				}
				return;
			}
			const ShopInfo* offer = findOffer(service, itemId, purchase);
			if (!offer || amount == 0 || amount > 100) {
				stop("shop_offer_unavailable", currentPosition);
				return;
			}
			if (conversationStep == ConversationStep::Ready) {
				serviceBeforeItemCount = getInventoryItemCount(*player, itemId);
				serviceBeforeMoney = player->getMoney();
				serviceBeforeBalance = player->getBankBalance();
				serviceItemId = itemId;
				serviceAmount = amount;
				conversationStep = ConversationStep::Verify;
				++counters.actionsAttempted;
				if (purchase) {
					g_game.playerPurchaseItem(playerId, Item::items[itemId].clientId, static_cast<uint8_t>(offer->subType),
					                         static_cast<uint8_t>(amount), false, false);
				} else {
					g_game.playerSellItem(playerId, Item::items[itemId].clientId, static_cast<uint8_t>(offer->subType),
					                     static_cast<uint8_t>(amount), false);
				}
				schedule(navigationDecisionDelay(*player));
				return;
			}

			const uint32_t currentCount = getInventoryItemCount(*player, serviceItemId);
			const bool changed = purchase ? currentCount >= serviceBeforeItemCount + serviceAmount :
			                              currentCount + serviceAmount <= serviceBeforeItemCount;
			if (changed) {
				completeServiceAction(player, action, serviceItemId, serviceAmount, currentPosition);
				return;
			}
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure(action, "transaction_not_verified", currentPosition);
				stop("shop_transaction_not_verified", currentPosition);
				return;
			}
			conversationStep = ConversationStep::Ready;
			schedule(navigationDecisionDelay(*player));
		}

		void processBank(Player* player, const Position& currentPosition, ServiceNpc& banker)
		{
			if (!approachServiceNpc(player, banker, currentPosition)) {
				return;
			}
			Npc* npc = g_game.getNpcByID(banker.id);
			if (!npc || npc->isRemoved()) {
				stop("banker_unavailable", currentPosition);
				return;
			}
			if (conversationStep == ConversationStep::Greet) {
				serviceGreetingAcknowledged = false;
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
				conversationStep = ConversationStep::Request;
				schedule(1000);
				return;
			}
			if (conversationStep == ConversationStep::Request) {
				if (!serviceGreetingAcknowledged) {
					if (++serviceAttempts >= maximumServiceAttempts) {
						logActionFailure("bank", "npc_focus_unconfirmed", currentPosition);
						stop("banker_focus_unconfirmed", currentPosition);
						return;
					}
					conversationStep = ConversationStep::Greet;
					schedule(1000);
					return;
				}
				serviceBeforeMoney = player->getMoney();
				serviceBeforeBalance = player->getBankBalance();
				if (serviceBeforeMoney == 0) {
					bankDepositComplete = true;
					conversationStep = ConversationStep::Ready;
					return;
				}
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "deposit all");
				conversationStep = ConversationStep::Confirm;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (conversationStep == ConversationStep::Confirm) {
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
				conversationStep = ConversationStep::Verify;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (conversationStep == ConversationStep::Verify && !bankDepositComplete) {
				if (player->getMoney() != 0 || player->getBankBalance() < serviceBeforeBalance + serviceBeforeMoney) {
					if (++serviceAttempts >= maximumServiceAttempts) {
						logActionFailure("bank_deposit", "transaction_not_verified", currentPosition);
						stop("bank_deposit_not_verified", currentPosition);
						return;
					}
					conversationStep = ConversationStep::Request;
					schedule(SCHEDULER_MINTICKS);
					return;
				}
				emit("action_result", currentPosition, "\"action\":\"bank_deposit\",\"result\":\"success\",\"count\":" +
				     std::to_string(serviceBeforeMoney) + ",\"bank_before\":" + std::to_string(serviceBeforeBalance) +
				     ",\"bank_after\":" + std::to_string(player->getBankBalance()));
				bankDepositComplete = true;
				conversationStep = ConversationStep::Ready;
			}
			if (conversationStep == ConversationStep::Ready) {
				serviceBeforeBalance = player->getBankBalance();
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "withdraw " + std::to_string(carriedGoldReserve));
				conversationStep = ConversationStep::Confirm;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (conversationStep == ConversationStep::Confirm) {
				++counters.actionsAttempted;
				npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
				conversationStep = ConversationStep::Verify;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (player->getMoney() == carriedGoldReserve && player->getBankBalance() + carriedGoldReserve == serviceBeforeBalance) {
				emit("action_result", currentPosition, "\"action\":\"bank_withdraw\",\"result\":\"success\",\"count\":100,\"bank_before\":" +
				     std::to_string(serviceBeforeBalance) + ",\"bank_after\":" + std::to_string(player->getBankBalance()));
				serviceStage = ServiceStage::Complete;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (++serviceAttempts >= maximumServiceAttempts) {
				logActionFailure("bank_withdraw", "transaction_not_verified", currentPosition);
				stop("bank_withdraw_not_verified", currentPosition);
				return;
			}
			conversationStep = ConversationStep::Ready;
			schedule(SCHEDULER_MINTICKS);
		}

		void processService(Player* player, const Position& currentPosition)
		{
			if (serviceStage == ServiceStage::Discover) {
				bankDepositComplete = false;
				discoverServices(currentPosition);
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			if (serviceStage == ServiceStage::SellLoot) {
				uint16_t itemId = 0;
				ServiceNpc* seller = findLootSeller(player, currentPosition, itemId);
				if (!seller) {
					serviceStage = ServiceStage::BuyPotions;
					resetConversation(0);
					schedule(SCHEDULER_MINTICKS);
					return;
				}
				if (serviceTargetId != seller->id) {
					resetConversation(seller->id);
				}
				processServiceShop(player, currentPosition, *seller, "sell", itemId,
				                   std::min<uint32_t>(100, getInventoryItemCount(*player, itemId)), false);
				return;
			}
			if (serviceStage == ServiceStage::BuyPotions || serviceStage == ServiceStage::BuyMeat) {
				const uint16_t itemId = serviceStage == ServiceStage::BuyPotions ? smallHealthPotionItemId : meatItemId;
				const uint32_t minimum = serviceStage == ServiceStage::BuyPotions ? minimumSmallHealthPotions : minimumMeat;
				const uint32_t currentCount = getInventoryItemCount(*player, itemId);
				if (currentCount >= minimum) {
					serviceStage = serviceStage == ServiceStage::BuyPotions ? ServiceStage::BuyMeat : ServiceStage::Bank;
					resetConversation(0);
					schedule(SCHEDULER_MINTICKS);
					return;
				}
				ServiceNpc* seller = findShopFor(itemId, true, currentPosition);
				if (!seller) {
					stop("required_shop_offer_unavailable", currentPosition);
					return;
				}
				if (serviceTargetId != seller->id) {
					resetConversation(seller->id);
				}
				processServiceShop(player, currentPosition, *seller, serviceStage == ServiceStage::BuyPotions ? "buy_potions" : "buy_meat",
				                   itemId, minimum - currentCount, true);
				return;
			}
			if (serviceStage == ServiceStage::Bank) {
				ServiceNpc* banker = findNearestService(serviceBankers, currentPosition);
				if (!banker) {
					stop("banker_unavailable", currentPosition);
					return;
				}
				if (serviceTargetId != banker->id) {
					resetConversation(banker->id);
				}
				processBank(player, currentPosition, *banker);
				return;
			}
			beginReturn(player, currentPosition, "service_complete");
		}

		Item* findNavigationItem(const PlayerBotNavigationStep& step) const
		{
			Tile* tile = g_game.map.getTile(step.target);
			if (!tile) {
				return nullptr;
			}
			if (Item* ground = tile->getGround(); ground && ground->getID() == step.itemId) {
				return ground;
			}
			if (TileItemVector* items = tile->getItemList()) {
				for (Item* item : *items) {
					if (item->getID() == step.itemId) {
						return item;
					}
				}
			}
			return nullptr;
		}

		bool executeNavigationStep(Player* player, const PlayerBotNavigationStep& step)
		{
			++counters.actionsAttempted;
			if (step.action == PlayerBotNavigationAction::Move) {
				g_game.playerMove(playerId, step.direction);
				return true;
			}

			Item* target = findNavigationItem(step);
			Tile* tile = g_game.map.getTile(step.target);
			const int32_t stackPosition = target && tile ? tile->getThingIndex(target) : -1;
			if (!target || stackPosition < 0 || stackPosition > UINT8_MAX) {
				return false;
			}

			if (step.action == PlayerBotNavigationAction::UseRope ||
			    step.action == PlayerBotNavigationAction::UseShovel) {
				const uint16_t toolId = step.action == PlayerBotNavigationAction::UseRope ? ropeItemId : 2554;
				Item* tool = g_game.findItemOfType(player, toolId, true);
				if (!tool) {
					return false;
				}
				g_game.playerUseItemEx(playerId, Position(0xFFFF, 0, 0), 0, tool->getClientID(), step.target,
				                         static_cast<uint8_t>(stackPosition), target->getClientID());
				return true;
			}

			g_game.playerUseItem(playerId, step.target, static_cast<uint8_t>(stackPosition), 0, target->getClientID());
			return true;
		}

		uint32_t navigationDecisionDelay(const Player& player) const
		{
			const uint32_t walkDelay = static_cast<uint32_t>(std::max<int32_t>(0, player.getWalkDelay()));
			return std::max<uint32_t>(SCHEDULER_MINTICKS, std::max(walkDelay, player.getNextActionTime()));
		}

		bool processNavigation(Player* player, const Position& currentPosition, const Position& destination)
		{
			if (currentPosition == destination) {
				clearNavigation();
				return true;
			}

			if (navigationPending) {
				if (currentPosition == navigationExpectedPosition) {
					navigationPending = false;
					blockedStepCount = 0;
					if (!navigationSteps.empty()) {
						navigationSteps.pop_front();
					}
				} else if ((player->getWalkDelay() > 0 || !player->canDoAction()) &&
				           std::chrono::steady_clock::now() - navigationStepStarted < navigationStepTimeout) {
					schedule(navigationDecisionDelay(*player));
					return false;
				} else {
					navigationPending = false;
					navigationSteps.clear();
					temporarilyBlockedPositions[navigationStepTarget] =
						std::chrono::steady_clock::now() + navigationBlockSuppression;
					logActionFailure("navigate", "step_result_mismatch", currentPosition);
					++blockedStepCount;
				}
			}
			if (worldChangePending) {
				const PlayerBotNavigationStep pendingStep = worldChangeStep;
				worldChangePending = false;
				if (Item* unchanged = findNavigationItem(pendingStep)) {
					temporarilyBlockedPositions[pendingStep.target] =
						std::chrono::steady_clock::now() + navigationBlockSuppression;
					logActionFailure("navigate", "transition_state_unchanged", currentPosition);
				}
			}

			if (navigationTarget != destination) {
				navigationSteps.clear();
				navigationTarget = destination;
			}
			if (navigationSteps.empty()) {
				const auto now = std::chrono::steady_clock::now();
				for (auto it = temporarilyBlockedPositions.begin(); it != temporarilyBlockedPositions.end();) {
					if (it->second <= now) {
						it = temporarilyBlockedPositions.erase(it);
					} else {
						++it;
					}
				}
				std::set<Position> blockedPositions;
				for (const auto& blocked : temporarilyBlockedPositions) {
					blockedPositions.insert(blocked.first);
				}
				uint64_t expandedNodes = 0;
				++counters.pathfindingCalls;
				const auto startedAt = std::chrono::steady_clock::now();
				const bool planned = navigator.plan(*player, destination, blockedPositions, navigationSteps, expandedNodes);
				counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - startedAt).count();
				if (!planned || navigationSteps.empty()) {
					++counters.pathfindingFailures;
					logActionFailure("navigate", "route_unavailable", currentPosition);
					if (blockedPositions.empty() && ++fixedTargetRouteFailureCount >= 20) {
						stop("navigation_route_unavailable", currentPosition);
					}
					schedule(blockedRouteRetryInterval);
					return false;
				}
				fixedTargetRouteFailureCount = 0;
				std::ostringstream fields;
				fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << navigationSteps.size()
				       << ",\"expanded_nodes\":" << expandedNodes << ",\"destination\":{\"x\":" << destination.x
				       << ",\"y\":" << destination.y << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}';
				emit("action_result", currentPosition, fields.str());
			}

			if (!player->canDoAction() || navigationSteps.empty()) {
				schedule(navigationDecisionDelay(*player));
				return false;
			}

			const PlayerBotNavigationStep& step = navigationSteps.front();
			if (!executeNavigationStep(player, step)) {
				navigationSteps.clear();
				logActionFailure("navigate", "transition_unavailable", currentPosition);
				schedule(blockedRouteRetryInterval);
				return false;
			}

			if (step.action == PlayerBotNavigationAction::UseDoor ||
			    step.action == PlayerBotNavigationAction::UseShovel) {
				worldChangeStep = step;
				worldChangePending = true;
				navigationSteps.clear();
				schedule(navigationDecisionDelay(*player));
				return false;
			}
			navigationExpectedPosition = step.expectedPosition;
			navigationStepTarget = step.target;
			navigationStepStarted = std::chrono::steady_clock::now();
			navigationPending = true;
			schedule(navigationDecisionDelay(*player));
			return false;
		}

		bool isProtectedDepositItem(const Item& item) const
		{
			return item.getID() == ropeItemId || item.getID() == 2554 || item.getID() == meatItemId ||
			       item.getID() == smallHealthPotionItemId || item.getWorth() != 0;
		}

		bool findDepositableItem(Container* container, Container*& source, Item*& depositItem) const
		{
			for (Item* item : container->getItemList()) {
				if (Container* child = item->getContainer()) {
					(void)child;
					source = container;
					depositItem = item;
					return true;
				}
				if (!isProtectedDepositItem(*item)) {
					source = container;
					depositItem = item;
					return true;
				}
			}
			return false;
		}

		void startHunt(const Position& position)
		{
			setCyclePhase(CyclePhase::Hunt, position, "deposit_complete");
			huntRouteIndex = 0;
			const int32_t duration = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS));
			huntDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
			clearNavigation();
			++completedCycles;
			std::ostringstream fields;
			fields << "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" << completedCycles
			       << ",\"duration_seconds\":" << duration;
			emit("action_result", position, fields.str());
		}

		void processDeposit(Player* player, const Position& currentPosition)
		{
			Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			Tile* destination = g_game.map.getTile(fakeDepotTilePosition);
			if (!backpack || !destination) {
				stop("fake_depot_unavailable", currentPosition);
				return;
			}
			if (pendingDepositItemId != 0) {
				const uint32_t destinationCount = destination->getItemTypeCount(pendingDepositItemId);
				if (destinationCount <= pendingDepositDestinationCount) {
					logActionFailure("deposit", "item_move_failed", currentPosition);
					stop("fake_depot_rejected_loot", currentPosition);
					return;
				}
				std::ostringstream fields;
				fields << "\"action\":\"deposit\",\"result\":\"success\",\"item_id\":" << pendingDepositItemId
				       << ",\"count\":" << (destinationCount - pendingDepositDestinationCount);
				emit("action_result", currentPosition, fields.str());
				pendingDepositItemId = 0;
			}

			if (player->getContainerByID(backpackContainerId) != backpack) {
				if (!player->canDoAction()) {
					schedule(navigationDecisionDelay(*player));
					return;
				}
				const int8_t existingContainerId = player->getContainerID(backpack);
				if (existingContainerId >= 0) {
					player->closeContainer(static_cast<uint8_t>(existingContainerId));
				}
				const Position backpackPosition(0xFFFF, CONST_SLOT_BACKPACK, 0);
				++counters.actionsAttempted;
				g_game.playerUseItem(playerId, backpackPosition, 0, backpackContainerId, backpack->getClientID());
				schedule(navigationDecisionDelay(*player));
				return;
			}

			Container* source = nullptr;
			Item* depositItem = nullptr;
			if (!findDepositableItem(backpack, source, depositItem)) {
				if (player->getFreeCapacity() < returnCapacityThreshold) {
					stop("depot_capacity_not_recovered", currentPosition);
					return;
				}
				std::ostringstream fields;
				fields << "\"action\":\"deposit\",\"result\":\"complete\",\"cycle\":" << completedCycles;
				emit("action_result", currentPosition, fields.str());
				startHunt(currentPosition);
				schedule(navigationInterval);
				return;
			}

			const int8_t sourceContainerId = player->getContainerID(source);
			const ItemDeque& sourceItems = source->getItemList();
			auto sourceItem = std::find(sourceItems.begin(), sourceItems.end(), depositItem);
			if (sourceContainerId < 0 || sourceItem == sourceItems.end() ||
			    std::distance(sourceItems.begin(), sourceItem) > UINT8_MAX) {
				stop("fake_depot_source_unavailable", currentPosition);
				return;
			}
			if (!player->canDoAction()) {
				schedule(navigationDecisionDelay(*player));
				return;
			}

			const uint8_t sourceIndex = static_cast<uint8_t>(std::distance(sourceItems.begin(), sourceItem));
			const uint8_t count = static_cast<uint8_t>(depositItem->getItemCount());
			const Position sourcePosition(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), sourceIndex);
			pendingDepositItemId = depositItem->getID();
			pendingDepositDestinationCount = destination->getItemTypeCount(pendingDepositItemId);
			++counters.actionsAttempted;
			g_game.playerMoveItem(player, sourcePosition, depositItem->getClientID(), sourceIndex,
			                      fakeDepotTilePosition, count, depositItem, destination);
			schedule(navigationDecisionDelay(*player));
		}

		void processTraversal(Player* player, const Position& currentPosition)
		{
			if (cyclePhase == CyclePhase::Hunt &&
			    (std::chrono::steady_clock::now() >= huntDeadline || player->getFreeCapacity() < returnCapacityThreshold)) {
				beginService(player, currentPosition,
				             player->getFreeCapacity() < returnCapacityThreshold ? "capacity" : "hunt_deadline");
			}

			if (cyclePhase == CyclePhase::Service) {
				processService(player, currentPosition);
				return;
			}

			if (cyclePhase == CyclePhase::ReturnToDepot) {
				if (!processNavigation(player, currentPosition, fakeDepotPosition)) {
					return;
				}
				setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
				processDeposit(player, currentPosition);
				return;
			}

			if (cyclePhase == CyclePhase::DepositLoot) {
				if (currentPosition != fakeDepotPosition) {
					setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "displaced_during_deposit");
					clearNavigation();
					if (!processNavigation(player, currentPosition, fakeDepotPosition)) {
						return;
					}
					setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
				}
				processDeposit(player, currentPosition);
				return;
			}

			if (scenarioStage == ScenarioStage::TraversalCombat) {
				processTraversalCombat(player, currentPosition);
				return;
			}
			if (scenarioStage == ScenarioStage::LootCorpse) {
				lootCorpse(player, currentPosition);
				return;
			}
			if (attackVisibleMonster(player, currentPosition)) {
				schedule(navigationInterval);
				return;
			}

			const Position& target = huntingLoop[huntRouteIndex];
			if (!processNavigation(player, currentPosition, target)) {
				return;
			}
			huntRouteIndex = (huntRouteIndex + 1) % huntingLoop.size();
			std::ostringstream fields;
			fields << "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" << huntRouteIndex;
			emit("action_result", currentPosition, fields.str());
			schedule(SCHEDULER_MINTICKS);
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
			    scenarioStage == ScenarioStage::LootCorpse || scenarioStage == ScenarioStage::Complete) {
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
			lootedCurrentCorpse = false;
			unavailableLootItemIds.clear();
			setStage(ScenarioStage::LootCorpse, currentPosition);
		}

		void finishLoot(Player* player, const Position& currentPosition)
		{
			player->closeContainer(corpseContainerId);
			route.clear();
			pendingLootItemId = 0;
			setStage(ScenarioStage::Traverse, currentPosition);
		}

		Container* findCorpse(Player* player, const Position& searchPosition)
		{
			Container* fallback = nullptr;
			Position fallbackPosition;
			for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
				for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
					Position position(searchPosition.x + offsetX, searchPosition.y + offsetY, searchPosition.z);
					Tile* tile = g_game.map.getTile(position);
					TileItemVector* items = tile ? tile->getItemList() : nullptr;
					if (!items) {
						continue;
					}

					for (auto it = items->getBeginDownItem(); it != items->getEndDownItem(); ++it) {
						Item* item = *it;
						Container* corpse = item->getContainer();
						if (!corpse || Item::items[item->getID()].corpseType == RACE_NONE) {
							continue;
						}
						const uint32_t corpseOwner = corpse->getCorpseOwner();
						if (corpseOwner != 0 && !player->canOpenCorpse(corpseOwner)) {
							continue;
						}
						if (position == searchPosition) {
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
			Container* corpse = player->getContainerByID(corpseContainerId);
			if (!corpse || Item::items[corpse->getID()].corpseType == RACE_NONE) {
				corpse = findCorpse(player, lootPosition);
			}
			if (!Position::areInRange<1, 1, 0>(currentPosition, lootPosition)) {
				processNavigation(player, currentPosition, lootPosition);
				return;
			}
			schedule(navigationInterval);

			if (!corpse) {
				if (++corpseSearchAttempts >= maxCorpseSearchAttempts) {
					logActionFailure("loot", "owned_corpse_unavailable", currentPosition);
					finishLoot(player, currentPosition);
				}
				return;
			}

			if (pendingLootItemId != 0) {
				const uint32_t inventoryCount = getInventoryItemCount(*player, pendingLootItemId);
				if (inventoryCount > pendingLootInventoryCount) {
					logLootSuccess(pendingLootItemId, inventoryCount - pendingLootInventoryCount, inventoryCount, currentPosition);
					lootedCurrentCorpse = true;
				} else {
					unavailableLootItemIds.insert(pendingLootItemId);
					logActionFailure("loot", "item_move_failed", currentPosition);
				}
				pendingLootItemId = 0;
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

			if (corpse->empty()) {
				if (!lootedCurrentCorpse) {
					std::ostringstream fields;
					fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_empty\""
					       << ",\"corpse_item_id\":" << corpse->getID() << ",\"corpse_owner_id\":" << corpse->getCorpseOwner()
					       << ",\"corpse_position\":{\"x\":" << lootPosition.x << ",\"y\":" << lootPosition.y
					       << ",\"z\":" << static_cast<uint16_t>(lootPosition.z) << '}';
					emit("action_result", currentPosition, fields.str());
				}
				finishLoot(player, currentPosition);
				return;
			}

			Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			if (!backpack) {
				logActionFailure("loot", "backpack_unavailable", currentPosition);
				finishLoot(player, currentPosition);
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
				lootItem = candidate;
				lootIndex = static_cast<uint8_t>(index);
				break;
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
		bool lootedCurrentCorpse = false;
		uint16_t pendingDepositItemId = 0;
		uint32_t pendingLootInventoryCount = 0;
		uint32_t pendingDepositDestinationCount = 0;
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
		CyclePhase cyclePhase = CyclePhase::ReturnToDepot;
		ServiceStage serviceStage = ServiceStage::Discover;
		ConversationStep conversationStep = ConversationStep::Greet;
		std::vector<ServiceNpc> serviceShops;
		std::vector<ServiceNpc> serviceBankers;
		uint32_t serviceTargetId = 0;
		Position serviceApproachTarget;
		std::set<Position> serviceRejectedApproaches;
		uint32_t serviceAttempts = 0;
		uint16_t serviceItemId = 0;
		uint32_t serviceAmount = 0;
		uint32_t serviceBeforeItemCount = 0;
		uint64_t serviceBeforeMoney = 0;
		uint64_t serviceBeforeBalance = 0;
		bool bankDepositComplete = false;
		bool serviceGreetingAcknowledged = false;
		size_t huntRouteIndex = 0;
		uint32_t completedCycles = 0;
		std::chrono::steady_clock::time_point huntDeadline;
		PlayerBotNavigator navigator;
		std::deque<PlayerBotNavigationStep> navigationSteps;
		Position navigationTarget;
		Position navigationExpectedPosition;
		Position navigationStepTarget;
		std::chrono::steady_clock::time_point navigationStepStarted;
		PlayerBotNavigationStep worldChangeStep;
		std::map<Position, std::chrono::steady_clock::time_point> temporarilyBlockedPositions;
		bool navigationPending = false;
		bool worldChangePending = false;
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

void PlayerBotManager::onNpcReply(uint32_t playerId, uint32_t npcId, uint8_t type, const std::string& text)
{
	if (controller) {
		controller->onNpcReply(playerId, npcId, type, text);
	}
}

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
