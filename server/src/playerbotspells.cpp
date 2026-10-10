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
#include "playerbotnpccapabilities.h"
#include "playerbotspellcalibration.h"
#include "playerbotplanningbudget.h"
#include "condition.h"
#include "spells.h"
#include "groups.h"
#include "guild.h"
#include <iomanip>

// Runtime spell-trainer discovery and normal NPC learning dialogue.
using namespace playerbot;

extern Spells* g_spells;

namespace {
	constexpr auto magicTrainingRetryDelay = std::chrono::seconds(2);
	constexpr size_t maximumSpellTrainerRoutes = 4;
	bool trainerFareLimited(Player& player, uint64_t protectedFunds)
	{
		const uint64_t funds = player.getMoney() + player.getBankBalance();
		const uint64_t spendable = funds >= protectedFunds ? funds - protectedFunds : 0;
		for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Travel, player.getPosition())) {
			for (const auto& offer : npc->getTravelOffers()) {
				if (offer.price > spendable && playerBotNpcTravelOfferEligible(player.getLevel(), player.isPremium(),
				    UINT64_MAX, offer.level, offer.premium, offer.price, offer.hasOpaqueCondition, offer.hasOpaqueAction))
					return true;
			}
		}
		return false;
	}

	std::vector<Position> trainerApproaches(Player& player, const Npc& npc)
	{
		std::vector<Position> tiles;
		for (int x = -3; x <= 3; ++x) for (int y = -3; y <= 3; ++y) {
			if (!x && !y) continue;
			const Position p(npc.getPosition().x + x, npc.getPosition().y + y, npc.getPosition().z);
			if (playerBotStableApproachTile(g_game.map.getTile(p), player)) tiles.push_back(p);
		}
		std::sort(tiles.begin(), tiles.end());
		return tiles;
	}

	const char* healthPotionFallback(uint16_t itemId)
	{
		return itemId == smallHealthPotionItemId ? "small_health_potion" : "health_potion";
	}

	const char* fallbackForRole(PlayerBotSpellRole role, uint16_t potionItemId)
	{
		return role == PlayerBotSpellRole::Healing ? healthPotionFallback(potionItemId) :
		       role == PlayerBotSpellRole::Support ? "continue_route" : "normal_melee";
	}

	const char* fallbackForNeed(const char* need, uint16_t potionItemId)
	{
		return std::strcmp(need, "recovery") == 0 ? healthPotionFallback(potionItemId) :
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
                                              const PlayerBotSpellPendingCast* pending, const Player* player, const char* fallback,
                                              std::optional<uint32_t> manaReserve) const
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
		if (const auto profile = survivalRuntime.calibrationProfile(*pending)) {
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
	// Static per learned spell set, so skip throttling still sees few distinct records.
	if (manaReserve) fields << ",\"mana_reserve\":" << *manaReserve;
	fields << ",\"fallback\":" << (fallback ? jsonString(fallback) : "null");
	if (!pending && std::strcmp(result, "skipped") == 0) {
		// Walking and combat turns re-check spells several times a second; an
		// unchanged skip is repeated at most once per summary interval.
		// Skip records hold only static spell, need, and reason fields, so the
		// set of distinct records stays small.
		const size_t fingerprint = std::hash<std::string>{}(fields.str());
		auto& throttle = spellSkipRecords.try_emplace(fingerprint, summaryInterval).first->second;
		const auto suppressed = throttle.admit(std::chrono::steady_clock::now());
		if (!suppressed) return;
		fields << ",\"suppressed_repeats\":" << *suppressed;
	}
	emit("action_result", position, fields.str());
}

bool PlayerBotController::dispatchSpellCommand(Player& player, const Position& position, PlayerBotSurvivalCommand command)
{
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	if (command.type != PlayerBotSurvivalCommandType::CastSpell || !command.spell) {
		if (command.reason.empty()) return false;
		const PlayerBotSpellDescriptor* descriptor = command.candidateName.empty() ? nullptr :
		                                              playerBotSpellDescriptor(command.candidateName.c_str());
		emitSpellCastEvent(position, descriptor ? descriptor->name : nullptr, descriptor ? descriptor->words : nullptr,
		                   descriptor ? playerBotSpellRoleName(descriptor->role) : nullptr, command.need.empty() ? "unknown" : command.need.c_str(),
		                   "skipped", "not_attempted", command.reason.c_str(), nullptr, &player,
		                   descriptor ? fallbackForRole(descriptor->role, potionItemId) : fallbackForNeed(command.need.c_str(), potionItemId),
		                   command.manaReserve);
		return false;
	}
	const auto& cast = *command.spell;
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName(cast.name) : nullptr;
	if (!spell || spell->getWords() != cast.words) {
		telemetry.recordActionFailure();
		emitSpellCastEvent(position, cast.name.c_str(), cast.words.c_str(), playerBotSpellRoleName(cast.role),
		                   cast.pending.need.c_str(), "failed", "rejected", "unsupported_metadata", &cast.pending, &player,
		                   fallbackForRole(cast.role, potionItemId));
		return false;
	}
	telemetry.recordActionAttempt();
	emitSpellCastEvent(position, cast.name.c_str(), cast.words.c_str(), playerBotSpellRoleName(cast.role),
	                   cast.pending.need.c_str(), "requested", "unchecked", nullptr, &cast.pending, &player, nullptr);
	// The normal speech handler remains the authoritative spell engine.
	survivalRuntime.beginEngineSpellCast();
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", cast.words);
	survivalRuntime.endEngineSpellCast();
	if (cast.role == PlayerBotSpellRole::Support) {
		if (Condition* haste = player.getCondition(CONDITION_HASTE)) survivalRuntime.observeHasteAfterCast(haste->getTicks(), haste->getEndTime());
	}
	return true;
}

