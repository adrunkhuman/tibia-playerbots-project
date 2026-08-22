/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotcontroller.h"
#include "playerbotspellcalibration.h"
#include "condition.h"
#include "spells.h"

// Runtime spell-trainer discovery and normal NPC learning dialogue.
using namespace playerbot;

extern Spells* g_spells;

namespace {
	constexpr uint32_t maximumSpellTrainerDistanceFromTemple = 200;
	constexpr uint32_t magicTrainingEmergencyReserve = 20;
	constexpr auto magicTrainingRetryDelay = std::chrono::seconds(2);

	const char* fallbackForRole(PlayerBotSpellRole role)
	{
		return role == PlayerBotSpellRole::Healing ? "small_health_potion" :
		       role == PlayerBotSpellRole::Support ? "continue_route" : "normal_melee";
	}

	const char* fallbackForNeed(const char* need)
	{
		return std::strcmp(need, "recovery") == 0 ? "small_health_potion" :
		       std::strcmp(need, "safe_route") == 0 ? "continue_route" : "normal_melee";
	}

	std::string targetClass(const Creature* target)
	{
		if (!target) return "self";
		if (!target->getMonster()) return "creature";
		std::string name = target->getName();
		name.resize(std::min<size_t>(name.size(), 48));
		return "monster:" + name;
	}

}

void PlayerBotController::emitSpellCastEvent(const Position& position, const char* spellName, const char* words, const char* role,
                                             const char* need, const char* result, const char* engineResult, const char* reason,
                                              const PlayerBotSpellPendingCast* pending, const Player* player, const char* fallback) const
{
	std::ostringstream fields;
	fields << "\"action\":\"cast_spell\",\"result\":" << jsonString(result)
	       << ",\"need\":" << jsonString(need)
	       << ",\"selected_method\":" << jsonString(spellName ? "spell" : "none")
	       << ",\"policy_candidate\":";
	if (spellName) {
		fields << "{\"spell\":" << jsonString(spellName) << ",\"words\":" << jsonString(words)
		       << ",\"role\":" << jsonString(role) << '}';
	} else {
		fields << "null";
	}
	fields << ",\"legal_candidates\":[";
	if (spellName && std::strcmp(engineResult, "accepted") == 0) {
		fields << jsonString(spellName);
	}
	fields << "],\"engine_result\":" << jsonString(engineResult);
	if (pending) {
		fields << ",\"mana_before\":" << pending->manaBefore
		       << ",\"mana_after\":" << (player ? player->getMana() : pending->manaBefore)
		       << ",\"mana_reserve\":" << pending->manaReserve
		       << ",\"reserve_survives\":" << ((player && player->getMana() >= pending->manaReserve) ? "true" : "false")
		       << ",\"health_before\":" << pending->healthBefore
		       << ",\"health_after\":" << (player ? player->getHealth() : pending->healthBefore);
		if (pending->targetId != 0) {
			Creature* target = g_game.getCreatureByID(pending->targetId);
			fields << ",\"target_id\":" << pending->targetId
			       << ",\"target_health_before\":" << pending->targetHealthBefore
			       << ",\"target_health_after\":" << (target && !target->isRemoved() ? target->getHealth() : 0)
			       << ",\"target_class\":" << jsonString(pending->targetClass);
		}
		fields << ",\"engine_bounds\":{\"minimum\":" << pending->envelope.minimum
		       << ",\"maximum\":" << pending->envelope.maximum << ",\"duration_ms\":" << pending->envelope.durationMs << '}'
		       << ",\"observation_age_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
		           std::chrono::steady_clock::now() - pending->observedAt).count();
		if (pending->targetId != 0) {
			fields << ",\"spell_victim_count\":" << static_cast<uint16_t>(pending->spellVictimCount)
			       << ",\"spell_victim_overflow\":" << (pending->spellVictimOverflow ? "true" : "false");
		}
		if (pending->role == PlayerBotSpellRole::Support) {
			fields << ",\"haste_ticks_after_cast\":" << pending->hasteTicksAfterCast
			       << ",\"haste_ticks_observed\":" << pending->hasteTicksObserved
			       << ",\"haste_duration_measured\":" << pending->hasteDurationMeasured;
		}
		if (const PlayerBotSpellProfile* profile = survivalRuntime.calibrationProfile(*pending)) {
			fields << ",\"calibration\":{\"accepted\":" << profile->accepted << ",\"rejected\":" << profile->rejected
			       << ",\"ambiguous\":" << profile->ambiguous << ",\"minimum\":" << profile->minimum
			       << ",\"maximum\":" << profile->maximum << ",\"conservative\":" << profile->conservative
			       << ",\"ranking\":" << profile->ranking << ",\"confidence\":" << profile->confidence << '}';
		} else {
			fields << ",\"calibration\":{\"accepted\":0,\"rejected\":0,\"ambiguous\":0,\"confidence\":0}"
			<< ",\"ranking_estimate\":" << survivalRuntime.calibrationRanking(*pending);
		}
	}
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	fields << ",\"fallback\":" << (fallback ? jsonString(fallback) : "null");
	emit("action_result", position, fields.str());
}

