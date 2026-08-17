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

namespace {
	Position nearestTargetApproach(Player& player, const Position& currentPosition, const Position& targetPosition)
	{
		std::vector<Direction> route;
		if (!player.getPathTo(targetPosition, route, 1, 1, true, true, 32)) {
			return targetPosition;
		}
		Position destination = currentPosition;
		for (Direction direction : route) {
			destination = getNextPosition(direction, destination);
		}
		return destination;
	}

	double projectedHuntStaminaMultiplier(const Player& player, double availableHuntSeconds)
	{
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		if (staminaMinutes == 0) {
			return 0;
		}
		if (!g_config.getBoolean(ConfigManager::STAMINA_SYSTEM)) {
			return 1;
		}
		if (staminaMinutes > 2400 && player.isPremium() && availableHuntSeconds > 0) {
			const double bonusSeconds = std::min(availableHuntSeconds,
			                                     std::max<int32_t>(0, staminaMinutes - 2402) * 60.0);
			return 1 + 0.5 * bonusSeconds / availableHuntSeconds;
		}
		return staminaMinutes <= 840 ? 0.5 : 1;
	}
}

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
			recordHuntRecovery(true);
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
	cancelHuntRegionPlanning();
	if (now < healRetryAfter || !player->canDoAction()) {
		return true;
	}
	if (handleSpellHealing(player, currentPosition)) {
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

void PlayerBotController::beginTargetPursuit(Player* player, const Position& currentPosition)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearNavigation();
	targetPursuitStarted = std::chrono::steady_clock::now();
	targetPursuitStartPosition = currentPosition;
	targetPursuitDestination = nearestTargetApproach(*player, currentPosition, ratPosition);
	setStage(ScenarioStage::TargetPursuit, currentPosition);
	emit("action_result", currentPosition,
	     "\"action\":\"target_pursuit\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(ratId) + ",\"last_seen_position\":{\"x\":" + std::to_string(ratPosition.x) +
	         ",\"y\":" + std::to_string(ratPosition.y) + ",\"z\":" + std::to_string(ratPosition.z) + '}');
}

void PlayerBotController::finishTargetPursuit(const Position& currentPosition, const char* reason)
{
	const uint32_t previousTargetId = ratId;
	if (previousTargetId != 0) {
		suppressedTraversalTargets[previousTargetId] = std::chrono::steady_clock::now() + lostTargetSuppression;
	}
	clearNavigation();
	clearRatTarget(currentPosition, reason);
	setStage(ScenarioStage::Traverse, currentPosition);
	emit("action_result", currentPosition,
	     "\"action\":\"target_pursuit\",\"result\":\"abandoned\",\"target_id\":" +
	         std::to_string(previousTargetId) + ",\"reason\":" + jsonString(reason));
}

