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

#include <set>

extern Game g_game;

namespace {
	constexpr uint32_t navigationInterval = 1000;
	constexpr uint32_t blockedRouteRetryInterval = 500;
	constexpr Position scenarioStart(32099, 32211, 7);
	constexpr Position sewerGratePosition(32097, 32205, 7);
	constexpr uint16_t sewerGrateItemId = 430;
	constexpr const char* botAccountName = "bot-one";
}

class PlayerBotController
{
	public:
		explicit PlayerBotController(uint32_t playerId) : playerId(playerId) {}

		void start(const Position& position)
		{
			previousPosition = position;
			if (position.z == sewerGratePosition.z + 1) {
				scenarioStage = ScenarioStage::FindRat;
			} else if (position.z != scenarioStart.z) {
				stop(">> Playerbot terminal: saved position is outside the supported scenario floors.");
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

		void schedule(uint32_t interval)
		{
			g_scheduler.addEvent(createSchedulerTask(interval, std::bind(&PlayerBotController::navigate, this)));
		}

		void stop(const std::string& reason)
		{
			scenarioStage = ScenarioStage::Stopped;
			if (!terminalLogged) {
				std::cout << reason << std::endl;
				terminalLogged = true;
			}
		}

		void navigate()
		{
			Player* player = g_game.getPlayerByID(playerId);
			if (!player || !player->isPlayerBot() || player->isRemoved() || player->isDead()) {
				stop(">> Playerbot terminal: controlled player is no longer alive.");
				return;
			}

			const Position currentPosition = player->getPosition();
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
					route.clear();
				}
				stepPending = false;
			}

			switch (scenarioStage) {
				case ScenarioStage::ToStart:
					if (currentPosition == scenarioStart) {
						scenarioStage = ScenarioStage::ToGrate;
						route.clear();
					} else if (route.empty()) {
						planRoute(player, scenarioStart, 0);
					}
					break;
				case ScenarioStage::ToGrate:
					if (Position::areInRange<1, 1, 0>(currentPosition, sewerGratePosition)) {
						scenarioStage = ScenarioStage::UseGrate;
						route.clear();
					} else if (route.empty()) {
						planRoute(player, sewerGratePosition, 1);
					}
					break;
				case ScenarioStage::UseGrate:
					if (currentPosition.z == sewerGratePosition.z + 1) {
						grateUseAttempts = 0;
						scenarioStage = ScenarioStage::FindRat;
						route.clear();
						std::cout << ">> Playerbot entered sewers at: " << currentPosition << std::endl;
					} else {
						useSewerGrate(player);
						if (++grateUseAttempts >= 20) {
							stop(">> Playerbot terminal: sewer grate did not produce a floor transition.");
						}
					}
					break;
				case ScenarioStage::FindRat:
				case ScenarioStage::ApproachRat: {
					Creature* rat = g_game.getCreatureByID(ratId);
					if (!rat || rat->isRemoved() || rat->isDead() || rat->getName() != "Rat" ||
					    !player->canSee(rat->getPosition()) || rat->getPosition() != ratPosition) {
						ratId = 0;
						route.clear();
					}

					if (planRatRoute(player)) {
						if (scenarioStage != ScenarioStage::Combat) {
							scenarioStage = ScenarioStage::ApproachRat;
						}
					} else {
						scenarioStage = ScenarioStage::Explore;
					}
					break;
				}
				case ScenarioStage::Combat: {
					Creature* rat = g_game.getCreatureByID(ratId);
					if (!rat || rat->isRemoved() || rat->isDead()) {
						g_game.playerSetAttackedCreature(playerId, 0);
						ratId = 0;
						route.clear();
						scenarioStage = ScenarioStage::FindRat;
						std::cout << ">> Playerbot combat ended; looking for another rat." << std::endl;
					} else if (player->getAttackedCreature() != rat || !player->canSee(rat->getPosition())) {
						g_game.playerSetAttackedCreature(playerId, 0);
						ratId = 0;
						route.clear();
						scenarioStage = ScenarioStage::FindRat;
						std::cout << ">> Playerbot lost combat target; rescanning." << std::endl;
					}
					break;
				}
				case ScenarioStage::Explore:
					if (planRatRoute(player)) {
						if (scenarioStage != ScenarioStage::Combat) {
							scenarioStage = ScenarioStage::ApproachRat;
						}
					} else if (route.empty() && !planExplorationRoute(player)) {
						stop(">> Playerbot terminal: no reachable unexplored sewer position remains.");
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
			if (player->getPathTo(target, route, pathParams) && !route.empty()) {
				fixedTargetRouteFailureCount = 0;
				return true;
			}

			if (++fixedTargetRouteFailureCount >= 20) {
				stop(">> Playerbot terminal: repeated route planning failures.");
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
				if (!player->getPathTo(candidate.position, candidateRoute, pathParams) || candidateRoute.empty()) {
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
					ratId = adjacentRatId;
					ratPosition = adjacentRat->getPosition();
					route.clear();
					g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
					g_game.playerSetAttackedCreature(playerId, ratId);
					if (player->getAttackedCreature() == adjacentRat) {
						scenarioStage = ScenarioStage::Combat;
						std::cout << ">> Playerbot attack accepted for rat " << ratId << " at: " << ratPosition << std::endl;
						return true;
					}
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
				if (!player->getPathTo(candidate.position, candidateRoute, pathParams) || candidateRoute.empty()) {
					continue;
				}

				ratId = candidate.id;
				ratPosition = candidate.position;
				route = std::move(candidateRoute);
				return true;
			}

			ratId = 0;
			return false;
		}

		uint32_t playerId;
		uint32_t ratId = 0;
		Position previousPosition;
		Position ratPosition;
		std::vector<Direction> route;
		std::set<Position> visitedPositions;
		std::set<Position> frontierPositions;
		ScenarioStage scenarioStage = ScenarioStage::ToStart;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t grateUseAttempts = 0;
		bool stepPending = false;
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

	controller = std::make_unique<PlayerBotController>(player->getID());
	controller->start(player->getPosition());
	std::cout << ">> Playerbot online: " << name << std::endl;
	return true;
}