void PlayerBotController::verifySpellCast(Player& player, const Position& position)
{
	// The runtime owns pending-cast verification and calibration; the controller only supplies live observations.
	const auto pending = survivalRuntime.pendingSpell();
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
	const char* fallback = verification.success ? nullptr : verification.pending.role == PlayerBotSpellRole::Healing ?
	                       healthPotionFallback(recoveryPotionItemId(player.getVocationId())) :
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
	progressionRuntime.completeMagicTraining(magicTrainingRetryDelay);
	if (progressionRuntime.activeGoal() == TopLevelGoal::MagicTraining) {
		emit("goal_result", position, "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
		     ",\"goal\":\"magic_training\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
		if (selectTopLevelGoal(player, position, "magic_training_complete")) {
			schedule(SCHEDULER_MINTICKS);
		}
	}
}

bool PlayerBotController::processMagicTraining(Player& player, const Position& position)
{
	const PlayerBotSurvivalSnapshot snapshot = survivalSnapshot(player);
	const char* reason = survivalRuntime.magicTrainingReason(snapshot);
	const std::optional<PlayerBotMagicTrainingCommand> selected = !reason ? survivalRuntime.decideMagicTraining(snapshot) : std::nullopt;
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
	        << ",\"spell\":" << jsonString(selected->name) << ",\"audited_priority\":"
	        << static_cast<uint32_t>(selected->priority) << ",\"refresh\":"
	        << (selected->refresh ? "true" : "false") << ",\"mana_before\":" << manaBefore
	        << ",\"mana_max\":" << player.getMaxMana() << ",\"mana_gain\":" << selected->manaGain
	        << ",\"mana_tick_interval\":" << selected->manaTickInterval << ",\"mana_tick_remaining\":" << selected->manaTickRemaining
	        << ",\"predicted_mana\":" << predictedMana << ",\"wasted_mana\":" << wastedMana
	        << ",\"mana_cost\":" << selected->cost << ",\"emergency_reserve\":" << selected->reserve;
	emit("action_result", position, request.str());
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName(selected->name) : nullptr;
	if (!spell || spell->getWords() != selected->words) {
		telemetry.recordActionFailure();
		finishMagicTraining(player, position, "failed", "unsupported_metadata");
		return false;
	}
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", selected->words);
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
	       << ",\"spell\":" << jsonString(selected->name) << ",\"mana_before\":" << manaBefore
	       << ",\"mana_after\":" << manaAfter << ",\"mana_cost\":" << selected->cost << ",\"mana_delta\":" << manaDelta
	       << ",\"mana_spent_before\":" << manaSpentBefore << ",\"mana_spent_after\":" << manaSpentAfter
	       << ",\"magic_level_before\":" << magicLevelBefore << ",\"magic_level_after\":" << magicLevelAfter
	       << ",\"emergency_reserve\":" << selected->reserve << ",\"mana_gain\":" << selected->manaGain
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
	    survivalRuntime.decideSupportSpell(survivalSnapshot(*player), std::chrono::steady_clock::now()));
}

bool PlayerBotController::tryOffensiveSpell(Player* player, const Position& currentPosition)
{
	if (!player) return false;
	const auto traversalTarget = huntCoordinator.traversalTarget();
	Creature* target = traversalTarget ? g_game.getCreatureByID(traversalTarget->id) : nullptr;
	return dispatchSpellCommand(*player, currentPosition,
	    survivalRuntime.decideOffensiveSpell(survivalSnapshot(*player, target), std::chrono::steady_clock::now()));
}

uint32_t PlayerBotController::potionStockTarget(const Player& player, uint32_t returnReserve) const
{
	const auto snapshot = survivalSnapshot(player);
	// Preserve survival floors while saving for a first usable heal. Below its
	// loaded requirements, buy normal hunt stock instead of waiting on training.
	const bool saveForHealing = !survivalRuntime.preferredHealingSpell(snapshot) &&
	    std::any_of(snapshot.spells.begin(), snapshot.spells.end(), [&](const auto& spell) {
		return !spell.learned && survivalRuntime.healingSpellWorthLearning(snapshot, spell.name.c_str());
	});
	const uint32_t base = saveForHealing ? healthPotionSafetyTarget : healthPotionAmmoTarget;
	return recoveryPotionRestockTargetForReserve(returnReserve, base);
}

uint32_t PlayerBotController::potionStockTarget(const Player& player) const
{
	return potionStockTarget(player, huntPotionReturnThreshold);
}

PlayerBotSupplyStocks PlayerBotController::supplyStocks(const Player& player) const
{
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	PlayerBotSupplyStocks stocks{{{PlayerBotSupplyKind::HealthPotion, potionItemId, healthPotionSafetyTarget,
	                               huntPotionReturnThreshold, potionStockTarget(player)},
	                              inventoryPolicy.inventoryItemCount(player, potionItemId)}};
	for (const PlayerBotSupplyStock& stock : PlayerBotInventoryPolicy::additionalSupplyStocks(player)) stocks.push_back(stock);
	playerBotRequireSupplies(stocks, huntSupplyHandoff.requirements());
	return stocks;
}

uint32_t PlayerBotController::carriedSupplyReserve(const Player& player, uint16_t itemId) const
{
	uint32_t reserve = inventoryPolicy.protectedItemReserve(player, itemId);
	for (const auto& stock : supplyStocks(player)) {
		if (stock.rule.itemId == itemId) reserve = std::max(reserve, stock.rule.target);
	}
	return reserve;
}

bool PlayerBotController::huntSuppliesReady(const Player& player) const
{
	const PlayerBotSupplyStocks stocks = supplyStocks(player);
	return std::all_of(stocks.begin(), stocks.end(), [this](const PlayerBotSupplyStock& stock) {
		return !stock.rule.active() || stock.count >
		    playerBotSupplyHuntReserve(stock.rule.kind, stock.rule.returnThreshold, supplyRecovery.active());
	});
}

uint64_t PlayerBotController::spellTrainingReserve(const Player& player, bool emergencyOnly) const
{
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, potionItemId);
	const auto stocks = supplyStocks(player);
	const auto* health = playerBotSupplyStock(stocks, PlayerBotSupplyKind::HealthPotion);
	const uint32_t reserveTarget = std::max(health ? health->rule.safetyFloor : 0, emergencyOnly ?
	    (huntPotionReturnThreshold == UINT32_MAX ? UINT32_MAX : huntPotionReturnThreshold + 1) : potionStockTarget(player));
	const uint64_t healthReserve = emergencyOnly && potionCount >= reserveTarget ? carriedGoldReserve :
	                               recoverySpendingReserve(player, reserveTarget);
	std::vector<PlayerBotSupplyFloorCost> others;
	for (const auto& stock : stocks) {
		if (stock.rule.kind == PlayerBotSupplyKind::HealthPotion) continue;
		others.push_back({stock.count, stock.rule.safetyFloor,
		    stock.count < stock.rule.safetyFloor ? cheapestShopPrice(player, stock.rule.itemId) : 0});
	}
	const uint64_t supplyReserve = playerBotSupplyFloorSpendingReserve(healthReserve, others);
	// First-heal bootstrap only: the default query ignores current mana and
	// overheal but still requires an eligible, usable learned healing spell.
	const bool firstUsableHeal = emergencyOnly && !survivalRuntime.preferredHealingSpell(survivalSnapshot(player));
	return playerBotSpellTrainingSpendingReserve(supplyReserve, carriedGoldReserve, firstUsableHeal);
}

uint32_t PlayerBotController::cheapestShopPrice(const Player& player, uint16_t itemId) const
{
	uint32_t price = std::numeric_limits<uint32_t>::max();
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Shop, player.getPosition())) {
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.itemId == itemId && offer.buyPrice != 0) price = std::min(price, offer.buyPrice);
		}
	}
	return price;
}

uint64_t PlayerBotController::recoverySpendingReserve(const Player& player, uint32_t target) const
{
	const uint16_t potionItemId = recoveryPotionItemId(player.getVocationId());
	const uint32_t potionPrice = cheapestShopPrice(player, potionItemId);
	if (potionPrice == std::numeric_limits<uint32_t>::max()) {
		return std::numeric_limits<uint64_t>::max();
	}
	return playerBotRecoverySpendingReserve(inventoryPolicy.inventoryItemCount(player, potionItemId), target, potionPrice,
	                                        carriedGoldReserve);
}

