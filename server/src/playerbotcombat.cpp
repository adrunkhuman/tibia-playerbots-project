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

PlayerBotExpectedCorpse PlayerBotController::expectedCorpseFor(const Creature& target) const
{
	PlayerBotExpectedCorpse expectation;
	const Monster* monster = target.getMonster();
	expectation.itemId = monster ? monster->getCorpseItemId() : 0;
	if (expectation.itemId != 0) {
		const ItemType& corpseType = Item::items[expectation.itemId];
		expectation.lootable = corpseType.corpseType != RACE_NONE && corpseType.isContainer();
	}
	return expectation;
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

void PlayerBotController::logHealResult(const char* result, const char* reason, const PlayerBotPotionAttempt& before,
					const PlayerBotPotionAttempt& after, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"heal\",\"result\":" << jsonString(result)
	       << ",\"method\":\"small_health_potion\",\"item_id\":" << smallHealthPotionItemId
	       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(stageName(scenarioStage))
	       << ",\"health_before\":" << before.health
	       << ",\"health_after\":" << after.health
	       << ",\"health_max\":" << before.healthMaximum
	       << ",\"resource_before\":" << before.potionCount
	       << ",\"resource_after\":" << after.potionCount;
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("action_result", position, fields.str());
}

bool PlayerBotController::handleHealing(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	if (const auto verification = recoverySession.verifyPotion(
		{player->getHealth(), player->getMaxHealth(), inventoryPolicy.inventoryItemCount(*player, smallHealthPotionItemId)},
		now, healingRetryInterval)) {
		if (verification->result == PlayerBotPotionVerificationResult::Success) {
			logHealResult("success", nullptr, verification->before, verification->after, currentPosition);
			recordHuntRecovery(true);
		} else {
			++counters.actionsFailed;
			logHealResult("failed", verification->result == PlayerBotPotionVerificationResult::IneffectiveRecovery ?
			              "ineffective_recovery" : "use_not_verified", verification->before, verification->after, currentPosition);
		}
	}
	if (serviceStage == ServiceStage::BuyPotions) {
		return false;
	}

	if (!needsHealing(*player)) {
		return false;
	}
	cancelHuntRegionPlanning();
	if (!recoverySession.canRetryPotion(now) || !player->canDoAction()) {
		return true;
	}
	if (handleSpellHealing(player, currentPosition)) {
		return true;
	}

	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(*player, smallHealthPotionItemId);
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

	recoverySession.beginPotion({player->getHealth(), player->getMaxHealth(), potionCount});
	++counters.actionsAttempted;
	g_game.playerUseWithCreature(playerId, Position(0xFFFF, 0, 0), 0, playerId, potion->getClientID());
	return true;
}

void PlayerBotController::logEatSuccess(uint16_t itemId, uint32_t inventoryCount, int32_t foodTicks, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"eat\",\"result\":\"success\",\"item_id\":" << itemId
	       << ",\"count\":1,\"inventory_count\":" << inventoryCount << ",\"food_ticks\":" << foodTicks;
	emit("action_result", position, fields.str());
}

bool PlayerBotController::handleFood(Player* player, const Position& currentPosition)
{
	if (pendingLootItemId != 0) {
		return false;
	}

	const auto now = std::chrono::steady_clock::now();
	if (const PlayerBotFoodAttempt* pending = recoverySession.pendingFood()) {
		const uint32_t inventoryCount = inventoryPolicy.inventoryItemCount(*player, pending->itemId);
		const int32_t foodTicks = getFoodTicks(*player);
		const auto verification = recoverySession.verifyFood(inventoryCount, foodTicks, canEatCheese(*player), now,
		                                                   maximumEatFailures, std::chrono::seconds(5), eatFailureCooldown);
		if (verification->result == PlayerBotFoodVerificationResult::Success) {
			logEatSuccess(verification->before.itemId, verification->inventoryCount, verification->foodTicks, currentPosition);
		} else if (verification->result == PlayerBotFoodVerificationResult::Failed ||
		           verification->result == PlayerBotFoodVerificationResult::Cooldown) {
			logActionFailure("eat", "consumption_not_verified", currentPosition);
			if (verification->result == PlayerBotFoodVerificationResult::Cooldown) {
				emit("action_result", currentPosition,
				     "\"action\":\"eat\",\"result\":\"cooldown\",\"reason\":\"retry_exhausted\",\"retry_after_ms\":" +
				         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(eatFailureCooldown).count()));
			}
		}
	}

	if (!recoverySession.canRetryFood(now) || !canEatCheese(*player)) {
		return false;
	}

	if (inventoryPolicy.foodInventory(*player).count <= preferredFoodCount) {
		return false;
	}
	Item* food = nullptr;
	std::function<void(Item*)> findFood = [&](Item* item) {
		if (!item || food) {
			return;
		}
		if (PlayerBotInventoryPolicy::isFoodItem(item->getID())) {
			food = item;
			return;
		}
		if (Container* container = item->getContainer()) {
			for (Item* child : container->getItemList()) {
				findFood(child);
			}
		}
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST && !food; ++slot) {
		findFood(player->getInventoryItem(static_cast<slots_t>(slot)));
	}
	if (!food) {
		return false;
	}
	if (!player->canDoAction()) {
		return true;
	}

	recoverySession.beginFood({food->getID(), inventoryPolicy.inventoryItemCount(*player, food->getID()), getFoodTicks(*player)});
	++counters.actionsAttempted;
	g_game.playerUseItem(playerId, Position(0xFFFF, 0, 0), 0, 0, food->getClientID());
	return true;
}