bool PlayerBotController::dispatchSpellCommand(Player& player, const Position& position, PlayerBotSurvivalCommand command)
{
	if (command.type != PlayerBotSurvivalCommandType::CastSpell || !command.spell) {
		if (!command.reason) return false;
		const PlayerBotSpellDescriptor* descriptor = command.candidate;
		const std::string key = std::string("cast_spell:") + command.reason + ':' + (descriptor ? descriptor->name : "unknown");
		if (!descriptor || shouldEmitRepeated(key)) {
			emitSpellCastEvent(position, descriptor ? descriptor->name : nullptr, descriptor ? descriptor->words : nullptr,
			                   descriptor ? playerBotSpellRoleName(descriptor->role) : nullptr, command.need ? command.need : "unknown",
			                   "skipped", "not_attempted", command.reason, nullptr, &player,
			                   descriptor ? fallbackForRole(descriptor->role) : fallbackForNeed(command.need ? command.need : ""));
		}
		return false;
	}
	const auto& cast = *command.spell;
	telemetry.recordActionAttempt();
	emitSpellCastEvent(position, cast.descriptor->name, cast.descriptor->words, playerBotSpellRoleName(cast.descriptor->role),
	                   cast.pending.need.c_str(), "requested", "unchecked", nullptr, &cast.pending, &player, nullptr);
	// The normal speech handler remains the authoritative spell engine.
	survivalRuntime.beginEngineSpellCast();
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", cast.spell->getWords());
	survivalRuntime.endEngineSpellCast();
	if (cast.descriptor->role == PlayerBotSpellRole::Support) {
		if (Condition* haste = player.getCondition(CONDITION_HASTE)) survivalRuntime.observeHasteAfterCast(haste->getTicks(), haste->getEndTime());
	}
	return true;
}

void PlayerBotController::verifySpellCast(Player& player, const Position& position)
{
	// The runtime owns pending-cast verification and calibration; the controller only supplies live observations.
	const PlayerBotSpellPendingCast* pending = survivalRuntime.pendingSpell();
	if (!pending) return;
	Creature* target = g_game.getCreatureByID(pending->targetId);
	Condition* haste = player.getCondition(CONDITION_HASTE);
	const PlayerBotSpellVerificationInput input{player.getMana(), player.getHealth(), haste ? haste->getTicks() : 0,
		haste ? haste->getEndTime() : 0, target && !target->isRemoved() && targetClass(target) == pending->targetClass,
		std::chrono::steady_clock::now()};
	const auto outcome = survivalRuntime.verifySpell(input);
	if (!outcome) return;
	const PlayerBotSpellVerification& verification = outcome->verification;
	const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor(verification.pending.name.c_str());
	if (outcome->evictedProfile) {
		emit("spell_calibration_eviction", position, "\"evicted_profile\":" + jsonString(*outcome->evictedProfile) +
		     ",\"replacement_profile\":" + jsonString(verification.pending.name + "\n" + verification.pending.targetClass));
	}
	const char* reason = playerBotSpellEvidenceName(verification.evidence);
	const char* fallback = verification.success ? nullptr : verification.pending.role == PlayerBotSpellRole::Healing ? "small_health_potion" :
	                       verification.pending.role == PlayerBotSpellRole::Support ? "continue_route" : "normal_melee";
	emitSpellCastEvent(position, descriptor ? descriptor->name : nullptr, descriptor ? descriptor->words : nullptr,
	                   descriptor ? playerBotSpellRoleName(descriptor->role) : nullptr, verification.pending.need.c_str(),
	                   verification.success ? "success" : "failed", verification.manaSpent ? "accepted" : "rejected", reason,
	                   &verification.pending, &player, fallback);
	if (!verification.success) {
		telemetry.recordActionFailure();
		survivalRuntime.deferSpellRetry(input.observedAt);
	} else if (verification.pending.role == PlayerBotSpellRole::Healing && verification.evidence == PlayerBotSpellEvidence::Accepted) {
		recordHuntRecovery(false);
	}
}