void PlayerBotController::processTargetPursuit(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	const uint32_t pursuitDistance = std::max(Position::getDistanceX(targetPursuitStartPosition, currentPosition),
	                                          Position::getDistanceY(targetPursuitStartPosition, currentPosition));
	if (now - targetPursuitStarted >= lostTargetPursuitTimeout ||
	    pursuitDistance > maximumLostTargetPursuitDistance) {
		finishTargetPursuit(currentPosition, "pursuit_budget_exhausted");
		schedule(navigationInterval);
		return;
	}

	Creature* target = g_game.getCreatureByID(ratId);
	if (!target || target->isRemoved() || target->isDead() || !player->canSee(target->getPosition()) ||
	    !player->canSeeCreature(target)) {
		target = nullptr;
	} else {
		ratPosition = target->getPosition();
		const uint32_t targetDistance = std::max(Position::getDistanceX(currentPosition, ratPosition),
		                                         Position::getDistanceY(currentPosition, ratPosition));
		if (targetDistance > maximumTargetReacquisitionDistance) {
			targetPursuitDestination = nearestTargetApproach(*player, currentPosition, ratPosition);
			target = nullptr;
		}
	}
	if (target) {
		++counters.actionsAttempted;
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		if (player->getAttackedCreature() == target) {
			combatStarted = std::chrono::steady_clock::now();
			clearNavigation();
			setStage(ScenarioStage::TraversalCombat, currentPosition);
			emit("action_result", currentPosition,
			     "\"action\":\"target_pursuit\",\"result\":\"reacquired\",\"target_id\":" +
			         std::to_string(target->getID()));
			schedule(navigationInterval);
			return;
		}
	}

	if (currentPosition == targetPursuitDestination ||
	    processNavigation(player, currentPosition, targetPursuitDestination)) {
		finishTargetPursuit(currentPosition, "last_seen_position_reached");
		schedule(navigationInterval);
	}
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
{
	Creature* target = g_game.getCreatureByID(ratId);
	if (!target || target->isRemoved() || target->isDead()) {
		beginLoot(player, currentPosition);
	} else if (!player->canSee(target->getPosition()) || !player->canSeeCreature(target) ||
	           player->getAttackedCreature() != target) {
		beginTargetPursuit(player, currentPosition);
	} else if (std::chrono::steady_clock::now() - combatStarted >= traversalCombatTimeout) {
		logActionFailure("attack", "combat_timeout", currentPosition);
		suppressedTraversalTargets[target->getID()] = std::chrono::steady_clock::now() + traversalTargetSuppression;
		finishTraversalCombat(player, currentPosition, "combat_timeout");
	} else {
		ratPosition = target->getPosition();
		if (tryOffensiveSpell(player, currentPosition)) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}
	schedule(navigationInterval);
}

bool PlayerBotController::isActiveHuntCombat(const Player& player) const
{
	return activeHuntRegion && cyclePhase == CyclePhase::Hunt && scenarioStage == ScenarioStage::TraversalCombat &&
	       const_cast<Player&>(player).getAttackedCreature() != nullptr;
}

void PlayerBotController::recordHuntCombatObservation(bool active, double elapsedSeconds, int32_t health,
	                                                     int32_t maximumHealth, uint32_t attackers)
{
	if (!active) {
		return;
	}
	huntCombatEvidence.activeSeconds += std::max(0.0, elapsedSeconds);
	huntCombatEvidence.minimumHealth = std::min(huntCombatEvidence.minimumHealth, health);
	const uint8_t healthPercent = maximumHealth <= 0 ? 0 : static_cast<uint8_t>(std::clamp(
		health * 100 / maximumHealth, 0, 100));
	++huntCombatEvidence.healthPercentSamples[healthPercent];
	huntCombatEvidence.maximumAttackerOverlap = std::max(huntCombatEvidence.maximumAttackerOverlap, attackers);
}

void PlayerBotController::recordActiveHuntCombat(const Player& player)
{
	const auto now = std::chrono::steady_clock::now();
	const bool active = isActiveHuntCombat(player);
	if (!active) {
		huntCombatEvidence.lastSample = now;
		return;
	}
	double elapsedSeconds = 0;
	if (huntCombatEvidence.lastSample.time_since_epoch().count() != 0) {
		elapsedSeconds = std::chrono::duration<double>(now - huntCombatEvidence.lastSample).count();
	}
	huntCombatEvidence.lastSample = now;
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, player.getPosition());
	uint32_t attackers = 0;
	for (Creature* creature : spectators) {
		if (creature->getMonster() && creature->getAttackedCreature() == &player) {
			++attackers;
		}
	}
	recordHuntCombatObservation(active, elapsedSeconds, player.getHealth(), player.getMaxHealth(), attackers);
}

uint8_t PlayerBotController::p10HuntCombatHealthPercent() const
{
	const uint32_t samples = std::accumulate(huntCombatEvidence.healthPercentSamples.begin(),
	                                         huntCombatEvidence.healthPercentSamples.end(), 0U);
	if (samples == 0) {
		return 0;
	}
	const uint32_t threshold = (samples + 9) / 10;
	uint32_t cumulative = 0;
	for (uint8_t percent = 0; percent <= 100; ++percent) {
		cumulative += huntCombatEvidence.healthPercentSamples[percent];
		if (cumulative >= threshold) {
			return percent;
		}
	}
	return 100;
}

void PlayerBotController::recordHuntRecovery(bool potion)
{
	Player* player = g_game.getPlayerByID(playerId);
	if (!player || !activeHuntRegion || !isActiveHuntCombat(*player)) {
		return;
	}
	if (potion) {
		++huntCombatEvidence.potionRecoveries;
	} else {
		++huntCombatEvidence.spellRecoveries;
	}
}

