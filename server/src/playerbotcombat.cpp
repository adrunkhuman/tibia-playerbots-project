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
#include "playerbothuntregionadapter.h"
#include "playerbottopology.h"
#include "spells.h"

// Playerbot survival, combat targeting, and hunt orchestration.
using namespace playerbot;

extern Spells* g_spells;

namespace {
	constexpr size_t maximumHuntCandidateTelemetry = 64;
	constexpr size_t maximumHuntRouteCandidates = 8;
	constexpr uint64_t maximumTargetApproachExpandedNodes = 10000;

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

	PlayerBotHuntRuntimePlayerObservation huntPlayerObservation(Player& player)
	{
		PlayerBotHuntRuntimePlayerObservation observation;
		observation.position = player.getPosition();
		observation.level = player.getLevel();
		observation.health = player.getHealth();
		observation.maximumHealth = player.getMaxHealth();
		observation.staminaMinutes = player.getStaminaMinutes();
		observation.experience = player.getExperience();
		observation.topologyGeneration = PlayerBotTopology::instance().generation();
		observation.canUseRope = g_game.findItemOfType(&player, ropeItemId, true) != nullptr;
		observation.canUseShovel = g_game.findItemOfType(&player, 2554, true) != nullptr;
		return observation;
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
	snapshot.potionItemId = recoveryPotionItemId(player.getVocationId());
	snapshot.potionMaximumHealing = recoveryPotionMaximumHealing(player.getVocationId());
	snapshot.potionCount = inventoryPolicy.inventoryItemCount(player, snapshot.potionItemId);
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
	snapshot.lootMovePending = huntCoordinator.hasPendingLootMove();
	snapshot.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	snapshot.progressionDeparture = progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture);
	snapshot.hunting = turnRouter.cyclePhase() == CyclePhase::Hunt;
	snapshot.combatActive = turnRouter.scenarioStage() != ScenarioStage::Traverse || huntCoordinator.hasActiveCombat() ||
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

void PlayerBotController::logHealResult(uint16_t itemId, const char* result, const char* reason, const PlayerBotPotionAttempt& before,
					const PlayerBotPotionAttempt& after, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"heal\",\"result\":" << jsonString(result)
	       << ",\"method\":" << jsonString(itemId == smallHealthPotionItemId ? "small_health_potion" : "health_potion")
	       << ",\"item_id\":" << itemId
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
	const PlayerBotSurvivalSnapshot snapshot = survivalSnapshot(*player);
	const PlayerBotSurvivalCommand command = survivalRuntime.decideHealing(snapshot, now);
	if (command.potionVerification) {
		const auto& verification = *command.potionVerification;
		if (verification.result == PlayerBotPotionVerificationResult::Success) {
			logHealResult(snapshot.potionItemId, "success", nullptr, verification.before, verification.after, currentPosition);
			recordHuntRecovery(true);
		} else {
			telemetry.recordActionFailure();
			logHealResult(snapshot.potionItemId, "failed", verification.result == PlayerBotPotionVerificationResult::IneffectiveRecovery ?
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
			       << ",\"method\":" << jsonString(snapshot.potionItemId == smallHealthPotionItemId ? "small_health_potion" : "health_potion")
			       << ",\"item_id\":" << snapshot.potionItemId
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
		if (progressionRuntime.activeGoal() != TopLevelGoal::Service) {
			beginService(player, currentPosition, "healing_supply_missing");
		}
		return false;
	}
	huntCoordinator.cancelPlanning();
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

bool PlayerBotController::attackVisibleMonster(Player* player, const Position& currentPosition, uint32_t preferredTargetId,
	                                            bool routeValidated)
{
	if (huntCoordinator.huntActive() && !huntRegionReached) return false;
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	std::vector<PlayerBotTraversalCandidate> candidates;
	for (Creature* creature : spectators) {
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || !player->canSee(creature->getPosition())) {
			continue;
		}
		if (preferredTargetId != 0 && creature->getID() != preferredTargetId) continue;
		if (huntCoordinator.huntActive() && creature->getAttackedCreature() != player && !huntCoordinator.matchesHuntMonster(creature->getName())) {
			continue;
		}
		candidates.push_back({{creature->getID(), creature->getPosition(), creature->getName()}, expectedCorpseFor(*creature),
		                      creature->getAttackedCreature() == player});
	}
	while (!candidates.empty()) {
		const auto command = huntCoordinator.selectTraversalAttack(candidates, currentPosition, std::chrono::steady_clock::now());
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
		if (!routeValidated && !Position::areInRange<1, 1, 0>(currentPosition, target->getPosition())) {
			const PlayerBotNavigationRoutePlan route = planNavigationRoute(
			    *player, PlayerBotNavigationGoal::withinRange(target->getPosition(), 1, 1), {},
			    maximumTargetApproachExpandedNodes, true);
			telemetry.recordPathfinding(route.metrics.elapsed, route.metrics.result == PlayerBotNavigationResult::Reached);
			if (route.metrics.result != PlayerBotNavigationResult::Reached) {
				huntCoordinator.suppressTraversalTarget(target->getID(), std::chrono::steady_clock::now(),
				                                              navigationBlockSuppression);
				if (shouldEmitRepeated("target:skip:route_unavailable:" + std::to_string(target->getID()))) {
					emit("action_result", currentPosition,
					     "\"action\":\"target_approach\",\"result\":\"skipped\",\"reason\":\"route_unavailable\",\"target_id\":" +
					         std::to_string(target->getID()) + ",\"same_floor\":true,\"expanded_nodes\":" +
					         std::to_string(route.metrics.expandedNodes));
				}
				return false;
			}
		}
		telemetry.recordActionAttempt();
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		const PlayerBotCombatDecision started = huntCoordinator.confirmCombatAttack(*command, player->getAttackedCreature() == target,
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
	if (huntCoordinator.traversalTarget()) return false;
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	const auto now = std::chrono::steady_clock::now();
	const size_t adjacentAttackers = std::count_if(spectators.begin(), spectators.end(), [player, &currentPosition](Creature* creature) {
		return creature->getMonster() && !creature->isRemoved() && !creature->isDead() &&
		       creature->getAttackedCreature() == player && player->canSee(creature->getPosition()) &&
		       Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition());
	});
	const bool overwhelmed = adjacentAttackers >= 4;
	if (!overwhelmed) {
		for (Creature* creature : spectators) {
			if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
			    !player->canSee(creature->getPosition()) ||
			    !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) continue;
			if (!navigationRuntime.avoidPendingRouteBlocker(creature->getID(), creature->getPosition(), now, navigationBlockSuppression)) continue;
			emit("navigation_progress", currentPosition,
			     "\"result\":\"replanning\",\"reason\":\"hostile_detour\",\"blocker_id\":" +
			         std::to_string(creature->getID()) + ",\"blocker_position\":{\"x\":" +
			         std::to_string(creature->getPosition().x) + ",\"y\":" +
			         std::to_string(creature->getPosition().y) + ",\"z\":" +
			         std::to_string(creature->getPosition().z) + "}");
			return true;
		}
	}
	auto isRouteCritical = [this, now](const Creature* creature) {
		return navigationRuntime.isRouteCritical(creature->getID(), creature->getPosition(), now);
	};
	const bool blockerOnly = (huntCoordinator.huntActive() && !huntRegionReached) ||
	                         turnRouter.scenarioStage() == ScenarioStage::LootCorpse;
	std::vector<PlayerBotDefensiveTarget> candidates;
	for (Creature* creature : spectators) {
		const bool routeCritical = isRouteCritical(creature);
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
		    (!routeCritical && ((blockerOnly && !overwhelmed) || creature->getAttackedCreature() != player)) ||
		    !player->canSee(creature->getPosition()) ||
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
	const auto command = huntCoordinator.selectDefensiveAttack(std::move(candidates), currentPosition);
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
	const PlayerBotCombatDecision started = huntCoordinator.confirmCombatAttack(*command, player->getAttackedCreature() == target,
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
	const uint32_t previousTarget = huntCoordinator.defensiveTarget() ? huntCoordinator.defensiveTarget()->id : 0;
	huntCoordinator.clearDefensiveTarget();
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
	const auto defensive = huntCoordinator.defensiveTarget();
	Creature* target = defensive ? g_game.getCreatureByID(defensive->id) : nullptr;
	PlayerBotCombatTargetSnapshot observed;
	if (target) observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target),
	                        Position::areInRange<1, 1, 0>(currentPosition, target->getPosition()), target->getAttackedCreature() == player,
	                        player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
	const PlayerBotCombatDecision decision = huntCoordinator.advanceCombat({currentPosition, std::chrono::steady_clock::now(), {}, observed});
	if (decision.command == PlayerBotCombatCommand::CompleteDefensiveCombat) finishDefensiveCombat(player, currentPosition, decision.result, decision.reason);
	schedule(navigationInterval);
}

void PlayerBotController::finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearTraversalTarget(currentPosition, reason);
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
{
	const auto traversal = huntCoordinator.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	if (target && target->getAttackedCreature() != player) {
		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, currentPosition);
		const auto now = std::chrono::steady_clock::now();
		const uint32_t currentTargetDistance = std::max(Position::getDistanceX(currentPosition, target->getPosition()),
		                                                Position::getDistanceY(currentPosition, target->getPosition()));
		uint64_t remainingExpandedNodes = maximumTargetApproachExpandedNodes;
		uint32_t reachableAttackerId = 0;
		for (Creature* creature : spectators) {
			if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || creature == target ||
			    creature->getAttackedCreature() != player || !player->canSee(creature->getPosition())) {
				continue;
			}
			const uint32_t candidateDistance = std::max(Position::getDistanceX(currentPosition, creature->getPosition()),
			                                            Position::getDistanceY(currentPosition, creature->getPosition()));
			if (candidateDistance >= currentTargetDistance) continue;
			const std::vector<PlayerBotTraversalCandidate> candidate{{
			    {creature->getID(), creature->getPosition(), creature->getName()}, expectedCorpseFor(*creature), true}};
			if (!huntCoordinator.selectTraversalAttack(candidate, currentPosition, now)) continue;
			if (Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) {
				reachableAttackerId = creature->getID();
				break;
			}
			if (remainingExpandedNodes == 0) break;
			const PlayerBotNavigationRoutePlan route = planNavigationRoute(
			    *player, PlayerBotNavigationGoal::withinRange(creature->getPosition(), 1, 1), {},
			    remainingExpandedNodes, true);
			telemetry.recordPathfinding(route.metrics.elapsed, route.metrics.result == PlayerBotNavigationResult::Reached);
			remainingExpandedNodes -= std::min(remainingExpandedNodes, route.metrics.expandedNodes);
			if (route.metrics.result == PlayerBotNavigationResult::Reached) {
				reachableAttackerId = creature->getID();
				break;
			}
		}
		if (reachableAttackerId != 0) {
			const uint32_t previousTargetId = target->getID();
			g_game.playerSetAttackedCreature(playerId, 0);
			huntCoordinator.suppressTraversalTarget(previousTargetId, now,
			                                              traversalTargetSuppression);
			huntCoordinator.clearTraversalTarget();
			resetNavigation();
			setStage(ScenarioStage::Traverse, currentPosition);
			emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTargetId) +
			     ",\"target_id\":null,\"reason\":\"active_attacker_preempted\"");
			attackVisibleMonster(player, currentPosition, reachableAttackerId, true);
			schedule(navigationInterval);
			return;
		}
	}
	PlayerBotCombatTargetSnapshot observed;
	if (target) observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target),
	                        Position::areInRange<1, 1, 0>(currentPosition, target->getPosition()), target->getAttackedCreature() == player,
	                        player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
	if (target && !target->isRemoved() && !target->isDead() && observed.visible && observed.visibleCreature && !observed.adjacent) {
		PlayerBotNavigationRuntimeOutcome navigation;
		if (!processNavigation(player, currentPosition, PlayerBotNavigationGoal::withinRange(target->getPosition(), 1, 1),
		                       &navigation, maximumTargetApproachExpandedNodes, false, true)) {
			if (navigation.routeUnavailable || navigation.oscillation) {
				const uint32_t previousTargetId = target->getID();
				huntCoordinator.suppressTraversalTarget(previousTargetId, std::chrono::steady_clock::now(),
				                                              navigationBlockSuppression);
				emit("action_result", currentPosition,
				     "\"action\":\"target_approach\",\"result\":\"skipped\",\"reason\":\"route_unavailable\",\"target_id\":" +
				         std::to_string(previousTargetId) + ",\"same_floor\":true");
				finishTraversalCombat(player, currentPosition, "target_route_unavailable");
			}
			return;
		}
	}
	const PlayerBotCombatDecision decision = huntCoordinator.advanceCombat({currentPosition, std::chrono::steady_clock::now(), observed, {}});
	if (decision.command == PlayerBotCombatCommand::BeginLoot) {
		beginLoot(player, currentPosition, decision);
	} else if (decision.command == PlayerBotCombatCommand::Abandon) {
		if (decision.reason && std::strcmp(decision.reason, "combat_timeout") == 0) {
			logActionFailure("attack", decision.reason, currentPosition);
		}
		finishTraversalCombat(player, currentPosition, decision.reason ? decision.reason : "target_lost");
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
	return huntCoordinator.huntActive() && turnRouter.cyclePhase() == CyclePhase::Hunt &&
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
	huntCoordinator.sampleHuntCombat({active, now, player.getHealth(), player.getMaxHealth(), attackers});
}

