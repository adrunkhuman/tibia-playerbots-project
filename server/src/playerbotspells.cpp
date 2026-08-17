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
#include "spells.h"

// Runtime spell-trainer discovery and normal NPC learning dialogue.
using namespace playerbot;

extern Spells* g_spells;

namespace {
	constexpr uint32_t maximumSpellTrainerDistanceFromTemple = 200;
	constexpr uint32_t higherPriorityRecoveryManaReserve = 20;
	constexpr uint32_t minimumHasteRouteSteps = 20;
	constexpr int32_t smallHealthPotionMaximumHealing = 90;

	enum class KnightSpellRole : uint8_t {
		Healing,
		Support,
		MeleeOffense,
		RangedOffense,
	};

	struct AuditedKnightSpellDescriptor {
		const char* name;
		const char* words;
		KnightSpellRole role;
	};

	constexpr std::array<AuditedKnightSpellDescriptor, 4> auditedKnightSpells = {{
		{"Light Healing", "exura", KnightSpellRole::Healing},
		{"Haste", "utani hur", KnightSpellRole::Support},
		{"Berserk", "exori", KnightSpellRole::MeleeOffense},
		{"Whirlwind Throw", "exori hur", KnightSpellRole::RangedOffense},
	}};

	const AuditedKnightSpellDescriptor* findAuditedKnightSpell(const char* name)
	{
		auto it = std::find_if(auditedKnightSpells.begin(), auditedKnightSpells.end(), [name](const auto& descriptor) {
			return std::strcmp(descriptor.name, name) == 0;
		});
		return it == auditedKnightSpells.end() ? nullptr : &*it;
	}

	const char* roleName(KnightSpellRole role)
	{
		switch (role) {
			case KnightSpellRole::Healing: return "healing";
			case KnightSpellRole::Support: return "support";
			case KnightSpellRole::MeleeOffense: return "melee_offense";
			case KnightSpellRole::RangedOffense: return "ranged_offense";
		}
		return "unsupported";
	}

	const char* fallbackForRole(KnightSpellRole role)
	{
		return role == KnightSpellRole::Healing ? "small_health_potion" :
		       role == KnightSpellRole::Support ? "continue_route" : "normal_melee";
	}

	const char* fallbackForNeed(const char* need)
	{
		return std::strcmp(need, "recovery") == 0 ? "small_health_potion" :
		       std::strcmp(need, "safe_route") == 0 ? "continue_route" : "normal_melee";
	}

}

void PlayerBotController::emitSpellCastEvent(const Position& position, const char* spellName, const char* words, const char* role,
                                             const char* need, const char* result, const char* engineResult, const char* reason,
                                             const PendingSpellCast* pending, const Player* player, const char* fallback) const
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
			fields << ",\"target_id\":" << pending->targetId
			       << ",\"target_health_before\":" << pending->targetHealthBefore;
		}
	}
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	fields << ",\"fallback\":" << (fallback ? jsonString(fallback) : "null");
	emit("action_result", position, fields.str());
}