void PlayerBotController::setTraversalTarget(Creature* target, const Position& position)
{
	const PlayerBotTarget selected{target->getID(), target->getPosition(), target->getName()};
	targetingSession.beginTraversalCombat(selected, expectedCorpseFor(*target), std::chrono::steady_clock::now());
	std::ostringstream fields;
	fields << "\"previous_target_id\":null,\"target_id\":" << selected.id
	       << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(target->getName())
	       << ",\"target_position\":{\"x\":" << selected.position.x << ",\"y\":" << selected.position.y
	       << ",\"z\":" << static_cast<uint16_t>(selected.position.z) << "},\"reason\":\"visible_monster\"";
	emit("target_changed", position, fields.str());
}

bool PlayerBotController::attackVisibleMonster(Player* player, const Position& currentPosition)
{
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	std::vector<PlayerBotTarget> candidates;
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
		candidates.push_back({creature->getID(), creature->getPosition(), creature->getName()});
	}
	while (!candidates.empty()) {
		const auto selected = targetingSession.selectVisibleTarget(candidates, currentPosition,
		                                                          std::chrono::steady_clock::now());
		if (!selected) {
			return false;
		}
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [selected](const PlayerBotTarget& candidate) {
			return candidate.id == selected->id;
		}), candidates.end());
		Creature* target = g_game.getCreatureByID(selected->id);
		if (!target) {
			continue;
		}
		++counters.actionsAttempted;
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		if (player->getAttackedCreature() != target) {
			continue;
		}
		setTraversalTarget(target, currentPosition);
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
		return navigationSession.isRouteCritical(creature->getPosition(), now);
	};
	std::vector<PlayerBotDefensiveTarget> candidates;
	for (Creature* creature : spectators) {
		const bool routeCritical = isRouteCritical(creature);
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
		    (creature->getAttackedCreature() != player && !routeCritical) || !player->canSee(creature->getPosition()) ||
		    !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) {
			continue;
		}

		PlayerBotDefensiveTarget candidate;
		candidate.id = creature->getID();
		candidate.position = creature->getPosition();
		candidate.name = creature->getName();
		candidate.routeCritical = routeCritical;
		candidates.push_back(std::move(candidate));
	}
	const auto selected = targetingSession.selectDefensiveTarget(std::move(candidates), currentPosition);
	if (!selected) {
		return false;
	}
	Creature* target = g_game.getCreatureByID(selected->id);
	if (!target) {
		return false;
	}
	++counters.actionsAttempted;
	g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
	g_game.playerSetAttackedCreature(playerId, target->getID());
	if (player->getAttackedCreature() != target) {
		logActionFailure("defensive_combat", "target_rejected", currentPosition);
		return false;
	}
	targetingSession.beginDefensiveCombat(*selected, std::chrono::steady_clock::now());
	clearNavigation();
	std::ostringstream targetFields;
	targetFields << "\"previous_target_id\":null,\"target_id\":" << selected->id
	             << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(selected->name)
	             << ",\"target_position\":{\"x\":" << selected->position.x
	             << ",\"y\":" << selected->position.y << ",\"z\":"
	             << static_cast<uint16_t>(selected->position.z) << "},\"reason\":"
	             << jsonString(selected->routeCritical ? "defensive_path_blocker" : "defensive_attacker")
	             << ",\"route_critical\":" << (selected->routeCritical ? "true" : "false");
	emit("target_changed", currentPosition, targetFields.str());
	emit("action_result", currentPosition,
	     "\"action\":\"defensive_combat\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(selected->id) + ",\"chase\":false,\"route_critical\":" +
	         (selected->routeCritical ? "true" : "false"));
	return true;
}

void PlayerBotController::finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason)
{
	const auto previous = targetingSession.clearDefensiveTarget();
	const uint32_t previousTarget = previous ? previous->id : 0;
	if (player->getAttackedCreature() && player->getAttackedCreature()->getID() == previousTarget) {
		g_game.playerSetAttackedCreature(playerId, 0);
	}
	clearNavigation();
	emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTarget) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	emit("action_result", currentPosition, "\"action\":\"defensive_combat\",\"result\":" +
	     jsonString(result) + ",\"target_id\":" + std::to_string(previousTarget) +
	     ",\"reason\":" + jsonString(reason));
}