void PlayerBotController::finishMagicTraining(Player& player, const Position& position, const char* result, const char* reason)
{
	progressionRuntime.setCooldown(TopLevelGoal::MagicTraining, magicTrainingRetryDelay);
	if (goalArbiter.activeGoal() == TopLevelGoal::MagicTraining) {
		emit("goal_result", position, "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
		     ",\"goal\":\"magic_training\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
		if (selectTopLevelGoal(player, position, "magic_training_complete")) {
			schedule(SCHEDULER_MINTICKS);
		}
	}
}

bool PlayerBotController::processMagicTraining(Player& player, const Position& position)
{
	const PlayerBotSurvivalSnapshot snapshot = survivalSnapshot(player);
	const char* reason = survivalRuntime.magicTrainingReason(player, snapshot);
	const std::optional<PlayerBotMagicTrainingCommand> selected = !reason ? survivalRuntime.decideMagicTraining(player, snapshot) : std::nullopt;
	if (!selected) {
		finishMagicTraining(player, position, "skipped", reason ? reason : "opportunity_lost");
		return false;
	}
	const uint64_t manaBefore = selected->manaBefore;
	const uint64_t manaSpentBefore = selected->manaSpentBefore;
	const uint32_t magicLevelBefore = selected->magicLevelBefore;
	const uint64_t predictedMana = selected->predictedMana;
	const uint64_t wastedMana = selected->wastedMana;
	telemetry.recordActionAttempt();
	std::ostringstream request;
	request << "\"action\":\"magic_training\",\"result\":\"requested\",\"source\":\"engine_path\""
	        << ",\"spell\":" << jsonString(selected->descriptor->name) << ",\"audited_priority\":"
	        << static_cast<uint32_t>(selected->descriptor->magicTrainingPriority) << ",\"refresh\":"
	        << (selected->refresh ? "true" : "false") << ",\"mana_before\":" << manaBefore
	        << ",\"mana_max\":" << player.getMaxMana() << ",\"mana_gain\":" << selected->manaGain
	        << ",\"mana_tick_interval\":" << selected->manaTickInterval << ",\"mana_tick_remaining\":" << selected->manaTickRemaining
	        << ",\"predicted_mana\":" << predictedMana << ",\"wasted_mana\":" << wastedMana
	        << ",\"mana_cost\":" << selected->cost << ",\"emergency_reserve\":" << magicTrainingEmergencyReserve;
	emit("action_result", position, request.str());
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", selected->spell->getWords());
	uint64_t manaAfter = player.getMana();
	const uint64_t manaSpentAfter = player.getSpentMana();
	const uint32_t magicLevelAfter = player.getBaseMagicLevel();
	manaAfter = fixtureDriver.observedMagicTrainingMana(manaAfter);
	const uint64_t manaDelta = manaBefore >= manaAfter ? manaBefore - manaAfter : UINT64_MAX;
	const bool progressed = magicLevelAfter > magicLevelBefore ||
	                        (magicLevelAfter == magicLevelBefore && manaSpentAfter > manaSpentBefore);
	const bool verified = manaDelta == selected->cost && progressed;
	std::ostringstream result;
	result << "\"action\":\"magic_training\",\"result\":" << jsonString(verified ? "success" : "failed")
	       << ",\"source\":\"engine_verification\",\"engine_result\":" << jsonString(verified ? "accepted" : "rejected")
	       << ",\"spell\":" << jsonString(selected->descriptor->name) << ",\"mana_before\":" << manaBefore
	       << ",\"mana_after\":" << manaAfter << ",\"mana_cost\":" << selected->cost << ",\"mana_delta\":" << manaDelta
	       << ",\"mana_spent_before\":" << manaSpentBefore << ",\"mana_spent_after\":" << manaSpentAfter
	       << ",\"magic_level_before\":" << magicLevelBefore << ",\"magic_level_after\":" << magicLevelAfter
	       << ",\"emergency_reserve\":" << magicTrainingEmergencyReserve << ",\"mana_gain\":" << selected->manaGain
	       << ",\"mana_tick_interval\":" << selected->manaTickInterval << ",\"mana_tick_remaining\":" << selected->manaTickRemaining
	       << ",\"predicted_mana\":" << predictedMana << ",\"wasted_mana\":" << wastedMana;
	emit("action_result", position, result.str());
	if (!verified) telemetry.recordActionFailure();
	finishMagicTraining(player, position, verified ? "success" : "failed", verified ? "cast_verified" : "cast_verification_failed");
	return false;
}

bool PlayerBotController::trySupportSpell(Player* player, const Position& currentPosition)
{
	if (!player) return false;
	return dispatchSpellCommand(*player, currentPosition,
	    survivalRuntime.decideSupportSpell(*player, survivalSnapshot(*player), currentPosition, std::chrono::steady_clock::now()));
}

bool PlayerBotController::tryOffensiveSpell(Player* player, const Position& currentPosition)
{
	if (!player) return false;
	const auto traversalTarget = combatRuntime.traversalTarget();
	Creature* target = traversalTarget ? g_game.getCreatureByID(traversalTarget->id) : nullptr;
	return dispatchSpellCommand(*player, currentPosition,
	    survivalRuntime.decideOffensiveSpell(*player, survivalSnapshot(*player), currentPosition, target, std::chrono::steady_clock::now()));
}

uint64_t PlayerBotController::spellTrainingReserve(const Player& player) const
{
	uint32_t potionPrice = std::numeric_limits<uint32_t>::max();
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || *capability != "shop") {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.itemId == smallHealthPotionItemId && offer.buyPrice != 0) {
				potionPrice = std::min(potionPrice, offer.buyPrice);
			}
		}
	}
	if (potionPrice == std::numeric_limits<uint32_t>::max()) {
		return std::numeric_limits<uint64_t>::max();
	}
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t potionGap = potionCount < smallHealthPotionRestockTarget ?
	                               smallHealthPotionRestockTarget - potionCount : 0;
	return carriedGoldReserve + static_cast<uint64_t>(potionGap) * potionPrice;
}