uint64_t PlayerBotController::supplyFloorSpendingReserve(const Player& player) const
{
	const auto stocks = supplyStocks(player);
	std::vector<PlayerBotSupplyFloorCost> costs;
	for (const auto& stock : stocks) {
		costs.push_back({stock.count, stock.rule.safetyFloor,
		    stock.count < stock.rule.safetyFloor ? cheapestShopPrice(player, stock.rule.itemId) : 0});
	}
	return playerBotSupplyFloorSpendingReserve(carriedGoldReserve, costs);
}

void PlayerBotController::emitSpellCandidate(const Npc& npc, const NpcSpellOffer& offer, const Position& position,
                                             const char* result, const char* reason, uint64_t reserve,
                                             uint32_t travelSteps, std::optional<uint8_t> learningPriority, uint64_t fare) const
{
	std::ostringstream fields;
	fields << "\"goal\":\"learn_spell\",\"result\":" << jsonString(result)
	       << ",\"npc_id\":" << npc.getID() << ",\"npc_name\":" << jsonString(npc.getName())
	       << ",\"spell\":" << jsonString(offer.spellName) << ",\"keyword\":" << jsonString(offer.keyword)
	       << ",\"price\":" << offer.price << ",\"level\":" << offer.level
	       << ",\"premium\":" << (offer.premium ? "true" : "false") << ",\"reserve\":" << reserve
	       << ",\"fare\":" << fare << ",\"travel_steps\":" << travelSteps
	       << ",\"implemented_use\":" << (learningPriority ? "true" : "false");
	if (learningPriority) fields << ",\"learning_priority\":" << static_cast<uint16_t>(*learningPriority);
	fields << ",\"provider_position\":{\"x\":" << npc.getPosition().x
	       << ",\"y\":" << npc.getPosition().y << ",\"z\":" << static_cast<uint16_t>(npc.getPosition().z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("spell_candidate", position, fields.str());
}

std::string PlayerBotController::trainerDecisionFacts(Player& player, const Position& position, bool coolingDown)
{
	// Do not include action delays, current mana/HP or regeneration ticks:
	// they do not change trainer eligibility/proof and must not restart a
	// completed phase on every preparation-budget retry.
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
	    PlayerBotEquipmentAdapter::loadout(player));
	const auto snapshot = survivalSnapshot(player);
	std::ostringstream key;
	key << std::setprecision(17) << position << ':' << coolingDown << ':' << player.getVocationId() << ':' << player.getVocation()->getFromVocation()
	    << ':' << player.getLevel() << ':' << player.isPremium() << ':' << player.isPzLocked()
	    << ':' << player.getMoney() << ':' << player.getBankBalance()
	    << ':' << spellTrainingReserve(player) << ':' << spellTrainingReserve(player, true)
	    << ':' << playerBotSupplyStockKey(supplyStocks(player)) << ':' << huntSuppliesReady(player)
	    << ':' << combat.maximumHealth << ':' << combat.armor << ':' << combat.defense << ':' << combat.attack
	    << ':' << combat.attackSkill << ':' << combat.attackFactor << ':' << combat.hitChance << ':' << combat.blockedByShield
	    << ':' << player.getStepDuration() << ':' << player.getStepDuration(DIRECTION_NORTHEAST)
	    << ':' << (g_game.findItemOfType(&player, ropeItemId, true) != nullptr)
	    << ':' << (g_game.findItemOfType(&player, 2554, true) != nullptr)
	    << ':' << (player.getGroup() && player.getGroup()->access)
	    << ':' << (player.getGroup() ? player.getGroup()->flags : 0)
	    << ':' << (player.getGuild() ? player.getGuild()->getId() : 0)
	    << ':' << (player.getGuildRank() ? player.getGuildRank()->id : 0)
	    << ':' << PlayerBotTopology::instance().generation() << ':' << PlayerBotHuntRegionPlanner::getCacheRevision()
	    << ':' << playerBotNavigationCostIdentity("", riskProfile);
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::SpellTrainer, position)) {
		key << ";trainer:" << npc->getID() << ':' << npc->getPosition();
		for (const auto& offer : npc->getSpellOffers()) {
			Spell* spell = g_spells ? g_spells->getSpellByName(offer.spellName) : nullptr;
			key << ':' << std::quoted(offer.spellName) << ':' << std::quoted(offer.keyword)
			    << ':' << offer.price << ':' << offer.level << ':' << offer.premium
			    << ':' << player.hasLearnedInstantSpell(offer.spellName);
			for (auto vocation : offer.vocationIds) key << ':' << vocation;
			key << "|registry:" << (spell && spell->isInstant() && spell->isLearnable())
			    << ':' << (spell ? spell->getLevel() : 0) << ':' << (spell && spell->isPremium())
			    << ':' << (spell && spell->getVocMap().count(player.getVocationId()))
			    << ':' << survivalRuntime.healingSpellWorthLearning(snapshot, offer.spellName.c_str());
		}
	}
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Travel, position)) {
		key << ";travel:" << npc->getID() << ':' << npc->getPosition();
		for (const auto& offer : npc->getTravelOffers()) {
			key << ':' << offer.destination << ':' << offer.price << ':' << offer.level << ':' << offer.premium
			    << ':' << offer.hasOpaqueCondition << ':' << offer.hasOpaqueAction;
			for (const auto& phrase : offer.dialogue) key << ':' << std::quoted(phrase);
		}
	}
	for (const auto& [offer, until] : unavailableTravelOffers) {
		if (until > std::chrono::steady_clock::now()) key << ";unavailable:" << offer.first << ':' << offer.second;
	}
	return playerBotNavigationBlockedIdentity(key.str(),
	    navigationRuntime.activeBlockedPositions(std::chrono::steady_clock::now()));
}

void PlayerBotController::interruptTrainerRouteWork()
{
	const bool arbitrationWork = trainerArbitration.completed();
	if (arbitrationWork) {
		trainerArbitration.reset();
		trainerDecisionWatch = {};
		selectedTrainerRoute.reset();
	}
	const bool discoveryWork = trainerDiscovery && (trainerDiscovery->next != 0 || trainerRouteSearch);
	const bool journeyWork = trainerJourney && !trainerJourneyNeedsRoute;
	if (!arbitrationWork && !discoveryWork && !journeyWork && !trainerRouteSearch) return;
	trainerRouteSearch.reset();
	if (discoveryWork) {
		++trainerDiscovery->progress.restarts;
		trainerDiscovery->next = 0;
		for (auto& offer : trainerDiscovery->offers) {
			playerBotTrainerDiscardRouteProof(offer);
			offer.routeRejection.clear();
		}
		for (auto& route : trainerDiscovery->routes) route = {};
	}
	if (journeyWork) {
		trainerJourney->evidence.reset();
		trainerJourneyNeedsRoute = true;
		resetNavigation();
	}
	auto& budget = playerBotHuntPlanningBudget();
	if (!budget.isActive(playerId)) budget.cancel(playerId, std::chrono::steady_clock::now());
}

