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

// Playerbot survival, combat targeting, and hunt orchestration.
using namespace playerbot;

void PlayerBotController::setExpectedCorpse(const Creature& target)
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

int32_t PlayerBotController::getFoodTicks(const Player& player) const
{
	Condition* condition = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT, 0);
	return condition ? condition->getTicks() : 0;
}

bool PlayerBotController::canEatCheese(const Player& player) const
{
	return getFoodTicks(player) / 1000 + meatFoodTicks / 1000 < maximumFoodSeconds;
}

bool PlayerBotController::needsHealing(const Player& player) const
{
	return static_cast<int64_t>(player.getHealth()) * 100 <=
	       static_cast<int64_t>(player.getMaxHealth()) * healingHealthPercent;
}

void PlayerBotController::logHealResult(const char* result, const char* reason, int32_t healthAfter,
                   uint32_t potionCountAfter, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"heal\",\"result\":" << jsonString(result)
	       << ",\"method\":\"small_health_potion\",\"item_id\":" << smallHealthPotionItemId
	       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"health_before\":" << pendingHealHealth
	       << ",\"health_after\":" << healthAfter
	       << ",\"health_max\":" << pendingHealHealthMax
	       << ",\"resource_before\":" << pendingHealPotionCount
	       << ",\"resource_after\":" << potionCountAfter;
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("action_result", position, fields.str());
}

bool PlayerBotController::handleHealing(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	if (pendingHeal) {
		const uint32_t potionCount = getInventoryItemCount(*player, smallHealthPotionItemId);
		const int32_t health = player->getHealth();
		if (potionCount < pendingHealPotionCount && health > pendingHealHealth) {
			logHealResult("success", nullptr, health, potionCount, currentPosition);
		} else {
			++counters.actionsFailed;
			logHealResult("failed", potionCount < pendingHealPotionCount ? "ineffective_recovery" : "use_not_verified",
			              health, potionCount, currentPosition);
			healRetryAfter = now + healingRetryInterval;
		}
		pendingHeal = false;
	}
	if (serviceStage == ServiceStage::BuyPotions) {
		return false;
	}

	if (!needsHealing(*player)) {
		return false;
	}
	if (now < healRetryAfter || !player->canDoAction()) {
		return true;
	}

	const uint32_t potionCount = getInventoryItemCount(*player, smallHealthPotionItemId);
	if (potionCount == 0) {
		if (shouldEmitRepeated("heal:missing_supply")) {
			std::ostringstream fields;
			fields << "\"action\":\"heal\",\"result\":\"skipped\",\"reason\":\"missing_supply\""
			       << ",\"method\":\"small_health_potion\",\"item_id\":" << smallHealthPotionItemId
			       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
			       << ",\"state\":" << jsonString(stageName(scenarioStage))
			       << ",\"health_before\":" << player->getHealth()
			       << ",\"health_after\":" << player->getHealth()
			       << ",\"health_max\":" << player->getMaxHealth()
			       << ",\"resource_before\":0,\"resource_after\":0";
			emit("action_result", currentPosition, fields.str());
		}
		if (progressionObjective != ProgressionObjective::None) {
			if (progressionObjective == ProgressionObjective::OracleDeparture) {
				finishOracleDeparture(player, currentPosition, "interrupted", "healing_supply_missing");
			} else {
				finishProgressionObjective(player, currentPosition, "interrupted", "healing_supply_missing", false);
			}
			return true;
		}
		if (cyclePhase != CyclePhase::Service) {
			beginService(player, currentPosition, "healing_supply_missing");
		}
		return false;
	}

	Item* potion = g_game.findItemOfType(player, smallHealthPotionItemId, true);
	if (!potion) {
		return true;
	}

	pendingHealHealth = player->getHealth();
	pendingHealHealthMax = player->getMaxHealth();
	pendingHealPotionCount = potionCount;
	pendingHeal = true;
	++counters.actionsAttempted;
	g_game.playerUseWithCreature(playerId, Position(0xFFFF, 0, 0), 0, playerId, potion->getClientID());
	return true;
}

void PlayerBotController::logEatSuccess(uint32_t inventoryCount, int32_t foodTicks, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"eat\",\"result\":\"success\",\"item_id\":" << meatItemId
	       << ",\"count\":1,\"inventory_count\":" << inventoryCount << ",\"food_ticks\":" << foodTicks;
	emit("action_result", position, fields.str());
}