void PlayerBotController::emitSpellCandidate(const Npc& npc, const NpcSpellOffer& offer, const Position& position,
                                             const char* result, const char* reason, uint64_t reserve,
                                             uint32_t travelSteps) const
{
	std::ostringstream fields;
	fields << "\"goal\":\"learn_spell\",\"result\":" << jsonString(result)
	       << ",\"npc_id\":" << npc.getID() << ",\"npc_name\":" << jsonString(npc.getName())
	       << ",\"spell\":" << jsonString(offer.spellName) << ",\"keyword\":" << jsonString(offer.keyword)
	       << ",\"price\":" << offer.price << ",\"level\":" << offer.level
	       << ",\"premium\":" << (offer.premium ? "true" : "false") << ",\"reserve\":" << reserve
	       << ",\"travel_steps\":" << travelSteps << ",\"provider_position\":{\"x\":" << npc.getPosition().x
	       << ",\"y\":" << npc.getPosition().y << ",\"z\":" << static_cast<uint16_t>(npc.getPosition().z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("spell_candidate", position, fields.str());
}

bool PlayerBotController::findSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan& plan,
                                            std::deque<PlayerBotNavigationStep>& selectedSteps)
{
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const uint16_t vocationId = player.getVocationId();
	const uint16_t baseVocationId = player.getVocation()->getFromVocation() == 0 ? vocationId :
	                               player.getVocation()->getFromVocation();
	const bool suppliesReady = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold;
	bool found = false;

	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || *capability != "spell_trainer") {
			continue;
		}
		const bool inScope = serviceDistance(player.getTemplePosition(), {npc->getID(), npc->getPosition()}) <=
		                     maximumSpellTrainerDistanceFromTemple;
		emit("spell_trainer_discovered", position, "\"npc_id\":" + std::to_string(npc->getID()) +
		     ",\"npc_name\":" + jsonString(npc->getName()) + ",\"offers\":" +
		     std::to_string(npc->getSpellOffers().size()) + ",\"in_scope\":" + (inScope ? "true" : "false"));
		bool routeEvaluated = false;
		bool routeReachable = false;
		Position trainerApproach;
		std::deque<PlayerBotNavigationStep> trainerSteps;
		auto findTrainerApproach = [&]() {
			if (routeEvaluated) {
				return routeReachable;
			}
			routeEvaluated = true;
			std::vector<Position> approaches;
			for (int32_t xOffset = -3; xOffset <= 3; ++xOffset) {
				for (int32_t yOffset = -3; yOffset <= 3; ++yOffset) {
					if (xOffset != 0 || yOffset != 0) {
						approaches.emplace_back(npc->getPosition().x + xOffset, npc->getPosition().y + yOffset,
						                        npc->getPosition().z);
					}
				}
			}
			std::sort(approaches.begin(), approaches.end(), [&position, npc](const Position& left, const Position& right) {
				const int32_t leftNpcDistance = std::max(Position::getDistanceX(npc->getPosition(), left),
				                                             Position::getDistanceY(npc->getPosition(), left));
				const int32_t rightNpcDistance = std::max(Position::getDistanceX(npc->getPosition(), right),
				                                              Position::getDistanceY(npc->getPosition(), right));
				if (leftNpcDistance != rightNpcDistance) {
					return leftNpcDistance < rightNpcDistance;
				}
				const int32_t leftDistance = std::max(Position::getDistanceX(position, left), Position::getDistanceY(position, left));
				const int32_t rightDistance = std::max(Position::getDistanceX(position, right), Position::getDistanceY(position, right));
				return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
			});
			for (const Position& approach : approaches) {
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
					continue;
				}
				std::deque<PlayerBotNavigationStep> steps;
				uint64_t expandedNodes = 0;
				const auto startedAt = std::chrono::steady_clock::now();
				const PlayerBotNavigationRoutePlan routePlan = approach == position ? PlayerBotNavigationRoutePlan{} :
					navigationRuntime.plan(player, approach);
				const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
				if (approach != position) {
					steps = routePlan.steps;
					expandedNodes = routePlan.metrics.expandedNodes;
				}
				telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - startedAt), result == PlayerBotNavigationResult::Reached);
				if (result != PlayerBotNavigationResult::Reached || (approach != position && steps.empty())) {
					continue;
				}
				trainerApproach = approach;
				trainerSteps = std::move(steps);
				routeReachable = true;
				return true;
			}
			return false;
		};
		for (const NpcSpellOffer& offer : npc->getSpellOffers()) {
			if (!inScope) {
				emitSpellCandidate(*npc, offer, position, "rejected", "outside_thais_scope", reserve);
				continue;
			}
			Spell* spell = g_spells ? g_spells->getSpellByName(offer.spellName) : nullptr;
			if (!spell || !spell->isInstant() || !spell->isLearnable() || spell->getLevel() != offer.level ||
			    spell->isPremium() != offer.premium) {
				emitSpellCandidate(*npc, offer, position, "rejected", "spell_registry_mismatch", reserve);
				continue;
			}
			if (std::find(offer.vocationIds.begin(), offer.vocationIds.end(), baseVocationId) == offer.vocationIds.end() ||
			    spell->getVocMap().find(vocationId) == spell->getVocMap().end()) {
				emitSpellCandidate(*npc, offer, position, "rejected", "vocation_ineligible", reserve);
				continue;
			}
			if (player.getLevel() < offer.level) {
				emitSpellCandidate(*npc, offer, position, "rejected", "level_ineligible", reserve);
				continue;
			}
			if (offer.premium && !player.isPremium()) {
				emitSpellCandidate(*npc, offer, position, "rejected", "premium_ineligible", reserve);
				continue;
			}
			if (player.hasLearnedInstantSpell(offer.spellName)) {
				emitSpellCandidate(*npc, offer, position, "rejected", "already_learned", reserve);
				continue;
			}
			if (!suppliesReady) {
				emitSpellCandidate(*npc, offer, position, "rejected", "supply_reserve_unmet", reserve);
				continue;
			}
			if (reserve == std::numeric_limits<uint64_t>::max()) {
				emitSpellCandidate(*npc, offer, position, "rejected", "recovery_reserve_unavailable", reserve);
				continue;
			}
			if (totalMoney < reserve + offer.price) {
				emitSpellCandidate(*npc, offer, position, "rejected", "unaffordable_after_reserves", reserve);
				continue;
			}

			if (!findTrainerApproach()) {
				emitSpellCandidate(*npc, offer, position, "rejected", "trainer_unreachable", reserve);
				continue;
			}
			PlayerBotSpellTrainingPlan candidate{npc->getID(), npc->getPosition(), trainerApproach, offer.spellName, offer.keyword,
			                            offer.price, offer.level, static_cast<uint32_t>(trainerSteps.size()), reserve};
			emitSpellCandidate(*npc, offer, position, "feasible", nullptr, reserve, candidate.travelSteps);
			if (!found || candidate.price < plan.price ||
			    (candidate.price == plan.price && (candidate.travelSteps < plan.travelSteps ||
			     (candidate.travelSteps == plan.travelSteps && candidate.spellName < plan.spellName)))) {
				plan = std::move(candidate);
				selectedSteps = trainerSteps;
				found = true;
			}
		}
	}
	return found;
}