bool PlayerBotController::startSpellCast(Player& player, const Position& position, const char* spellName, const char* need,
                                         Creature* target)
{
	const AuditedKnightSpellDescriptor* descriptor = findAuditedKnightSpell(spellName);
	if (!descriptor) {
		emitSpellCastEvent(position, nullptr, nullptr, nullptr, need, "skipped", "not_attempted", "unsupported_descriptor", nullptr,
		                   &player, fallbackForNeed(need));
		return false;
	}
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName(descriptor->name) : nullptr;
	if (!spell || spell->getWords() != descriptor->words || !spell->isLearnable()) {
		emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
		                   "not_attempted", "unsupported_metadata", nullptr, &player, fallbackForRole(descriptor->role));
		return false;
	}
	if (!player.hasLearnedInstantSpell(descriptor->name)) {
		if (shouldEmitRepeated("cast_spell:unlearned:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
			                   "not_attempted", "unlearned", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}
	if (!player.canDoAction() || !pendingSpellCast.name.empty()) {
		return false;
	}
	const bool healingGroup = descriptor->role == KnightSpellRole::Healing || descriptor->role == KnightSpellRole::Support;
	if (player.hasCondition(healingGroup ? CONDITION_EXHAUST_HEAL : CONDITION_EXHAUST_COMBAT)) {
		if (shouldEmitRepeated("cast_spell:cooldown:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
			                   "not_attempted", "cooldown", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}
	if ((descriptor->role == KnightSpellRole::MeleeOffense || descriptor->role == KnightSpellRole::RangedOffense) &&
		(!target || target->isRemoved() || target->isDead() || player.getAttackedCreature() != target ||
		 !player.canSeeCreature(target) || !player.canSee(target->getPosition()) ||
		 !Position::areInRange<1, 1, 0>(position, target->getPosition()))) {
		if (shouldEmitRepeated("cast_spell:lost_target:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
			                   "not_attempted", "lost_target", nullptr, &player, "normal_melee");
		}
		return false;
	}
	if (spell->getNeedTarget() && !spell->canThrowSpell(&player, target)) {
		if (shouldEmitRepeated("cast_spell:target_unreachable:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
			                   "not_attempted", "target_unreachable", nullptr, &player, "normal_melee");
		}
		return false;
	}
	const uint32_t manaCost = spell->getManaCost(&player);
	const uint32_t reserve = descriptor->role == KnightSpellRole::Healing ? 0 : higherPriorityRecoveryManaReserve;
	if (player.getMana() < manaCost + reserve) {
		if (shouldEmitRepeated("cast_spell:insufficient_mana_reserve:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "skipped",
			                   "not_attempted", "insufficient_mana_reserve", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}

	pendingSpellCast = {descriptor->name, roleName(descriptor->role), need, player.getMana(), reserve, player.getHealth(),
	                    target ? target->getID() : 0, target ? target->getHealth() : 0};
	++counters.actionsAttempted;
	emitSpellCastEvent(position, descriptor->name, descriptor->words, roleName(descriptor->role), need, "requested", "unchecked",
	                   nullptr, &pendingSpellCast, &player, nullptr);
	// Route through the normal player speech handler so the live spell engine owns legality and costs.
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", spell->getWords());
	return true;
}

void PlayerBotController::verifySpellCast(Player& player, const Position& position)
{
	if (pendingSpellCast.name.empty()) {
		return;
	}
	const AuditedKnightSpellDescriptor* descriptor = findAuditedKnightSpell(pendingSpellCast.name.c_str());
	const bool manaSpent = player.getMana() < pendingSpellCast.manaBefore;
	bool observed = false;
	if (pendingSpellCast.role == "healing") {
		observed = player.getHealth() > pendingSpellCast.healthBefore;
	} else if (pendingSpellCast.role == "support") {
		observed = player.hasCondition(CONDITION_HASTE);
	} else {
		Creature* target = g_game.getCreatureByID(pendingSpellCast.targetId);
		observed = !target || target->isRemoved() || target->isDead() || target->getHealth() < pendingSpellCast.targetHealthBefore;
	}
	const bool success = manaSpent && observed;
	const char* reason = success ? nullptr : !manaSpent ? "cast_not_verified" : "ineffective_result";
	const char* fallback = success ? nullptr : pendingSpellCast.role == "healing" ? "small_health_potion" :
	                       pendingSpellCast.role == "support" ? "continue_route" : "normal_melee";
	emitSpellCastEvent(position, descriptor ? descriptor->name : nullptr, descriptor ? descriptor->words : nullptr,
	                   descriptor ? roleName(descriptor->role) : nullptr, pendingSpellCast.need.c_str(),
	                   success ? "success" : "failed", manaSpent ? "accepted" : "rejected", reason,
	                   &pendingSpellCast, &player, fallback);
	if (!success) {
		++counters.actionsFailed;
		spellRetryAfter = std::chrono::steady_clock::now() + healingRetryInterval;
	} else if (pendingSpellCast.role == "healing") {
		recordHuntRecovery(false);
	}
	pendingSpellCast = PendingSpellCast{};
}

bool PlayerBotController::handleSpellHealing(Player* player, const Position& currentPosition)
{
	if (!player || !pendingSpellCast.name.empty() || std::chrono::steady_clock::now() < spellRetryAfter) {
		return false;
	}
	const int32_t missingHealth = player->getMaxHealth() - player->getHealth();
	if (missingHealth > smallHealthPotionMaximumHealing && getInventoryItemCount(*player, smallHealthPotionItemId) != 0) {
		return false;
	}
	return startSpellCast(*player, currentPosition, "Light Healing", "recovery");
}

bool PlayerBotController::trySupportSpell(Player* player, const Position& currentPosition)
{
	if (!player || !pendingSpellCast.name.empty() || player->hasCondition(CONDITION_HASTE) ||
		std::chrono::steady_clock::now() < spellRetryAfter || navigationSteps.size() < minimumHasteRouteSteps ||
		needsHealing(*player)) {
		return false;
	}
	return startSpellCast(*player, currentPosition, "Haste", "safe_route");
}

bool PlayerBotController::tryOffensiveSpell(Player* player, const Position& currentPosition)
{
	if (!player || !pendingSpellCast.name.empty() || std::chrono::steady_clock::now() < spellRetryAfter || needsHealing(*player)) {
		return false;
	}
	Creature* target = g_game.getCreatureByID(ratId);
	if (player->hasLearnedInstantSpell("Berserk") && player->getLevel() >= 35) {
		return startSpellCast(*player, currentPosition, "Berserk", "offense", target);
	}
	return startSpellCast(*player, currentPosition, "Whirlwind Throw", "offense", target);
}

uint64_t PlayerBotController::spellTrainingReserve(const Player& player) const
{
	uint32_t potionPrice = std::numeric_limits<uint32_t>::max();
	uint32_t foodPrice = std::numeric_limits<uint32_t>::max();
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!capability || *capability != "shop") {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.itemId == smallHealthPotionItemId && offer.buyPrice != 0) {
				potionPrice = std::min(potionPrice, offer.buyPrice);
			} else if (offer.itemId == meatItemId && offer.buyPrice != 0) {
				foodPrice = std::min(foodPrice, offer.buyPrice);
			}
		}
	}
	if (potionPrice == std::numeric_limits<uint32_t>::max() || foodPrice == std::numeric_limits<uint32_t>::max()) {
		return std::numeric_limits<uint64_t>::max();
	}
	return carriedGoldReserve + static_cast<uint64_t>(minimumSmallHealthPotions) * potionPrice +
	       static_cast<uint64_t>(minimumMeat) * foodPrice;
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

bool PlayerBotController::findSpellTraining(Player& player, const Position& position, SpellTrainingPlan& plan,
                                            std::deque<PlayerBotNavigationStep>& selectedSteps)
{
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const uint16_t vocationId = player.getVocationId();
	const uint16_t baseVocationId = player.getVocation()->getFromVocation() == 0 ? vocationId :
	                               player.getVocation()->getFromVocation();
	const bool suppliesReady = getInventoryItemCount(player, smallHealthPotionItemId) >= minimumSmallHealthPotions &&
	                           getInventoryItemCount(player, meatItemId) >= minimumMeat;
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
			std::sort(approaches.begin(), approaches.end(), [&position](const Position& left, const Position& right) {
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
				++counters.pathfindingCalls;
				const auto startedAt = std::chrono::steady_clock::now();
				const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached :
				                                        navigator.plan(player, approach, {}, steps, expandedNodes);
				counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - startedAt).count();
				if (result != PlayerBotNavigationResult::Reached || (approach != position && steps.empty())) {
					++counters.pathfindingFailures;
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
			SpellTrainingPlan candidate{npc->getID(), npc->getPosition(), trainerApproach, offer.spellName, offer.keyword,
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

void PlayerBotController::beginSpellTraining(Player& player, const Position& position, SpellTrainingPlan plan,
                                             std::deque<PlayerBotNavigationStep> steps)
{
	spellTrainingPlan = std::move(plan);
	progressionObjective = ProgressionObjective::LearnSpell;
	spellTrainingStage = SpellTrainingStage::Travel;
	progressionAttempts = 0;
	serviceTargetId = spellTrainingPlan.npcId;
	serviceGreetingAcknowledged = false;
	navigationTarget = spellTrainingPlan.approachPosition;
	navigationSteps = std::move(steps);
	emit("strategy_selection", position, "\"goal\":\"learn_spell\",\"npc_id\":" +
	     std::to_string(spellTrainingPlan.npcId) + ",\"spell\":" + jsonString(spellTrainingPlan.spellName) +
	     ",\"keyword\":" + jsonString(spellTrainingPlan.keyword) + ",\"price\":" +
	     std::to_string(spellTrainingPlan.price) + ",\"reserve\":" + std::to_string(spellTrainingPlan.reserve) +
	     ",\"travel_steps\":" + std::to_string(spellTrainingPlan.travelSteps));
	say(player, "Going to learn " + spellTrainingPlan.spellName + ".");
}

void PlayerBotController::finishSpellTraining(Player* player, const Position& position, const char* result, const char* reason)
{
	emit("strategy_objective_result", position, "\"goal\":\"learn_spell\",\"spell\":" +
	     jsonString(spellTrainingPlan.spellName) + ",\"result\":" + jsonString(result) + ",\"reason\":" +
	     jsonString(reason));
	emit("goal_result", position, "\"decision_id\":" + std::to_string(goalDecisionId) +
	     ",\"goal\":\"learn_spell\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, "Spell training " + std::string(result) + ": " + reason + '.');
	}
	progressionObjective = ProgressionObjective::None;
	spellTrainingStage = SpellTrainingStage::Travel;
	spellTrainingPlan = SpellTrainingPlan{};
	serviceTargetId = 0;
	clearNavigation();
	spellTrainingCooldownUntil = std::chrono::steady_clock::now() +
	                                  (std::strcmp(result, "success") == 0 ? spellTrainingSuccessCooldown : spellTrainingFailureCooldown);
	if (player && testPolicy.progressionEnabled) {
		selectTopLevelGoal(*player, position, std::strcmp(result, "success") == 0 ? "spell_training_complete" : "spell_training_failed");
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processSpellTraining(Player* player, const Position& currentPosition)
{
	if (spellTrainingStage == SpellTrainingStage::Travel) {
		if (!processNavigation(player, currentPosition, spellTrainingPlan.approachPosition)) {
			if (fixedTargetRouteFailureCount >= maximumProgressionAttempts) {
				finishSpellTraining(player, currentPosition, "failed", "route_unavailable");
			}
			return;
		}
		spellTrainingStage = SpellTrainingStage::Greet;
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	Npc* trainer = g_game.getNpcByID(spellTrainingPlan.npcId);
	if (!trainer || trainer->isRemoved() || !Position::areInRange<3, 3, 0>(currentPosition, trainer->getPosition())) {
		finishSpellTraining(player, currentPosition, "failed", "trainer_unavailable");
		return;
	}
	if (spellTrainingStage == SpellTrainingStage::Greet) {
		serviceGreetingAcknowledged = false;
		++counters.actionsAttempted;
		trainer->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
		spellTrainingStage = SpellTrainingStage::Request;
		schedule(1000);
		return;
	}
	if (spellTrainingStage == SpellTrainingStage::Request) {
		if (!serviceGreetingAcknowledged) {
			if (++progressionAttempts >= maximumProgressionAttempts) {
				finishSpellTraining(player, currentPosition, "failed", "trainer_focus_unconfirmed");
				return;
			}
			spellTrainingStage = SpellTrainingStage::Greet;
			schedule(1000);
			return;
		}
		++counters.actionsAttempted;
		trainer->receiveSpeech(player, TALKTYPE_PRIVATE_PN, spellTrainingPlan.keyword);
		spellTrainingStage = SpellTrainingStage::Confirm;
		schedule(1000);
		return;
	}
	if (spellTrainingStage == SpellTrainingStage::Confirm) {
		spellTrainingPlan.moneyBefore = player->getMoney() + player->getBankBalance();
		++counters.actionsAttempted;
		trainer->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "yes");
		spellTrainingStage = SpellTrainingStage::Verify;
		schedule(1000);
		return;
	}

	const uint64_t totalMoney = player->getMoney() + player->getBankBalance();
	const bool learned = player->hasLearnedInstantSpell(spellTrainingPlan.spellName);
	if (learned && spellTrainingPlan.moneyBefore >= spellTrainingPlan.price &&
	    totalMoney == spellTrainingPlan.moneyBefore - spellTrainingPlan.price) {
		emit("action_result", currentPosition, "\"action\":\"learn_spell\",\"result\":\"success\",\"spell\":" +
	     jsonString(spellTrainingPlan.spellName) + ",\"price\":" + std::to_string(spellTrainingPlan.price) +
	     ",\"money_before\":" + std::to_string(spellTrainingPlan.moneyBefore) + ",\"money_after\":" +
	     std::to_string(totalMoney));
		finishSpellTraining(player, currentPosition, "success", "learned_state_and_payment_verified");
		return;
	}
	if (learned || totalMoney != spellTrainingPlan.moneyBefore) {
		finishSpellTraining(player, currentPosition, "failed", "transaction_delta_mismatch");
		return;
	}
	if (++progressionAttempts >= maximumProgressionAttempts) {
		finishSpellTraining(player, currentPosition, "failed", "learning_not_verified");
		return;
	}
	spellTrainingStage = SpellTrainingStage::Greet;
	schedule(1000);
}
