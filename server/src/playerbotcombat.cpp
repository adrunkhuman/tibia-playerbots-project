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
#include "spells.h"

// Playerbot survival, combat targeting, and hunt orchestration.
using namespace playerbot;

extern Spells* g_spells;

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

	PlayerBotHuntRuntimePlayerObservation huntPlayerObservation(const Player& player)
	{
		return {player.getPosition(), player.getLevel(), player.getHealth(), player.getMaxHealth(),
		        player.getStaminaMinutes(), player.getExperience()};
	}

	PlayerBotHuntPlanningProfile huntPlanningFacts(Player& player, const PlayerBotCombatProfile& combat)
	{
		return playerBotHuntPlanningProfile(player, combat, 0);
	}

	PlayerBotCombatProfile huntCombatProfile(Player& player)
	{
		const Item* weapon = player.getWeapon(true);
		return {player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(),
		        weapon ? weapon->getAttack() : 7,
		        weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor()};
	}
}

std::set<Position> PlayerBotController::activeHuntCooldowns(std::chrono::steady_clock::time_point now)
{
	std::set<Position> excluded;
	for (auto it = huntRegionCooldowns.begin(); it != huntRegionCooldowns.end();) {
		if (now >= it->second) it = huntRegionCooldowns.erase(it);
		else { excluded.insert(it->first); ++it; }
	}
	return excluded;
}

void PlayerBotController::applyHuntCooldown(const std::optional<PlayerBotHuntRuntimeCooldownCommand>& cooldown,
	std::chrono::steady_clock::time_point now)
{
	if (cooldown) huntRegionCooldowns[cooldown->region] = now + cooldown->duration;
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

PlayerBotSurvivalSnapshot PlayerBotController::survivalSnapshot(const Player& player, const Creature* target) const
{
	PlayerBotSurvivalSnapshot snapshot;
	snapshot.health = player.getHealth();
	snapshot.healthMaximum = player.getMaxHealth();
	snapshot.mana = player.getMana();
	snapshot.manaMaximum = player.getMaxMana();
	snapshot.manaSpent = player.getSpentMana();
	snapshot.level = player.getLevel();
	snapshot.magicLevel = player.getBaseMagicLevel();
	snapshot.potionCount = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	snapshot.foodInventoryCount = inventoryPolicy.foodInventory(player).count;
	if (const uint16_t pendingFoodId = survivalRuntime.pendingFoodItemId()) {
		snapshot.pendingFoodCount = inventoryPolicy.inventoryItemCount(player, pendingFoodId);
	}
	if (Condition* condition = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT, 0)) {
		snapshot.foodTicks = condition->getTicks();
	}
	std::function<Item*(Item*)> findFood = [&](Item* item) -> Item* {
		if (!item) return nullptr;
		if (PlayerBotInventoryPolicy::isFoodItem(item->getID())) return item;
		if (Container* container = item->getContainer()) {
			for (Item* child : container->getItemList()) {
				if (Item* food = findFood(child)) return food;
			}
		}
		return nullptr;
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST && snapshot.foodClientId == 0; ++slot) {
		if (Item* food = findFood(player.getInventoryItem(static_cast<slots_t>(slot)))) {
			snapshot.foodItemId = food->getID();
			snapshot.foodClientId = food->getClientID();
			snapshot.foodCount = inventoryPolicy.inventoryItemCount(player, food->getID());
		}
	}
	snapshot.canDoAction = player.canDoAction();
	snapshot.buyingPotions = serviceWorkflow.stage() == PlayerBotServiceStage::BuyPotions;
	snapshot.lootMovePending = lootWorkflow.hasPendingLootMove();
	snapshot.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	snapshot.progressionDeparture = progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture);
	snapshot.hunting = turnRouter.cyclePhase() == CyclePhase::Hunt;
	snapshot.combatOrPursuit = turnRouter.scenarioStage() != ScenarioStage::Traverse || combatRuntime.hasActiveCombat() ||
	                          const_cast<Player&>(player).getAttackedCreature() != nullptr;
	snapshot.navigationPending = navigationRuntime.hasPendingWork();
	snapshot.healingExhausted = player.hasCondition(CONDITION_EXHAUST_HEAL);
	snapshot.combatExhausted = player.hasCondition(CONDITION_EXHAUST_COMBAT);
	snapshot.hasteActive = player.hasCondition(CONDITION_HASTE);
	if (snapshot.hasteActive) snapshot.hasteTicks = player.getCondition(CONDITION_HASTE)->getTicks();
	snapshot.lightActive = player.hasCondition(CONDITION_LIGHT);
	snapshot.regenerationActive = player.hasCondition(CONDITION_REGENERATION);
	snapshot.protectionZone = player.getZone() == ZONE_PROTECTION;
	if (const auto forecast = player.getManaRegenerationForecast()) {
		snapshot.regenerationForecastActive = true;
		snapshot.regenerationManaGain = forecast->gain;
		snapshot.regenerationTickInterval = forecast->interval;
		snapshot.regenerationTickRemaining = forecast->remaining;
	}
	snapshot.routeSteps = navigationRuntime.routeSize();
	if (target) {
		snapshot.target.id = target->getID();
		snapshot.target.health = target->getHealth();
		snapshot.target.targetClass = target->getMonster() ? "monster:" + target->getName() : "creature";
		snapshot.target.targetClass.resize(std::min<size_t>(snapshot.target.targetClass.size(), 56));
		snapshot.target.valid = !target->isRemoved() && !target->isDead() &&
		                        const_cast<Player&>(player).getAttackedCreature() == target && player.canSeeCreature(target) &&
		                        player.canSee(target->getPosition()) &&
		                        Position::areInRange<1, 1, 0>(player.getPosition(), target->getPosition());
	}
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		PlayerBotSurvivalSpellObservation spell;
		spell.name = descriptor.name;
		InstantSpell* engineSpell = g_spells ? g_spells->getInstantSpellByName(descriptor.name) : nullptr;
		spell.metadataMatches = engineSpell && engineSpell->getWords() == descriptor.words && engineSpell->isLearnable();
		spell.learned = player.hasLearnedInstantSpell(descriptor.name);
		if (engineSpell) {
			spell.targetReachable = !engineSpell->getNeedTarget() ||
			                        (target && !target->isRemoved() && engineSpell->canThrowSpell(&player, target));
			spell.manaCost = engineSpell->getManaCost(&player);
			spell.envelope = playerBotSpellEnvelope(player, descriptor);
			spell.magicTrainingEligible = descriptor.magicTrainingSafe && descriptor.magicTrainingPriority != 0 &&
			                              descriptor.magicTrainingEffect != PlayerBotTrainingEffect::None && spell.learned &&
			                              spell.metadataMatches && engineSpell->isEnabled() && player.getLevel() >= engineSpell->getLevel() &&
			                              player.getMagicLevel() >= engineSpell->getMagicLevel() && player.getSoul() >= engineSpell->getSoulCost() &&
			                              (!engineSpell->isPremium() || player.isPremium()) &&
			                              (!engineSpell->getNeedWeapon() || player.getWeapon(true)) && !snapshot.healingExhausted &&
			                              !engineSpell->getAggressive() && engineSpell->getSelfTarget() && !engineSpell->getNeedTarget() &&
			                              !engineSpell->getHasParam() && !engineSpell->getHasPlayerNameParam() &&
			                              !engineSpell->getNeedDirection() && !engineSpell->getNeedCasterTargetOrDirection();
		}
		snapshot.spells.push_back(std::move(spell));
	}
	return snapshot;
}