void PlayerBotController::clearTrainerWork()
{
	trainerDiscovery.reset();
	trainerArbitration.reset();
	trainerDecisionWatch = {};
	selectedTrainerRoute.reset();
	trainerJourney.reset();
	trainerRouteSearch.reset();
	trainerGoalSelectionPending = false;
	trainerJourneyNeedsRoute = false;
	trainerTravelReceiptFailed = false;
	auto& budget = playerBotHuntPlanningBudget();
	if (!budget.isActive(playerId)) budget.cancel(playerId, std::chrono::steady_clock::now());
}

std::optional<PlayerBotNavigationRoutePlan> PlayerBotController::advanceTrainerRoute(
    Player& player, const std::vector<Position>& destinations, uint64_t reserve,
    std::shared_ptr<PlayerBotHuntTravelEvidence>& evidence)
{
	const Position source = player.getPosition();
	const auto blocked = navigationRuntime.activeBlockedPositions(std::chrono::steady_clock::now());
	if (!trainerRouteSearch || trainerRouteSearch->origin != source ||
	    trainerRouteSearch->destination != destinations.front() || trainerRouteSearch->reserve != reserve ||
	    trainerRouteSearch->blockedPositions != blocked) {
		trainerRouteSearch.emplace();
		auto& search = *trainerRouteSearch;
		search.origin = source;
		search.destination = destinations.front();
		search.reserve = reserve;
		search.blockedPositions = blocked;
		search.pass = (uint64_t(1) << 61) | (uint64_t(playerId) << 24) | ++trainerRouteSerial;
	}
	auto& budget = playerBotHuntPlanningBudget();
	const auto admission = budget.request(playerId, std::chrono::steady_clock::now());
	if (!admission.admitted) {
		schedule(static_cast<uint32_t>(std::clamp<int64_t>(
		    std::chrono::duration_cast<std::chrono::milliseconds>(admission.wait).count(),
		    blockedRouteRetryInterval, 5000)));
		return std::nullopt;
	}
	PlayerBotPlanningBudget::Charge charge(budget, playerId);
	auto& search = *trainerRouteSearch;
	PlayerBotHuntRouteRequest request;
	request.planningPass = search.pass;
	request.from = source;
	request.to = destinations.front();
	request.destinations = destinations;
	request.blockedPositions = search.blockedPositions;
	request.preferSafeWalking = std::any_of(destinations.begin(), destinations.end(), [&](Position p) {
		return playerBotNavigationDistance(source, p) <= 128;
	});
	PlayerBotHuntRouteTiming timing;
	std::optional<PlayerBotNavigationRoutePlan> planned;
	{
		PlayerBotRouteWorkSlot slot(huntTravelWork, search.work);
		planned = advanceHuntTravelRoute(player, request, source, timing, reserve, false, nullptr, &evidence);
	}
	search.expandedNodes += timing.localExpandedNodes;
	const bool exhausted = !planned && ++search.turns >= maximumRouteSearchTurns;
	if (planned || exhausted || search.turns == 1 || timing.invalidations != 0 || timing.transportRestarts != 0) {
		emit("trainer_route", source, "\"npc_id\":" + std::to_string(
		    trainerDiscovery ? trainerDiscovery->offers[trainerDiscovery->next].npcId :
		    progressionRuntime.spellTraining().plan().npcId) +
		    ",\"spell\":" + jsonString(trainerDiscovery ? trainerDiscovery->offers[trainerDiscovery->next].spellName :
		                                      progressionRuntime.spellTraining().plan().spellName) +
		    ",\"phase\":" + jsonString(trainerDiscovery ? "discovery" : "journey") +
		    ",\"result\":" + jsonString(planned ? playerBotNavigationResultName(planned->metrics.result) :
		                                    exhausted ? "proof_exhausted" : "pending") +
		    (exhausted ? ",\"reason\":\"route_turn_limit\"" : "") +
		    (trainerDiscovery ? ",\"scan_continuations\":" + std::to_string(trainerDiscovery->progress.continuations) +
		        ",\"offer_index\":" + std::to_string(trainerDiscovery->next) : "") +
		    ",\"expanded_nodes\":" + std::to_string(search.expandedNodes) +
		    ",\"fare\":" + std::to_string(planned ? planned->metrics.fare : 0) +
		    ",\"protected_funds\":" + std::to_string(reserve) +
		    ",\"slices\":" + std::to_string(search.turns + (planned ? 1 : 0)) +
		    ",\"blocked_tiles\":" + std::to_string(search.blockedPositions.size()) +
		    ",\"invalidations\":" + std::to_string(timing.invalidations) +
		    ",\"transport_restarts\":" + std::to_string(timing.transportRestarts));
	}
	if (exhausted) {
		planned.emplace();
		planned->metrics.result = PlayerBotNavigationResult::NodeLimit;
	}
	if (planned) trainerRouteSearch.reset();
	else if (admission.admitted) schedule(routeSearchContinuationInterval);
	return planned;
}

