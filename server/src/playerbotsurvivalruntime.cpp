/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotsurvivalruntime.h"

#include "playerbotinventorypolicy.h"
#include "playerbothealingpolicy.h"

#include <algorithm>

namespace {
	constexpr int32_t healingHealthPercent = 60;
	// Provisional rescue threshold: do not delay direct healing for mana or training.
	constexpr int32_t criticalHealthPercent = 30;
	constexpr int32_t meatFoodTicks = 108000;
	constexpr int32_t maximumFoodSeconds = 1200;
	constexpr uint32_t maximumEatFailures = 3;
	constexpr uint32_t minimumHasteRouteSteps = 20;
	constexpr auto retryDelay = std::chrono::seconds(2);
	constexpr auto foodCooldown = std::chrono::minutes(5);

	const PlayerBotSurvivalSpellObservation* spellObservation(const PlayerBotSurvivalSnapshot& snapshot, const char* name)
	{
		const auto found = std::find_if(snapshot.spells.begin(), snapshot.spells.end(), [name](const auto& spell) {
			return spell.name == name;
		});
		return found == snapshot.spells.end() ? nullptr : &*found;
	}

	bool offensive(PlayerBotSpellRole role)
	{
		return role == PlayerBotSpellRole::MeleeOffense || role == PlayerBotSpellRole::RangedOffense;
	}
}

bool PlayerBotSurvivalRuntime::needsHealing(const PlayerBotSurvivalSnapshot& snapshot) const
{
	return static_cast<int64_t>(snapshot.health) * 100 <= static_cast<int64_t>(snapshot.healthMaximum) * healingHealthPercent;
}

bool PlayerBotSurvivalRuntime::hasPendingDefensiveWork() const
{
	return spells.hasPending() || recovery.hasPendingPotion() || recovery.hasPendingFood();
}

uint16_t PlayerBotSurvivalRuntime::pendingFoodItemId() const
{
	const PlayerBotFoodAttempt* pending = recovery.pendingFood();
	return pending ? pending->itemId : 0;
}

const PlayerBotSurvivalSpellObservation* PlayerBotSurvivalRuntime::preferredHealingSpell(
	const PlayerBotSurvivalSnapshot& snapshot, bool affordable, bool capOverheal) const
{
	const PlayerBotSurvivalSpellObservation* selected = nullptr;
	const double missing = capOverheal ? std::max<int64_t>(0, static_cast<int64_t>(snapshot.healthMaximum) - snapshot.health) :
	                                    std::numeric_limits<double>::max();
	auto value = [&](const PlayerBotSurvivalSpellObservation& spell) {
		return PlayerBotHealingSpellValue{spell.name, spell.manaCost, calibration.ranking(spell.name, "self", spell.envelope)};
	};
	for (const auto& spell : snapshot.spells) {
		const auto* descriptor = playerBotSpellDescriptor(spell.name.c_str());
		if (!descriptor || descriptor->role != PlayerBotSpellRole::Healing || !spell.metadataMatches || !spell.learned ||
		    !spell.vocationAllowed || !spell.requirementsMet || !spell.targetReachable ||
		    (affordable && snapshot.mana < spell.manaCost) || playerBotHealingEfficiency(value(spell), missing) <= 0) continue;
		if (!selected || playerBotPreferHealingSpell(value(spell), value(*selected), missing)) selected = &spell;
	}
	return selected;
}

bool PlayerBotSurvivalRuntime::healingSpellWorthLearning(const PlayerBotSurvivalSnapshot& snapshot, const char* name) const
{
	const auto* candidate = spellObservation(snapshot, name);
	const auto* descriptor = playerBotSpellDescriptor(name);
	if (!candidate || !descriptor || descriptor->role != PlayerBotSpellRole::Healing || !candidate->metadataMatches ||
	    !candidate->vocationAllowed || !candidate->requirementsMet) return false;
	const PlayerBotHealingSpellValue value{candidate->name, candidate->manaCost,
	    calibration.ranking(candidate->name, "self", candidate->envelope)};
	if (playerBotHealingEfficiency(value) <= 0) return false;
	const auto* learned = preferredHealingSpell(snapshot);
	return !learned || playerBotHealingPurchaseImproves(value, {learned->name, learned->manaCost,
	    calibration.ranking(learned->name, "self", learned->envelope)});
}

uint32_t PlayerBotSurvivalRuntime::healingManaReserve(const PlayerBotSurvivalSnapshot& snapshot) const
{
	const auto* preferred = preferredHealingSpell(snapshot);
	return preferred ? preferred->manaCost : 0;
}