void PlayerBotController::updateChallengeFrontier(const Player& player, const Position& position,
	                                                  uint64_t huntDurationSeconds, const char* reason)
{
	const double uptime = huntDurationSeconds == 0 ? 0 : huntCombatEvidence.activeSeconds / huntDurationSeconds;
	const uint32_t recoveries = huntCombatEvidence.potionRecoveries + huntCombatEvidence.spellRecoveries;
	const bool enoughActiveCombat = huntCombatEvidence.activeSeconds >= 30 && uptime >= 0.5;
	const bool nearFullHealth = huntCombatEvidence.minimumHealth != std::numeric_limits<int32_t>::max() &&
	                            static_cast<int64_t>(huntCombatEvidence.minimumHealth) * 100 >=
	                                static_cast<int64_t>(player.getMaxHealth()) * challengeHealthSafetyPercent;
	const double before = challengeFrontier.target;
	const bool backoff = huntCombatEvidence.dangerObserved || huntCombatEvidence.deathObserved || recoveries != 0;
	const bool qualifyingEasy = enoughActiveCombat && nearFullHealth && !backoff;
	const bool retreatObserved = std::strcmp(reason, "hunt_region_observed_danger") == 0;
	const char* result = "insufficient_active_combat";
	if (backoff && (huntCombatEvidence.activeSeconds > 0 || huntCombatEvidence.deathObserved)) {
		challengeFrontier.target = std::max(minimumChallengeFrontier, challengeFrontier.target - challengeBackoff);
		challengeFrontier.qualifyingHuntsToHold = 2;
		result = "backoff";
	} else if (qualifyingEasy && challengeFrontier.qualifyingHuntsToHold != 0) {
		--challengeFrontier.qualifyingHuntsToHold;
		result = "hold";
	} else if (qualifyingEasy) {
		challengeFrontier.target = std::min(maximumChallengeFrontier, challengeFrontier.target + challengeEscalation);
		result = challengeFrontier.target == before ? "clamped" : "escalated";
	}
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(3)
	       << "\"result\":" << jsonString(result)
	       << ",\"reason\":" << jsonString(reason)
	       << ",\"frontier_before\":" << before
	       << ",\"frontier_after\":" << challengeFrontier.target
	       << ",\"hold_qualifying_hunts\":" << static_cast<uint16_t>(challengeFrontier.qualifyingHuntsToHold)
	       << ",\"active_combat_seconds\":" << huntCombatEvidence.activeSeconds
	       << ",\"active_combat_uptime\":" << uptime
	       << ",\"minimum_health\":" << (huntCombatEvidence.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : huntCombatEvidence.minimumHealth)
	       << ",\"verified_recoveries\":" << recoveries
	       << ",\"retreat\":" << (retreatObserved ? "true" : "false")
	       << ",\"danger\":" << (huntCombatEvidence.dangerObserved ? "true" : "false")
	       << ",\"death\":" << (huntCombatEvidence.deathObserved ? "true" : "false");
	emit("hunt_challenge_frontier", position, fields.str());
}