bool PlayerBotController::findSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan& plan)
{
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t healingReserve = spellTrainingReserve(player, true);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const uint16_t vocationId = player.getVocationId();
	const uint16_t baseVocationId = player.getVocation()->getFromVocation() == 0 ? vocationId :
	                               player.getVocation()->getFromVocation();
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, recoveryPotionItemId(vocationId));
	const bool suppliesReady = huntSuppliesReady(player);
	const auto spellSnapshot = survivalSnapshot(player);
	auto healingOffer = [](const std::string& name) {
		const auto* descriptor = playerBotSpellDescriptor(name.c_str());
		return descriptor && descriptor->role == PlayerBotSpellRole::Healing;
	};
	auto worthwhile = [&](const std::string& name) {
		return !healingOffer(name) || survivalRuntime.healingSpellWorthLearning(spellSnapshot, name.c_str());
	};
	auto refresh = [&](PlayerBotSpellOfferSnapshot& snapshot) {
		Npc* npc = g_game.getNpcByID(snapshot.npcId);
		Spell* spell = g_spells ? g_spells->getSpellByName(snapshot.spellName) : nullptr;
		const NpcSpellOffer* loadedOffer = nullptr;
		if (npc) {
			const auto found = std::find_if(npc->getSpellOffers().begin(), npc->getSpellOffers().end(), [&](const auto& offer) {
				return offer.spellName == snapshot.spellName && offer.keyword == snapshot.keyword &&
				    offer.price == snapshot.price && offer.level == snapshot.level && offer.premium == snapshot.premium;
			});
			if (found != npc->getSpellOffers().end()) loadedOffer = &*found;
		}
		snapshot.registryMatches = npc && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::SpellTrainer) &&
		    loadedOffer && spell && spell->isInstant() && spell->isLearnable() &&
		    spell->getLevel() == snapshot.level && spell->isPremium() == snapshot.premium;
		snapshot.vocationEligible = snapshot.registryMatches && spell->getVocMap().count(vocationId) != 0 &&
		    std::find(loadedOffer->vocationIds.begin(), loadedOffer->vocationIds.end(), baseVocationId) != loadedOffer->vocationIds.end();
		snapshot.levelEligible = player.getLevel() >= snapshot.level;
		snapshot.premiumEligible = !snapshot.premium || player.isPremium();
		snapshot.known = player.hasLearnedInstantSpell(snapshot.spellName);
		snapshot.worthLearning = worthwhile(snapshot.spellName);
		snapshot.suppliesReady = suppliesReady && potionCount > snapshot.potionReserve;
	};
	const bool factsChanged = trainerDiscovery && (trainerDiscovery->origin != position ||
	    trainerDiscovery->money != totalMoney || trainerDiscovery->reserve != reserve ||
	    trainerDiscovery->healingReserve != healingReserve || trainerDiscovery->potionCount != potionCount);
	auto progress = trainerDiscovery ? trainerDiscovery->progress : PlayerBotTrainerScanProgress{};
	if (!trainerDiscovery) progress.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	if (factsChanged) {
		++progress.restarts;
		trainerDiscovery.reset();
		trainerRouteSearch.reset();
	}
	const char* exhaustion = progress.exhaustion(std::chrono::steady_clock::now(), PlayerBotApproachLimits::providerMoves);
	if (exhaustion) {
		emit("trainer_route", position, "\"phase\":\"discovery\",\"result\":\"proof_exhausted\",\"reason\":" +
		    jsonString(exhaustion) + ",\"scan_continuations\":" + std::to_string(progress.continuations) +
		    ",\"evaluated_offers\":" + std::to_string(trainerDiscovery ? trainerDiscovery->next : 0));
		trainerRouteSearch.reset();
		if (!trainerDiscovery) {
			clearTrainerWork();
			progressionRuntime.completeSpellTraining(false, spellTrainingFailureCooldown);
			return false;
		}
		// End pending proofs, not completed candidates. Selection below still
		// refreshes eligibility, risk, supplies, and each retained route proof.
		playerBotTrainerEndPendingProofs(trainerDiscovery->offers, trainerDiscovery->next);
		trainerDiscovery->next = trainerDiscovery->offers.size();
	}
	if (!trainerDiscovery) {
		trainerDiscovery.emplace();
		auto& scan = *trainerDiscovery;
		scan.origin = position; scan.money = totalMoney;
		scan.reserve = reserve; scan.healingReserve = healingReserve; scan.potionCount = potionCount;
		scan.progress = progress;
		std::vector<Npc*> trainers = playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::SpellTrainer, position);
		auto hasRelevantOffer = [&](const Npc* trainer) {
			return std::any_of(trainer->getSpellOffers().begin(), trainer->getSpellOffers().end(), [&](const NpcSpellOffer& offer) {
				Spell* spell = g_spells ? g_spells->getSpellByName(offer.spellName) : nullptr;
				return playerBotSpellLearningPriority(offer.spellName.c_str()) && worthwhile(offer.spellName) &&
				       spell && spell->isInstant() && spell->isLearnable() && spell->getLevel() == offer.level &&
				       spell->isPremium() == offer.premium && player.getLevel() >= offer.level &&
				       (!offer.premium || player.isPremium()) && !player.hasLearnedInstantSpell(offer.spellName) &&
				       std::find(offer.vocationIds.begin(), offer.vocationIds.end(), baseVocationId) != offer.vocationIds.end() &&
				       spell->getVocMap().find(vocationId) != spell->getVocMap().end();
			});
		};
		const auto eligibleEnd = std::stable_partition(trainers.begin(), trainers.end(), hasRelevantOffer);
		const size_t eligibleCount = static_cast<size_t>(std::distance(trainers.begin(), eligibleEnd));
		if (eligibleCount > maximumSpellTrainerRoutes) {
			const size_t rotatingCount = eligibleCount - 1;
			const size_t offset = spellTrainerScanOffset % rotatingCount;
			std::rotate(trainers.begin() + 1, trainers.begin() + 1 + offset, eligibleEnd);
			spellTrainerScanOffset = (offset + maximumSpellTrainerRoutes - 1) % rotatingCount;
		} else if (eligibleEnd != trainers.end()) {
			const size_t remainingCount = static_cast<size_t>(std::distance(eligibleEnd, trainers.end()));
			const size_t offset = spellTrainerScanOffset % remainingCount;
			std::rotate(eligibleEnd, eligibleEnd + offset, trainers.end());
			const size_t remainingSlots = maximumSpellTrainerRoutes > eligibleCount ? maximumSpellTrainerRoutes - eligibleCount : 0;
			spellTrainerScanOffset = (offset + std::min(remainingSlots, remainingCount)) % remainingCount;
		}
		if (trainers.size() > maximumSpellTrainerRoutes) {
			trainers.resize(maximumSpellTrainerRoutes);
		}

		for (Npc* npc : trainers) {
			emit("spell_trainer_discovered", position, "\"npc_id\":" + std::to_string(npc->getID()) +
			    ",\"npc_name\":" + jsonString(npc->getName()) + ",\"offers\":" +
			    std::to_string(npc->getSpellOffers().size()) + ",\"in_scope\":true");
			for (const NpcSpellOffer& offer : npc->getSpellOffers()) {
				const auto priority = playerBotSpellLearningPriority(offer.spellName.c_str());
				PlayerBotSpellOfferSnapshot snapshot;
				snapshot.npcId = npc->getID(); snapshot.npcPosition = npc->getPosition(); snapshot.npcName = npc->getName();
				snapshot.spellName = offer.spellName; snapshot.keyword = offer.keyword; snapshot.price = offer.price;
				snapshot.level = offer.level; snapshot.premium = offer.premium; snapshot.inScope = true;
				snapshot.implementedUse = priority.has_value(); snapshot.learningPriority = priority.value_or(UINT8_MAX);
				snapshot.reserve = healingOffer(offer.spellName) ? healingReserve : reserve;
				refresh(snapshot);
				scan.offers.push_back(std::move(snapshot));
			}
		}
		std::stable_sort(scan.offers.begin(), scan.offers.end(), [](const auto& a, const auto& b) {
			return playerBotTrainerPriority(a) < playerBotTrainerPriority(b);
		});
		scan.routes.resize(scan.offers.size());
	}
	auto& scan = *trainerDiscovery;
	PlayerBotRouteChanges::Scope decisionDependencies(trainerDecisionWatch);
	++scan.progress.continuations;
	auto select = [&] {
		return spellTrainingPlanner.select({reserve, totalMoney, reserve != UINT64_MAX,
		    static_cast<uint32_t>(riskProfile.maximumRouteHealthLoss * riskProfile.healthLossCost),
		    riskProfile.maximumHealthLossPerSecond, scan.offers});
	};
	// Cheap rejections do not request the shared planner. At most one route
	// slice runs per turn; the offer cursor and all frontiers survive a yield.
	while (scan.next < scan.offers.size()) {
		auto& offer = scan.offers[scan.next];
		auto& route = scan.routes[scan.next];
		refresh(offer);
		if (!offer.registryMatches || !offer.implementedUse || !offer.vocationEligible || !offer.levelEligible ||
		    !offer.premiumEligible || offer.known || !offer.worthLearning || !offer.suppliesReady ||
		    !playerBotTrainerTripAffordable(totalMoney, *offer.reserve, offer.price, 0)) {
			++scan.next;
			continue;
		}
		Npc* npc = g_game.getNpcByID(offer.npcId);
		auto destinations = trainerApproaches(player, *npc);
		if (!trainerRouteSearch) {
			// Refresh at candidate boundaries, not on every admitted search slice.
			// Learning/loaded eligibility may change without moving the actor.
			for (auto& candidate : scan.offers) refresh(candidate);
			const auto incumbent = select();
			if (incumbent.selectedOfferIndex && playerBotTrainerPriority(scan.offers[*incumbent.selectedOfferIndex]) <
			    playerBotTrainerPriority(offer)) {
				// Keep equal-ranked providers for the original travel-step tie break.
				// Final proof validation can reopen deferred offers if this winner moved.
				for (size_t i = scan.next; i < scan.offers.size(); ++i)
					scan.offers[i].routeRejection = "lower_learning_priority";
				scan.next = scan.offers.size();
				break;
			}
			if (!offer.route.reachable && incumbent.selectedOfferIndex &&
			    !playerBotTrainerOptimisticCanBeat(offer, playerBotTrainerMinimumSteps(position, destinations),
			        scan.offers[*incumbent.selectedOfferIndex])) {
				offer.routeRejection = "route_rank_dominated";
				++scan.next;
				continue;
			}
		}
		if (offer.route.reachable) { ++scan.next; continue; } // Retained proof is validated below.
		offer.routeRejection.clear();
		if (destinations.empty()) {
			offer.routeRejection = "trainer_approach_unavailable";
		} else {
			if (route.destinations != destinations) {
				route.destinations = destinations;
				trainerRouteSearch.reset();
			}
			// Affordability above makes this addition safe.
			auto planned = advanceTrainerRoute(player, destinations, *offer.reserve + offer.price, route.evidence);
			if (!planned) return false; // Pending is not a rejection or a completed arbitration.
			route.plan = std::move(*planned);
			offer.fare = route.plan.metrics.fare;
			const auto result = route.plan.metrics.result;
			offer.route = {result == PlayerBotNavigationResult::Reached, result == PlayerBotNavigationResult::NodeLimit,
			    route.plan.metrics.firstNpcTravelOffer ? destinations.front() : route.plan.metrics.waypoint,
			    static_cast<uint32_t>(route.plan.metrics.steps), route.plan.metrics.expandedNodes,
			    route.plan.metrics.dangerCost, route.plan.metrics.maximumHealthLossPerSecond};
			offer.npcPosition = npc->getPosition();
			offer.routeRejection = result == PlayerBotNavigationResult::NodeLimit ? "route_proof_exhausted" :
			    result == PlayerBotNavigationResult::RiskRejected ? "route_risk_rejected" :
			    result == PlayerBotNavigationResult::Unreachable ?
			        (trainerFareLimited(player, *offer.reserve + offer.price) ? "route_fare_limited" : "trainer_unreachable") : "";
			const uint32_t routeReserve = recoveryPotionRouteReserve(vocationId, player.getMaxHealth(),
			    offer.route.dangerCost, static_cast<uint32_t>(riskProfile.healthLossCost));
			offer.potionReserve = std::max(huntPotionReturnThreshold, routeReserve);
			offer.suppliesReady = suppliesReady && potionCount > offer.potionReserve;
		}
		++scan.next;
		if (scan.next < scan.offers.size()) {
			schedule(routeSearchContinuationInterval);
			return false;
		}
	}
	// Earlier candidates may have become stale while later candidates yielded.
	// Never publish stale evidence, and never restart this scan without a bound.
	size_t firstStale = scan.offers.size();
	for (size_t i = 0; i < scan.offers.size(); ++i) {
		auto& offer = scan.offers[i];
		refresh(offer);
		offer.suppliesReady = suppliesReady && potionCount > offer.potionReserve;
		if (offer.route.reachable) {
			Npc* npc = g_game.getNpcByID(offer.npcId);
			auto& route = scan.routes[i];
			if (!npc || !playerBotTrainerDiscoveryDestinationRetained(route.plan, route.destinations,
			        trainerApproaches(player, *npc)) || !route.evidence ||
			    !huntTravelEvidenceValid(player, position, route.evidence->destination, *route.evidence)) {
				firstStale = std::min(firstStale, i);
				playerBotTrainerDiscardRouteProof(offer);
				offer.routeRejection = exhaustion ? "route_proof_exhausted" : "";
				route = {};
			}
		}
	}
	if (firstStale < scan.offers.size() && !exhaustion) {
		++scan.progress.restarts;
		scan.next = firstStale;
		for (auto& offer : scan.offers)
			if (offer.routeRejection == "lower_learning_priority" || offer.routeRejection == "route_rank_dominated")
				offer.routeRejection.clear();
		schedule(routeSearchContinuationInterval);
		return false;
	}
	const auto decision = select();
	if (!exhaustion) {
		const auto deferred = playerBotTrainerDeferredNeeded(scan.offers, decision.selectedOfferIndex, [&](size_t index) {
			Npc* npc = g_game.getNpcByID(scan.offers[index].npcId);
			return npc ? playerBotTrainerMinimumSteps(position, trainerApproaches(player, *npc)) : uint32_t(0);
		});
		if (deferred) {
			scan.next = *deferred;
			for (auto& offer : scan.offers)
				if (offer.routeRejection == "lower_learning_priority" || offer.routeRejection == "route_rank_dominated")
					offer.routeRejection.clear();
			schedule(routeSearchContinuationInterval);
			return false;
		}
	}
	for (size_t i = 0; i < scan.offers.size(); ++i) {
		const auto& offer = scan.offers[i];
		const auto rejection = std::find_if(decision.rejections.begin(), decision.rejections.end(),
		    [i](const auto& r) { return r.offerIndex == i; });
		if (Npc* npc = g_game.getNpcByID(offer.npcId))
			emitSpellCandidate(*npc, {offer.spellName, offer.keyword, offer.price, offer.level, offer.premium, {}}, position,
			    rejection == decision.rejections.end() ? "feasible" : "rejected",
			    rejection == decision.rejections.end() ? nullptr : rejection->reason.c_str(),
			    *offer.reserve, offer.route.steps, offer.implementedUse ? std::optional<uint8_t>(offer.learningPriority) : std::nullopt, offer.fare);
	}
	if (decision.selected && decision.selectedOfferIndex) {
		plan = *decision.selected;
		selectedTrainerRoute = std::move(scan.routes[*decision.selectedOfferIndex]);
	}
	trainerRouteSearch.reset();
	trainerDiscovery.reset();
	if (exhaustion && !decision.selected) progressionRuntime.completeSpellTraining(false, spellTrainingFailureCooldown);
	return decision.selected.has_value();
}