void PlayerBotController::processDefensiveCombat(Player* player, const Position& currentPosition)
{
	const auto& defensive = targetingSession.defensiveTarget();
	Creature* target = defensive ? g_game.getCreatureByID(defensive->id) : nullptr;
	if (!target || target->isRemoved() || target->isDead()) {
		finishDefensiveCombat(player, currentPosition, "success", "target_defeated");
	} else if ((!defensive->routeCritical && target->getAttackedCreature() != player) || !player->canSee(target->getPosition()) ||
	           !Position::areInRange<1, 1, 0>(currentPosition, target->getPosition())) {
		finishDefensiveCombat(player, currentPosition, "skipped", "threat_disengaged");
	} else if (player->getAttackedCreature() != target) {
		finishDefensiveCombat(player, currentPosition, "failed", "target_lost");
	} else if (targetingSession.defensiveCombatTimedOut(std::chrono::steady_clock::now(), traversalCombatTimeout)) {
		finishDefensiveCombat(player, currentPosition, "failed", "combat_timeout");
	} else {
		targetingSession.updateDefensiveTargetPosition(target->getPosition());
	}
	schedule(navigationInterval);
}

void PlayerBotController::finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearTraversalTarget(currentPosition, reason);
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::beginTargetPursuit(Player* player, const Position& currentPosition)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearNavigation();
	const auto& target = targetingSession.traversalTarget();
	if (!target) {
		return;
	}
	const Position destination = nearestTargetApproach(*player, currentPosition, target->position);
	targetingSession.beginPursuit(currentPosition, destination, std::chrono::steady_clock::now());
	setStage(ScenarioStage::TargetPursuit, currentPosition);
	emit("action_result", currentPosition,
	     "\"action\":\"target_pursuit\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(target->id) + ",\"last_seen_position\":{\"x\":" + std::to_string(target->position.x) +
	         ",\"y\":" + std::to_string(target->position.y) + ",\"z\":" + std::to_string(target->position.z) + '}');
}

void PlayerBotController::finishTargetPursuit(const Position& currentPosition, const char* reason)
{
	const auto previous = targetingSession.abandonPursuit(std::chrono::steady_clock::now(), lostTargetSuppression);
	const uint32_t previousTargetId = previous ? previous->id : 0;
	clearNavigation();
	if (previous && shouldEmitRepeated(std::string("target:clear:") + reason)) {
		emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTargetId) +
		     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	}
	setStage(ScenarioStage::Traverse, currentPosition);
	emit("action_result", currentPosition,
	     "\"action\":\"target_pursuit\",\"result\":\"abandoned\",\"target_id\":" +
	         std::to_string(previousTargetId) + ",\"reason\":" + jsonString(reason));
}