void PlayerBotController::runAdaptiveChallengeFixture(Player& player, const Position& position)
{
	adaptiveChallengeFixtureRun = true;
	auto applyEvidence = [this, &player, &position](double activeSeconds, uint32_t potionRecoveries, bool death = false) {
		huntCombatEvidence = HuntCombatEvidence{};
		recordHuntCombatObservation(activeSeconds != 0, activeSeconds, player.getMaxHealth(), player.getMaxHealth(), 1);
		huntCombatEvidence.potionRecoveries = potionRecoveries;
		huntCombatEvidence.deathObserved = death;
		updateChallengeFrontier(player, position, 60, "adaptive_challenge_fixture");
	};
	huntCombatEvidence = HuntCombatEvidence{};
	recordHuntCombatObservation(false, 30, player.getMaxHealth(), player.getMaxHealth(), 1);
	const double idleObservedSeconds = huntCombatEvidence.activeSeconds;
	recordHuntCombatObservation(true, 30, player.getMaxHealth(), player.getMaxHealth(), 1);
	const double activeObservedSeconds = huntCombatEvidence.activeSeconds;
	applyEvidence(0, 0); // Travel and idle health must not advance the frontier.
	applyEvidence(30, 0);
	applyEvidence(30, 0);
	applyEvidence(30, 1);
	applyEvidence(30, 0);
	applyEvidence(30, 0);
	applyEvidence(30, 0);
	applyEvidence(0, 0, true);

	const PlayerBotHuntRegionScan scan = huntRegionPlanner.beginScan(player);
	PlayerBotHuntRegion current;
	PlayerBotHuntRegion equipped;
	PlayerBotHuntPlanningProfile planningProfile;
	if (!scan.candidateIndices.empty()) {
		const Item* weapon = player.getWeapon(true);
		const PlayerBotCombatProfile profile{
			player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(), weapon ? weapon->getAttack() : 7,
			weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor(),
		};
		planningProfile = playerBotHuntPlanningProfile(player, profile, challengeFrontier.target);
		PlayerBotCombatProfile improved = profile;
		improved.armor += 20;
		huntRegionPlanner.score(player, planningProfile, scan.revision, scan.candidateIndices.front(), {}, huntRegionPerformance,
		                       60, current);
		const PlayerBotHuntPlanningProfile improvedProfile = playerBotHuntPlanningProfile(player, improved, challengeFrontier.target);
		huntRegionPlanner.score(player, improvedProfile, scan.revision, scan.candidateIndices.front(), {}, huntRegionPerformance,
		                       60, equipped);
	}
	const PlayerBotRecoveryPrediction recovery = playerBotPredictRecovery(planningProfile, 30);
	PlayerBotHuntRegion inBand;
	inBand.score = 10;
	inBand.suitable = true;
	inBand.reachable = true;
	inBand.inChallengeBand = true;
	PlayerBotHuntRegion easier;
	easier.score = 1000;
	easier.suitable = true;
	easier.reachable = true;
	std::vector<PlayerBotHuntRegion> exhausted(1);
	exhausted.front().suitable = true;
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2)
	       << "\"recovery_total\":" << recovery.totalMinimumHealing
	       << ",\"recovery_spell_legal\":" << (recovery.lightHealingLegal ? "true" : "false")
	       << ",\"recovery_spell_casts\":" << recovery.spellCasts
	       << ",\"equipment_pressure_before\":" << current.threatRatio
	       << ",\"equipment_pressure_after\":" << equipped.threatRatio
	       << ",\"idle_observed_seconds\":" << idleObservedSeconds
	       << ",\"active_observed_seconds\":" << activeObservedSeconds
	       << ",\"in_band_outranks_easier\":" << (playerBotPreferHuntRegion(inBand, easier) ? "true" : "false")
	       << ",\"wounded_lethal\":" << (playerBotPredictedLethal(40, 40) ? "true" : "false")
	       << ",\"zero_health_lethal\":" << (playerBotPredictedLethal(0, 0) ? "true" : "false")
	       << ",\"helper_scope_exhausted\":" << (playerBotHuntScopeExhausted(exhausted) ? "true" : "false");
	emit("adaptive_challenge_fixture", position, fields.str());
	huntCombatEvidence = HuntCombatEvidence{};
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
	       << ",\"stamina_minutes\":" << region.staminaMinutes
	       << ",\"stamina_experience_multiplier\":" << region.staminaExperienceMultiplier
	       << ",\"projected_experience\":" << region.projectedExperience
	       << ",\"threat_ratio\":" << region.threatRatio
	       << ",\"raw_threat_ratio\":" << region.rawThreatRatio
	       << ",\"current_health\":" << region.currentHealth
	       << ",\"predicted_fight_seconds\":" << region.predictedFightSeconds
	       << ",\"challenge_frontier\":" << region.challengeFrontier
	       << ",\"challenge_band_minimum\":" << region.challengeBandMinimum
	       << ",\"challenge_band_maximum\":" << region.challengeBandMaximum
	       << ",\"in_challenge_band\":" << (region.inChallengeBand ? "true" : "false")
	       << ",\"predicted_lethal\":" << (region.predictedLethal ? "true" : "false")
	       << ",\"recovery\":{\"light_healing_legal\":" << (region.recovery.lightHealingLegal ? "true" : "false")
	       << ",\"spell_minimum_healing\":" << region.recovery.spellMinimumHealing
	       << ",\"potion_minimum_healing\":" << region.recovery.potionMinimumHealing
	       << ",\"total_minimum_healing\":" << region.recovery.totalMinimumHealing
	       << ",\"available_before_lethal\":" << region.recovery.availableBeforeLethal
	       << ",\"spell_mana_cost\":" << region.recovery.spellManaCost
	       << ",\"spell_cooldown\":" << region.recovery.spellCooldown
	       << ",\"spell_casts\":" << region.recovery.spellCasts
	       << ",\"potion_uses\":" << region.recovery.potionUses
	       << ",\"mana_reserve\":" << region.recovery.manaReserve << '}'
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
	const uint8_t p10HealthPercent = p10HuntCombatHealthPercent();
	const int32_t p10Health = player.getMaxHealth() * p10HealthPercent / 100;
	const double activeDamagePerMinute = huntCombatEvidence.activeSeconds == 0 ? 0 :
	                                    huntCombatEvidence.damageTaken * 60.0 / huntCombatEvidence.activeSeconds;
	const double frontierBefore = challengeFrontier.target;
	const bool retreatObserved = std::strcmp(reason, "hunt_region_observed_danger") == 0;
	updateChallengeFrontier(player, position, std::max<int64_t>(0, duration), reason);
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
	       << ",\"damage_taken\":" << huntRegionDamageTaken
	       << ",\"active_combat_seconds\":" << huntCombatEvidence.activeSeconds
	       << ",\"active_combat_uptime\":" << (duration <= 0 ? 0 : huntCombatEvidence.activeSeconds / duration)
	       << ",\"active_combat_damage_per_minute\":" << activeDamagePerMinute
	       << ",\"minimum_health\":" << (huntCombatEvidence.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : huntCombatEvidence.minimumHealth)
	       << ",\"p10_health\":" << p10Health
	       << ",\"p10_health_percent\":" << static_cast<uint16_t>(p10HealthPercent)
	       << ",\"verified_potion_recoveries\":" << huntCombatEvidence.potionRecoveries
	       << ",\"verified_spell_recoveries\":" << huntCombatEvidence.spellRecoveries
	       << ",\"maximum_attacker_overlap\":" << huntCombatEvidence.maximumAttackerOverlap
	       << ",\"retreat_observed\":" << (retreatObserved ? "true" : "false")
	       << ",\"danger_observed\":" << (huntCombatEvidence.dangerObserved ? "true" : "false")
	       << ",\"death_observed\":" << (huntCombatEvidence.deathObserved ? "true" : "false")
	       << ",\"frontier_before\":" << frontierBefore
	       << ",\"frontier_after\":" << challengeFrontier.target;
	emit("hunt_region_outcome", position, fields.str());
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) {
		say(*speakingPlayer, "Leaving hunt: " + std::string(reason) + ". " +
		     std::to_string(huntRegionKills) + " kills, " +
		     std::to_string(player.getExperience() >= huntRegionStartExperience ?
		         player.getExperience() - huntRegionStartExperience : 0) + " experience.");
	}
	activeHuntRegion.reset();
	huntCombatEvidence = HuntCombatEvidence{};
}