void PlayerBotController::beginSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan plan)
{
	trainerJourney = std::move(selectedTrainerRoute);
	trainerArbitration.reset();
	trainerDecisionWatch = {};
	selectedTrainerRoute.reset();
	trainerDiscovery.reset();
	trainerGoalSelectionPending = false;
	trainerFareRemaining = plan.fare;
	trainerJourneyRestarts = 0;
	trainerJourneyNeedsRoute = !trainerJourney.has_value();
	trainerTravelReceiptFailed = false;
	trainerJourneyDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
	progressionRuntime.beginSpellTraining(std::move(plan));
	const auto& training = progressionRuntime.spellTraining().plan();
	resetNavigation();
	if (trainerJourney) navigationRuntime.observePlan({PlayerBotNavigationGoal::anyOf(trainerJourney->destinations),
	    trainerJourney->plan, player.canDoAction(), true, std::chrono::steady_clock::now()});
	emit("strategy_selection", position, "\"goal\":\"learn_spell\",\"npc_id\":" +
	     std::to_string(training.npcId) + ",\"spell\":" + jsonString(training.spellName) +
	     ",\"keyword\":" + jsonString(training.keyword) + ",\"price\":" +
	     std::to_string(training.price) + ",\"fare\":" + std::to_string(training.fare) +
	     ",\"reserve\":" + std::to_string(training.reserve) +
	     ",\"potion_reserve\":" + std::to_string(training.potionReserve) +
	     ",\"travel_steps\":" + std::to_string(training.travelSteps));
	say(player, "Going to learn " + training.spellName + ".");
}