bool PlayerBotController::handleFood(Player* player, const Position& currentPosition)
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

void PlayerBotController::setTraversalTarget(Creature* target, const Position& position)
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

bool PlayerBotController::attackVisibleMonster(Player* player, const Position& currentPosition)
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
		if (activeHuntRegion && creature->getAttackedCreature() != player &&
		    std::none_of(activeHuntRegion->monsters.begin(), activeHuntRegion->monsters.end(),
			[creature](const PlayerBotHuntMonsterProfile& profile) {
				return strcasecmp(profile.name.c_str(), creature->getName().c_str()) == 0;
			})) {
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

bool PlayerBotController::attackDefensiveThreat(Player* player, const Position& currentPosition)
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

void PlayerBotController::finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason)
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

void PlayerBotController::processDefensiveCombat(Player* player, const Position& currentPosition)
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

void PlayerBotController::finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearRatTarget(currentPosition, reason);
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
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

void PlayerBotController::emitHuntRegionCandidate(const PlayerBotHuntRegion& region, const Position& position) const
{
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2)
	       << "\"region_id\":" << region.id
	       << ",\"floor\":" << static_cast<uint16_t>(region.floor)
	       << ",\"center\":{\"x\":" << region.center.x << ",\"y\":" << region.center.y
	       << ",\"z\":" << static_cast<uint16_t>(region.center.z) << '}'
	       << ",\"destination\":{\"x\":" << region.destination.x << ",\"y\":" << region.destination.y
	       << ",\"z\":" << static_cast<uint16_t>(region.destination.z) << '}'
	       << ",\"patrol_points\":" << region.patrolPoints.size()
	       << ",\"experience_per_minute\":" << region.experiencePerMinute
	       << ",\"estimated_travel_seconds\":" << region.estimatedTravelSeconds
	       << ",\"available_hunt_seconds\":" << region.availableHuntSeconds
	       << ",\"observed_experience_per_minute\":" << region.observedExperiencePerMinute
	       << ",\"observed_correction\":" << region.observedCorrection
	       << ",\"projected_experience\":" << region.projectedExperience
	       << ",\"threat_ratio\":" << region.threatRatio
	       << ",\"score\":" << region.score
	       << ",\"travel_steps\":" << region.travelSteps
	       << ",\"expanded_nodes\":" << region.expandedNodes
	       << ",\"suitable\":" << (region.suitable ? "true" : "false")
	       << ",\"reachable\":" << (region.reachable ? "true" : "false")
	       << ",\"rejection_reason\":" << (region.rejectionReason.empty() ? "null" : jsonString(region.rejectionReason))
	       << ",\"monsters\":[";
	for (size_t index = 0; index < region.monsters.size(); ++index) {
		const PlayerBotHuntMonsterProfile& monster = region.monsters[index];
		if (index != 0) {
			fields << ',';
		}
		fields << "{\"name\":" << jsonString(monster.name)
		       << ",\"expected_spawns\":" << monster.expectedSpawns
		       << ",\"experience\":" << monster.experience
		       << ",\"health\":" << monster.health
		       << ",\"expected_dps\":" << monster.expectedDamagePerSecond
		       << ",\"predicted_fight_damage\":" << monster.predictedFightDamage << '}';
	}
	fields << ']';
	emit("hunt_region_candidate", position, fields.str());
}