void PlayerBotController::recordHuntRecovery(bool potion)
{
	Player* player = g_game.getPlayerByID(playerId);
	if (!player || !huntCoordinator.huntActive() || !isActiveHuntCombat(*player)) {
		return;
	}
	if (potion) {
		huntCoordinator.observeHuntRecovery(true);
	} else {
		huntCoordinator.observeHuntRecovery(false);
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
	       << ",\"atlas_site_id\":" << region.atlasSiteId
	       << ",\"atlas_variant_id\":" << region.atlasVariantId
	       << ",\"atlas_pockets\":" << region.atlasPocketCount
	       << ",\"atlas_spawns\":" << region.atlasSpawnCount
	       << ",\"atlas_floors\":" << region.atlasFloorCount
	       << ",\"floor\":" << static_cast<uint16_t>(region.floor)
	       << ",\"center\":{\"x\":" << region.center.x << ",\"y\":" << region.center.y
	       << ",\"z\":" << static_cast<uint16_t>(region.center.z) << '}'
	       << ",\"destination\":{\"x\":" << region.destination.x << ",\"y\":" << region.destination.y
	       << ",\"z\":" << static_cast<uint16_t>(region.destination.z) << '}'
	       << ",\"patrol_points\":" << region.patrolPoints.size()
	       << ",\"experience_per_minute\":" << region.experiencePerMinute
	       << ",\"spawn_experience_per_minute\":" << region.spawnExperiencePerMinute
	       << ",\"clear_experience_per_minute\":" << region.clearExperiencePerMinute
	       << ",\"estimated_travel_seconds\":" << region.estimatedTravelSeconds
	       << ",\"available_hunt_seconds\":" << region.availableHuntSeconds
	       << ",\"observed_experience_per_minute\":" << region.observedExperiencePerMinute
	       << ",\"observed_correction\":" << region.observedCorrection
	       << ",\"stamina_minutes\":" << region.staminaMinutes
	       << ",\"stamina_experience_multiplier\":" << region.staminaExperienceMultiplier
	       << ",\"projected_experience\":" << region.projectedExperience
	       << ",\"optimistic_projected_experience\":" << region.optimisticProjectedExperience
	       << ",\"threat_ratio\":" << region.threatRatio
	       << ",\"raw_threat_ratio\":" << region.rawThreatRatio
	       << ",\"corridor_danger_available\":" << (region.corridorDangerAvailable ? "true" : "false")
	       << ",\"corridor_danger_ratio\":" << region.corridorDangerRatio
	       << ",\"corridor_samples\":" << region.corridorSampleCount
	       << ",\"corridor_spawn_blocks\":" << region.corridorSpawnBlocks
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
	       << ",\"topology_reachable\":" << (region.topologyReachable ? "true" : "false")
	       << ",\"topology_travel_steps\":" << region.topologyTravelSteps
	       << ",\"route_danger_cost\":" << region.routeDangerCost
	       << ",\"maximum_route_danger\":" << region.maximumRouteDanger
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
	       << ",\"topology_time_us\":" << planning.topologyTimeUs()
	       << ",\"scoring_time_us\":" << planning.scoringTimeUs() << ",\"candidate_count\":" << planning.totalCandidates()
	       << ",\"scored_candidate_count\":" << planning.scoredCandidates() << ",\"suitable_candidate_count\":" << planning.suitableCandidates()
	       << ",\"yields\":" << planning.yields() << ",\"selection_strategy\":"
	       << jsonString(planning.topologySelection() ? "atlas_topology_selection" : "atlas_geometric_selection")
	       << ",\"challenge_frontier\":" << planning.profile().challengeFrontier << ",\"decision_latency_us\":" << latencyUs;
	emit("hunt_region_scan", position, fields.str());
}

void PlayerBotController::finishHuntRegion(const Player& player, const Position& position, const char* reason)
{
	Player& mutablePlayer = const_cast<Player&>(player);
	const auto completion = huntCoordinator.finishHunt(huntPlayerObservation(mutablePlayer), std::chrono::steady_clock::now(),
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
		input.player.excludedVariants = huntCoordinator.activeHuntCooldowns(now);
		input.cacheRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
		input.huntDurationSeconds = duration;
		input.reason = reason;
		if (huntCoordinator.planningStartRequired(now)) {
			PlayerBotHuntRegionPlanner planner;
			const auto topologyStarted = std::chrono::steady_clock::now();
			std::shared_ptr<PlayerBotTopologyDistances> topologyDistances;
			if (PlayerBotTopology::instance().walkComponent(player.getPosition())) {
				topologyDistances = std::make_shared<PlayerBotTopologyDistances>(PlayerBotTopology::instance().distancesFrom(
				    player.getPosition(), input.player.canUseRope, input.player.canUseShovel, player.getLevel()));
			}
			const uint64_t topologyTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now() - topologyStarted).count();
			PlayerBotHuntRegionScan scan = planner.beginScan(player, topologyDistances.get());
			input.start = {{std::move(scan), huntPlanningFacts(player, huntCombatProfile(player)),
			                std::move(topologyDistances), topologyTimeUs}};
		}
		return input;
	};
	PlayerBotHuntRuntimeOutcome outcome = huntCoordinator.advancePlanning(planningInput(), now, fixtureObservation);
	if (outcome.invalidateCache) {
		PlayerBotHuntRegionPlanner::invalidateCache();
		PlayerBotHuntPlanningObservation refreshedObservation = fixtureObservation;
		refreshedObservation.invalidateCacheRevision = false;
		outcome = huntCoordinator.advancePlanning(planningInput(), now, refreshedObservation);
		outcome.staleRevision = true;
	}
	if (!outcome.scoreWork.empty()) {
		const auto started = std::chrono::steady_clock::now();
		std::vector<PlayerBotHuntRuntimeScoreObservation> scores;
		scores.reserve(outcome.scoreWork.size());
		PlayerBotHuntRegionPlanner planner;
		for (const PlayerBotHuntRuntimeScoreWork& work : outcome.scoreWork) {
				auto score = planner.score(player, work.profile, work.cacheRevision, work.candidateIndex,
				                          work.excludedVariants, work.performance, work.huntDurationSeconds,
			                          work.topologyDistances.get());
			scores.push_back({work.candidateIndex, score.valid, score.candidateFactsAvailable,
			                  score.withinPlanningScope, std::move(score.region)});
		}
		outcome = huntCoordinator.completeScoreWork(scores,
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
	if (const auto planning = huntCoordinator.planningSession();
	    planning && outcome.command != PlayerBotHuntRuntimeCommand::RegionSelected) {
		const char* phase = outcome.command == PlayerBotHuntRuntimeCommand::PlanningStarted ? "scoring_started" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningYield ? "scoring_yield" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningScored ? "scored" : "planning_yield";
		emitHuntRegionPlanning(*planning, position, phase);
	}
	if (outcome.command != PlayerBotHuntRuntimeCommand::RegionSelected) {
		const size_t telemetryCandidates = std::min(outcome.candidates.size(), maximumHuntCandidateTelemetry);
		for (size_t index = 0; index < telemetryCandidates; ++index) {
			emitHuntRegionCandidate(outcome.candidates[index], position);
		}
	}
	if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeExhausted) {
		emit("hunt_region_selection", position, "\"result\":\"failed\",\"reason\":\"no_suitable_reachable_region\"");
		emit("hunt_scope_exhausted", position, "\"reason\":\"local_scope_exhausted\",\"attempt\":" + std::to_string(outcome.scopeExhaustionAttempt) + ",\"maximum_attempts\":3,\"retry_delay_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(outcome.retryAfter).count()));
		if (outcome.stopForScopeExhaustion) stop("hunt_scope_exhausted", position);
		return false;
	}
	if (!outcome.selectedRegion) return false;
	const PlayerBotNavigationRiskProfile risk;
	std::optional<PlayerBotHuntRegion> safeSelection;
	uint32_t returnRouteDangerCost = 0;
	size_t routedCandidates = 0;
	for (const PlayerBotHuntRegion& candidate : outcome.candidates) {
		if (!candidate.suitable || !candidate.reachable) continue;
		if (routedCandidates++ >= maximumHuntRouteCandidates) break;
		PlayerBotHuntRegion routed = candidate;
		const PlayerBotNavigationRoutePlan routePlan = planCompleteNavigationRoute(player, candidate.destination);
		routed.travelSteps = static_cast<uint32_t>(routePlan.metrics.steps);
		routed.routeDangerCost = routePlan.metrics.dangerCost;
		routed.maximumRouteDanger = routePlan.metrics.maximumHealthLossPerSecond;
		const double travelSeconds = routePlan.metrics.estimatedTravelSeconds > 0 ?
		    routePlan.metrics.estimatedTravelSeconds : routePlan.metrics.steps * player.getStepDuration() / 1000.0;
		if (travelSeconds > 0) {
			const double projectedPerSecond = routed.availableHuntSeconds > 0 ?
			    routed.projectedExperience / routed.availableHuntSeconds : 0;
			routed.estimatedTravelSeconds = travelSeconds;
			routed.availableHuntSeconds = std::max(0.0, duration - travelSeconds);
			routed.projectedExperience = projectedPerSecond * routed.availableHuntSeconds;
		}
		if (routePlan.metrics.result != PlayerBotNavigationResult::Reached) {
			routed.suitable = false;
			routed.rejectionReason = "route_unreachable";
		} else if (routePlan.metrics.dangerCost > static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost)) {
			routed.suitable = false;
			routed.rejectionReason = "route_danger_above_tolerance";
		} else if (routePlan.metrics.maximumHealthLossPerSecond > risk.maximumHealthLossPerSecond) {
			routed.suitable = false;
			routed.rejectionReason = "route_peak_danger_above_tolerance";
		}
		PlayerBotNavigationRoutePlan returnPlan;
		if (routed.suitable) {
			returnPlan = planCompleteNavigationRoute(player, candidate.destination, position);
			if (returnPlan.metrics.result != PlayerBotNavigationResult::Reached) {
				routed.suitable = false;
				routed.rejectionReason = "return_route_unreachable";
			} else if (returnPlan.metrics.dangerCost > static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost)) {
				routed.suitable = false;
				routed.rejectionReason = "return_route_danger_above_tolerance";
			} else if (returnPlan.metrics.maximumHealthLossPerSecond > risk.maximumHealthLossPerSecond) {
				routed.suitable = false;
				routed.rejectionReason = "return_route_peak_danger_above_tolerance";
			}
		}
		if (!routed.suitable) {
			emitHuntRegionCandidate(routed, position);
			huntCoordinator.rejectHuntVariant(routed.atlasVariantId, now, std::chrono::minutes(10));
			continue;
		}
		returnRouteDangerCost = returnPlan.metrics.dangerCost;
		safeSelection = std::move(routed);
		break;
	}
	if (!safeSelection) {
		emit("hunt_region_selection", position,
		     "\"result\":\"failed\",\"reason\":\"no_safe_route_in_shortlist\"");
		huntCoordinator.cancelPlanning();
		if (retryAfter) *retryAfter = std::chrono::seconds(1);
		return false;
	}
	PlayerBotHuntRegion selected = std::move(*safeSelection);
	const uint32_t healthLossCost = static_cast<uint32_t>(risk.healthLossCost);
	huntPotionReturnThreshold = recoveryPotionRouteReserve(
		player.getVocationId(), player.getMaxHealth(), returnRouteDangerCost, healthLossCost);
	huntPotionRestockTarget = recoveryPotionRestockTargetForReserve(huntPotionReturnThreshold);
	emit("hunt_supply_reserve", position,
	     "\"source\":\"selected_return_route\",\"route_danger_cost\":" +
	         std::to_string(returnRouteDangerCost) + ",\"health_loss_cost\":" + std::to_string(healthLossCost) +
	         ",\"maximum_health\":" + std::to_string(player.getMaxHealth()) + ",\"minimum_potion_healing\":" +
	         std::to_string(recoveryPotionMinimumHealing(player.getVocationId())) + ",\"return_threshold\":" +
	         std::to_string(huntPotionReturnThreshold) + ",\"restock_target\":" +
	         std::to_string(huntPotionRestockTarget));
	huntCoordinator.selectPlanningRegion(selected, huntPlayerObservation(player), now);
	emitHuntRegionCandidate(selected, position);
	emit("hunt_region_selection", position, "\"result\":\"selected\",\"region_id\":" + std::to_string(selected.id) +
		",\"atlas_site_id\":" + std::to_string(selected.atlasSiteId) + ",\"atlas_variant_id\":" +
		std::to_string(selected.atlasVariantId) + ",\"atlas_pockets\":" + std::to_string(selected.atlasPocketCount) +
		",\"atlas_spawns\":" + std::to_string(selected.atlasSpawnCount) + ",\"atlas_floors\":" +
		std::to_string(selected.atlasFloorCount) + ",\"reason\":" + jsonString(reason) + ",\"center\":{\"x\":" +
		std::to_string(selected.center.x) + ",\"y\":" + std::to_string(selected.center.y) + ",\"z\":" +
		std::to_string(selected.center.z) + "}");
	if (const auto planning = huntCoordinator.planningSession()) emitHuntRegionPlanning(*planning, position, "selected");
	std::ostringstream speech;
	speech << "Going hunting. Expecting: ";
	for (size_t index = 0; index < selected.monsters.size(); ++index) { if (index != 0) speech << ", "; speech << selected.monsters[index].name; }
	speech << ". Projected " << std::fixed << std::setprecision(0) << selected.projectedExperience << " experience after " << selected.estimatedTravelSeconds << " seconds travel.";
	say(player, speech.str());
	huntCoordinator.completePlanningSelection();
	return true;
}

