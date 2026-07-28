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
		{}

		void start(const Position& position)
		{
			lastPosition = position;
			refreshItemValues();
			emit("lifecycle", position, "\"status\":\"online\",\"message\":\"Playerbot online\"");
			const char* gameplayMode = std::getenv("PLAYERBOT_GAMEPLAY_MODE");
			if (gameplayMode && (std::strcmp(gameplayMode, "navigation") == 0 || std::strcmp(gameplayMode, "corpse") == 0 ||
			                     std::strcmp(gameplayMode, "value") == 0)) {
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
			LootCorpse,
			Traverse,
			TraversalCombat,
			Stopped,
		};

		struct CargoCandidate {
			Item* item;
			uint8_t index;
			uint32_t unitValue;
			uint32_t unitWeight;
			uint32_t availableCount;
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
				case ScenarioStage::LootCorpse: return "loot_corpse";
				case ScenarioStage::Traverse: return "traverse";
				case ScenarioStage::TraversalCombat: return "traversal_combat";
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

		void setExpectedCorpse(const Creature& target)
		{
			const Monster* monster = target.getMonster();
			expectedCorpseItemId = monster ? monster->getCorpseItemId() : 0;
			if (expectedCorpseItemId == 0) {
				expectedCorpseLootable = false;
				return;
			}
			const ItemType& corpseType = Item::items[expectedCorpseItemId];
			expectedCorpseLootable = corpseType.corpseType != RACE_NONE && corpseType.isContainer();
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
			       << ",\"count\":" << count << ",\"inventory_count\":" << inventoryCount
			       << ",\"unit_value\":" << itemUnitValue(itemId)
			       << ",\"total_value\":" << static_cast<uint64_t>(itemUnitValue(itemId)) * count
			       << ",\"unit_weight\":" << Item::items[itemId].weight;
			emit("action_result", position, fields.str());
		}

		uint32_t getInventoryItemCount(const Player& player, uint16_t itemId) const
		{
			return static_cast<const Cylinder&>(player).getItemTypeCount(itemId);
		}

		uint32_t itemUnitValue(uint16_t itemId) const
		{
			const ItemType& type = Item::items[itemId];
			if (type.worth != 0) {
				return type.worth;
			}
			auto it = itemSellValues.find(itemId);
			return it == itemSellValues.end() ? 0 : it->second;
		}

		uint32_t protectedItemReserve(uint16_t itemId) const
		{
			if (itemId == ropeItemId || itemId == 2554) {
				return 1;
			}
			if (itemId == meatItemId) {
				return minimumMeat;
			}
			if (itemId == smallHealthPotionItemId) {
				return minimumSmallHealthPotions;
			}
			return 0;
		}

		uint32_t getSaleItemCount(const Player& player, uint16_t itemId) const
		{
			const ItemType& type = Item::items[itemId];
			if ((type.isContainer() && type.corpseType == RACE_NONE) || type.isFluidContainer() || type.isSplash()) {
				return 0;
			}
			Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			if (!backpack) {
				return 0;
			}
			uint32_t count = 0;
			for (Item* item : backpack->getItemList()) {
				if (item->getID() == itemId) {
					const Container* container = item->getContainer();
					if (container && !container->empty()) {
						return 0;
					}
					count += item->getItemCount();
				}
			}
			uint32_t removableCount = 0;
			for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
				Item* inventoryItem = player.getInventoryItem(static_cast<slots_t>(slot));
				Container* container = inventoryItem ? inventoryItem->getContainer() : nullptr;
				if (!container) {
					continue;
				}
				for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
					Item* nestedItem = *it;
					if (nestedItem->getID() == itemId) {
						removableCount += nestedItem->getItemCount();
					}
				}
			}
			if (removableCount != count) {
				return 0;
			}
			const uint32_t reserve = protectedItemReserve(itemId);
			return count > reserve ? count - reserve : 0;
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
			const uint32_t activeTargetId = defensiveTargetId != 0 ? defensiveTargetId : ratId;
			const Position& activeTargetPosition = defensiveTargetId != 0 ? defensiveTargetPosition : ratPosition;
			fields << "\"final\":" << (final ? "true" : "false")
			       << ",\"uptime_ms\":" << uptimeMs
			       << ",\"state\":" << jsonString(stageName(scenarioStage))
			       << ",\"target_id\":";
			if (activeTargetId == 0) {
				fields << "null";
			} else {
				fields << activeTargetId
				       << ",\"target_position\":{\"x\":" << activeTargetPosition.x << ",\"y\":" << activeTargetPosition.y
				       << ",\"z\":" << static_cast<uint16_t>(activeTargetPosition.z) << '}';
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

		void setTraversalTarget(Creature* target, const Position& position)
		{
			ratId = target->getID();
			ratPosition = target->getPosition();
			setExpectedCorpse(*target);
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
				clearNavigation();
				setStage(ScenarioStage::TraversalCombat, currentPosition);
				return true;
			}
			return false;
		}

		bool attackDefensiveThreat(Player* player, const Position& currentPosition)
		{
			SpectatorVec spectators;
			g_game.map.getSpectators(spectators, currentPosition);
			const auto now = std::chrono::steady_clock::now();
			auto isRouteCritical = [this, now](const Creature* creature) {
				return (navigationPending && creature->getPosition() == navigationStepTarget) ||
				       (now < blockedNavigationTargetExpires && creature->getPosition() == blockedNavigationTarget);
			};
			std::sort(spectators.begin(), spectators.end(), [&currentPosition, &isRouteCritical](Creature* left, Creature* right) {
				const bool leftRouteCritical = isRouteCritical(left);
				const bool rightRouteCritical = isRouteCritical(right);
				if (leftRouteCritical != rightRouteCritical) {
					return leftRouteCritical;
				}
				const int32_t leftDistance = std::max(Position::getDistanceX(currentPosition, left->getPosition()),
				                                      Position::getDistanceY(currentPosition, left->getPosition()));
				const int32_t rightDistance = std::max(Position::getDistanceX(currentPosition, right->getPosition()),
				                                       Position::getDistanceY(currentPosition, right->getPosition()));
				return leftDistance == rightDistance ? left->getID() < right->getID() : leftDistance < rightDistance;
			});

			for (Creature* creature : spectators) {
				if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
				    creature->getAttackedCreature() != player || !player->canSee(creature->getPosition()) ||
				    !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) {
					continue;
				}

				++counters.actionsAttempted;
				g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
				g_game.playerSetAttackedCreature(playerId, creature->getID());
				if (player->getAttackedCreature() != creature) {
					logActionFailure("defensive_combat", "target_rejected", currentPosition);
					return false;
				}

				defensiveTargetId = creature->getID();
				defensiveTargetPosition = creature->getPosition();
				defensiveCombatStarted = std::chrono::steady_clock::now();
				const bool routeCritical = isRouteCritical(creature);
				clearNavigation();
				std::ostringstream targetFields;
				targetFields << "\"previous_target_id\":null,\"target_id\":" << defensiveTargetId
				             << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(creature->getName())
				             << ",\"target_position\":{\"x\":" << creature->getPosition().x
				             << ",\"y\":" << creature->getPosition().y << ",\"z\":"
				             << static_cast<uint16_t>(creature->getPosition().z) << "},\"reason\":"
				             << jsonString(routeCritical ? "defensive_path_blocker" : "defensive_attacker")
				             << ",\"route_critical\":" << (routeCritical ? "true" : "false");
				emit("target_changed", currentPosition, targetFields.str());
				emit("action_result", currentPosition,
				     "\"action\":\"defensive_combat\",\"result\":\"started\",\"target_id\":" +
				         std::to_string(defensiveTargetId) + ",\"chase\":false,\"route_critical\":" +
				         (routeCritical ? "true" : "false"));
				return true;
			}
			return false;
		}

		void finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason)
		{
			const uint32_t previousTarget = defensiveTargetId;
			if (player->getAttackedCreature() && player->getAttackedCreature()->getID() == previousTarget) {
				g_game.playerSetAttackedCreature(playerId, 0);
			}
			defensiveTargetId = 0;
			clearNavigation();
			emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTarget) +
			     ",\"target_id\":null,\"reason\":" + jsonString(reason));
			emit("action_result", currentPosition, "\"action\":\"defensive_combat\",\"result\":" +
			     jsonString(result) + ",\"target_id\":" + std::to_string(previousTarget) +
			     ",\"reason\":" + jsonString(reason));
		}

		void processDefensiveCombat(Player* player, const Position& currentPosition)
		{
			Creature* target = g_game.getCreatureByID(defensiveTargetId);
			if (!target || target->isRemoved() || target->isDead()) {
				finishDefensiveCombat(player, currentPosition, "success", "target_defeated");
			} else if (target->getAttackedCreature() != player || !player->canSee(target->getPosition()) ||
			           !Position::areInRange<1, 1, 0>(currentPosition, target->getPosition())) {
				finishDefensiveCombat(player, currentPosition, "skipped", "threat_disengaged");
			} else if (player->getAttackedCreature() != target) {
				finishDefensiveCombat(player, currentPosition, "failed", "target_lost");
			} else if (std::chrono::steady_clock::now() - defensiveCombatStarted >= traversalCombatTimeout) {
				finishDefensiveCombat(player, currentPosition, "failed", "combat_timeout");
			} else {
				defensiveTargetPosition = target->getPosition();
			}
			schedule(navigationInterval);
		}

		void finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
		{
			g_game.playerSetAttackedCreature(playerId, 0);
			clearRatTarget(currentPosition, reason);
			setStage(ScenarioStage::Traverse, currentPosition);
		}

		void processTraversalCombat(Player* player, const Position& currentPosition)
		{
			Creature* target = g_game.getCreatureByID(ratId);
			if (!target || target->isRemoved() || target->isDead()) {
				beginLoot(player, currentPosition);
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
			clearNavigation();
			pendingLootItemId = 0;
			pendingDiscardItemId = 0;
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

		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
		{
			if (deathObserved || player.getID() != playerId) {
				return;
			}
			deathObserved = true;
			lastPosition = player.getPosition();
			std::ostringstream fields;
			fields << "\"status\":\"dead\",\"level\":" << player.getLevel()
			       << ",\"health\":" << player.getHealth() << ",\"objective\":" << jsonString(cyclePhaseName())
			       << ",\"state\":" << jsonString(stageName(scenarioStage))
			       << ",\"target_id\":";
			const uint32_t targetId = defensiveTargetId != 0 ? defensiveTargetId : ratId;
			fields << (targetId == 0 ? "null" : std::to_string(targetId));
			fields << ",\"killer_id\":" << (killer ? std::to_string(killer->getID()) : "null")
			       << ",\"killer_name\":" << (killer ? jsonString(killer->getName()) : "null")
			       << ",\"killer_type\":" << (killer ? jsonString(killer->getPlayer() ? "player" : killer->getMonster() ? "monster" : "other") : "null")
			       << ",\"most_damage_id\":" << (mostDamageKiller ? std::to_string(mostDamageKiller->getID()) : "null")
			       << ",\"most_damage_name\":" << (mostDamageKiller ? jsonString(mostDamageKiller->getName()) : "null");
			emit("lifecycle", lastPosition, fields.str());
		}

		void beginService(Player* player, const Position& position, const char* reason)
		{
			g_game.playerCancelAttackAndFollow(playerId);
			clearRatTarget(position, reason);
			clearNavigation();
			pendingLootItemId = 0;
			pendingDiscardItemId = 0;
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
			refreshItemValues();
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

		void refreshItemValues()
		{
			itemSellValues.clear();
			for (const auto& entry : g_game.getNpcs()) {
				Npc* npc = entry.second;
				const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
				if (!capability || *capability != "shop") {
					continue;
				}
				for (const ShopInfo& offer : npc->getShopOffers()) {
					const ItemType& type = Item::items[offer.itemId];
					if (offer.sellPrice != 0 && !type.isFluidContainer() && !type.isSplash()) {
						itemSellValues[offer.itemId] = std::max(itemSellValues[offer.itemId], offer.sellPrice);
					}
				}
			}
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
			uint32_t selectedSellPrice = 0;
			for (ServiceNpc& service : serviceShops) {
				Npc* npc = g_game.getNpcByID(service.id);
				if (!npc || npc->isRemoved()) {
					continue;
				}
				for (const ShopInfo& offer : npc->getShopOffers()) {
					if (offer.sellPrice != 0 && getSaleItemCount(*player, offer.itemId) > 0 &&
					    (!nearest || offer.sellPrice > selectedSellPrice ||
					     (offer.sellPrice == selectedSellPrice && serviceDistance(position, service) < serviceDistance(position, *nearest)))) {
						itemId = offer.itemId;
						nearest = &service;
						selectedSellPrice = offer.sellPrice;
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
			serviceItemId = 0;
			serviceAmount = 0;
			schedule(SCHEDULER_MINTICKS);
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
					                     static_cast<uint8_t>(amount), true);
				}
				schedule(navigationDecisionDelay(*player));
				return;
			}

			const uint32_t currentCount = getInventoryItemCount(*player, serviceItemId);
			const uint64_t expectedMoneyDelta = static_cast<uint64_t>(serviceAmount) *
			                                    (purchase ? offer->buyPrice : offer->sellPrice);
			const bool itemChanged = purchase ? currentCount == serviceBeforeItemCount + serviceAmount :
			                                  currentCount + serviceAmount == serviceBeforeItemCount;
			const uint64_t expectedMoney = purchase ? (serviceBeforeMoney > expectedMoneyDelta ? serviceBeforeMoney - expectedMoneyDelta : 0) :
			                                           serviceBeforeMoney + expectedMoneyDelta;
			const uint64_t expectedBalance = purchase && expectedMoneyDelta > serviceBeforeMoney ?
			                                     serviceBeforeBalance - (expectedMoneyDelta - serviceBeforeMoney) : serviceBeforeBalance;
			const bool economyChanged = player->getMoney() == expectedMoney && player->getBankBalance() == expectedBalance;
			if (itemChanged && economyChanged) {
				completeServiceAction(player, action, serviceItemId, serviceAmount, currentPosition);
				return;
			}
			if (currentCount != serviceBeforeItemCount || player->getMoney() != serviceBeforeMoney ||
			    player->getBankBalance() != serviceBeforeBalance) {
				logActionFailure(action, "transaction_delta_mismatch", currentPosition);
				stop("shop_transaction_delta_mismatch", currentPosition);
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
			if (conversationStep == ConversationStep::Verify && serviceItemId != 0 && serviceAmount != 0 &&
			    (serviceStage == ServiceStage::SellLoot || serviceStage == ServiceStage::BuyPotions ||
			     serviceStage == ServiceStage::BuyMeat)) {
				auto service = std::find_if(serviceShops.begin(), serviceShops.end(), [this](const ServiceNpc& candidate) {
					return candidate.id == serviceTargetId;
				});
				if (service == serviceShops.end()) {
					stop("shop_transaction_service_unavailable", currentPosition);
					return;
				}
				const bool purchase = serviceStage != ServiceStage::SellLoot;
				const char* action = serviceStage == ServiceStage::SellLoot ? "sell" :
				                     (serviceStage == ServiceStage::BuyPotions ? "buy_potions" : "buy_meat");
				processServiceShop(player, currentPosition, *service, action, serviceItemId, serviceAmount, purchase);
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
				                   std::min<uint32_t>(100, getSaleItemCount(*player, itemId)), false);
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
					blockedNavigationTarget = navigationStepTarget;
					blockedNavigationTargetExpires = std::chrono::steady_clock::now() + navigationBlockSuppression;
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
			if (cyclePhase != CyclePhase::Hunt) {
				if (defensiveTargetId != 0) {
					processDefensiveCombat(player, currentPosition);
					return;
				}
				if (attackDefensiveThreat(player, currentPosition)) {
					schedule(navigationInterval);
					return;
				}
			}
			if (scenarioStage == ScenarioStage::LootCorpse) {
				lootCorpse(player, currentPosition);
				return;
			}

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
			if (deathObserved) {
				stop("controlled_player_dead", lastPosition);
				return;
			}
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
				onDeath(*player, nullptr, nullptr);
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
			if (scenarioStage == ScenarioStage::Stopped) {
				return;
			}
			processTraversal(player, currentPosition);
		}

		void beginLoot(Player* player, const Position& currentPosition)
		{
			clearRatTarget(currentPosition, "target_defeated");
			lootPosition = ratPosition;
			corpseSearchAttempts = 0;
			corpseOpenAttempts = 0;
			pendingLootItemId = 0;
			pendingDiscardItemId = 0;
			lootedCurrentCorpse = false;
			unavailableLootItemIds.clear();
			if (!expectedCorpseLootable) {
				std::ostringstream fields;
				fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_not_lootable\""
				       << ",\"expected_corpse_item_id\":" << expectedCorpseItemId;
				emit("action_result", currentPosition, fields.str());
				finishLoot(player, currentPosition);
				return;
			}
			setStage(ScenarioStage::LootCorpse, currentPosition);
		}

		void finishLoot(Player* player, const Position& currentPosition)
		{
			player->closeContainer(corpseContainerId);
			pendingLootItemId = 0;
			pendingDiscardItemId = 0;
			expectedCorpseItemId = 0;
			expectedCorpseLootable = false;
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

		bool isReplaceableCargo(const Item& item) const
		{
			const ItemType& type = Item::items[item.getID()];
			const Container* container = item.getContainer();
			return (!container || (type.corpseType != RACE_NONE && container->empty())) && item.getWorth() == 0 && protectedItemReserve(item.getID()) == 0 &&
			       itemUnitValue(item.getID()) != 0 && item.getBaseWeight() != 0;
		}

		bool chooseCargoReplacement(const Container& backpack, const Item& incoming, uint32_t freeCapacity,
		                            CargoCandidate& replacement, uint8_t& replacementCount) const
		{
			const uint32_t incomingWeight = incoming.getWeight();
			if (incomingWeight <= freeCapacity || incomingWeight == 0) {
				return false;
			}

			std::vector<CargoCandidate> candidates;
			const ItemDeque& items = backpack.getItemList();
			for (size_t index = 0; index < items.size() && index <= UINT8_MAX; ++index) {
				Item* item = items[index];
				if (!isReplaceableCargo(*item)) {
					continue;
				}
				candidates.push_back({item, static_cast<uint8_t>(index), itemUnitValue(item->getID()),
				                      item->getBaseWeight(), item->getItemCount()});
			}
			std::sort(candidates.begin(), candidates.end(), [](const CargoCandidate& left, const CargoCandidate& right) {
				const uint64_t leftDensity = static_cast<uint64_t>(left.unitValue) * right.unitWeight;
				const uint64_t rightDensity = static_cast<uint64_t>(right.unitValue) * left.unitWeight;
				return leftDensity == rightDensity ? left.item->getID() < right.item->getID() : leftDensity < rightDensity;
			});

			uint32_t requiredWeight = incomingWeight - freeCapacity;
			uint64_t totalDiscardedValue = 0;
			bool selected = false;
			for (const CargoCandidate& candidate : candidates) {
				const uint32_t count = std::min(candidate.availableCount,
				                                (requiredWeight + candidate.unitWeight - 1) / candidate.unitWeight);
				if (count == 0) {
					continue;
				}
				if (!selected) {
					replacement = candidate;
					replacementCount = static_cast<uint8_t>(count);
					selected = true;
				}
				totalDiscardedValue += static_cast<uint64_t>(count) * candidate.unitValue;
				const uint32_t releasedWeight = count * candidate.unitWeight;
				if (releasedWeight >= requiredWeight) {
					requiredWeight = 0;
					break;
				}
				requiredWeight -= releasedWeight;
			}

			const uint64_t incomingValue = static_cast<uint64_t>(itemUnitValue(incoming.getID())) * incoming.getItemCount();
			if (!selected || requiredWeight != 0 || incomingValue <= totalDiscardedValue) {
				return false;
			}
			return true;
		}

		void discardCargoForLoot(Player* player, Container* backpack, Item* incoming, const Position& currentPosition)
		{
			CargoCandidate replacement{};
			uint8_t replacementCount = 0;
			if (!chooseCargoReplacement(*backpack, *incoming, player->getFreeCapacity(), replacement,
			                            replacementCount)) {
				std::ostringstream fields;
				fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_capacity\""
				       << ",\"item_id\":" << incoming->getID() << ",\"count\":" << incoming->getItemCount()
				       << ",\"unit_value\":" << itemUnitValue(incoming->getID())
				       << ",\"weight\":" << incoming->getWeight() << ",\"free_capacity\":" << player->getFreeCapacity();
				emit("action_result", currentPosition, fields.str());
				unavailableLootItemIds.insert(incoming->getID());
				return;
			}

			Tile* destination = g_game.map.getTile(currentPosition);
			if (!destination || !player->canDoAction()) {
				return;
			}
			pendingDiscardItemId = replacement.item->getID();
			pendingDiscardCount = replacementCount;
			pendingDiscardInventoryCount = getInventoryItemCount(*player, pendingDiscardItemId);
			pendingDiscardValue = replacementCount * replacement.unitValue;
			pendingDiscardIncomingItemId = incoming->getID();
			const Position fromPosition(0xFFFF, 0x40 | backpackContainerId, replacement.index);
			++counters.actionsAttempted;
			g_game.playerMoveItem(player, fromPosition, replacement.item->getClientID(), replacement.index,
			                      currentPosition, replacementCount, replacement.item, destination);
		}

		void verifyPendingLootMoves(Player* player, const Position& currentPosition)
		{
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
			if (pendingDiscardItemId != 0) {
				const uint32_t inventoryCount = getInventoryItemCount(*player, pendingDiscardItemId);
				if (inventoryCount + pendingDiscardCount <= pendingDiscardInventoryCount) {
					std::ostringstream fields;
					fields << "\"action\":\"loot_replace\",\"result\":\"success\",\"discarded_item_id\":"
					       << pendingDiscardItemId << ",\"discarded_count\":" << static_cast<uint32_t>(pendingDiscardCount)
					       << ",\"discarded_value\":" << pendingDiscardValue
					       << ",\"incoming_item_id\":" << pendingDiscardIncomingItemId
					       << ",\"incoming_unit_value\":" << itemUnitValue(pendingDiscardIncomingItemId);
					emit("action_result", currentPosition, fields.str());
				} else {
					logActionFailure("loot_replace", "discard_not_verified", currentPosition);
					unavailableLootItemIds.insert(pendingDiscardIncomingItemId);
				}
				pendingDiscardItemId = 0;
			}
		}

		void lootCorpse(Player* player, const Position& currentPosition)
		{
			verifyPendingLootMoves(player, currentPosition);
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
				const uint32_t candidateValue = itemUnitValue(candidate->getID());
				if (candidateValue == 0 || unavailableLootItemIds.find(candidate->getID()) != unavailableLootItemIds.end()) {
					continue;
				}
				if (!lootItem) {
					lootItem = candidate;
					lootIndex = static_cast<uint8_t>(index);
					continue;
				}
				const uint64_t candidateDensity = static_cast<uint64_t>(candidateValue) * lootItem->getBaseWeight();
				const uint64_t selectedDensity = static_cast<uint64_t>(itemUnitValue(lootItem->getID())) * candidate->getBaseWeight();
				if (candidateDensity > selectedDensity ||
				    (candidateDensity == selectedDensity && candidateValue > itemUnitValue(lootItem->getID()))) {
					lootItem = candidate;
					lootIndex = static_cast<uint8_t>(index);
				}
			}

			if (!lootItem) {
				if (!lootedCurrentCorpse) {
					emit("action_result", currentPosition,
					     "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_eligible_loot\"");
				}
				finishLoot(player, currentPosition);
				return;
			}

			const uint32_t inventoryCount = getInventoryItemCount(*player, lootItem->getID());
			const uint8_t moveCount = static_cast<uint8_t>(lootItem->getItemCount());
			if (lootItem->getWeight() > player->getFreeCapacity()) {
				discardCargoForLoot(player, backpack, lootItem, currentPosition);
				return;
			}
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

		uint32_t playerId;
		uint32_t playerGuid;
		std::string playerName;
		uint32_t ratId = 0;
		uint32_t defensiveTargetId = 0;
		Position lastPosition;
		Position ratPosition;
		Position defensiveTargetPosition;
		Position lootPosition;
		ScenarioStage scenarioStage = ScenarioStage::Traverse;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t blockedStepCount = 0;
		uint32_t corpseSearchAttempts = 0;
		uint32_t corpseOpenAttempts = 0;
		uint16_t pendingLootItemId = 0;
		uint16_t pendingDiscardItemId = 0;
		uint16_t expectedCorpseItemId = 0;
		bool expectedCorpseLootable = false;
		bool lootedCurrentCorpse = false;
		uint16_t pendingDepositItemId = 0;
		uint32_t pendingLootInventoryCount = 0;
		uint8_t pendingDiscardCount = 0;
		uint32_t pendingDiscardInventoryCount = 0;
		uint32_t pendingDiscardValue = 0;
		uint16_t pendingDiscardIncomingItemId = 0;
		uint32_t pendingDepositDestinationCount = 0;
		std::set<uint16_t> unavailableLootItemIds;
		std::map<uint16_t, uint32_t> itemSellValues;
		bool pendingEat = false;
		uint32_t pendingEatInventoryCount = 0;
		int32_t pendingEatFoodTicks = 0;
		std::chrono::steady_clock::time_point eatRetryAfter;
		std::chrono::steady_clock::time_point combatStarted;
		std::chrono::steady_clock::time_point defensiveCombatStarted;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> suppressedTraversalTargets;
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
		Position blockedNavigationTarget;
		std::chrono::steady_clock::time_point navigationStepStarted;
		std::chrono::steady_clock::time_point blockedNavigationTargetExpires;
		PlayerBotNavigationStep worldChangeStep;
		std::map<Position, std::chrono::steady_clock::time_point> temporarilyBlockedPositions;
		bool navigationPending = false;
		bool worldChangePending = false;
		Counters counters;
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> repeatedEventTimes;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point lastSummary = started;
		std::chrono::steady_clock::time_point decisionStarted;
		bool decisionActive = false;
		bool terminalLogged = false;
		bool deathObserved = false;
};

PlayerBotManager g_playerBots;

PlayerBotManager::~PlayerBotManager() = default;

void PlayerBotManager::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	if (controller) {
		controller->onDeath(player, killer, mostDamageKiller);
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