void PlayerBotController::cancelHuntRegionPlanning()
{
	huntRegionPlanning.reset();
}

void PlayerBotController::emitHuntRegionPlanning(const HuntRegionPlanning& planning, const Position& position, const char* phase) const
{
	const auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - planning.started).count();
	std::ostringstream fields;
	fields << "\"phase\":" << jsonString(phase)
	       << ",\"cache\":" << jsonString(planning.cacheHit ? "hit" : "build")
	       << ",\"snapshot_time_us\":" << planning.snapshotTimeUs
	       << ",\"clustering_time_us\":" << planning.clusteringTimeUs
		       << ",\"scoring_time_us\":" << planning.scoringTimeUs
		       << ",\"candidate_count\":" << planning.totalCandidates
		       << ",\"scored_candidate_count\":" << planning.scoredCandidates
		       << ",\"suitable_candidate_count\":" << planning.suitableCandidates
		       << ",\"pathfinding_calls\":" << planning.pathfindingCalls
		       << ",\"batch_pathfinding_calls\":" << planning.batchPathfindingCalls
	       << ",\"expanded_nodes\":" << planning.expandedNodes
	       << ",\"yields\":" << planning.yields
	       << ",\"challenge_frontier\":" << planning.profile.challengeFrontier
	       << ",\"decision_latency_us\":" << latencyUs;
	emit("hunt_region_scan", position, fields.str());
}