void PlayerBotController::logHealResult(const char* result, const char* reason, const PlayerBotPotionAttempt& before,
					const PlayerBotPotionAttempt& after, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"heal\",\"result\":" << jsonString(result)
	       << ",\"method\":\"small_health_potion\",\"item_id\":" << smallHealthPotionItemId
	       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(turnRouter.stateName())
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
	const PlayerBotSurvivalCommand command = survivalRuntime.decideHealing(survivalSnapshot(*player), now);
	if (command.potionVerification) {
		const auto& verification = *command.potionVerification;
		if (verification.result == PlayerBotPotionVerificationResult::Success) {
			logHealResult("success", nullptr, verification.before, verification.after, currentPosition);
			recordHuntRecovery(true);
		} else {
			telemetry.recordActionFailure();
			logHealResult("failed", verification.result == PlayerBotPotionVerificationResult::IneffectiveRecovery ?
			              "ineffective_recovery" : "use_not_verified", verification.before, verification.after, currentPosition);
		}
	}
	if (!command.reason.empty() && !command.candidateName.empty()) {
		dispatchSpellCommand(*player, currentPosition, command);
	}
	if (command.type == PlayerBotSurvivalCommandType::None) return false;
	if (command.type == PlayerBotSurvivalCommandType::CastSpell) return dispatchSpellCommand(*player, currentPosition, command);
	if (command.type == PlayerBotSurvivalCommandType::Wait) return true;
	if (command.type == PlayerBotSurvivalCommandType::InterruptForService) {
		if (shouldEmitRepeated("heal:missing_supply")) {
			std::ostringstream fields;
			fields << "\"action\":\"heal\",\"result\":\"skipped\",\"reason\":\"missing_supply\""
			       << ",\"method\":\"small_health_potion\",\"item_id\":" << smallHealthPotionItemId
			       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
			       << ",\"state\":" << jsonString(turnRouter.stateName())
			       << ",\"health_before\":" << player->getHealth()
			       << ",\"health_after\":" << player->getHealth()
			       << ",\"health_max\":" << player->getMaxHealth()
			       << ",\"resource_before\":0,\"resource_after\":0";
			emit("action_result", currentPosition, fields.str());
		}
		if (progressionRuntime.session().active() != PlayerBotProgressionProcedure::None) {
			if (progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture)) {
				finishOracleDeparture(player, currentPosition, "interrupted", "healing_supply_missing");
			} else {
				finishProgressionObjective(player, currentPosition, "interrupted", "healing_supply_missing", false);
			}
			return true;
		}
		if (turnRouter.cyclePhase() != CyclePhase::Service) {
			beginService(player, currentPosition, "healing_supply_missing");
		}
		return false;
	}
	huntRuntime.cancelPlanning();
	Item* potion = g_game.findItemOfType(player, command.itemId, true);
	if (!potion) {
		return true;
	}

	survivalRuntime.beginPotion(survivalSnapshot(*player));
	telemetry.recordActionAttempt();
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
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotSurvivalCommand command = survivalRuntime.decideFood(survivalSnapshot(*player), now);
	if (command.foodVerification) {
		const auto& verification = *command.foodVerification;
		if (verification.result == PlayerBotFoodVerificationResult::Success) {
			logEatSuccess(verification.before.itemId, verification.inventoryCount, verification.foodTicks, currentPosition);
		} else if (verification.result == PlayerBotFoodVerificationResult::Failed ||
		           verification.result == PlayerBotFoodVerificationResult::Cooldown) {
			logActionFailure("eat", "consumption_not_verified", currentPosition);
			if (verification.result == PlayerBotFoodVerificationResult::Cooldown) {
				emit("action_result", currentPosition,
				     "\"action\":\"eat\",\"result\":\"cooldown\",\"reason\":\"retry_exhausted\",\"retry_after_ms\":" +
				         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(eatFailureCooldown).count()));
			}
		}
	}
	if (command.type == PlayerBotSurvivalCommandType::None) return false;
	if (command.type == PlayerBotSurvivalCommandType::Wait) return true;
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, Position(0xFFFF, 0, 0), 0, 0, command.itemClientId);
	return true;
}