void PlayerBotController::processTargetPursuit(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	if (targetingSession.pursuitBudgetExhausted(currentPosition, now, lostTargetPursuitTimeout,
	                                           maximumLostTargetPursuitDistance)) {
		finishTargetPursuit(currentPosition, "pursuit_budget_exhausted");
		schedule(navigationInterval);
		return;
	}

	const auto& traversal = targetingSession.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	if (!target || target->isRemoved() || target->isDead() || !player->canSee(target->getPosition()) ||
	    !player->canSeeCreature(target)) {
		target = nullptr;
	} else {
		targetingSession.updateTraversalTargetPosition(target->getPosition());
		const uint32_t targetDistance = std::max(Position::getDistanceX(currentPosition, target->getPosition()),
		                                         Position::getDistanceY(currentPosition, target->getPosition()));
		if (targetDistance > maximumTargetReacquisitionDistance) {
			targetingSession.updatePursuitDestination(nearestTargetApproach(*player, currentPosition, target->getPosition()));
			target = nullptr;
		}
	}
	if (target) {
		++counters.actionsAttempted;
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		if (player->getAttackedCreature() == target) {
			targetingSession.beginTraversalCombat({target->getID(), target->getPosition(), target->getName()},
			                                           traversal->expectedCorpse, std::chrono::steady_clock::now());
			clearNavigation();
			setStage(ScenarioStage::TraversalCombat, currentPosition);
			emit("action_result", currentPosition,
			     "\"action\":\"target_pursuit\",\"result\":\"reacquired\",\"target_id\":" +
			         std::to_string(target->getID()));
			schedule(navigationInterval);
			return;
		}
	}

	if (currentPosition == targetingSession.pursuitDestination() ||
	    processNavigation(player, currentPosition, targetingSession.pursuitDestination())) {
		finishTargetPursuit(currentPosition, "last_seen_position_reached");
		schedule(navigationInterval);
	}
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
{
	const auto& traversal = targetingSession.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	if (!target || target->isRemoved() || target->isDead()) {
		beginLoot(player, currentPosition);
	} else if (!player->canSee(target->getPosition()) || !player->canSeeCreature(target) ||
	           player->getAttackedCreature() != target) {
		beginTargetPursuit(player, currentPosition);
	} else if (targetingSession.traversalCombatTimedOut(std::chrono::steady_clock::now(), traversalCombatTimeout)) {
		logActionFailure("attack", "combat_timeout", currentPosition);
		targetingSession.suppressTraversalTarget(target->getID(), std::chrono::steady_clock::now() + traversalTargetSuppression);
		finishTraversalCombat(player, currentPosition, "combat_timeout");
	} else {
		targetingSession.updateTraversalTargetPosition(target->getPosition());
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

void PlayerBotController::recordActiveHuntCombat(const Player& player)
{
	const auto now = std::chrono::steady_clock::now();
	const bool active = isActiveHuntCombat(player);
	SpectatorVec spectators;
	uint32_t attackers = 0;
	if (active) {
		g_game.map.getSpectators(spectators, player.getPosition());
		for (Creature* creature : spectators) {
			if (creature->getMonster() && creature->getAttackedCreature() == &player) {
				++attackers;
			}
		}
	}
	huntPolicy.sampleCombat({active, now, player.getHealth(), player.getMaxHealth(), attackers});
}

void PlayerBotController::recordHuntRecovery(bool potion)
{
	Player* player = g_game.getPlayerByID(playerId);
	if (!player || !activeHuntRegion || !isActiveHuntCombat(*player)) {
		return;
	}
	if (potion) {
		huntPolicy.observeRecovery(true);
	} else {
		huntPolicy.observeRecovery(false);
	}
}

void PlayerBotController::emitChallengeFrontier(const PlayerBotHuntChallengeUpdate& update, const Position& position,
	                                                const char* reason) const
{
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(3)
	       << "\"result\":" << jsonString(playerBotHuntChallengeResultName(update.result))
	       << ",\"reason\":" << jsonString(reason)
	       << ",\"frontier_before\":" << update.frontierBefore
	       << ",\"frontier_after\":" << update.frontierAfter
	       << ",\"hold_qualifying_hunts\":" << static_cast<uint16_t>(update.qualifyingHuntsToHold)
	       << ",\"active_combat_seconds\":" << update.combat.activeSeconds
	       << ",\"active_combat_uptime\":" << update.activeCombatUptime
	       << ",\"kills\":" << update.combat.kills
	       << ",\"minimum_active_combat_seconds\":" << update.minimumActiveCombatSeconds
	       << ",\"minimum_kills\":" << update.minimumKills
	       << ",\"minimum_health\":" << (update.combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : update.combat.minimumHealth)
	       << ",\"verified_recoveries\":" << update.verifiedRecoveries
	       << ",\"retreat\":" << (std::strcmp(reason, "hunt_region_observed_danger") == 0 ? "true" : "false")
	       << ",\"danger\":" << (update.combat.dangerObserved ? "true" : "false")
	       << ",\"death\":" << (update.combat.deathObserved ? "true" : "false");
	emit("hunt_challenge_frontier", position, fields.str());
}

void PlayerBotController::runAdaptiveChallengeFixture(Player& player, const Position& position)
{
	adaptiveChallengeFixtureRun = true;
	auto applyEvidence = [this, &player, &position](double activeSeconds, uint32_t kills,
	                                             uint32_t potionRecoveries, bool death = false) {
		huntPolicy.resetCombatEvidence();
		huntPolicy.observeCombat({activeSeconds != 0, activeSeconds, player.getMaxHealth(), player.getMaxHealth(), 1});
		for (uint32_t kill = 0; kill < kills; ++kill) {
			huntPolicy.observeKill();
		}
		for (uint32_t recovery = 0; recovery < potionRecoveries; ++recovery) {
			huntPolicy.observeRecovery(true);
		}
		if (death) {
			huntPolicy.observeDeath();
		}
		emitChallengeFrontier(huntPolicy.updateChallengeFrontier({300, player.getMaxHealth()}), position,
		                      "adaptive_challenge_fixture");
	};
	huntPolicy.resetCombatEvidence();
	huntPolicy.observeCombat({false, 30, player.getMaxHealth(), player.getMaxHealth(), 1});
	const double idleObservedSeconds = huntPolicy.combatSummary().activeSeconds;
	huntPolicy.observeCombat({true, 30, player.getMaxHealth(), player.getMaxHealth(), 1});
	const double activeObservedSeconds = huntPolicy.combatSummary().activeSeconds;
	applyEvidence(0, 0, 0); // Travel and idle health must not advance the frontier.
	applyEvidence(30, 0, 0); // Target engagement without a kill is not qualifying evidence.
	applyEvidence(30, 1, 0);
	applyEvidence(30, 1, 0);
	applyEvidence(30, 1, 1);
	applyEvidence(30, 1, 0);
	applyEvidence(30, 1, 0);
	applyEvidence(30, 1, 0);
	applyEvidence(0, 0, 0, true);

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
		planningProfile = playerBotHuntPlanningProfile(player, profile, huntPolicy.challengeFrontier());
		PlayerBotCombatProfile improved = profile;
		improved.armor += 20;
		huntRegionPlanner.score(player, planningProfile, scan.revision, scan.candidateIndices.front(), {}, huntPolicy.regionPerformance(),
		                       60, current);
		const PlayerBotHuntPlanningProfile improvedProfile = playerBotHuntPlanningProfile(player, improved, huntPolicy.challengeFrontier());
		huntRegionPlanner.score(player, improvedProfile, scan.revision, scan.candidateIndices.front(), {}, huntPolicy.regionPerformance(),
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
	huntPolicy.resetCombatEvidence();
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
	const PlayerBotHuntCombatSummary combat = huntPolicy.combatSummary();
	const PlayerBotHuntPerformanceUpdate performance = huntPolicy.observePerformance(activeHuntRegion->center, {
		static_cast<uint64_t>(std::max<int64_t>(0, duration)), combat.kills, experienceGained,
		activeHuntRegion->projectedExperience, activeHuntRegion->observedCorrection,
		static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS))),
	});
	const int32_t p10Health = player.getMaxHealth() * combat.p10HealthPercent / 100;
	const double activeDamagePerMinute = combat.activeSeconds == 0 ? 0 : combat.damageTaken * 60.0 / combat.activeSeconds;
	const bool retreatObserved = std::strcmp(reason, "hunt_region_observed_danger") == 0;
	const PlayerBotHuntChallengeUpdate challenge = huntPolicy.updateChallengeFrontier({
		static_cast<uint64_t>(std::max<int64_t>(0, duration)), player.getMaxHealth(),
	});
	emitChallengeFrontier(challenge, position, reason);
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2);
	fields << "\"region_id\":" << activeHuntRegion->id
	       << ",\"reason\":" << jsonString(reason)
	       << ",\"duration_seconds\":" << duration
	       << ",\"level_before\":" << huntRegionStartLevel
	       << ",\"level_after\":" << player.getLevel()
	       << ",\"experience_gained\":" << experienceGained
	       << ",\"actual_experience_per_minute\":" << performance.actualExperiencePerMinute
	       << ",\"predicted_experience\":" << activeHuntRegion->projectedExperience
	       << ",\"updated_observed_correction\":" << performance.updatedCorrection
	       << ",\"kills\":" << combat.kills
	       << ",\"damage_taken\":" << combat.damageTaken
	       << ",\"active_combat_seconds\":" << combat.activeSeconds
	       << ",\"active_combat_uptime\":" << (duration <= 0 ? 0 : combat.activeSeconds / duration)
	       << ",\"active_combat_damage_per_minute\":" << activeDamagePerMinute
	       << ",\"minimum_health\":" << (combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : combat.minimumHealth)
	       << ",\"p10_health\":" << p10Health
	       << ",\"p10_health_percent\":" << static_cast<uint16_t>(combat.p10HealthPercent)
	       << ",\"verified_potion_recoveries\":" << combat.potionRecoveries
	       << ",\"verified_spell_recoveries\":" << combat.spellRecoveries
	       << ",\"maximum_attacker_overlap\":" << combat.maximumAttackerOverlap
	       << ",\"retreat_observed\":" << (retreatObserved ? "true" : "false")
	       << ",\"danger_observed\":" << (combat.dangerObserved ? "true" : "false")
	       << ",\"death_observed\":" << (combat.deathObserved ? "true" : "false")
	       << ",\"frontier_before\":" << challenge.frontierBefore
	       << ",\"frontier_after\":" << challenge.frontierAfter;
	emit("hunt_region_outcome", position, fields.str());
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) {
		say(*speakingPlayer, "Leaving hunt: " + std::string(reason) + ". " +
		     std::to_string(combat.kills) + " kills, " +
		     std::to_string(player.getExperience() >= huntRegionStartExperience ?
		         player.getExperience() - huntRegionStartExperience : 0) + " experience.");
	}
	activeHuntRegion.reset();
	huntPolicy.resetCombatEvidence();
}