void PlayerBotController::beginHuntCycle(Player* player, const Position& position, const char* reason)
{
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	huntCoordinator.beginHuntCycle(std::chrono::steady_clock::now(), duration);
	huntRegionReached = false;
	resetNavigation();
	emit("action_result", position, "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" + std::to_string(huntCoordinator.completedHuntCycles()) + ",\"duration_seconds\":" + std::to_string(duration));
	schedule(SCHEDULER_MINTICKS);
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
	if (fixtureDriver.huntObservation().selectRegion && !huntCoordinator.huntActive()) {
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
	if (turnRouter.cyclePhase() == CyclePhase::Hunt && huntCoordinator.huntActive() && !huntRegionReached &&
	    huntCoordinator.insideHuntArea(currentPosition, Map::maxClientViewportX, Map::maxClientViewportX + 1,
	                                    Map::maxClientViewportY, Map::maxClientViewportY + 1)) {
		huntRegionReached = true;
		const PlayerBotHuntPatrolOutcome patrol = huntCoordinator.huntPatrolTarget();
		emit("hunt_area_entered", currentPosition,
		     "\"region_id\":" + (patrol.regionId ? std::to_string(*patrol.regionId) : "null") +
		         ",\"waypoint\":" + std::to_string(patrol.waypoint) + ",\"destination\":{\"x\":" +
		         std::to_string(patrol.destination.x) + ",\"y\":" + std::to_string(patrol.destination.y) +
		         ",\"z\":" + std::to_string(patrol.destination.z) + "}");
	}
	if (progressionRuntime.readinessEquipmentPending()) {
		processReadinessEquipment(player, currentPosition);
		return;
	}
	if (huntCoordinator.hasDefensiveCombat()) {
		if (turnRouter.scenarioStage() == ScenarioStage::LootCorpse && huntCoordinator.lootNavigationSuspended() &&
		    huntCoordinator.lootTimedOut(std::chrono::steady_clock::now())) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
		}
		processDefensiveCombat(player, currentPosition);
		return;
	}
	if (attackDefensiveThreat(player, currentPosition)) {
		schedule(navigationInterval);
		return;
	}
	PlayerBotTurnObservation turn;
	turn.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	turn.magicTrainingActive = progressionRuntime.activeGoal() == TopLevelGoal::MagicTraining;
	bool capacityPressureElapsed = false;
	if (!turn.progressionActive && !turn.magicTrainingActive) {
		const bool inHuntPhase = turnRouter.cyclePhase() == CyclePhase::Hunt;
		const PlayerBotHuntTurnObservation hunt = huntCoordinator.observeTurn(
			inHuntPhase, fixtureDriver.huntObservation().selectRegion, std::chrono::steady_clock::now());
		turn.huntRegionSelectionRequired = hunt.regionSelectionRequired;
		turn.huntPlanningActive = hunt.planningActive;
		turn.lootNavigationSuspended = hunt.lootNavigationSuspended;
		capacityPressureElapsed = hunt.capacityPressureElapsed;
		turn.huntCycleFinished = hunt.cycleFinished;
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
	    !ensureCombatReady(player, currentPosition, "readiness_continuous_check", false)) {
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
		if (huntCoordinator.lootTimedOut(std::chrono::steady_clock::now())) {
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
		const char* reason = capacityPressureElapsed ? "capacity" : "hunt_deadline";
		finishHuntAndReturn(player, currentPosition, reason);
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
	const PlayerBotHuntPatrolOutcome patrol = huntCoordinator.huntPatrolTarget();
	const auto now = std::chrono::steady_clock::now();
	PlayerBotNavigationRuntimeOutcome navigation;
	if (!processNavigation(player, currentPosition, patrol.destination, &navigation)) {
		if (fixtureDriver.navigationRecovery(navigation.routeUnavailable).pause) navigationRuntime.clearBlockedPositions();
		const PlayerBotHuntPatrolOutcome recovery = huntCoordinator.observeHuntPatrolNavigation(navigation, now,
			maximumRepeatedNavigationStepFailures, maximumPatrolRouteFailures);
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
	const PlayerBotHuntPatrolOutcome reached = huntCoordinator.observeHuntPatrolNavigation(navigation, now,
		maximumRepeatedNavigationStepFailures, maximumPatrolRouteFailures);
	if (reached.command == PlayerBotHuntPatrolCommand::WaypointReached) {
		huntRegionReached = true;
		emit("action_result", currentPosition, "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" +
			std::to_string(reached.waypoint) + ",\"region_id\":" + (reached.regionId ? std::to_string(*reached.regionId) : "null"));
	}
	schedule(reached.command == PlayerBotHuntPatrolCommand::WaitAtWaypoint ? 1000 : SCHEDULER_MINTICKS);
}