bool PlayerBotController::attackVisibleMonster(Player* player, const Position& currentPosition)
{
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	std::vector<PlayerBotTraversalCandidate> candidates;
	for (Creature* creature : spectators) {
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || !player->canSee(creature->getPosition())) {
			continue;
		}
		if (huntRuntime.active() && creature->getAttackedCreature() != player && !huntRuntime.matchesMonster(creature->getName())) {
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
		candidates.push_back({{creature->getID(), creature->getPosition(), creature->getName()}, expectedCorpseFor(*creature)});
	}
	while (!candidates.empty()) {
		const auto command = combatRuntime.selectTraversalAttack(candidates, currentPosition, std::chrono::steady_clock::now());
		if (!command) {
			return false;
		}
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [command](const PlayerBotTraversalCandidate& candidate) {
			return candidate.id == command->target.id;
		}), candidates.end());
		Creature* target = g_game.getCreatureByID(command->target.id);
		if (!target) {
			continue;
		}
		telemetry.recordActionAttempt();
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		const PlayerBotCombatDecision started = combatRuntime.confirmAttack(*command, player->getAttackedCreature() == target,
		                                                                  std::chrono::steady_clock::now());
		if (!started.result || std::strcmp(started.result, "started") != 0) {
			continue;
		}
		resetNavigation();
		setStage(ScenarioStage::TraversalCombat, currentPosition);
		std::ostringstream fields;
		fields << "\"previous_target_id\":null,\"target_id\":" << started.target.id
		       << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(started.target.name)
		       << ",\"target_position\":{\"x\":" << started.target.position.x << ",\"y\":" << started.target.position.y
		       << ",\"z\":" << static_cast<uint16_t>(started.target.position.z) << "},\"reason\":\"visible_monster\"";
		emit("target_changed", currentPosition, fields.str());
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
		return navigationRuntime.isRouteCritical(creature->getPosition(), now);
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
	const auto command = combatRuntime.selectDefensiveAttack(std::move(candidates), currentPosition);
	if (!command) {
		return false;
	}
	Creature* target = g_game.getCreatureByID(command->target.id);
	if (!target) {
		return false;
	}
	telemetry.recordActionAttempt();
	g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
	g_game.playerSetAttackedCreature(playerId, target->getID());
	const PlayerBotCombatDecision started = combatRuntime.confirmAttack(*command, player->getAttackedCreature() == target,
	                                                                  std::chrono::steady_clock::now());
	if (!started.result || std::strcmp(started.result, "started") != 0) {
		logActionFailure("defensive_combat", "target_rejected", currentPosition);
		return false;
	}
	resetNavigation();
	std::ostringstream targetFields;
	targetFields << "\"previous_target_id\":null,\"target_id\":" << started.target.id
	             << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(started.target.name)
	             << ",\"target_position\":{\"x\":" << started.target.position.x
	             << ",\"y\":" << started.target.position.y << ",\"z\":"
	             << static_cast<uint16_t>(started.target.position.z) << "},\"reason\":"
	             << jsonString(started.routeCritical ? "defensive_path_blocker" : "defensive_attacker")
	             << ",\"route_critical\":" << (started.routeCritical ? "true" : "false");
	emit("target_changed", currentPosition, targetFields.str());
	emit("action_result", currentPosition,
	     "\"action\":\"defensive_combat\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(started.target.id) + ",\"chase\":false,\"route_critical\":" +
	         (started.routeCritical ? "true" : "false"));
	return true;
}