void PlayerBotController::finishHuntRegion(const Player& player, const Position& position, const char* reason)
{
	if (!activeHuntRegion) {
		return;
	}
	const auto duration = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::steady_clock::now() - huntRegionStarted).count();
	const uint64_t experienceGained = player.getExperience() >= huntRegionStartExperience ?
	                                  player.getExperience() - huntRegionStartExperience : 0;
	double actualExperiencePerMinute = 0;
	double updatedCorrection = activeHuntRegion->observedCorrection;
	if (duration >= 30 && huntRegionKills != 0) {
		actualExperiencePerMinute = experienceGained * 60.0 / duration;
		const double predictedNetRate = activeHuntRegion->projectedExperience * 60.0 /
		                                std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS));
		if (predictedNetRate > 0) {
			PlayerBotHuntRegionPerformance& performance = huntRegionPerformance[activeHuntRegion->center];
			const double sampleCorrection = std::clamp(
				activeHuntRegion->observedCorrection * actualExperiencePerMinute / predictedNetRate, 0.25, 2.0);
			if (performance.samples == 0) {
				performance.observedExperiencePerMinute = actualExperiencePerMinute;
				performance.correction = sampleCorrection;
			} else {
				performance.observedExperiencePerMinute = performance.observedExperiencePerMinute * 0.65 +
				                                          actualExperiencePerMinute * 0.35;
				performance.correction = performance.correction * 0.65 + sampleCorrection * 0.35;
			}
			++performance.samples;
			updatedCorrection = performance.correction;
		}
	}
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2);
	fields << "\"region_id\":" << activeHuntRegion->id
	       << ",\"reason\":" << jsonString(reason)
	       << ",\"duration_seconds\":" << duration
	       << ",\"level_before\":" << huntRegionStartLevel
	       << ",\"level_after\":" << player.getLevel()
	       << ",\"experience_gained\":" << experienceGained
	       << ",\"actual_experience_per_minute\":" << actualExperiencePerMinute
	       << ",\"predicted_experience\":" << activeHuntRegion->projectedExperience
	       << ",\"updated_observed_correction\":" << updatedCorrection
	       << ",\"kills\":" << huntRegionKills
	       << ",\"damage_taken\":" << huntRegionDamageTaken;
	emit("hunt_region_outcome", position, fields.str());
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) {
		say(*speakingPlayer, "Leaving hunt: " + std::string(reason) + ". " +
		     std::to_string(huntRegionKills) + " kills, " +
		     std::to_string(player.getExperience() >= huntRegionStartExperience ?
		         player.getExperience() - huntRegionStartExperience : 0) + " experience.");
	}
	activeHuntRegion.reset();
}

bool PlayerBotController::selectHuntRegion(Player& player, const Position& position, const char* reason)
{
	const auto now = std::chrono::steady_clock::now();
	std::set<Position> excludedRegions;
	for (auto cooldown = huntRegionCooldowns.begin(); cooldown != huntRegionCooldowns.end();) {
		if (now >= cooldown->second) {
			cooldown = huntRegionCooldowns.erase(cooldown);
		} else {
			excludedRegions.insert(cooldown->first);
			++cooldown;
		}
	}
	const uint32_t huntDurationSeconds = static_cast<uint32_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	std::vector<PlayerBotHuntRegion> regions = huntRegionPlanner.evaluate(
		player, navigator, excludedRegions, huntRegionPerformance, huntDurationSeconds);
	for (const PlayerBotHuntRegion& region : regions) {
		emitHuntRegionCandidate(region, position);
	}

	std::ostringstream scan;
	scan << "\"reason\":" << jsonString(reason)
	     << ",\"player_level\":" << player.getLevel()
	     << ",\"player_health\":" << player.getHealth()
	     << ",\"player_health_max\":" << player.getMaxHealth()
	     << ",\"player_armor\":" << player.getArmor()
	     << ",\"player_defense\":" << player.getDefense()
	     << ",\"candidate_count\":" << regions.size();
	emit("hunt_region_scan", position, scan.str());

	auto selected = std::max_element(regions.begin(), regions.end(), [](const PlayerBotHuntRegion& left,
	                                                                      const PlayerBotHuntRegion& right) {
		const bool leftAvailable = left.suitable && left.reachable;
		const bool rightAvailable = right.suitable && right.reachable;
		if (leftAvailable != rightAvailable) {
			return !leftAvailable;
		}
		return left.score < right.score;
	});
	if (selected == regions.end() || !selected->suitable || !selected->reachable) {
		emit("hunt_region_selection", position,
		     "\"result\":\"failed\",\"reason\":\"no_suitable_reachable_region\"");
		return false;
	}

	activeHuntRegion = *selected;
	auto first = std::find(activeHuntRegion->patrolPoints.begin(), activeHuntRegion->patrolPoints.end(),
	                       activeHuntRegion->destination);
	if (first != activeHuntRegion->patrolPoints.end()) {
		std::rotate(activeHuntRegion->patrolPoints.begin(), first, activeHuntRegion->patrolPoints.end());
	}
	huntRouteIndex = 0;
	huntRegionStarted = now;
	huntRegionStartLevel = player.getLevel();
	huntRegionStartExperience = player.getExperience();
	huntRegionKills = 0;
	huntRegionDamageTaken = 0;
	emit("hunt_region_selection", position,
	     "\"result\":\"selected\",\"region_id\":" + std::to_string(selected->id) +
	         ",\"reason\":" + jsonString(reason) + ",\"center\":{\"x\":" +
	         std::to_string(selected->center.x) + ",\"y\":" + std::to_string(selected->center.y) +
	         ",\"z\":" + std::to_string(selected->center.z) + "}");
	std::ostringstream speech;
	speech << "Going hunting. Expecting: ";
	for (size_t index = 0; index < selected->monsters.size(); ++index) {
		if (index != 0) {
			speech << ", ";
		}
		speech << selected->monsters[index].name;
	}
	speech << ". Projected " << std::fixed << std::setprecision(0) << selected->projectedExperience
	       << " experience after " << selected->estimatedTravelSeconds << " seconds travel.";
	say(player, speech.str());
	return true;
}