bool PlayerBotController::selectHuntRegion(Player& player, const Position& position, const char* reason)
{
	const auto now = std::chrono::steady_clock::now();
	if (!huntRegionPlanning && now < huntScopeReevaluationAfter) {
		schedule(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			huntScopeReevaluationAfter - now).count()));
		return false;
	}
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
	const bool staleRevision = huntRegionPlanning &&
	                           huntRegionPlanning->cacheRevision != PlayerBotHuntRegionPlanner::getCacheRevision();
	if (huntRegionPlanning &&
	    (huntRegionPlanning->playerPosition != player.getPosition() || huntRegionPlanning->playerLevel != player.getLevel() ||
	     player.getHealth() < huntRegionPlanning->profile.currentHealth ||
	     huntRegionPlanning->staminaMinutes != player.getStaminaMinutes() ||
	     huntRegionPlanning->excludedRegions != excludedRegions ||
	     staleRevision)) {
		if (staleRevision) {
			emitHuntRegionPlanning(*huntRegionPlanning, position, "stale_revision");
		}
		cancelHuntRegionPlanning();
	}
	if (!huntRegionPlanning) {
		if (testPolicy.adaptiveChallengeFixture && !adaptiveChallengeFixtureRun) {
			runAdaptiveChallengeFixture(player, position);
		}
		PlayerBotHuntRegionScan scan = huntRegionPlanner.beginScan(player);
		HuntRegionPlanning planning;
		planning.regions.reserve(scan.candidateCount);
		planning.reason = reason;
		planning.started = now;
		planning.cacheHit = scan.cacheHit;
		planning.snapshotTimeUs = scan.snapshotTimeUs;
		planning.clusteringTimeUs = scan.clusteringTimeUs;
		planning.cacheRevision = scan.revision;
		planning.candidateIndices = std::move(scan.candidateIndices);
		planning.totalCandidates = static_cast<uint32_t>(scan.candidateCount);
		planning.playerPosition = player.getPosition();
		planning.playerLevel = player.getLevel();
		planning.staminaMinutes = player.getStaminaMinutes();
		planning.excludedRegions = excludedRegions;
		const Item* weapon = player.getWeapon(true);
		const PlayerBotCombatProfile combat{
			player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(), weapon ? weapon->getAttack() : 7,
			weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor(),
		};
		planning.profile = playerBotHuntPlanningProfile(player, combat, challengeFrontier.target);
		huntRegionPlanning = std::move(planning);
		emitHuntRegionPlanning(*huntRegionPlanning, position, "scoring_started");
		return false;
	}

	HuntRegionPlanning& planning = *huntRegionPlanning;
	planning.batchPathfindingCalls = 0;
	if (planning.phase == HuntRegionPlanning::Phase::Scoring) {
		const auto scoringStarted = std::chrono::steady_clock::now();
		for (uint32_t scoredThisTurn = 0;
		     scoredThisTurn < huntRegionScoringCandidatesPerTurn && planning.nextScoringCandidate < planning.totalCandidates;
		     ++scoredThisTurn, ++planning.nextScoringCandidate) {
			PlayerBotHuntRegion region;
			if (!huntRegionPlanner.score(player, planning.profile, planning.cacheRevision,
			                             planning.candidateIndices[planning.nextScoringCandidate], excludedRegions,
			                             huntRegionPerformance, huntDurationSeconds, region)) {
				emitHuntRegionPlanning(planning, position, "stale_revision");
				cancelHuntRegionPlanning();
				return false;
			}
			planning.regions.push_back(std::move(region));
			++planning.scoredCandidates;
		}
		planning.scoringTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - scoringStarted).count();
		if (testPolicy.cancelHuntPlanningAtScoreBarrier && !huntPlanningFixtureCancelled && planning.scoredCandidates != 0) {
			huntPlanningFixtureCancelled = true;
			emitHuntRegionPlanning(planning, position, "cancelled");
			cancelHuntRegionPlanning();
			return false;
		}
		if (testPolicy.cancelHuntPlanningAtScoreBarrier && !huntPlanningFixtureStaleRevisionTriggered && planning.scoredCandidates != 0) {
			huntPlanningFixtureStaleRevisionTriggered = true;
			PlayerBotHuntRegionPlanner::invalidateCache();
			return false;
		}
		if (planning.nextScoringCandidate < planning.totalCandidates) {
			++planning.yields;
			emitHuntRegionPlanning(planning, position, "scoring_yield");
			return false;
		}
		std::sort(planning.regions.begin(), planning.regions.end(), [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
			return playerBotPreferHuntRegion(left, right);
		});
		if (testPolicy.forceHuntScopeExhaustion) {
			for (PlayerBotHuntRegion& region : planning.regions) {
				region.suitable = false;
				region.inChallengeBand = false;
				region.rejectionReason = "fixture_scope_exhausted";
			}
			planning.suitableCandidates = 0;
		}
		uint32_t regionId = 1;
		for (PlayerBotHuntRegion& region : planning.regions) {
			region.id = regionId++;
		}
		planning.suitableCandidates = static_cast<uint32_t>(std::count_if(planning.regions.begin(), planning.regions.end(),
			[](const PlayerBotHuntRegion& region) { return region.suitable; }));
		planning.phase = HuntRegionPlanning::Phase::Reachability;
		emitHuntRegionPlanning(planning, position, "scored");
		return false;
	}

	for (uint32_t pathsThisTurn = 0; pathsThisTurn < huntRegionPathfindingCallsPerTurn;) {
		while (planning.nextCandidate < planning.regions.size()) {
			PlayerBotHuntRegion& candidate = planning.regions[planning.nextCandidate];
			if (!candidate.suitable) {
				++planning.nextCandidate;
				continue;
			}
			std::set<Position> blockedPositions;
			for (const auto& blocked : temporarilyBlockedPositions) {
				if (blocked.second > now) {
					blockedPositions.insert(blocked.first);
				}
			}
			std::deque<PlayerBotNavigationStep> route;
			const bool forcedUnreachable = testPolicy.forceFirstHuntCandidateUnreachable && !planning.fixtureForcedUnreachable;
			const bool forcedNodeLimit = testPolicy.forceSecondHuntCandidateNodeLimit && !planning.fixtureForcedNodeLimit &&
			                             planning.fixtureForcedUnreachable && !forcedUnreachable;
			planning.fixtureForcedUnreachable = planning.fixtureForcedUnreachable || forcedUnreachable;
			planning.fixtureForcedNodeLimit = planning.fixtureForcedNodeLimit || forcedNodeLimit;
			PlayerBotNavigationResult planResult = PlayerBotNavigationResult::Unreachable;
			if (!forcedUnreachable) {
				const auto pathStarted = std::chrono::steady_clock::now();
				++counters.pathfindingCalls;
				++planning.pathfindingCalls;
				++planning.batchPathfindingCalls;
				planResult = navigator.plan(player, candidate.destination, blockedPositions, route, candidate.expandedNodes,
				                            forcedNodeLimit ? 0 : playerBotNavigationMaximumExpandedNodes);
				counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - pathStarted).count();
			}
			planning.expandedNodes += candidate.expandedNodes;
			++pathsThisTurn;
			++planning.nextCandidate;
			if (planResult != PlayerBotNavigationResult::Reached) {
				++counters.pathfindingFailures;
				candidate.rejectionReason = planResult == PlayerBotNavigationResult::NodeLimit ?
					"navigation_node_budget" : "unreachable";
			} else {
				candidate.reachable = true;
				candidate.travelSteps = static_cast<uint32_t>(route.size());
				candidate.estimatedTravelSeconds = 0;
				for (const PlayerBotNavigationStep& step : route) {
					candidate.estimatedTravelSeconds += step.action == PlayerBotNavigationAction::Move ?
						player.getStepDuration(step.direction) / 1000.0 : 1.0;
				}
				candidate.availableHuntSeconds = std::max(0.0, huntDurationSeconds - candidate.estimatedTravelSeconds);
				candidate.staminaExperienceMultiplier = projectedHuntStaminaMultiplier(player, candidate.availableHuntSeconds);
				candidate.projectedExperience = candidate.experiencePerMinute * candidate.observedCorrection *
					candidate.staminaExperienceMultiplier * candidate.availableHuntSeconds / 60.0;
				candidate.score = candidate.projectedExperience;
			}
			++planning.yields;
			emitHuntRegionPlanning(planning, position, "reachability_yield");
			return false;
		}
		if (pathsThisTurn != 0) {
			++planning.yields;
			emitHuntRegionPlanning(planning, position, "reachability_yield");
			return false;
		}
		break;
	}

	auto selected = std::max_element(planning.regions.begin(), planning.regions.end(),
	                                 [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
		                                 return playerBotPreferHuntRegion(right, left);
	                                 });
	for (const PlayerBotHuntRegion& region : planning.regions) {
		emitHuntRegionCandidate(region, position);
	}
	if (playerBotHuntScopeExhausted(planning.regions)) {
		const bool validationBudgetExhausted = std::any_of(planning.regions.begin(), planning.regions.end(),
			[](const PlayerBotHuntRegion& region) { return region.rejectionReason == "navigation_node_budget"; });
		emitHuntRegionPlanning(planning, position, "exhausted");
		emit("hunt_region_selection", position,
		     "\"result\":\"failed\",\"reason\":" +
		         jsonString(validationBudgetExhausted ? "route_validation_budget_exhausted" : "no_suitable_reachable_region"));
		uint32_t reachableCandidates = 0;
		for (const PlayerBotHuntRegion& region : planning.regions) {
			reachableCandidates += region.reachable ? 1 : 0;
		}
		emit("hunt_scope_exhausted", position,
		     "\"candidate_count\":" + std::to_string(planning.totalCandidates) +
		         ",\"scored_candidate_count\":" + std::to_string(planning.scoredCandidates) +
		         ",\"suitable_candidate_count\":" + std::to_string(planning.suitableCandidates) +
		         ",\"reachable_candidate_count\":" + std::to_string(reachableCandidates) +
		         ",\"frontier\":" + std::to_string(challengeFrontier.target) +
		         ",\"retry_delay_ms\":" + std::to_string(huntScopeReevaluationDelay.count() * 1000) +
		         ",\"reason\":" + jsonString(validationBudgetExhausted ? "route_validation_budget_exhausted" : "local_scope_exhausted"));
		huntScopeReevaluationAfter = now + huntScopeReevaluationDelay;
		cancelHuntRegionPlanning();
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
	huntCombatEvidence = HuntCombatEvidence{};
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
	emitHuntRegionPlanning(planning, position, "selected");
	cancelHuntRegionPlanning();
	return true;
}