void PlayerBotController::finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason)
{
	const uint32_t previousTarget = combatRuntime.defensiveTarget() ? combatRuntime.defensiveTarget()->id : 0;
	combatRuntime.clearDefensiveTarget();
	if (player->getAttackedCreature() && player->getAttackedCreature()->getID() == previousTarget) {
		g_game.playerSetAttackedCreature(playerId, 0);
	}
	resetNavigation();
	emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTarget) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	emit("action_result", currentPosition, "\"action\":\"defensive_combat\",\"result\":" +
	     jsonString(result) + ",\"target_id\":" + std::to_string(previousTarget) +
	     ",\"reason\":" + jsonString(reason));
}

void PlayerBotController::processDefensiveCombat(Player* player, const Position& currentPosition)
{
	const auto defensive = combatRuntime.defensiveTarget();
	Creature* target = defensive ? g_game.getCreatureByID(defensive->id) : nullptr;
	PlayerBotCombatTargetSnapshot observed;
	if (target) observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target),
	                        Position::areInRange<1, 1, 0>(currentPosition, target->getPosition()), target->getAttackedCreature() == player,
	                        player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
	const PlayerBotCombatDecision decision = combatRuntime.advance({currentPosition, std::chrono::steady_clock::now(), {}, observed});
	if (decision.command == PlayerBotCombatCommand::CompleteDefensiveCombat) finishDefensiveCombat(player, currentPosition, decision.result, decision.reason);
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
	resetNavigation();
	const auto target = combatRuntime.traversalTarget();
	if (!target) {
		return;
	}
	const Position destination = nearestTargetApproach(*player, currentPosition, target->position);
	const PlayerBotCombatDecision decision = combatRuntime.beginPursuit(currentPosition, destination, std::chrono::steady_clock::now());
	if (decision.command != PlayerBotCombatCommand::PursueDestination) return;
	setStage(ScenarioStage::TargetPursuit, currentPosition);
	emit("action_result", currentPosition,
	     "\"action\":\"target_pursuit\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(decision.target.id) + ",\"last_seen_position\":{\"x\":" + std::to_string(decision.target.position.x) +
	         ",\"y\":" + std::to_string(target->position.y) + ",\"z\":" + std::to_string(target->position.z) + '}');
}

void PlayerBotController::finishTargetPursuit(const Position& currentPosition, const char* reason)
{
	const PlayerBotCombatDecision decision = combatRuntime.abandonPursuit(std::chrono::steady_clock::now());
	const uint32_t previousTargetId = decision.target.id;
	resetNavigation();
	if (previousTargetId != 0 && shouldEmitRepeated(std::string("target:clear:") + reason)) {
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
	const auto traversal = combatRuntime.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	PlayerBotCombatTargetSnapshot observed;
	std::optional<Position> approach;
	if (target) {
		observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target), false,
		            false, player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
		approach = nearestTargetApproach(*player, currentPosition, target->getPosition());
	}
	const PlayerBotCombatDecision decision = combatRuntime.advance({currentPosition, std::chrono::steady_clock::now(), observed, {}, approach});
	if (decision.command == PlayerBotCombatCommand::Abandon) {
		finishTargetPursuit(currentPosition, decision.reason);
		schedule(navigationInterval);
		return;
	}
	if (decision.command == PlayerBotCombatCommand::AttackTraversal && target) {
		telemetry.recordActionAttempt();
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, true, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		const PlayerBotCombatDecision started = combatRuntime.confirmAttack(decision, player->getAttackedCreature() == target,
		                                                                  std::chrono::steady_clock::now());
		if (started.result && std::strcmp(started.result, "started") == 0) {
			resetNavigation();
			setStage(ScenarioStage::TraversalCombat, currentPosition);
			emit("action_result", currentPosition,
			     "\"action\":\"target_pursuit\",\"result\":\"reacquired\",\"target_id\":" +
			         std::to_string(target->getID()));
			schedule(navigationInterval);
			return;
		}
		if (started.command == PlayerBotCombatCommand::PursueDestination) {
			processNavigation(player, currentPosition, started.destination);
			return;
		}
	}

	if (decision.command == PlayerBotCombatCommand::PursueDestination &&
	    (currentPosition == decision.destination || processNavigation(player, currentPosition, decision.destination))) {
		finishTargetPursuit(currentPosition, "last_seen_position_reached");
		schedule(navigationInterval);
	}
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
{
	const auto traversal = combatRuntime.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	PlayerBotCombatTargetSnapshot observed;
	if (target) observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target), false,
	                        false, player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
	const PlayerBotCombatDecision decision = combatRuntime.advance({currentPosition, std::chrono::steady_clock::now(), observed, {}});
	if (decision.command == PlayerBotCombatCommand::BeginLoot) {
		beginLoot(player, currentPosition, decision);
	} else if (decision.command == PlayerBotCombatCommand::BeginPursuit) {
		beginTargetPursuit(player, currentPosition);
	} else if (decision.command == PlayerBotCombatCommand::Abandon) {
		logActionFailure("attack", "combat_timeout", currentPosition);
		finishTraversalCombat(player, currentPosition, "combat_timeout");
	} else if (target) {
		if (tryOffensiveSpell(player, currentPosition)) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
	}
	schedule(navigationInterval);
}