void PlayerBotController::startHunt(Player* player, const Position& position, const char* reason)
{
	if (!player) {
		stop("controlled_player_not_found", position);
		return;
	}
	activeGoal = TopLevelGoal::Hunt;
	setCyclePhase(CyclePhase::Hunt, position, reason);
	huntRouteIndex = 0;
	const bool fixedFixtureRoute = std::getenv("PLAYERBOT_GAMEPLAY_MODE") != nullptr;
	if (!fixedFixtureRoute && !activeHuntRegion && !selectHuntRegion(*player, position, "hunt_started")) {
		stop("hunt_region_unavailable", position);
		return;
	}
	const int32_t duration = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS));
	huntDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
	clearNavigation();
	++completedCycles;
	std::ostringstream fields;
	fields << "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" << completedCycles
	       << ",\"duration_seconds\":" << duration;
	emit("action_result", position, fields.str());
}

void PlayerBotController::processTraversal(Player* player, const Position& currentPosition)
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
	if (progressionObjective != ProgressionObjective::None) {
		processProgression(player, currentPosition);
		return;
	}
	if (scenarioStage == ScenarioStage::LootCorpse) {
		lootCorpse(player, currentPosition);
		return;
	}
	if (cyclePhase == CyclePhase::Hunt &&
	    (std::chrono::steady_clock::now() >= huntDeadline || player->getFreeCapacity() < returnCapacityThreshold)) {
		const char* reason = player->getFreeCapacity() < returnCapacityThreshold ? "capacity" : "hunt_deadline";
		if (progressionEnabled) {
			finishHuntAndSelectGoal(player, currentPosition, reason);
			return;
		} else {
			beginService(player, currentPosition, reason);
		}
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

	const std::vector<Position>* patrolPoints = activeHuntRegion ? &activeHuntRegion->patrolPoints : nullptr;
	const Position& target = patrolPoints && !patrolPoints->empty() ?
	                         (*patrolPoints)[huntRouteIndex % patrolPoints->size()] : huntingLoop[huntRouteIndex];
	if (!processNavigation(player, currentPosition, target)) {
		if (activeHuntRegion && (navigationOscillationDetected || fixedTargetRouteFailureCount >= 3)) {
			emit("hunt_region_patrol", currentPosition,
			     "\"result\":\"skipped\",\"reason\":" +
			         jsonString(navigationOscillationDetected ? "position_oscillation" : "unreachable") +
			         ",\"region_id\":" +
			         std::to_string(activeHuntRegion->id) + ",\"destination\":{\"x\":" +
			         std::to_string(target.x) + ",\"y\":" + std::to_string(target.y) +
			         ",\"z\":" + std::to_string(target.z) + "}");
			fixedTargetRouteFailureCount = 0;
			clearNavigation();
			activeHuntRegion->patrolPoints.erase(activeHuntRegion->patrolPoints.begin() + huntRouteIndex);
			if (activeHuntRegion->patrolPoints.empty()) {
				huntRegionCooldowns[activeHuntRegion->center] = std::chrono::steady_clock::now() + huntRegionCooldown;
				beginService(player, currentPosition, "hunt_region_patrol_unreachable");
			} else {
				huntRouteIndex %= activeHuntRegion->patrolPoints.size();
			}
		}
		return;
	}
	huntRouteIndex = patrolPoints && !patrolPoints->empty() ?
	                 (huntRouteIndex + 1) % patrolPoints->size() : (huntRouteIndex + 1) % huntingLoop.size();
	std::ostringstream fields;
	fields << "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" << huntRouteIndex
	       << ",\"region_id\":" << (activeHuntRegion ? std::to_string(activeHuntRegion->id) : "null");
	emit("action_result", currentPosition, fields.str());
	schedule(SCHEDULER_MINTICKS);
}