void PlayerBotController::beginSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan plan,
                                             std::deque<PlayerBotNavigationStep> steps)
{
	progressionRuntime.beginSpellTraining(std::move(plan));
	const auto& training = spellTrainingSession.plan();
	serviceWorkflow.resetNpc(training.npcId);
	navigationRuntime.adopt(training.approachPosition, std::move(steps));
	emit("strategy_selection", position, "\"goal\":\"learn_spell\",\"npc_id\":" +
	     std::to_string(training.npcId) + ",\"spell\":" + jsonString(training.spellName) +
	     ",\"keyword\":" + jsonString(training.keyword) + ",\"price\":" +
	     std::to_string(training.price) + ",\"reserve\":" + std::to_string(training.reserve) +
	     ",\"travel_steps\":" + std::to_string(training.travelSteps));
	say(player, "Going to learn " + training.spellName + ".");
}

void PlayerBotController::finishSpellTraining(Player* player, const Position& position, const char* result, const char* reason)
{
	emit("strategy_objective_result", position, "\"goal\":\"learn_spell\",\"spell\":" +
	     jsonString(spellTrainingSession.plan().spellName) + ",\"result\":" + jsonString(result) + ",\"reason\":" +
	     jsonString(reason));
	emit("goal_result", position, "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
	     ",\"goal\":\"learn_spell\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, "Spell training " + std::string(result) + ": " + reason + '.');
	}
	progressionRuntime.finish();
	serviceWorkflow.resetNpc();
	clearNavigation();
	progressionRuntime.setCooldown(TopLevelGoal::LearnSpell,
	                       std::strcmp(result, "success") == 0 ? spellTrainingSuccessCooldown : spellTrainingFailureCooldown);
	if (player && fixtureDriver.progressionEnabled()) {
		selectTopLevelGoal(*player, position, std::strcmp(result, "success") == 0 ? "spell_training_complete" : "spell_training_failed");
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processSpellTraining(Player* player, const Position& currentPosition)
{
	const auto& training = spellTrainingSession.plan();
	Npc* trainer = g_game.getNpcByID(training.npcId);
	PlayerBotSpellTrainingObservation observation;
	observation.totalMoney = player->getMoney() + player->getBankBalance();
	if (spellTrainingSession.stage() == PlayerBotSpellTrainingStage::Travel) {
		observation.navigationReached = processNavigation(player, currentPosition, training.approachPosition);
		observation.navigationFailed = navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts;
	} else {
		observation.npcAvailable = trainer && !trainer->isRemoved() && Position::areInRange<3, 3, 0>(currentPosition, trainer->getPosition());
		observation.greetingAcknowledged = serviceWorkflow.isGreetingAcknowledged();
		observation.learned = player->hasLearnedInstantSpell(training.spellName);
	}
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceSpellTraining(observation);
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded) {
		emit("action_result", currentPosition, "\"action\":\"learn_spell\",\"result\":\"success\",\"spell\":" +
	     jsonString(training.spellName) + ",\"price\":" + std::to_string(training.price) +
	     ",\"money_before\":" + std::to_string(spellTrainingSession.moneyBefore()) + ",\"money_after\":" +
	     std::to_string(observation.totalMoney));
		finishSpellTraining(player, currentPosition, "success", result.reason);
		return;
	}
	if (result.type == PlayerBotProgressionOutcomeType::Failed) {
		finishSpellTraining(player, currentPosition, "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Speak) {
		if (!trainer || trainer->isRemoved()) return;
		const char* words = std::strcmp(result.command.reason, "request") == 0 ? training.keyword.c_str() : result.command.reason;
		if (std::strcmp(words, "hi") == 0) serviceWorkflow.resetGreetingAcknowledgement();
		telemetry.recordActionAttempt();
		trainer->receiveSpeech(player, TALKTYPE_PRIVATE_PN, words);
	}
	if (result.command.type != PlayerBotProgressionCommandType::Navigate) schedule(result.command.type == PlayerBotProgressionCommandType::Speak ? 1000 : SCHEDULER_MINTICKS);
}