bool PlayerBotController::isActiveHuntCombat(const Player& player) const
{
	return huntRuntime.active() && turnRouter.cyclePhase() == CyclePhase::Hunt &&
	       turnRouter.scenarioStage() == ScenarioStage::TraversalCombat &&
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
	huntRuntime.sampleCombat({active, now, player.getHealth(), player.getMaxHealth(), attackers});
}

void PlayerBotController::recordHuntRecovery(bool potion)
{
	Player* player = g_game.getPlayerByID(playerId);
	if (!player || !huntRuntime.active() || !isActiveHuntCombat(*player)) {
		return;
	}
	if (potion) {
		huntRuntime.observeRecovery(true);
	} else {
		huntRuntime.observeRecovery(false);
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


void PlayerBotController::emitHuntRegionPlanning(const PlayerBotHuntPlanningSession& planning, const Position& position,
                                                  const char* phase) const
{
	const auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - planning.started()).count();
	std::ostringstream fields;
	fields << "\"phase\":" << jsonString(phase) << ",\"cache\":" << jsonString(planning.cacheHit() ? "hit" : "build")
	       << ",\"snapshot_time_us\":" << planning.snapshotTimeUs() << ",\"clustering_time_us\":" << planning.clusteringTimeUs()
	       << ",\"scoring_time_us\":" << planning.scoringTimeUs() << ",\"candidate_count\":" << planning.totalCandidates()
	       << ",\"scored_candidate_count\":" << planning.scoredCandidates() << ",\"suitable_candidate_count\":" << planning.suitableCandidates()
	       << ",\"pathfinding_calls\":" << planning.pathfindingCalls() << ",\"batch_pathfinding_calls\":" << planning.batchPathfindingCalls()
	       << ",\"expanded_nodes\":" << planning.expandedNodes() << ",\"yields\":" << planning.yields()
	       << ",\"challenge_frontier\":" << planning.profile().challengeFrontier << ",\"decision_latency_us\":" << latencyUs;
	emit("hunt_region_scan", position, fields.str());
}

void PlayerBotController::finishHuntRegion(const Player& player, const Position& position, const char* reason)
{
	Player& mutablePlayer = const_cast<Player&>(player);
	const auto completion = huntRuntime.complete(huntPlayerObservation(mutablePlayer), std::chrono::steady_clock::now(),
		static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS))));
	if (!completion) return;
	const auto& combat = completion->combat;
	const int32_t p10Health = player.getMaxHealth() * combat.p10HealthPercent / 100;
	const double activeDamagePerMinute = combat.activeSeconds == 0 ? 0 : combat.damageTaken * 60.0 / combat.activeSeconds;
	emitChallengeFrontier(completion->challenge, position, reason);
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2) << "\"region_id\":" << completion->region.id
	       << ",\"reason\":" << jsonString(reason) << ",\"duration_seconds\":" << completion->durationSeconds
	       << ",\"level_before\":" << completion->levelBefore << ",\"level_after\":" << player.getLevel()
	       << ",\"experience_gained\":" << completion->experienceGained
	       << ",\"actual_experience_per_minute\":" << completion->performance.actualExperiencePerMinute
	       << ",\"predicted_experience\":" << completion->region.projectedExperience
	       << ",\"updated_observed_correction\":" << completion->performance.updatedCorrection
	       << ",\"kills\":" << combat.kills << ",\"damage_taken\":" << combat.damageTaken
	       << ",\"active_combat_seconds\":" << combat.activeSeconds
	       << ",\"active_combat_uptime\":" << (completion->durationSeconds == 0 ? 0 : combat.activeSeconds / completion->durationSeconds)
	       << ",\"active_combat_damage_per_minute\":" << activeDamagePerMinute
	       << ",\"minimum_health\":" << (combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : combat.minimumHealth)
	       << ",\"p10_health\":" << p10Health << ",\"p10_health_percent\":" << static_cast<uint16_t>(combat.p10HealthPercent)
	       << ",\"verified_potion_recoveries\":" << combat.potionRecoveries << ",\"verified_spell_recoveries\":" << combat.spellRecoveries
	       << ",\"maximum_attacker_overlap\":" << combat.maximumAttackerOverlap
	       << ",\"retreat_observed\":" << (std::strcmp(reason, "hunt_region_observed_danger") == 0 ? "true" : "false")
	       << ",\"danger_observed\":" << (combat.dangerObserved ? "true" : "false") << ",\"death_observed\":" << (combat.deathObserved ? "true" : "false")
	       << ",\"frontier_before\":" << completion->challenge.frontierBefore << ",\"frontier_after\":" << completion->challenge.frontierAfter;
	emit("hunt_region_outcome", position, fields.str());
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) say(*speakingPlayer, "Leaving hunt: " + std::string(reason) + ". " + std::to_string(combat.kills) + " kills, " + std::to_string(completion->experienceGained) + " experience.");
}