PlayerBotPotionAttempt PlayerBotSurvivalRuntime::potionObservation(const PlayerBotSurvivalSnapshot& snapshot, uint16_t itemId) const
{
	const bool restoresMana = itemId != 0 && itemId == snapshot.manaPotionItemId;
	return {snapshot.health, snapshot.healthMaximum, restoresMana ? snapshot.manaPotionCount : snapshot.potionCount,
	        snapshot.mana, itemId, restoresMana};
}

// Below the healing reserve, top up only when a fight can drain health before
// regeneration refills the pool. Health recovery always comes first.
bool PlayerBotSurvivalRuntime::needsManaPotion(const PlayerBotSurvivalSnapshot& snapshot) const
{
	return snapshot.manaPotionItemId != 0 && snapshot.manaPotionCount != 0 && (snapshot.hunting || snapshot.combatActive) &&
	       snapshot.mana < healingManaReserve(snapshot);
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::manaPotion(PlayerBotSurvivalCommand command,
	const PlayerBotSurvivalSnapshot& snapshot, const char* reason) const
{
	command.type = PlayerBotSurvivalCommandType::UsePotion;
	command.itemId = snapshot.manaPotionItemId;
	command.need = "mana";
	command.reason = reason;
	command.manaReserve = healingManaReserve(snapshot);
	return command;
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideHealing(const PlayerBotSurvivalSnapshot& snapshot,
	std::chrono::steady_clock::time_point now)
{
	PlayerBotSurvivalCommand command;
	PlayerBotSurvivalCommand spellAttempt;
	const PlayerBotPotionAttempt* pending = recovery.pendingPotion();
	if (const auto verification = recovery.verifyPotion(potionObservation(snapshot, pending ? pending->itemId : 0), now, retryDelay)) {
		command.potionVerification = verification;
	}
	if (snapshot.buyingPotions) return command;
	if (!needsHealing(snapshot)) {
		// A top-up never holds the turn: combat continues while the potion is unavailable.
		if (!needsManaPotion(snapshot) || !recovery.canRetryPotion(now) || !snapshot.canDoAction) return command;
		return manaPotion(command, snapshot, "healing_reserve");
	}
	if (!recovery.canRetryPotion(now) || !snapshot.canDoAction) {
		command.type = PlayerBotSurvivalCommandType::Wait;
		return command;
	}
	const bool critical = static_cast<int64_t>(snapshot.health) * 100 <=
	                      static_cast<int64_t>(snapshot.healthMaximum) * criticalHealthPercent;
	if (critical && snapshot.potionCount != 0) {
		command.type = PlayerBotSurvivalCommandType::UsePotion;
		command.itemId = snapshot.potionItemId;
		command.reason = "critical_health";
		return command;
	}
	const auto* healing = preferredHealingSpell(snapshot, true, true);
	if (!healing) healing = preferredHealingSpell(snapshot, false, true);
	// Preserve a diagnostic candidate when no eligible learned heal exists.
	if (!healing) {
		const auto found = std::find_if(snapshot.spells.begin(), snapshot.spells.end(), [](const auto& spell) {
			const auto* descriptor = playerBotSpellDescriptor(spell.name.c_str());
			return descriptor && descriptor->role == PlayerBotSpellRole::Healing;
		});
		if (found != snapshot.spells.end()) healing = &*found;
	}
	spellAttempt = decideSpell(snapshot, healing ? healing->name.c_str() : "", "recovery", now);
	if (!healing) spellAttempt.reason = "unsupported_metadata";
	if (spellAttempt.type == PlayerBotSurvivalCommandType::CastSpell) {
		spellAttempt.potionVerification = command.potionVerification;
		return spellAttempt;
	}
	// Reevaluate each turn: peaceful mana-assisted healing is not a promise to
	// cast next turn if combat starts or health becomes critical.
	if (spellAttempt.reason == "insufficient_mana_reserve" && snapshot.manaPotionItemId != 0 && snapshot.manaPotionCount != 0 &&
	    (!snapshot.combatActive || snapshot.potionCount == 0)) {
		return manaPotion(command, snapshot, "healing_spell_mana");
	}
	if (snapshot.potionCount == 0) {
		command.type = PlayerBotSurvivalCommandType::InterruptForService;
		command.reason = "healing_supply_missing";
		return command;
	}
	command.type = PlayerBotSurvivalCommandType::UsePotion;
	command.itemId = snapshot.potionItemId;
	command.candidateName = spellAttempt.candidateName;
	command.need = spellAttempt.need;
	command.reason = spellAttempt.reason;
	command.manaReserve = spellAttempt.manaReserve;
	return command;
}

void PlayerBotSurvivalRuntime::beginPotion(const PlayerBotSurvivalSnapshot& snapshot, uint16_t itemId, const std::string& trigger)
{
	PlayerBotPotionAttempt attempt = potionObservation(snapshot, itemId);
	attempt.trigger = trigger;
	recovery.beginPotion(attempt);
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideFood(const PlayerBotSurvivalSnapshot& snapshot,
	std::chrono::steady_clock::time_point now)
{
	PlayerBotSurvivalCommand command;
	if (snapshot.lootMovePending) return command;
	const bool canEat = snapshot.foodTicks / 1000 + meatFoodTicks / 1000 < maximumFoodSeconds;
	if (const PlayerBotFoodAttempt* pending = recovery.pendingFood()) {
		command.foodVerification = recovery.verifyFood(snapshot.pendingFoodCount,
		    snapshot.foodTicks, canEat, now, maximumEatFailures, std::chrono::seconds(5), foodCooldown);
	}
	if (!recovery.canRetryFood(now) || !canEat || snapshot.foodClientId == 0) return command;
	if (!snapshot.canDoAction) {
		command.type = PlayerBotSurvivalCommandType::Wait;
		return command;
	}
	recovery.beginFood({snapshot.foodItemId, snapshot.foodCount, snapshot.foodTicks});
	command.type = PlayerBotSurvivalCommandType::UseFood;
	command.itemId = snapshot.foodItemId;
	command.itemClientId = snapshot.foodClientId;
	return command;
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideSpell(const PlayerBotSurvivalSnapshot& snapshot,
	const char* spellName, const char* need, std::chrono::steady_clock::time_point now)
{
	PlayerBotSurvivalCommand command;
	command.need = need;
	const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor(spellName);
	if (!descriptor) { command.reason = "unsupported_descriptor"; return command; }
	command.candidateName = descriptor->name;
	const PlayerBotSurvivalSpellObservation* spell = spellObservation(snapshot, descriptor->name);
	if (!spell || !spell->metadataMatches) { command.reason = "unsupported_metadata"; return command; }
	if (!spell->learned) { command.reason = "unlearned"; return command; }
	if (!spell->vocationAllowed || !spell->requirementsMet) { command.reason = "ineligible"; return command; }
	if (!snapshot.canDoAction || spells.hasPending() || !spells.canRetry(now)) return command;
	const bool healingGroup = descriptor->role == PlayerBotSpellRole::Healing || descriptor->role == PlayerBotSpellRole::Support;
	if (healingGroup ? snapshot.healingExhausted : snapshot.combatExhausted) { command.reason = "cooldown"; return command; }
	if ((descriptor->role == PlayerBotSpellRole::MeleeOffense || descriptor->role == PlayerBotSpellRole::RangedOffense) &&
	    !snapshot.target.valid) { command.reason = "lost_target"; return command; }
	if (!spell->targetReachable) { command.reason = "target_unreachable"; return command; }
	const uint32_t manaCost = spell->manaCost;
	// Healing may spend the whole pool; everything else spends only mana above the healing reserve.
	const uint32_t reserve = descriptor->role == PlayerBotSpellRole::Healing ? 0 : healingManaReserve(snapshot);
	if (snapshot.mana < manaCost + reserve) {
		command.reason = "insufficient_mana_reserve";
		command.manaReserve = reserve;
		return command;
	}
	PlayerBotSpellPendingCast pending;
	pending.name = descriptor->name;
	pending.role = descriptor->role;
	pending.need = need;
	pending.manaBefore = snapshot.mana;
	pending.manaReserve = reserve;
	pending.healthBefore = snapshot.health;
	pending.targetId = descriptor->role == PlayerBotSpellRole::Healing ? 0 : snapshot.target.id;
	pending.targetHealthBefore = snapshot.target.health;
	pending.missingHealth = snapshot.healthMaximum - snapshot.health;
	pending.hasteTicksBefore = snapshot.hasteTicks;
	pending.envelope = spell->envelope;
	pending.targetClass = descriptor->role == PlayerBotSpellRole::Healing ? "self" : snapshot.target.targetClass;
	pending.otherRecovery = descriptor->role == PlayerBotSpellRole::Healing && snapshot.regenerationActive;
	pending.observedAt = now;
	spells.begin(pending);
	command.type = PlayerBotSurvivalCommandType::CastSpell;
	command.spell = PlayerBotSurvivalSpellCommand{descriptor->name, descriptor->words, descriptor->role, snapshot.target.id, std::move(pending)};
	return command;
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideSupportSpell(const PlayerBotSurvivalSnapshot& snapshot,
	std::chrono::steady_clock::time_point now)
{
	if (snapshot.hasteActive || snapshot.routeSteps < minimumHasteRouteSteps || needsHealing(snapshot)) return {};
	return decideSpell(snapshot, "Haste", "safe_route", now);
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideOffensiveSpell(const PlayerBotSurvivalSnapshot& snapshot,
	std::chrono::steady_clock::time_point now)
{
	if (needsHealing(snapshot)) return {};
	// The default is the highest-level castable spell. A confidently calibrated
	// alternative replaces it only when it ranks higher against this target class.
	// Without a castable spell, the lowest-level one reports why it is unavailable.
	const PlayerBotSpellDescriptor* fallback = nullptr;
	const PlayerBotSurvivalSpellObservation* fallbackSpell = nullptr;
	const PlayerBotSpellDescriptor* preferred = nullptr;
	const PlayerBotSurvivalSpellObservation* preferredSpell = nullptr;
	auto castable = [&snapshot](const PlayerBotSurvivalSpellObservation& spell) {
		return spell.learned && snapshot.level >= spell.level;
	};
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		const PlayerBotSurvivalSpellObservation* spell = spellObservation(snapshot, descriptor.name);
		if (!offensive(descriptor.role) || !spell || !spell->vocationAllowed) continue;
		if (!fallbackSpell || spell->level < fallbackSpell->level) {
			fallback = &descriptor;
			fallbackSpell = spell;
		}
		if (castable(*spell) && (!preferredSpell || spell->level > preferredSpell->level)) {
			preferred = &descriptor;
			preferredSpell = spell;
		}
	}
	if (!preferred) return fallback ? decideSpell(snapshot, fallback->name, "offense", now) : PlayerBotSurvivalCommand{};
	const std::string& kind = snapshot.target.targetClass;
	const PlayerBotSpellDescriptor* selected = preferred;
	double selectedRanking = calibration.ranking(preferred->name, kind, preferredSpell->envelope);
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		const PlayerBotSurvivalSpellObservation* spell = spellObservation(snapshot, descriptor.name);
		if (&descriptor == preferred || !offensive(descriptor.role) || !spell || !spell->vocationAllowed || !castable(*spell)) continue;
		const PlayerBotSpellProfile* profile = calibration.find(descriptor.name, kind, spell->envelope);
		const double ranking = calibration.ranking(descriptor.name, kind, spell->envelope);
		if (profile && profile->confidence >= 1.0 && ranking > selectedRanking) {
			selected = &descriptor;
			selectedRanking = ranking;
		}
	}
	return decideSpell(snapshot, selected->name, "offense", now);
}

std::optional<PlayerBotSurvivalSpellVerification> PlayerBotSurvivalRuntime::verifySpell(const PlayerBotSpellVerificationInput& input)
{
	const auto verification = spells.verify(input);
	if (!verification) return std::nullopt;
	PlayerBotSurvivalSpellVerification outcome;
	outcome.verification = *verification;
	outcome.calibration = calibration.observe(verification->pending.name, verification->pending.targetClass,
	    verification->pending.envelope, verification->evidence, verification->observation.value);
	outcome.rankingEstimate = calibration.ranking(verification->pending.name, verification->pending.targetClass, verification->pending.envelope);
	outcome.evictedProfile = calibration.takeEvictedProfile();
	return outcome;
}

void PlayerBotSurvivalRuntime::beginEngineSpellCast() { spells.beginEngineCast(); }
void PlayerBotSurvivalRuntime::endEngineSpellCast() { spells.endEngineCast(); }
void PlayerBotSurvivalRuntime::observeHasteAfterCast(int32_t ticks, int64_t endTime) { spells.observeHasteAfterCast(ticks, endTime); }
void PlayerBotSurvivalRuntime::observeHealthDrain(bool controlledPlayer) { spells.observeHealthDrain(controlledPlayer); }
void PlayerBotSurvivalRuntime::observeCombatDamage(uint32_t attackerId, uint32_t targetId, uint32_t playerId, uint32_t damage) { spells.observeCombatDamage(attackerId, targetId, playerId, damage); }
void PlayerBotSurvivalRuntime::observeHealthGain(bool controlledHealer, bool controlledTarget, uint32_t gain) { spells.observeHealthGain(controlledHealer, controlledTarget, gain); }
bool PlayerBotSurvivalRuntime::canRetrySpell(std::chrono::steady_clock::time_point now) const { return spells.canRetry(now); }
std::optional<PlayerBotSpellPendingCast> PlayerBotSurvivalRuntime::pendingSpell() const
{
	if (const PlayerBotSpellPendingCast* pending = spells.pending()) return *pending;
	return std::nullopt;
}
void PlayerBotSurvivalRuntime::deferSpellRetry(std::chrono::steady_clock::time_point now) { spells.deferRetry(now, retryDelay); }
std::optional<PlayerBotSpellProfile> PlayerBotSurvivalRuntime::calibrationProfile(const PlayerBotSpellPendingCast& pending) const
{
	if (const PlayerBotSpellProfile* profile = calibration.find(pending.name, pending.targetClass, pending.envelope)) return *profile;
	return std::nullopt;
}
double PlayerBotSurvivalRuntime::calibrationRanking(const PlayerBotSpellPendingCast& pending) const { return calibration.ranking(pending.name, pending.targetClass, pending.envelope); }
size_t PlayerBotSurvivalRuntime::calibrationSize() const { return calibration.size(); }

const char* PlayerBotSurvivalRuntime::magicTrainingReason(const PlayerBotSurvivalSnapshot& snapshot) const
{
	if (snapshot.hunting) return "hunting";
	if (snapshot.progressionActive) return "progression_objective";
	if (snapshot.combatActive) return "combat";
	if (snapshot.navigationPending) return "pending_navigation";
	if (hasPendingDefensiveWork() || needsHealing(snapshot)) return "defensive_work";
	if (!snapshot.canDoAction || snapshot.healingExhausted) return "spell_cooldown";
	if (snapshot.protectionZone) return "regeneration_paused";
	if (!snapshot.regenerationForecastActive) return "no_active_regeneration_forecast";
	if (static_cast<uint64_t>(snapshot.mana) + snapshot.regenerationManaGain <= snapshot.manaMaximum) return "next_tick_not_overflow";
	return decideMagicTraining(snapshot) ? nullptr : "no_audited_safe_spell";
}

std::optional<PlayerBotMagicTrainingCommand> PlayerBotSurvivalRuntime::decideMagicTraining(const PlayerBotSurvivalSnapshot& snapshot) const
{
	if (!snapshot.regenerationForecastActive) return std::nullopt;
	std::optional<PlayerBotMagicTrainingCommand> useful;
	std::optional<PlayerBotMagicTrainingCommand> refresh;
	const uint32_t reserve = healingManaReserve(snapshot);
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		const PlayerBotSurvivalSpellObservation* spell = spellObservation(snapshot, descriptor.name);
		if (!spell || !spell->magicTrainingEligible || spell->manaCost == 0 ||
		    spell->manaCost > static_cast<uint64_t>(snapshot.mana) - std::min<uint64_t>(snapshot.mana, reserve)) continue;
		PlayerBotMagicTrainingCommand candidate{descriptor.name, descriptor.words, descriptor.magicTrainingPriority, spell->manaCost,
		    reserve, false, snapshot.mana, snapshot.manaSpent, snapshot.magicLevel, snapshot.regenerationManaGain,
		    snapshot.regenerationTickInterval, snapshot.regenerationTickRemaining,
		    static_cast<uint64_t>(snapshot.mana) + snapshot.regenerationManaGain,
		    static_cast<uint64_t>(snapshot.mana) + snapshot.regenerationManaGain - snapshot.manaMaximum};
		const bool usefulEffect = descriptor.magicTrainingEffect == PlayerBotTrainingEffect::Haste ? !snapshot.hasteActive :
		                           descriptor.magicTrainingEffect == PlayerBotTrainingEffect::Light ? !snapshot.lightActive : false;
		if (usefulEffect) {
			if (!useful || descriptor.magicTrainingPriority > useful->priority) useful = candidate;
		} else if (descriptor.magicTrainingRefreshSafe && (!refresh || spell->manaCost < refresh->cost ||
		           (spell->manaCost == refresh->cost && descriptor.magicTrainingPriority > refresh->priority))) {
			candidate.refresh = true;
			refresh = candidate;
		}
	}
	return useful ? useful : refresh;
}