void PlayerBotController::finishSpellTraining(Player* player, const Position& position, const char* result, const char* reason)
{
	clearTrainerWork();
	emit("strategy_objective_result", position, "\"goal\":\"learn_spell\",\"spell\":" +
	     jsonString(progressionRuntime.spellTraining().plan().spellName) + ",\"result\":" + jsonString(result) + ",\"reason\":" +
	     jsonString(reason));
	emit("goal_result", position, "\"decision_id\":" + std::to_string(progressionRuntime.decisionId()) +
	     ",\"goal\":\"learn_spell\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, "Spell training " + std::string(result) + ": " + reason + '.');
	}
	progressionRuntime.finish();
	resetNavigation();
	progressionRuntime.completeSpellTraining(std::strcmp(result, "success") == 0,
	    std::strcmp(result, "success") == 0 ? spellTrainingSuccessCooldown : spellTrainingFailureCooldown);
	if (player && fixtureDriver.progressionGoalLoop(true).selectGoal) {
		selectTopLevelGoal(*player, position, std::strcmp(result, "success") == 0 ? "spell_training_complete" : "spell_training_failed");
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processSpellTraining(Player* player, const Position& currentPosition)
{
	const auto& training = progressionRuntime.spellTraining().plan();
	Npc* trainer = g_game.getNpcByID(training.npcId);
	const uint16_t vocationId = player->getVocationId();
	const uint16_t baseVocationId = player->getVocation()->getFromVocation() == 0 ? vocationId :
		player->getVocation()->getFromVocation();
	Spell* spell = g_spells ? g_spells->getSpellByName(training.spellName) : nullptr;
	const bool offerAvailable = trainer && playerBotNpcHasCapability(*trainer, PlayerBotNpcCapability::SpellTrainer) &&
	    spell && spell->isInstant() && spell->isLearnable() && spell->getLevel() == training.level &&
	    spell->isPremium() == training.premium && spell->getVocMap().count(vocationId) != 0 &&
	    std::any_of(trainer->getSpellOffers().begin(), trainer->getSpellOffers().end(), [&training, player, baseVocationId](const NpcSpellOffer& offer) {
		return offer.spellName == training.spellName && offer.keyword == training.keyword && offer.price == training.price &&
		       offer.level == training.level && offer.premium == training.premium && player->getLevel() >= offer.level &&
		       (!offer.premium || player->isPremium()) &&
		       std::find(offer.vocationIds.begin(), offer.vocationIds.end(), baseVocationId) != offer.vocationIds.end();
	    });
	PlayerBotSpellTrainingObservation observation;
	observation.totalMoney = player->getMoney() + player->getBankBalance();
	const bool verifying = progressionRuntime.spellTraining().stage() == PlayerBotSpellTrainingStage::Verify;
	const auto* descriptor = playerBotSpellDescriptor(training.spellName.c_str());
	const bool healing = descriptor && descriptor->role == PlayerBotSpellRole::Healing;
	const uint64_t reserve = std::max(training.reserve, spellTrainingReserve(*player, healing));
	const uint32_t potionReserve = trainerJourney ? std::max(training.potionReserve,
	    recoveryPotionRouteReserve(vocationId, player->getMaxHealth(), trainerJourney->plan.metrics.dangerCost,
	        static_cast<uint32_t>(riskProfile.healthLossCost))) : training.potionReserve;
	if (!verifying && (!playerBotTrainerTripAffordable(observation.totalMoney, reserve, training.price, trainerFareRemaining) ||
	    inventoryPolicy.inventoryItemCount(*player, recoveryPotionItemId(vocationId)) <=
	        std::max(huntPotionReturnThreshold, potionReserve))) {
		finishSpellTraining(player, currentPosition, "failed", "recovery_reserve_changed");
		return;
	}
	if (!verifying && (!offerAvailable || player->hasLearnedInstantSpell(training.spellName))) {
		finishSpellTraining(player, currentPosition, "failed", "trainer_offer_changed");
		return;
	}
	if (trainerTravelReceiptFailed || trainerJourneyRestarts > PlayerBotApproachLimits::rejectedTiles ||
	    (!verifying && std::chrono::steady_clock::now() >= trainerJourneyDeadline)) {
		finishSpellTraining(player, currentPosition, "failed",
		    trainerTravelReceiptFailed ? "travel_payment_or_landing_mismatch" :
		    trainerJourneyRestarts > PlayerBotApproachLimits::rejectedTiles ? "trainer_route_retry_limit" : "trainer_journey_deadline");
		return;
	}
	if (progressionRuntime.spellTraining().stage() == PlayerBotSpellTrainingStage::Travel) {
		auto destinations = trainerApproaches(*player, *trainer);
		if (destinations.empty()) {
			finishSpellTraining(player, currentPosition, "failed", "trainer_approach_unavailable");
			return;
		}
		// Discovery proves a choice against the whole search graph. Execution
		// keeps only material actor/risk/world facts and this boarding leg.
		// Ordinary position/ground timing and unused providers are not changes
		// to that leg. A landing always requests a fresh remaining-trip proof.
		const char* invalidation = !trainerJourney ? "missing_route" :
		    !playerBotTrainerDestinationRetained(trainerJourney->plan, trainerJourney->destinations, destinations) ?
		        "trainer_approach_changed" : nullptr;
		if (trainerJourney && !trainerJourneyNeedsRoute && trainerJourney->plan.metrics.firstNpcTravelOffer) {
			const auto& quote = *trainerJourney->plan.metrics.firstNpcTravelOffer;
			Npc* provider = g_game.getNpcByID(quote.npcId);
			if (!provider || !playerBotNpcHasCapability(*provider, PlayerBotNpcCapability::Travel) ||
			    std::none_of(provider->getTravelOffers().begin(), provider->getTravelOffers().end(), [&](const auto& offer) {
				return offer.destination == quote.destination && offer.price == quote.price &&
				    offer.level == quote.minimumLevel && offer.premium == quote.premium &&
				    offer.dialogue == quote.dialogue && !offer.hasOpaqueCondition && !offer.hasOpaqueAction;
			    })) {
				finishSpellTraining(player, currentPosition, "failed", "travel_offer_changed");
				return;
			}
			const auto approach = playerBotTrainerBoardingApproach(trainerJourney->plan,
			    trainerJourney->evidence ? trainerJourney->evidence->source : currentPosition);
			if (!approach || !Position::areInRange<3, 3, 0>(*approach, provider->getPosition()))
				invalidation = "boarding_approach_changed";
		}
		if (!invalidation && !trainerJourneyNeedsRoute) {
			invalidation = trainerJourney->evidence ?
			    trainerTravelExecutionChange(*player, *trainerJourney->evidence) : "missing_execution_proof";
		}
		if (invalidation && !trainerJourneyNeedsRoute) {
			emit("trainer_route", currentPosition, "\"phase\":\"journey\",\"result\":\"invalidated\",\"reason\":" +
			    jsonString(invalidation) + ",\"restarts\":" + std::to_string(trainerJourneyRestarts + 1) +
			    ",\"remaining_fare\":" + std::to_string(trainerFareRemaining));
			if (++trainerJourneyRestarts > PlayerBotApproachLimits::rejectedTiles) {
				finishSpellTraining(player, currentPosition, "failed", "trainer_route_invalidation_limit");
				return;
			}
			trainerJourneyNeedsRoute = true;
			trainerRouteSearch.reset();
			resetNavigation();
		}
		if (trainerJourneyNeedsRoute) {
			if (!trainerJourney) trainerJourney.emplace();
			trainerJourney->destinations = destinations;
			// Preserve the quoted allowance for all remaining legs; never borrow
			// spell/supply money, even if the actor gains cash during the journey.
			auto planned = advanceTrainerRoute(*player, destinations, reserve + training.price, trainerJourney->evidence);
			if (!planned) return;
			const char* failure = planned->metrics.result == PlayerBotNavigationResult::NodeLimit ? "route_proof_exhausted" :
			    planned->metrics.result == PlayerBotNavigationResult::RiskRejected ? "route_risk_rejected" :
			    planned->metrics.result != PlayerBotNavigationResult::Reached ?
			        (trainerFareLimited(*player, reserve + training.price) ? "route_fare_limited" : "trainer_unreachable") :
			    planned->metrics.fare > trainerFareRemaining ? "fare_quote_overrun" :
			    playerBotNavigationRiskVerdict(riskProfile, planned->metrics) != PlayerBotNavigationRiskVerdict::Accepted ? "route_risk_rejected" :
			    inventoryPolicy.inventoryItemCount(*player, recoveryPotionItemId(vocationId)) <=
			        std::max(huntPotionReturnThreshold, recoveryPotionRouteReserve(vocationId, player->getMaxHealth(),
			            planned->metrics.dangerCost, static_cast<uint32_t>(riskProfile.healthLossCost))) ? "route_potion_reserve_unmet" : nullptr;
			if (failure) {
				finishSpellTraining(player, currentPosition, "failed", failure);
				return;
			}
			trainerJourney->plan = *planned;
			navigationRuntime.observePlan({PlayerBotNavigationGoal::anyOf(destinations), std::move(*planned),
			    player->canDoAction(), true, std::chrono::steady_clock::now()});
			trainerJourneyNeedsRoute = false;
		}
		PlayerBotNavigationRuntimeOutcome navigation;
		observation.navigationReached = processNavigation(player, currentPosition,
		    PlayerBotNavigationGoal::anyOf(trainerJourney->destinations),
		    &navigation, playerBotNavigationMaximumExpandedNodes, true, false, &riskProfile, false);
		if (navigation.routeRequest) {
			if (++trainerJourneyRestarts > PlayerBotApproachLimits::rejectedTiles) {
				finishSpellTraining(player, currentPosition, "failed", "trainer_route_retry_limit");
				return;
			}
			trainerJourneyNeedsRoute = true;
			schedule(blockedRouteRetryInterval);
			return;
		}
		observation.navigationFailed = navigation.routeUnsafe || navigation.fixedTargetRouteExhausted;
		if (observation.navigationReached) {
			// Reaching a stable talking tile ends the itinerary; no future fare
			// can be spent while the normal paid-learning conversation owns it.
			trainerFareRemaining = 0;
			trainerJourney.reset();
		}
	} else if (!verifying && !Position::areInRange<3, 3, 0>(currentPosition, trainer->getPosition())) {
		// A trainer wandering away must obtain a new destination route, not fall
		// back to the old walking-only NPC approach planner.
		progressionRuntime.restartSpellTrainingTravel();
		trainerJourneyNeedsRoute = true;
		resetNavigation();
		schedule(SCHEDULER_MINTICKS);
		return;
	} else {
		observation.npcAvailable = offerAvailable && Position::areInRange<3, 3, 0>(currentPosition, trainer->getPosition());
		observation.greetingAcknowledged = progressionRuntime.greetingAcknowledged();
		observation.learned = player->hasLearnedInstantSpell(training.spellName);
	}
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceSpellTraining(observation);
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded) {
		emit("action_result", currentPosition, "\"action\":\"learn_spell\",\"result\":\"success\",\"spell\":" +
	     jsonString(training.spellName) + ",\"price\":" + std::to_string(training.price) +
	     ",\"money_before\":" + std::to_string(progressionRuntime.spellTraining().moneyBefore()) + ",\"money_after\":" +
	     std::to_string(observation.totalMoney));
		finishSpellTraining(player, currentPosition, "success", result.reason);
		return;
	}
	if (result.type == PlayerBotProgressionOutcomeType::Failed) {
		finishSpellTraining(player, currentPosition, "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Speak) {
		if (!offerAvailable) return;
		const char* words = std::strcmp(result.command.reason, "request") == 0 ? training.keyword.c_str() : result.command.reason;
		if (std::strcmp(words, "hi") == 0) progressionRuntime.clearGreetingAcknowledgement();
		telemetry.recordActionAttempt();
		trainer->receiveSpeech(player, TALKTYPE_PRIVATE_PN, words);
	}
	if (result.command.type != PlayerBotProgressionCommandType::Navigate) schedule(result.command.type == PlayerBotProgressionCommandType::Speak ? 1000 : SCHEDULER_MINTICKS);
}