bool PlayerBotController::selectHuntRegion(Player& player, const Position& position, const char* reason,
                                           std::chrono::steady_clock::duration* retryAfter)
{
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotHuntPlanningObservation fixtureObservation = fixtureDriver.huntPlanningObservation();
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	auto planningInput = [&]() {
		PlayerBotHuntRuntimePlanningInput input;
		input.player = huntPlayerObservation(player);
		input.player.excludedRegions = activeHuntCooldowns(now);
		input.cacheRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
		input.huntDurationSeconds = duration;
		input.reason = reason;
		if (huntRuntime.planningStartRequired(now)) {
			PlayerBotHuntRegionPlanner planner;
			input.start = {{planner.beginScan(player), huntPlanningFacts(player, huntCombatProfile(player))}};
		}
		return input;
	};
	PlayerBotHuntRuntimeOutcome outcome = huntRuntime.advancePlanning(planningInput(), now, fixtureObservation);
	if (outcome.invalidateCache) {
		PlayerBotHuntRegionPlanner::invalidateCache();
		PlayerBotHuntPlanningObservation refreshedObservation = fixtureObservation;
		refreshedObservation.invalidateCacheRevision = false;
		outcome = huntRuntime.advancePlanning(planningInput(), now, refreshedObservation);
		outcome.staleRevision = true;
	}
	if (!outcome.scoreWork.empty()) {
		const auto started = std::chrono::steady_clock::now();
		std::vector<PlayerBotHuntRuntimeScoreObservation> scores;
		scores.reserve(outcome.scoreWork.size());
		PlayerBotHuntRegionPlanner planner;
		for (const PlayerBotHuntRuntimeScoreWork& work : outcome.scoreWork) {
			auto score = planner.score(player, work.profile, work.cacheRevision, work.candidateIndex,
			                          work.excludedRegions, work.performance, work.huntDurationSeconds);
			scores.push_back({work.candidateIndex, score.valid, score.candidateFactsAvailable,
			                  score.withinPlanningScope, std::move(score.region)});
		}
		outcome = huntRuntime.completeScoreWork(scores,
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
	}
	fixtureDriver.observeHuntPlanning(outcome);
	if (outcome.command == PlayerBotHuntRuntimeCommand::PlanningCancelled) {
		emit("hunt_region_scan", position, "\"phase\":\"cancelled\"");
	}
	if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeReevaluationPending) {
		if (retryAfter) *retryAfter = outcome.retryAfter;
		return false;
	}
	if (outcome.staleRevision) {
		emit("hunt_region_scan", position, "\"phase\":\"stale_revision\"");
	}
	if (outcome.routeWork) {
		const PlayerBotFixtureRoutePlan fixturePlan = fixtureDriver.huntRoutePlan(playerBotNavigationMaximumExpandedNodes);
		PlayerBotNavigationRoutePlan plan;
		if (fixturePlan.forceFailure) {
			plan.metrics.attempted = false;
			plan.metrics.result = PlayerBotNavigationResult::Unreachable;
		} else {
			plan = planNavigationRoute(player, outcome.routeWork->destination, navigationRuntime.activeBlockedPositions(now),
			                           fixturePlan.maximumExpandedNodes);
			telemetry.recordPathfinding(plan.metrics.elapsed, plan.metrics.result == PlayerBotNavigationResult::Reached);
		}
		double travelSeconds = 0;
		for (const PlayerBotNavigationStep& step : plan.steps) {
			travelSeconds += step.action == PlayerBotNavigationAction::Move ? player.getStepDuration(step.direction) / 1000.0 : 1.0;
		}
		const double availableHuntSeconds = std::max(0.0, duration - travelSeconds);
		huntRuntime.completeRouteWork(*outcome.routeWork, {plan.metrics.attempted,
			plan.metrics.result == PlayerBotNavigationResult::Reached, plan.metrics.result == PlayerBotNavigationResult::NodeLimit,
			plan.metrics.expandedNodes, static_cast<uint32_t>(plan.steps.size()), travelSeconds,
			projectedHuntStaminaMultiplier(player, availableHuntSeconds)});
	}
	if (const auto planning = huntRuntime.planningSession();
	    planning && outcome.command != PlayerBotHuntRuntimeCommand::RegionSelected) {
		const char* phase = outcome.command == PlayerBotHuntRuntimeCommand::PlanningStarted ? "scoring_started" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningYield ? "scoring_yield" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningScored ? "scored" :
		                    outcome.routeWork ? "reachability_yield" : "planning_yield";
		emitHuntRegionPlanning(*planning, position, phase);
	}
	for (const PlayerBotHuntRegion& candidate : outcome.candidates) emitHuntRegionCandidate(candidate, position);
	if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeExhausted) {
		emit("hunt_region_selection", position, "\"result\":\"failed\",\"reason\":\"no_suitable_reachable_region\"");
		emit("hunt_scope_exhausted", position, "\"reason\":\"local_scope_exhausted\",\"attempt\":" + std::to_string(outcome.scopeExhaustionAttempt) + ",\"maximum_attempts\":3,\"retry_delay_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(outcome.retryAfter).count()));
		if (outcome.stopForScopeExhaustion) stop("hunt_scope_exhausted", position);
		return false;
	}
	if (!outcome.selectedRegion) return false;
	const PlayerBotHuntRegion& selected = *outcome.selectedRegion;
	emit("hunt_region_selection", position, "\"result\":\"selected\",\"region_id\":" + std::to_string(selected.id) + ",\"reason\":" + jsonString(reason) + ",\"center\":{\"x\":" + std::to_string(selected.center.x) + ",\"y\":" + std::to_string(selected.center.y) + ",\"z\":" + std::to_string(selected.center.z) + "}");
	if (const auto planning = huntRuntime.planningSession()) emitHuntRegionPlanning(*planning, position, "selected");
	std::ostringstream speech;
	speech << "Going hunting. Expecting: ";
	for (size_t index = 0; index < selected.monsters.size(); ++index) { if (index != 0) speech << ", "; speech << selected.monsters[index].name; }
	speech << ". Projected " << std::fixed << std::setprecision(0) << selected.projectedExperience << " experience after " << selected.estimatedTravelSeconds << " seconds travel.";
	say(player, speech.str());
	huntRuntime.completePlanningSelection();
	return true;
}