void PlayerBotController::beginHuntCycle(Player* player, const Position& position, const char* reason)
{
	const int32_t duration = std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS));
	huntDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
	clearNavigation();
	++completedCycles;
	std::ostringstream fields;
	fields << "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" << completedCycles
	       << ",\"duration_seconds\":" << duration;
	emit("action_result", position, fields.str());
}

void PlayerBotController::startHunt(Player* player, const Position& position, const char* reason)
{
	if (!player) {
		stop("controlled_player_not_found", position);
		return;
	}
	if (!ensureCombatReady(player, position, reason)) {
		return;
	}
	activeGoal = TopLevelGoal::Hunt;
	setCyclePhase(CyclePhase::Hunt, position, reason);
	huntRouteIndex = 0;
	if (!testPolicy.fixedFixtureRoute && !activeHuntRegion) {
		if (!selectHuntRegion(*player, position, "hunt_started")) {
			if (huntScopeReevaluationAfter.time_since_epoch().count() == 0 ||
			    std::chrono::steady_clock::now() >= huntScopeReevaluationAfter) {
				schedule(SCHEDULER_MINTICKS);
			}
			return;
		}
	}
	beginHuntCycle(player, position, reason);
}

void PlayerBotController::processTraversal(Player* player, const Position& currentPosition)
{
	if (readinessEquipmentPending) {
		processReadinessEquipment(player, currentPosition);
		return;
	}
	if (cyclePhase != CyclePhase::Hunt || progressionObjective != ProgressionObjective::None) {
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
	if (cyclePhase == CyclePhase::Hunt && !ensureCombatReady(player, currentPosition, "readiness_continuous_check")) {
		return;
	}
	if (cyclePhase == CyclePhase::Hunt && !testPolicy.fixedFixtureRoute && !activeHuntRegion && !huntRegionPlanning) {
		startHunt(player, currentPosition, "hunt_region_restart");
		return;
	}
	if (huntRegionPlanning) {
		if (selectHuntRegion(*player, currentPosition, "hunt_planning")) {
			beginHuntCycle(player, currentPosition, "hunt_region_selected");
		} else {
			schedule(SCHEDULER_MINTICKS);
		}
		return;
	}
	if (scenarioStage == ScenarioStage::LootCorpse) {
		lootCorpse(player, currentPosition);
		return;
	}
	if (cyclePhase == CyclePhase::Hunt &&
	    (std::chrono::steady_clock::now() >= huntDeadline || player->getFreeCapacity() < returnCapacityThreshold)) {
		const char* reason = player->getFreeCapacity() < returnCapacityThreshold ? "capacity" : "hunt_deadline";
		if (testPolicy.progressionEnabled) {
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
		if (testPolicy.fixedFixtureRoute) {
			if (!processNavigation(player, currentPosition, fakeDepotPosition)) {
				return;
			}
			setCyclePhase(CyclePhase::DepositLoot, currentPosition, "fixture_depot_reached");
			processFixtureDeposit(player, currentPosition);
			return;
		}
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Approach, currentPosition)) {
			return;
		}
		if (!processNavigation(player, currentPosition, depotApproachPosition)) {
			return;
		}
		setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
		processDeposit(player, currentPosition);
		return;
	}

	if (cyclePhase == CyclePhase::DepositLoot) {
		if (testPolicy.fixedFixtureRoute) {
			if (currentPosition != fakeDepotPosition) {
				setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "fixture_displaced_during_deposit");
				clearNavigation();
				return;
			}
			processFixtureDeposit(player, currentPosition);
			return;
		}
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		if (!Position::areInRange<1, 1, 0>(currentPosition, depotLockerPosition)) {
			setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "displaced_during_deposit");
			clearNavigation();
			if (!processNavigation(player, currentPosition, depotApproachPosition)) {
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
	if (scenarioStage == ScenarioStage::TargetPursuit) {
		processTargetPursuit(player, currentPosition);
		return;
	}
	if (attackVisibleMonster(player, currentPosition)) {
		schedule(navigationInterval);
		return;
	}
	if (trySupportSpell(player, currentPosition)) {
		schedule(navigationDecisionDelay(*player));
		return;
	}

	const std::vector<Position>* patrolPoints = activeHuntRegion ? &activeHuntRegion->patrolPoints : nullptr;
	const Position& target = patrolPoints && !patrolPoints->empty() ?
	                         (*patrolPoints)[huntRouteIndex % patrolPoints->size()] : huntingLoop[huntRouteIndex];
	if (!processNavigation(player, currentPosition, target)) {
		const bool repeatedStepFailure = blockedStepCount >= maximumRepeatedNavigationStepFailures;
		if (navigationOscillationDetected || repeatedStepFailure ||
		    (activeHuntRegion && fixedTargetRouteFailureCount >= 3)) {
			const char* reason = navigationOscillationDetected ? "position_oscillation" :
			                     repeatedStepFailure ? "repeated_step_failure" : "unreachable";
			emit("hunt_region_patrol", currentPosition,
			     "\"result\":\"skipped\",\"reason\":" +
			         jsonString(reason) +
			         ",\"step_failures\":" + std::to_string(blockedStepCount) +
			         ",\"region_id\":" +
			         (activeHuntRegion ? std::to_string(activeHuntRegion->id) : "null") +
			         ",\"destination\":{\"x\":" +
			         std::to_string(target.x) + ",\"y\":" + std::to_string(target.y) +
			         ",\"z\":" + std::to_string(target.z) + "}");
			if (repeatedStepFailure) {
				++counters.stuckEvents;
			}
			fixedTargetRouteFailureCount = 0;
			blockedStepCount = 0;
			clearNavigation();
			if (activeHuntRegion) {
				activeHuntRegion->patrolPoints.erase(activeHuntRegion->patrolPoints.begin() + huntRouteIndex);
				if (activeHuntRegion->patrolPoints.empty()) {
					huntRegionCooldowns[activeHuntRegion->center] = std::chrono::steady_clock::now() + huntRegionCooldown;
					beginService(player, currentPosition, "hunt_region_patrol_unreachable");
				} else {
					huntRouteIndex %= activeHuntRegion->patrolPoints.size();
				}
			} else {
				huntRouteIndex = (huntRouteIndex + 1) % huntingLoop.size();
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