void PlayerBotController::cancelHuntRegionPlanning()
{
	huntRegionPlanning.reset();
}

void PlayerBotController::emitHuntRegionPlanning(const PlayerBotHuntPlanningSession& planning, const Position& position,
                                                 const char* phase) const
{
	const auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - planning.started()).count();
	std::ostringstream fields;
	fields << "\"phase\":" << jsonString(phase)
	       << ",\"cache\":" << jsonString(planning.cacheHit() ? "hit" : "build")
	       << ",\"snapshot_time_us\":" << planning.snapshotTimeUs()
	       << ",\"clustering_time_us\":" << planning.clusteringTimeUs()
		       << ",\"scoring_time_us\":" << planning.scoringTimeUs()
		       << ",\"candidate_count\":" << planning.totalCandidates()
		       << ",\"scored_candidate_count\":" << planning.scoredCandidates()
		       << ",\"suitable_candidate_count\":" << planning.suitableCandidates()
		       << ",\"pathfinding_calls\":" << planning.pathfindingCalls()
		       << ",\"batch_pathfinding_calls\":" << planning.batchPathfindingCalls()
	       << ",\"expanded_nodes\":" << planning.expandedNodes()
	       << ",\"yields\":" << planning.yields()
	       << ",\"challenge_frontier\":" << planning.profile().challengeFrontier
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
	const uint64_t cacheRevision = huntRegionPlanning ? PlayerBotHuntRegionPlanner::getCacheRevision() : 0;
	const PlayerBotHuntPlanningSnapshot currentSnapshot{
		player.getPosition(), player.getLevel(), player.getHealth(), player.getStaminaMinutes(), cacheRevision, excludedRegions,
	};
	const bool staleRevision = huntRegionPlanning && huntRegionPlanning->snapshot().cacheRevision != cacheRevision;
	if (huntRegionPlanning && huntRegionPlanning->invalidated(currentSnapshot)) {
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
		const uint64_t scanRevision = scan.revision;
		const Item* weapon = player.getWeapon(true);
		const PlayerBotCombatProfile combat{
			player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(), weapon ? weapon->getAttack() : 7,
			weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor(),
		};
		huntRegionPlanning.emplace(PlayerBotHuntPlanningStart{
			std::move(scan), playerBotHuntPlanningProfile(player, combat, huntPolicy.challengeFrontier()),
			{player.getPosition(), player.getLevel(), player.getHealth(), player.getStaminaMinutes(),
			 scanRevision, std::move(excludedRegions)},
			reason, now,
		});
		huntPlanningFixtureForcedUnreachable = false;
		huntPlanningFixtureForcedNodeLimit = false;
		emitHuntRegionPlanning(*huntRegionPlanning, position, "scoring_started");
		return false;
	}

	PlayerBotHuntPlanningSession& planning = *huntRegionPlanning;
	planning.beginTurn();
	if (planning.scoring()) {
		const auto scoringStarted = std::chrono::steady_clock::now();
		while (const auto work = planning.nextScoringWork(huntRegionScoringCandidatesPerTurn)) {
			PlayerBotHuntRegion region;
			if (!huntRegionPlanner.score(player, planning.profile(), planning.snapshot().cacheRevision, work->candidateIndex,
			                             planning.snapshot().excludedRegions,
			                             huntPolicy.regionPerformance(), huntDurationSeconds, region)) {
				emitHuntRegionPlanning(planning, position, "stale_revision");
				cancelHuntRegionPlanning();
				return false;
			}
			planning.scoreCompleted(std::move(region));
		}
		planning.addScoringTime(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - scoringStarted).count());
		if (testPolicy.cancelHuntPlanningAtScoreBarrier && !huntPlanningFixtureCancelled && planning.scoredCandidates() != 0) {
			huntPlanningFixtureCancelled = true;
			emitHuntRegionPlanning(planning, position, "cancelled");
			cancelHuntRegionPlanning();
			return false;
		}
		if (testPolicy.cancelHuntPlanningAtScoreBarrier && !huntPlanningFixtureStaleRevisionTriggered && planning.scoredCandidates() != 0) {
			huntPlanningFixtureStaleRevisionTriggered = true;
			PlayerBotHuntRegionPlanner::invalidateCache();
			return false;
		}
		if (planning.completeScoring() == PlayerBotHuntPlanningProgress::ScoringYield) {
			emitHuntRegionPlanning(planning, position, "scoring_yield");
			return false;
		}
		if (testPolicy.forceHuntScopeExhaustion) {
			for (PlayerBotHuntRegion& region : planning.regions()) {
				region.suitable = false;
				region.inChallengeBand = false;
				region.rejectionReason = "fixture_scope_exhausted";
			}
			planning.refreshSuitableCandidates();
		}
		emitHuntRegionPlanning(planning, position, "scored");
		return false;
	}

	if (const auto work = planning.nextRouteValidationWork(huntRegionPathfindingCallsPerTurn)) {
		PlayerBotHuntRegion& candidate = planning.regions()[work->regionIndex];
		const std::set<Position> blockedPositions = navigationSession.activeBlockedPositions(now);
		std::deque<PlayerBotNavigationStep> route;
		const bool forcedUnreachable = testPolicy.forceFirstHuntCandidateUnreachable && !huntPlanningFixtureForcedUnreachable;
		const bool forcedNodeLimit = testPolicy.forceSecondHuntCandidateNodeLimit && !huntPlanningFixtureForcedNodeLimit &&
		                             huntPlanningFixtureForcedUnreachable && !forcedUnreachable;
		huntPlanningFixtureForcedUnreachable = huntPlanningFixtureForcedUnreachable || forcedUnreachable;
		huntPlanningFixtureForcedNodeLimit = huntPlanningFixtureForcedNodeLimit || forcedNodeLimit;
		PlayerBotNavigationResult planResult = PlayerBotNavigationResult::Unreachable;
		uint64_t expandedNodes = 0;
		if (!forcedUnreachable) {
			const auto pathStarted = std::chrono::steady_clock::now();
			++counters.pathfindingCalls;
			planResult = navigator.plan(player, candidate.destination, blockedPositions, route, expandedNodes,
			                            forcedNodeLimit ? 0 : playerBotNavigationMaximumExpandedNodes);
			counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - pathStarted).count();
		}
		if (planResult != PlayerBotNavigationResult::Reached) {
			++counters.pathfindingFailures;
		}
		double estimatedTravelSeconds = 0;
		for (const PlayerBotNavigationStep& step : route) {
			estimatedTravelSeconds += step.action == PlayerBotNavigationAction::Move ? player.getStepDuration(step.direction) / 1000.0 : 1.0;
		}
		const double availableHuntSeconds = std::max(0.0, huntDurationSeconds - estimatedTravelSeconds);
		planning.routeValidationCompleted(work->regionIndex, !forcedUnreachable, planResult == PlayerBotNavigationResult::Reached,
		                                 planResult == PlayerBotNavigationResult::NodeLimit, expandedNodes,
		                                 static_cast<uint32_t>(route.size()), estimatedTravelSeconds,
		                                 projectedHuntStaminaMultiplier(player, availableHuntSeconds), huntDurationSeconds);
		planning.completeRouteValidation();
		emitHuntRegionPlanning(planning, position, "reachability_yield");
		return false;
	}

	planning.completeRouteValidation();
	auto selected = std::max_element(planning.regions().begin(), planning.regions().end(),
	                                 [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
		                                 return playerBotPreferHuntRegion(right, left);
	                                 });
	for (const PlayerBotHuntRegion& region : planning.regions()) {
		emitHuntRegionCandidate(region, position);
	}
	if (playerBotHuntScopeExhausted(planning.regions())) {
		++consecutiveHuntScopeExhaustions;
		const std::chrono::seconds retryDelay = testPolicy.forceHuntScopeExhaustion ? std::chrono::seconds(1) :
		                                                                                huntScopeReevaluationDelay;
		const bool validationBudgetExhausted = std::any_of(planning.regions().begin(), planning.regions().end(),
			[](const PlayerBotHuntRegion& region) { return region.rejectionReason == "navigation_node_budget"; });
		emitHuntRegionPlanning(planning, position, "exhausted");
		emit("hunt_region_selection", position,
		     "\"result\":\"failed\",\"reason\":" +
		         jsonString(validationBudgetExhausted ? "route_validation_budget_exhausted" : "no_suitable_reachable_region"));
		uint32_t reachableCandidates = 0;
		for (const PlayerBotHuntRegion& region : planning.regions()) {
			reachableCandidates += region.reachable ? 1 : 0;
		}
		emit("hunt_scope_exhausted", position,
		     "\"candidate_count\":" + std::to_string(planning.totalCandidates()) +
		         ",\"scored_candidate_count\":" + std::to_string(planning.scoredCandidates()) +
		         ",\"suitable_candidate_count\":" + std::to_string(planning.suitableCandidates()) +
		         ",\"reachable_candidate_count\":" + std::to_string(reachableCandidates) +
		         ",\"frontier\":" + std::to_string(huntPolicy.challengeFrontier()) +
		         ",\"attempt\":" + std::to_string(consecutiveHuntScopeExhaustions) +
		         ",\"maximum_attempts\":" + std::to_string(maximumHuntScopeExhaustions) +
		         ",\"retry_delay_ms\":" + std::to_string(retryDelay.count() * 1000) +
		         ",\"reason\":" + jsonString(validationBudgetExhausted ? "route_validation_budget_exhausted" : "local_scope_exhausted"));
		cancelHuntRegionPlanning();
		if (consecutiveHuntScopeExhaustions >= maximumHuntScopeExhaustions) {
			stop("hunt_scope_exhausted", position);
			return false;
		}
		huntScopeReevaluationAfter = now + retryDelay;
		return false;
	}

	activeHuntRegion = *selected;
	consecutiveHuntScopeExhaustions = 0;
	huntScopeReevaluationAfter = {};
	auto first = std::find(activeHuntRegion->patrolPoints.begin(), activeHuntRegion->patrolPoints.end(),
	                       activeHuntRegion->destination);
	if (first != activeHuntRegion->patrolPoints.end()) {
		std::rotate(activeHuntRegion->patrolPoints.begin(), first, activeHuntRegion->patrolPoints.end());
	}
	huntRouteIndex = 0;
	huntRegionStarted = now;
	huntRegionStartLevel = player.getLevel();
	huntRegionStartExperience = player.getExperience();
	huntPolicy.resetCombatEvidence();
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
	goalArbiter.setActiveGoal(TopLevelGoal::Hunt);
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
		if (targetingSession.defensiveTarget()) {
			processDefensiveCombat(player, currentPosition);
			return;
		}
		if (attackDefensiveThreat(player, currentPosition)) {
			schedule(navigationInterval);
			return;
		}
	}
	if (targetingSession.defensiveTarget()) {
		if (scenarioStage == ScenarioStage::LootCorpse && corpseNavigationSuspended &&
		    std::chrono::steady_clock::now() - corpseLootStarted >= corpseLootTimeout) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
		}
		processDefensiveCombat(player, currentPosition);
		return;
	}
	if (progressionObjective != ProgressionObjective::None) {
		processProgression(player, currentPosition);
		return;
	}
	if (goalArbiter.activeGoal() == TopLevelGoal::MagicTraining) {
		processMagicTraining(*player, currentPosition);
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
	if (scenarioStage == ScenarioStage::LootCorpse && corpseNavigationSuspended) {
		if (std::chrono::steady_clock::now() - corpseLootStarted >= corpseLootTimeout) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
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
	if (cyclePhase == CyclePhase::Hunt) {
		const uint32_t usableCapacity = inventoryPolicy.effectiveFreeCapacity(*player);
		if (std::chrono::steady_clock::now() >= huntDeadline || usableCapacity < returnCapacityThreshold) {
			const char* reason = usableCapacity < returnCapacityThreshold ? "capacity" : "hunt_deadline";
			if (testPolicy.progressionEnabled) {
				finishHuntAndSelectGoal(player, currentPosition, reason);
			} else {
				beginService(player, currentPosition, reason);
				schedule(navigationInterval);
			}
			return;
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
		if (!processNavigation(player, currentPosition, depotSession.approachPosition())) {
			if (fixedTargetRouteFailureCount != 0) {
				depotSession.rejectApproach(depotSession.approachPosition(), std::chrono::steady_clock::now() + depotApproachSuppression);
				clearDepotDiscovery();
				clearNavigation();
			}
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
		if (!Position::areInRange<1, 1, 0>(currentPosition, depotSession.lockerPosition())) {
			setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "displaced_during_deposit");
			clearNavigation();
			if (!processNavigation(player, currentPosition, depotSession.approachPosition())) {
				if (fixedTargetRouteFailureCount != 0) {
					depotSession.rejectApproach(depotSession.approachPosition(), std::chrono::steady_clock::now() + depotApproachSuppression);
					clearDepotDiscovery();
					clearNavigation();
				}
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
	const auto now = std::chrono::steady_clock::now();
	if ((patrolRouteFailureCount != 0 || navigationSession.stepFailureCount() != 0 ||
	     navigationSession.hasActiveRouteBlock(now)) &&
	    attackDefensiveThreat(player, currentPosition)) {
		resetPatrolRouteFailures();
		schedule(navigationInterval);
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
	if (patrolRouteFailureTarget != target) {
		resetPatrolRouteFailures();
		patrolRouteFailureTarget = target;
	}
	if (!processNavigation(player, currentPosition, target)) {
		const bool forcedStepRecoveryPending = testPolicy.forceRepeatedNavigationStepFailures &&
		                                       forcedNavigationStepFailuresRemaining != 0;
		if (lastNavigationRouteUnavailable && forcedStepRecoveryPending) {
			navigationSession.clearBlockedPositions();
		}
		if (lastNavigationRouteUnavailable && !forcedStepRecoveryPending) {
			if (patrolRouteFailureCount == 0) {
				patrolRouteFailureStarted = std::chrono::steady_clock::now();
			}
			++patrolRouteFailureCount;
			patrolRouteFailureExpandedNodes += lastNavigationExpandedNodes;
		}
		const bool repeatedStepFailure = navigationSession.stepFailureCount() >= maximumRepeatedNavigationStepFailures;
		const bool repeatedRouteFailure = patrolRouteFailureCount >= maximumPatrolRouteFailures;
		if (navigationSession.oscillationDetected() || repeatedStepFailure || repeatedRouteFailure) {
			const char* reason = navigationSession.oscillationDetected() ? "position_oscillation" :
			                     repeatedStepFailure ? "repeated_step_failure" : "route_unavailable";
			const auto routeFailureElapsed = patrolRouteFailureStarted == std::chrono::steady_clock::time_point{} ? 0 :
			                                 std::chrono::duration_cast<std::chrono::milliseconds>(
			                                     std::chrono::steady_clock::now() - patrolRouteFailureStarted).count();
			emit("hunt_region_patrol", currentPosition,
			     "\"result\":\"skipped\",\"reason\":" +
			         jsonString(reason) +
			         ",\"step_failures\":" + std::to_string(navigationSession.stepFailureCount()) +
			         ",\"route_failures\":" + std::to_string(patrolRouteFailureCount) +
			         ",\"elapsed_ms\":" + std::to_string(routeFailureElapsed) +
			         ",\"expanded_nodes\":" + std::to_string(patrolRouteFailureExpandedNodes) +
			         ",\"region_id\":" +
			         (activeHuntRegion ? std::to_string(activeHuntRegion->id) : "null") +
			         ",\"destination\":{\"x\":" +
			         std::to_string(target.x) + ",\"y\":" + std::to_string(target.y) +
			         ",\"z\":" + std::to_string(target.z) + "}");
			if (repeatedStepFailure || repeatedRouteFailure) {
				++counters.stuckEvents;
			}
			resetPatrolRouteFailures();
			navigationSession.clearBlockedPositions();
			navigationSession.resetStepFailures();
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
	resetPatrolRouteFailures();
	huntRouteIndex = patrolPoints && !patrolPoints->empty() ?
	                 (huntRouteIndex + 1) % patrolPoints->size() : (huntRouteIndex + 1) % huntingLoop.size();
	std::ostringstream fields;
	fields << "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" << huntRouteIndex
	       << ",\"region_id\":" << (activeHuntRegion ? std::to_string(activeHuntRegion->id) : "null");
	emit("action_result", currentPosition, fields.str());
	schedule(SCHEDULER_MINTICKS);
}