void PlayerBotController::beginHuntCycle(Player* player, const Position& position, const char* reason)
{
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	huntRuntime.beginCycle(std::chrono::steady_clock::now(), duration);
	resetNavigation();
	emit("action_result", position, "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" + std::to_string(huntRuntime.completedCycles()) + ",\"duration_seconds\":" + std::to_string(duration));
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
	progressionRuntime.enterHunt();
	setCyclePhase(CyclePhase::Hunt, position, reason);
	if (fixtureDriver.huntObservation().selectRegion && !huntRuntime.active()) {
		std::chrono::steady_clock::duration retryAfter{};
		if (!selectHuntRegion(*player, position, "hunt_started", &retryAfter)) {
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(retryAfter).count();
			schedule(static_cast<uint32_t>(delay > 0 ? delay : SCHEDULER_MINTICKS));
			return;
		}
	}
	beginHuntCycle(player, position, reason);
}

void PlayerBotController::processTraversal(Player* player, const Position& currentPosition)
{
	if (progressionRuntime.readinessEquipmentPending()) {
		processReadinessEquipment(player, currentPosition);
		return;
	}
	if (turnRouter.cyclePhase() != CyclePhase::Hunt ||
	    progressionRuntime.session().active() != PlayerBotProgressionProcedure::None) {
		if (combatRuntime.hasDefensiveCombat()) {
			processDefensiveCombat(player, currentPosition);
			return;
		}
		if (attackDefensiveThreat(player, currentPosition)) {
			schedule(navigationInterval);
			return;
		}
	}
	if (combatRuntime.hasDefensiveCombat()) {
		if (turnRouter.scenarioStage() == ScenarioStage::LootCorpse && lootWorkflow.navigationSuspended() &&
		    lootWorkflow.timedOut(std::chrono::steady_clock::now())) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
		}
		processDefensiveCombat(player, currentPosition);
		return;
	}
	PlayerBotTurnObservation turn;
	turn.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	turn.magicTrainingActive = progressionRuntime.activeGoal() == TopLevelGoal::MagicTraining;
	uint32_t usableCapacity = 0;
	if (!turn.progressionActive && !turn.magicTrainingActive) {
		turn.huntRegionSelectionRequired = turnRouter.cyclePhase() == CyclePhase::Hunt &&
		                                   fixtureDriver.huntObservation().selectRegion && !huntRuntime.active() &&
		                                   !huntRuntime.planningActive();
		turn.huntPlanningActive = huntRuntime.planningActive();
		turn.lootNavigationSuspended = lootWorkflow.navigationSuspended();
		if (turnRouter.cyclePhase() == CyclePhase::Hunt) {
			usableCapacity = inventoryPolicy.effectiveFreeCapacity(*player);
			turn.huntCycleFinished = huntRuntime.deadlineReached(std::chrono::steady_clock::now()) ||
			                         usableCapacity < returnCapacityThreshold;
		}
	}
	const PlayerBotTurnCommand turnCommand = turnRouter.route(turn);

	if (turnCommand == PlayerBotTurnCommand::Progression) {
		processProgression(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::MagicTraining) {
		processMagicTraining(*player, currentPosition);
		return;
	}
	if (turnRouter.cyclePhase() == CyclePhase::Hunt &&
	    !ensureCombatReady(player, currentPosition, "readiness_continuous_check")) {
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::StartHunt) {
		startHunt(player, currentPosition, "hunt_region_restart");
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::PlanHunt) {
		std::chrono::steady_clock::duration retryAfter{};
		if (selectHuntRegion(*player, currentPosition, "hunt_planning", &retryAfter)) {
			beginHuntCycle(player, currentPosition, "hunt_region_selected");
		} else {
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(retryAfter).count();
			schedule(static_cast<uint32_t>(delay > 0 ? delay : SCHEDULER_MINTICKS));
		}
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::SuspendedLoot) {
		if (lootWorkflow.timedOut(std::chrono::steady_clock::now())) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		if (attackDefensiveThreat(player, currentPosition)) {
			schedule(navigationInterval);
			return;
		}
		lootCorpse(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::Loot) {
		lootCorpse(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::FinishHunt) {
		const char* reason = usableCapacity < returnCapacityThreshold ? "capacity" : "hunt_deadline";
		finishHuntAndSelectGoal(player, currentPosition, reason);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::Service) {
		processService(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::ReturnToDepot) {
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		const PlayerBotDepotSnapshot depot = depotWorkflow.snapshot();
		if (!depot.hasSelectedDepot) {
			schedule(blockedRouteRetryInterval);
			return;
		}
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Approach, currentPosition)) {
			return;
		}
		if (!processNavigation(player, currentPosition, depot.selected.approachPosition)) {
			if (navigationRuntime.fixedTargetRouteFailureCount() != 0) {
				PlayerBotDepotObservation observation;
				observation.currentPosition = currentPosition;
				observation.now = std::chrono::steady_clock::now();
				observation.routeResult = PlayerBotDepotRouteResult::Unreachable;
				depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotDiscoveryAttempts,
				                      depotApproachSuppression);
				resetNavigation();
			}
			return;
		}
		setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
		processDeposit(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::DepositLoot) {
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		const PlayerBotDepotSnapshot depot = depotWorkflow.snapshot();
		if (!depot.hasSelectedDepot) {
			schedule(blockedRouteRetryInterval);
			return;
		}
		if (!Position::areInRange<1, 1, 0>(currentPosition, depot.selected.approachPosition)) {
			setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "displaced_during_deposit");
			resetNavigation();
			if (!processNavigation(player, currentPosition, depot.selected.approachPosition)) {
				if (navigationRuntime.fixedTargetRouteFailureCount() != 0) {
					PlayerBotDepotObservation observation;
					observation.currentPosition = currentPosition;
					observation.now = std::chrono::steady_clock::now();
					observation.routeResult = PlayerBotDepotRouteResult::Unreachable;
					depotWorkflow.advance(observation, depotRouteValidationsPerDecision, maximumDepotDiscoveryAttempts,
					                      depotApproachSuppression);
					resetNavigation();
				}
				return;
			}
			setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
		}
		processDeposit(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::TraversalCombat) {
		processTraversalCombat(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::TargetPursuit) {
		processTargetPursuit(player, currentPosition);
		return;
	}
	if (turnCommand != PlayerBotTurnCommand::Hunt) {
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
	const PlayerBotHuntPatrolOutcome patrol = huntRuntime.patrolTarget();
	const auto now = std::chrono::steady_clock::now();
	PlayerBotNavigationRuntimeOutcome navigation;
	if (!processNavigation(player, currentPosition, patrol.destination, &navigation)) {
		if (fixtureDriver.navigationRecovery(navigation.routeUnavailable).pause) navigationRuntime.clearBlockedPositions();
		const PlayerBotHuntPatrolOutcome recovery = huntRuntime.observePatrolNavigation(navigation, now,
			maximumRepeatedNavigationStepFailures, maximumPatrolRouteFailures);
		applyHuntCooldown(recovery.cooldown, now);
		if (recovery.command == PlayerBotHuntPatrolCommand::SkipWaypoint || recovery.command == PlayerBotHuntPatrolCommand::RegionExhausted) {
			emit("hunt_region_patrol", currentPosition, "\"result\":\"skipped\",\"reason\":" + jsonString(recovery.reason) +
				",\"step_failures\":" + std::to_string(recovery.stepFailures) + ",\"route_failures\":" + std::to_string(recovery.routeFailures) +
				",\"elapsed_ms\":" + std::to_string(recovery.elapsedMs) + ",\"expanded_nodes\":" + std::to_string(recovery.expandedNodes) +
				",\"region_id\":" + (recovery.regionId ? std::to_string(*recovery.regionId) : "null") +
				",\"destination\":{\"x\":" + std::to_string(recovery.destination.x) + ",\"y\":" +
				std::to_string(recovery.destination.y) + ",\"z\":" + std::to_string(recovery.destination.z) + "}");
			if (recovery.stepFailures != 0 || recovery.routeFailures != 0) telemetry.recordStuckEvent();
			navigationRuntime.resetPatrolRecovery();
			resetNavigation();
			if (recovery.command == PlayerBotHuntPatrolCommand::RegionExhausted) beginService(player, currentPosition, "hunt_region_patrol_unreachable");
		}
		return;
	}
	const PlayerBotHuntPatrolOutcome reached = huntRuntime.observePatrolNavigation(navigation, now,
		maximumRepeatedNavigationStepFailures, maximumPatrolRouteFailures);
	applyHuntCooldown(reached.cooldown, now);
	emit("action_result", currentPosition, "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" +
		std::to_string(reached.waypoint) + ",\"region_id\":" + (reached.regionId ? std::to_string(*reached.regionId) : "null"));
	schedule(SCHEDULER_MINTICKS);
}
