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
	constexpr uint32_t higherPriorityRecoveryManaReserve = 20;
	constexpr uint32_t minimumHasteRouteSteps = 20;
	constexpr int32_t smallHealthPotionMaximumHealing = 90;
	constexpr int32_t maximumHasteObservationDelay = 2000;
	constexpr uint32_t magicTrainingEmergencyReserve = 20;
	constexpr auto magicTrainingRetryDelay = std::chrono::seconds(2);

	struct MagicTrainingSpell {
		const PlayerBotSpellDescriptor* descriptor = nullptr;
		InstantSpell* spell = nullptr;
		uint64_t cost = 0;
		bool refresh = false;
	};

	std::optional<ManaRegenerationForecast> manaRegenerationForecast(const Player& player)
	{
		return player.getManaRegenerationForecast();
	}

	bool magicTrainingEffectUseful(const Player& player, PlayerBotTrainingEffect effect)
	{
		return effect == PlayerBotTrainingEffect::Haste ? !player.hasCondition(CONDITION_HASTE) :
		       effect == PlayerBotTrainingEffect::Light ? !player.hasCondition(CONDITION_LIGHT) : false;
	}

	bool magicTrainingSpellLegal(const Player& player, const PlayerBotSpellDescriptor& descriptor, InstantSpell*& spell,
	                            uint64_t& cost)
	{
		if (!descriptor.magicTrainingSafe || descriptor.magicTrainingPriority == 0 ||
		    descriptor.magicTrainingEffect == PlayerBotTrainingEffect::None || !player.hasLearnedInstantSpell(descriptor.name)) {
			return false;
		}
		spell = g_spells ? g_spells->getInstantSpellByName(descriptor.name) : nullptr;
		if (!spell || spell->getWords() != descriptor.words || !spell->isLearnable() || !spell->isEnabled() ||
		    player.getLevel() < spell->getLevel() || player.getMagicLevel() < spell->getMagicLevel() ||
		    player.getSoul() < spell->getSoulCost() || (spell->isPremium() && !player.isPremium()) ||
		    (spell->getNeedWeapon() && !player.getWeapon(true)) || player.hasCondition(CONDITION_EXHAUST_HEAL) ||
		    spell->getAggressive() || !spell->getSelfTarget() || spell->getNeedTarget() || spell->getHasParam() ||
		    spell->getHasPlayerNameParam() || spell->getNeedDirection() || spell->getNeedCasterTargetOrDirection()) {
			return false;
		}
		cost = spell->getManaCost(&player);
		return cost != 0 && cost <= static_cast<uint64_t>(player.getMana()) -
		       std::min<uint64_t>(player.getMana(), magicTrainingEmergencyReserve);
	}

	std::optional<MagicTrainingSpell> selectMagicTrainingSpell(const Player& player)
	{
		std::optional<MagicTrainingSpell> useful;
		std::optional<MagicTrainingSpell> refresh;
		for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
			InstantSpell* spell = nullptr;
			uint64_t cost = 0;
			if (!magicTrainingSpellLegal(player, descriptor, spell, cost)) continue;
			if (magicTrainingEffectUseful(player, descriptor.magicTrainingEffect)) {
				if (!useful || descriptor.magicTrainingPriority > useful->descriptor->magicTrainingPriority) {
					useful = MagicTrainingSpell{&descriptor, spell, cost, false};
				}
			} else if (descriptor.magicTrainingRefreshSafe &&
			           (!refresh || cost < refresh->cost ||
			            (cost == refresh->cost && descriptor.magicTrainingPriority > refresh->descriptor->magicTrainingPriority))) {
				refresh = MagicTrainingSpell{&descriptor, spell, cost, true};
			}
		}
		return useful ? useful : refresh;
	}

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
		if (pending->role == "support") {
			fields << ",\"haste_ticks_after_cast\":" << pending->hasteTicksAfterCast
			       << ",\"haste_ticks_observed\":" << pending->hasteTicksObserved
			       << ",\"haste_duration_measured\":" << pending->hasteDurationMeasured;
		}
		if (const PlayerBotSpellProfile* profile = spellCalibration.find(pending->name, pending->targetClass)) {
			fields << ",\"calibration\":{\"accepted\":" << profile->accepted << ",\"rejected\":" << profile->rejected
			       << ",\"ambiguous\":" << profile->ambiguous << ",\"minimum\":" << profile->minimum
			       << ",\"maximum\":" << profile->maximum << ",\"conservative\":" << profile->conservative
			       << ",\"ranking\":" << profile->ranking << ",\"confidence\":" << profile->confidence << '}';
		} else {
			fields << ",\"calibration\":{\"accepted\":0,\"rejected\":0,\"ambiguous\":0,\"confidence\":0}"
			       << ",\"ranking_estimate\":" << spellCalibration.ranking(pending->name, pending->targetClass, pending->envelope);
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
	const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor(spellName);
	if (!descriptor) {
		emitSpellCastEvent(position, nullptr, nullptr, nullptr, need, "skipped", "not_attempted", "unsupported_descriptor", nullptr,
		                   &player, fallbackForNeed(need));
		return false;
	}
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName(descriptor->name) : nullptr;
	if (!spell || spell->getWords() != descriptor->words || !spell->isLearnable()) {
		emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "unsupported_metadata", nullptr, &player, fallbackForRole(descriptor->role));
		return false;
	}
	if (!player.hasLearnedInstantSpell(descriptor->name)) {
		if (shouldEmitRepeated("cast_spell:unlearned:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "unlearned", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}
	if (!player.canDoAction() || !pendingSpellCast.name.empty()) {
		return false;
	}
	const bool healingGroup = descriptor->role == PlayerBotSpellRole::Healing || descriptor->role == PlayerBotSpellRole::Support;
	if (player.hasCondition(healingGroup ? CONDITION_EXHAUST_HEAL : CONDITION_EXHAUST_COMBAT)) {
		if (shouldEmitRepeated("cast_spell:cooldown:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "cooldown", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}
	if ((descriptor->role == PlayerBotSpellRole::MeleeOffense || descriptor->role == PlayerBotSpellRole::RangedOffense) &&
		(!target || target->isRemoved() || target->isDead() || player.getAttackedCreature() != target ||
		 !player.canSeeCreature(target) || !player.canSee(target->getPosition()) ||
		 !Position::areInRange<1, 1, 0>(position, target->getPosition()))) {
		if (shouldEmitRepeated("cast_spell:lost_target:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "lost_target", nullptr, &player, "normal_melee");
		}
		return false;
	}
	if (spell->getNeedTarget() && !spell->canThrowSpell(&player, target)) {
		if (shouldEmitRepeated("cast_spell:target_unreachable:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "target_unreachable", nullptr, &player, "normal_melee");
		}
		return false;
	}
	const uint32_t manaCost = spell->getManaCost(&player);
	const uint32_t reserve = descriptor->role == PlayerBotSpellRole::Healing ? 0 : higherPriorityRecoveryManaReserve;
	if (player.getMana() < manaCost + reserve) {
		if (shouldEmitRepeated("cast_spell:insufficient_mana_reserve:" + std::string(descriptor->name))) {
			emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "skipped",
			                   "not_attempted", "insufficient_mana_reserve", nullptr, &player, fallbackForRole(descriptor->role));
		}
		return false;
	}

	const PlayerBotSpellEnvelope envelope = playerBotSpellEnvelope(player, *descriptor);
	pendingSpellCast = PendingSpellCast{};
	pendingSpellCast.name = descriptor->name;
	pendingSpellCast.role = playerBotSpellRoleName(descriptor->role);
	pendingSpellCast.need = need;
	pendingSpellCast.manaBefore = player.getMana();
	pendingSpellCast.manaReserve = reserve;
	pendingSpellCast.healthBefore = player.getHealth();
	pendingSpellCast.targetId = target ? target->getID() : 0;
	pendingSpellCast.targetHealthBefore = target ? target->getHealth() : 0;
	pendingSpellCast.missingHealth = player.getMaxHealth() - player.getHealth();
	pendingSpellCast.hasteTicksBefore = player.hasCondition(CONDITION_HASTE) ? player.getCondition(CONDITION_HASTE)->getTicks() : 0;
	pendingSpellCast.envelope = envelope;
	pendingSpellCast.targetClass = targetClass(target);
	pendingSpellCast.otherRecovery = descriptor->role == PlayerBotSpellRole::Healing && player.hasCondition(CONDITION_REGENERATION);
	pendingSpellCast.observedAt = std::chrono::steady_clock::now();
	++counters.actionsAttempted;
	emitSpellCastEvent(position, descriptor->name, descriptor->words, playerBotSpellRoleName(descriptor->role), need, "requested", "unchecked",
	                   nullptr, &pendingSpellCast, &player, nullptr);
	// Route through the normal player speech handler so the live spell engine owns legality and costs.
	spellCastExecuting = true;
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", spell->getWords());
	spellCastExecuting = false;
	if (descriptor->role == PlayerBotSpellRole::Support) {
		if (Condition* haste = player.getCondition(CONDITION_HASTE)) {
			pendingSpellCast.hasteTicksAfterCast = haste->getTicks();
			pendingSpellCast.hasteEndTimeAfterCast = haste->getEndTime();
		}
	}
	return true;
}

void PlayerBotController::verifySpellCast(Player& player, const Position& position)
{
	if (pendingSpellCast.name.empty()) {
		return;
	}
	const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor(pendingSpellCast.name.c_str());
	const bool manaSpent = player.getMana() < pendingSpellCast.manaBefore;
	PlayerBotSpellObservation observation;
	observation.manaSpent = manaSpent;
	observation.concurrentDamage = pendingSpellCast.concurrentDamage;
	observation.otherRecovery = pendingSpellCast.otherRecovery;
	observation.otherAttacker = pendingSpellCast.otherAttacker;
	observation.meleeOrOtherBotDamage = pendingSpellCast.meleeOrOtherBotDamage;
	bool observed = false;
	if (pendingSpellCast.role == "healing") {
		observation.value = pendingSpellCast.observedSpellHealing;
		observed = player.getHealth() > pendingSpellCast.healthBefore;
		if (!pendingSpellCast.concurrentDamage && player.getHealth() - pendingSpellCast.healthBefore !=
		    static_cast<int32_t>(pendingSpellCast.observedSpellHealing)) {
			observation.otherRecovery = true;
		}
	} else if (pendingSpellCast.role == "support") {
		Condition* haste = player.getCondition(CONDITION_HASTE);
		const int32_t elapsed = static_cast<int32_t>(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - pendingSpellCast.observedAt).count(), 0, maximumHasteObservationDelay));
		pendingSpellCast.hasteTicksObserved = haste ? haste->getTicks() : 0;
		pendingSpellCast.hasteDurationMeasured = pendingSpellCast.hasteTicksObserved + elapsed;
		const bool newlyApplied = haste && pendingSpellCast.hasteTicksBefore == 0 && pendingSpellCast.hasteTicksAfterCast > 0 &&
		                          pendingSpellCast.hasteEndTimeAfterCast != 0 &&
		                          haste->getEndTime() == pendingSpellCast.hasteEndTimeAfterCast;
		observation.value = newlyApplied ? pendingSpellCast.hasteDurationMeasured : 0;
		observed = observation.value > 0;
	} else {
		Creature* target = g_game.getCreatureByID(pendingSpellCast.targetId);
		observation.targetStable = target && !target->isRemoved() && targetClass(target) == pendingSpellCast.targetClass;
		observation.value = pendingSpellCast.observedSpellDamage;
		observation.multiTarget = pendingSpellCast.spellVictimOverflow || pendingSpellCast.spellVictimCount > 1;
		observed = observation.value > 0;
	}
	const PlayerBotSpellEvidence evidence = descriptor ? playerBotClassifySpellObservation(descriptor->role, observation,
		pendingSpellCast.missingHealth, pendingSpellCast.envelope) : PlayerBotSpellEvidence::CastNotVerified;
	spellCalibration.observe(pendingSpellCast.name, pendingSpellCast.targetClass, pendingSpellCast.envelope, evidence, observation.value);
	if (std::optional<std::string> evicted = spellCalibration.takeEvictedProfile()) {
		emit("spell_calibration_eviction", position, "\"evicted_profile\":" + jsonString(*evicted) +
		     ",\"replacement_profile\":" + jsonString(pendingSpellCast.name + "\n" + pendingSpellCast.targetClass));
	}
	const bool success = manaSpent && observed;
	const char* reason = playerBotSpellEvidenceName(evidence);
	const char* fallback = success ? nullptr : pendingSpellCast.role == "healing" ? "small_health_potion" :
	                       pendingSpellCast.role == "support" ? "continue_route" : "normal_melee";
	emitSpellCastEvent(position, descriptor ? descriptor->name : nullptr, descriptor ? descriptor->words : nullptr,
	                   descriptor ? playerBotSpellRoleName(descriptor->role) : nullptr, pendingSpellCast.need.c_str(),
	                   success ? "success" : "failed", manaSpent ? "accepted" : "rejected", reason,
	                   &pendingSpellCast, &player, fallback);
	if (!success) {
		++counters.actionsFailed;
		spellRetryAfter = std::chrono::steady_clock::now() + healingRetryInterval;
	} else if (pendingSpellCast.role == "healing" && evidence == PlayerBotSpellEvidence::Accepted) {
		recordHuntRecovery(false);
	}
	pendingSpellCast = PendingSpellCast{};
}

const char* PlayerBotController::magicTrainingSafetyReason(const Player& player) const
{
	if (cyclePhase == CyclePhase::Hunt) return "hunting";
	if (progressionObjective != ProgressionObjective::None) return "progression_objective";
	if (scenarioStage != ScenarioStage::Traverse || ratId != 0 || defensiveTargetId != 0 ||
	    const_cast<Player&>(player).getAttackedCreature() != nullptr) return "combat_or_pursuit";
	if (navigationPending || worldChangePending || !navigationSteps.empty()) return "pending_navigation";
	if (!pendingSpellCast.name.empty() || pendingHeal || pendingEat || needsHealing(player)) return "defensive_work";
	if (!player.canDoAction() || player.hasCondition(CONDITION_EXHAUST_HEAL)) return "spell_cooldown";
	return nullptr;
}

const char* PlayerBotController::magicTrainingCandidateReason(const Player& player) const
{
	if (const char* reason = magicTrainingSafetyReason(player)) return reason;
	if (player.getZone() == ZONE_PROTECTION) return "regeneration_paused";
	const std::optional<ManaRegenerationForecast> forecast = manaRegenerationForecast(player);
	if (!forecast) return "no_active_regeneration_forecast";
	const uint64_t predictedMana = static_cast<uint64_t>(player.getMana()) + forecast->gain;
	if (predictedMana <= player.getMaxMana()) return "next_tick_not_overflow";
	return selectMagicTrainingSpell(player) ? nullptr : "no_audited_safe_spell";
}

bool PlayerBotController::magicTrainingSafe(const Player& player) const
{
	return magicTrainingSafetyReason(player) == nullptr;
}

void PlayerBotController::finishMagicTraining(Player& player, const Position& position, const char* result, const char* reason)
{
	magicTrainingCooldownUntil = std::chrono::steady_clock::now() + magicTrainingRetryDelay;
	if (activeGoal == TopLevelGoal::MagicTraining) {
		emit("goal_result", position, "\"decision_id\":" + std::to_string(goalDecisionId) +
		     ",\"goal\":\"magic_training\",\"result\":" + jsonString(result) + ",\"reason\":" + jsonString(reason));
		if (selectTopLevelGoal(player, position, "magic_training_complete")) {
			schedule(SCHEDULER_MINTICKS);
		}
	}
}

bool PlayerBotController::processMagicTraining(Player& player, const Position& position)
{
	const char* reason = magicTrainingCandidateReason(player);
	const std::optional<ManaRegenerationForecast> forecast = manaRegenerationForecast(player);
	const std::optional<MagicTrainingSpell> selected = !reason ? selectMagicTrainingSpell(player) : std::nullopt;
	if (!selected || !forecast) {
		finishMagicTraining(player, position, "skipped", reason ? reason : "opportunity_lost");
		return false;
	}
	const uint64_t manaBefore = player.getMana();
	const uint64_t manaSpentBefore = player.getSpentMana();
	const uint32_t magicLevelBefore = player.getBaseMagicLevel();
	const uint64_t predictedMana = manaBefore + forecast->gain;
	const uint64_t wastedMana = predictedMana - player.getMaxMana();
	++counters.actionsAttempted;
	std::ostringstream request;
	request << "\"action\":\"magic_training\",\"result\":\"requested\",\"source\":\"engine_path\""
	        << ",\"spell\":" << jsonString(selected->descriptor->name) << ",\"audited_priority\":"
	        << static_cast<uint32_t>(selected->descriptor->magicTrainingPriority) << ",\"refresh\":"
	        << (selected->refresh ? "true" : "false") << ",\"mana_before\":" << manaBefore
	        << ",\"mana_max\":" << player.getMaxMana() << ",\"mana_gain\":" << forecast->gain
	        << ",\"mana_tick_interval\":" << forecast->interval << ",\"mana_tick_remaining\":" << forecast->remaining
	        << ",\"predicted_mana\":" << predictedMana << ",\"wasted_mana\":" << wastedMana
	        << ",\"mana_cost\":" << selected->cost << ",\"emergency_reserve\":" << magicTrainingEmergencyReserve;
	emit("action_result", position, request.str());
	g_game.playerSay(playerId, 0, TALKTYPE_SAY, "", selected->spell->getWords());
	uint64_t manaAfter = player.getMana();
	const uint64_t manaSpentAfter = player.getSpentMana();
	const uint32_t magicLevelAfter = player.getBaseMagicLevel();
	if (testPolicy.forceMagicTrainingVerificationFailure) {
		// The engine cast remains real; this fixture corrupts only the post-cast observation snapshot.
		++manaAfter;
	}
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
	       << ",\"emergency_reserve\":" << magicTrainingEmergencyReserve << ",\"mana_gain\":" << forecast->gain
	       << ",\"mana_tick_interval\":" << forecast->interval << ",\"mana_tick_remaining\":" << forecast->remaining
	       << ",\"predicted_mana\":" << predictedMana << ",\"wasted_mana\":" << wastedMana;
	emit("action_result", position, result.str());
	if (!verified) ++counters.actionsFailed;
	finishMagicTraining(player, position, verified ? "success" : "failed", verified ? "cast_verified" : "cast_verification_failed");
	return false;
}

void PlayerBotController::runSpellCalibrationFixture(Player& player, const Position& position)
{
	auto emitFixture = [this, &position](const char* spell, const char* targetClass, const PlayerBotSpellEnvelope& envelope,
	                                     PlayerBotSpellEvidence evidence, int32_t value, const char* phase) {
		const PlayerBotSpellProfile& profile = spellCalibration.observe(spell, targetClass, envelope, evidence, value);
		std::ostringstream fields;
		const bool profileMath = std::strcmp(phase, "low_confidence") == 0 || std::strcmp(phase, "gradual_ranking") == 0 ||
		                         std::strcmp(phase, "bounded_range") == 0;
		fields << "\"source\":" << jsonString(profileMath ? "profile_math" : "classifier_helper")
		       << ",\"phase\":" << jsonString(phase) << ",\"spell\":" << jsonString(spell)
		       << ",\"target_class\":" << jsonString(targetClass) << ",\"evidence\":" << jsonString(playerBotSpellEvidenceName(evidence))
		       << ",\"engine_bounds\":{\"minimum\":" << envelope.minimum << ",\"maximum\":" << envelope.maximum
		       << ",\"duration_ms\":" << envelope.durationMs << "},\"calibration\":{\"accepted\":" << profile.accepted
		       << ",\"rejected\":" << profile.rejected << ",\"ambiguous\":" << profile.ambiguous
		       << ",\"minimum\":" << profile.minimum << ",\"maximum\":" << profile.maximum
		       << ",\"conservative\":" << profile.conservative << ",\"ranking\":" << profile.ranking
		       << ",\"confidence\":" << profile.confidence << '}';
		if (std::strcmp(phase, "low_confidence") == 0) {
			fields << ",\"policy_unchanged\":true";
		}
		emit("spell_calibration", position, fields.str());
	};
	const PlayerBotSpellDescriptor* healing = playerBotSpellDescriptor("Light Healing");
	const PlayerBotSpellDescriptor* ranged = playerBotSpellDescriptor("Whirlwind Throw");
	const PlayerBotSpellDescriptor* melee = playerBotSpellDescriptor("Berserk");
	const PlayerBotSpellDescriptor* support = playerBotSpellDescriptor("Haste");
	if (!healing || !ranged || !melee || !support) return;
	const PlayerBotSpellEnvelope healingEnvelope = playerBotSpellEnvelope(player, *healing);
	const PlayerBotSpellEnvelope rangedEnvelope = playerBotSpellEnvelope(player, *ranged);
	const PlayerBotSpellEnvelope meleeEnvelope = playerBotSpellEnvelope(player, *melee);
	const PlayerBotSpellEnvelope supportEnvelope = playerBotSpellEnvelope(player, *support);
	PlayerBotSpellObservation acceptedHeal{true, false, false, true, false, false, false,
	                                      std::max(1, healingEnvelope.minimum)};
	emitFixture(healing->name, "self", healingEnvelope,
	            playerBotClassifySpellObservation(healing->role, acceptedHeal, healingEnvelope.maximum + 1, healingEnvelope),
	            acceptedHeal.value, "isolated_healing");
	emitFixture(healing->name, "self", healingEnvelope,
	            playerBotClassifySpellObservation(healing->role, acceptedHeal, healingEnvelope.maximum, healingEnvelope),
	            acceptedHeal.value, "healing_equality_exact");
	emitFixture(healing->name, "self", healingEnvelope, PlayerBotSpellEvidence::CensoredOverheal, 0, "overheal_censored");
	emitFixture(healing->name, "self", healingEnvelope, PlayerBotSpellEvidence::ConcurrentDamage, 0, "concurrent_damage");
	emitFixture(healing->name, "self", healingEnvelope, PlayerBotSpellEvidence::CastNotVerified, 0, "rejected_cast");
	PlayerBotSpellObservation acceptedDamage{true, false, false, true, false, false, false,
	                                        std::max(1, rangedEnvelope.minimum)};
	emitFixture(ranged->name, "monster:fixture", rangedEnvelope,
	            playerBotClassifySpellObservation(ranged->role, acceptedDamage, 0, rangedEnvelope), acceptedDamage.value,
	            "single_target_damage");
	emitFixture(ranged->name, "monster:fixture", rangedEnvelope, PlayerBotSpellEvidence::MeleeOrOtherBotDamage, 0, "melee_ambiguous");
	emitFixture(ranged->name, "monster:fixture", rangedEnvelope, PlayerBotSpellEvidence::OtherAttacker, 0, "other_attacker_ambiguous");
	emitFixture(ranged->name, "monster:fixture", rangedEnvelope, PlayerBotSpellEvidence::TargetLost, 0, "target_loss_ambiguous");
	emitFixture(melee->name, "monster:fixture", meleeEnvelope, PlayerBotSpellEvidence::MultiTarget, 0, "multi_target_ambiguous");
	emitFixture(support->name, "self", supportEnvelope, PlayerBotSpellEvidence::Accepted, supportEnvelope.durationMs,
	            "support_duration");
	emitFixture(support->name, "self", supportEnvelope, PlayerBotSpellEvidence::PreexistingOrReplacedCondition, 0,
	            "support_preexisting_or_replaced");
	for (uint16_t sample = 0; sample < 9; ++sample) {
		emitFixture(ranged->name, "monster:fixture", rangedEnvelope, PlayerBotSpellEvidence::Accepted,
			sample == 8 ? 70000 : rangedEnvelope.maximum + sample,
			sample == 0 ? "low_confidence" : sample == 8 ? "bounded_range" : "gradual_ranking");
	}
	for (uint8_t profile = 0; profile <= 12; ++profile) {
		spellCalibration.observe(ranged->name, "monster:eviction-" + std::to_string(profile), rangedEnvelope,
		                         PlayerBotSpellEvidence::Accepted, rangedEnvelope.minimum);
		if (std::optional<std::string> evicted = spellCalibration.takeEvictedProfile()) {
			emit("spell_calibration_eviction", position, "\"source\":\"profile_math\",\"evicted_profile\":" +
			     jsonString(*evicted) + ",\"profile_count\":" + std::to_string(spellCalibration.size()));
		}
	}
	const size_t profilesBeforeReset = spellCalibration.size();
	spellCalibration.clear();
	emit("spell_calibration", position, "\"source\":\"profile_math\",\"phase\":\"fixture_profile_clear\",\"profiles_before\":" +
	     std::to_string(profilesBeforeReset) + ",\"profiles_after\":" + std::to_string(spellCalibration.size()) +
	     ",\"persistent\":false");
}

void PlayerBotController::runMagicTrainingFixture(Player& player, const Position& position)
{
	const std::optional<ManaRegenerationForecast> defaultForecast = player.getManaRegenerationForecast();
	std::ostringstream defaultFields;
	defaultFields << "\"source\":\"authoritative_forecast\",\"case\":\"active_default\",\"active\":"
	              << (defaultForecast ? "true" : "false");
	if (defaultForecast) {
		defaultFields << ",\"gain\":" << defaultForecast->gain << ",\"interval\":" << defaultForecast->interval
		              << ",\"remaining\":" << defaultForecast->remaining;
	}
	emit("magic_training_fixture", position, defaultFields.str());

	ConditionRegeneration active(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 10000);
	active.setParam(CONDITION_PARAM_MANAGAIN, 10);
	active.setParam(CONDITION_PARAM_MANATICKS, 1000);
	active.executeCondition(&player, 250);
	const std::optional<ManaRegenerationForecast> forecast = active.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL);
	if (forecast) {
		const uint64_t exactFull = 990 + forecast->gain;
		const uint64_t overflow = 995 + forecast->gain;
		std::ostringstream fields;
		fields << "\"source\":\"authoritative_forecast\",\"case\":\"active\",\"gain\":" << forecast->gain
		       << ",\"interval\":" << forecast->interval << ",\"remaining\":" << forecast->remaining
		       << ",\"exact_full_predicted\":" << exactFull << ",\"exact_full_overflow\":false"
		       << ",\"overflow_predicted\":" << overflow << ",\"overflow_wasted\":" << overflow - 1000;
		emit("magic_training_fixture", position, fields.str());
	}
	ConditionRegeneration finite(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 500);
	finite.setParam(CONDITION_PARAM_MANAGAIN, 10);
	finite.setParam(CONDITION_PARAM_MANATICKS, 1000);
	finite.executeCondition(&player, 250);
	emit("magic_training_fixture", position,
	     std::string("\"source\":\"authoritative_forecast\",\"case\":\"finite_final_tick\",\"active\":") +
	         (finite.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false"));
	finite.setParam(CONDITION_PARAM_MANATICKS, 3000);
	emit("magic_training_fixture", position,
	     std::string("\"source\":\"authoritative_forecast\",\"case\":\"finite_expires_before_tick\",\"active\":") +
	         (finite.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false"));
	ConditionRegeneration nonDefault(CONDITIONID_COMBAT, CONDITION_REGENERATION, 10000);
	nonDefault.setParam(CONDITION_PARAM_MANAGAIN, 4);
	nonDefault.setParam(CONDITION_PARAM_MANATICKS, 1000);
	nonDefault.executeCondition(&player, 500);
	const std::optional<ManaRegenerationForecast> nonDefaultForecast =
		nonDefault.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL);
	emit("magic_training_fixture", position,
	     "\"source\":\"authoritative_forecast\",\"case\":\"non_default\",\"active\":" +
	         std::string(nonDefaultForecast ? "true" : "false") + ",\"gain\":" +
	         std::to_string(nonDefaultForecast ? nonDefaultForecast->gain : 0) + ",\"remaining\":" +
	         std::to_string(nonDefaultForecast ? nonDefaultForecast->remaining : 0));
	if (player.getZone() != ZONE_PROTECTION) {
		auto addForecastCondition = [&player](ConditionId_t id, uint32_t gain, uint32_t interval) {
			auto* condition = new ConditionRegeneration(id, CONDITION_REGENERATION, 10000);
			condition->setParam(CONDITION_PARAM_MANAGAIN, gain);
			condition->setParam(CONDITION_PARAM_MANATICKS, interval);
			condition->executeCondition(&player, 500);
			player.addCondition(condition);
		};
		addForecastCondition(CONDITIONID_COMBAT, 3, 1000);
		addForecastCondition(CONDITIONID_HEAD, 7, 2000);
		const std::optional<ManaRegenerationForecast> aggregated = player.getManaRegenerationForecast();
		emit("magic_training_fixture", position,
		     "\"source\":\"authoritative_forecast\",\"case\":\"earliest_same_engine_cycle\",\"active\":" +
		         std::string(aggregated ? "true" : "false") + ",\"gain\":" +
		         std::to_string(aggregated ? aggregated->gain : 0) + ",\"remaining\":" +
		         std::to_string(aggregated ? aggregated->remaining : 0));
		player.removeCondition(CONDITION_REGENERATION, CONDITIONID_COMBAT);
		player.removeCondition(CONDITION_REGENERATION, CONDITIONID_HEAD);
	}
	ConditionRegeneration expired(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 0);
	expired.setParam(CONDITION_PARAM_MANAGAIN, 10);
	expired.setParam(CONDITION_PARAM_MANATICKS, 1000);
	emit("magic_training_fixture", position, std::string("\"source\":\"authoritative_forecast\",\"case\":\"expired\",\"active\":") +
	     (expired.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false"));
	if (player.getZone() == ZONE_PROTECTION) {
		emit("magic_training_fixture", position, std::string("\"source\":\"authoritative_forecast\",\"case\":\"protection_zone\",\"active\":") +
		     (active.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false"));
	}
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
		const PlayerBotSpellDescriptor* ranged = playerBotSpellDescriptor("Whirlwind Throw");
		const PlayerBotSpellDescriptor* melee = playerBotSpellDescriptor("Berserk");
		const std::string targetKind = targetClass(target);
		if (ranged && melee) {
			const PlayerBotSpellProfile* profile = spellCalibration.find(ranged->name, targetKind);
			if (profile && profile->confidence >= 1.0 &&
			    spellCalibration.ranking(ranged->name, targetKind, playerBotSpellEnvelope(*player, *ranged)) >
			        spellCalibration.ranking(melee->name, targetKind, playerBotSpellEnvelope(*player, *melee))) {
				return startSpellCast(*player, currentPosition, ranged->name, "offense", target);
			}
		}
		return startSpellCast(*player, currentPosition, "Berserk", "offense", target);
	}
	return startSpellCast(*player, currentPosition, "Whirlwind Throw", "offense", target);
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
	const uint32_t potionCount = getInventoryItemCount(player, smallHealthPotionItemId);
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

bool PlayerBotController::findSpellTraining(Player& player, const Position& position, SpellTrainingPlan& plan,
                                            std::deque<PlayerBotNavigationStep>& selectedSteps)
{
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const uint16_t vocationId = player.getVocationId();
	const uint16_t baseVocationId = player.getVocation()->getFromVocation() == 0 ? vocationId :
	                               player.getVocation()->getFromVocation();
	const bool suppliesReady = getInventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold;
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
