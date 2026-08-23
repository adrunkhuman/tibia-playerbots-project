/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotsurvivalruntime.h"

#include "playerbotinventorypolicy.h"

#include <algorithm>

namespace {
	constexpr int32_t healingHealthPercent = 60;
	constexpr int32_t meatFoodTicks = 108000;
	constexpr int32_t maximumFoodSeconds = 1200;
	constexpr uint32_t maximumEatFailures = 3;
	constexpr int32_t smallHealthPotionMaximumHealing = 90;
	constexpr uint32_t higherPriorityRecoveryManaReserve = 20;
	constexpr uint32_t minimumHasteRouteSteps = 20;
	constexpr uint32_t magicTrainingEmergencyReserve = 20;
	constexpr auto retryDelay = std::chrono::seconds(2);
	constexpr auto foodCooldown = std::chrono::minutes(5);

	const PlayerBotSurvivalSpellObservation* spellObservation(const PlayerBotSurvivalSnapshot& snapshot, const char* name)
	{
		const auto found = std::find_if(snapshot.spells.begin(), snapshot.spells.end(), [name](const auto& spell) {
			return spell.name == name;
		});
		return found == snapshot.spells.end() ? nullptr : &*found;
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

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideHealing(const PlayerBotSurvivalSnapshot& snapshot,
	std::chrono::steady_clock::time_point now)
{
	PlayerBotSurvivalCommand command;
	PlayerBotSurvivalCommand spellAttempt;
	if (const auto verification = recovery.verifyPotion({snapshot.health, snapshot.healthMaximum, snapshot.potionCount}, now, retryDelay)) {
		command.potionVerification = verification;
	}
	if (snapshot.buyingPotions || !needsHealing(snapshot)) return command;
	if (!recovery.canRetryPotion(now) || !snapshot.canDoAction) {
		command.type = PlayerBotSurvivalCommandType::Wait;
		return command;
	}
	if (snapshot.healthMaximum - snapshot.health <= smallHealthPotionMaximumHealing || snapshot.potionCount == 0) {
		spellAttempt = decideSpell(snapshot, "Light Healing", "recovery", now);
		if (spellAttempt.type == PlayerBotSurvivalCommandType::CastSpell) return spellAttempt;
	}
	if (snapshot.potionCount == 0) {
		command.type = PlayerBotSurvivalCommandType::InterruptForService;
		command.reason = "healing_supply_missing";
		return command;
	}
	command.type = PlayerBotSurvivalCommandType::UsePotion;
	command.itemId = playerbot::smallHealthPotionItemId;
	command.candidateName = spellAttempt.candidateName;
	command.need = spellAttempt.need;
	command.reason = spellAttempt.reason;
	return command;
}

void PlayerBotSurvivalRuntime::beginPotion(const PlayerBotSurvivalSnapshot& snapshot)
{
	recovery.beginPotion({snapshot.health, snapshot.healthMaximum, snapshot.potionCount});
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
	if (!recovery.canRetryFood(now) || !canEat || snapshot.foodInventoryCount <= playerbot::preferredFoodCount || snapshot.foodClientId == 0) return command;
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
	if (!snapshot.canDoAction || spells.hasPending() || !spells.canRetry(now)) return command;
	const bool healingGroup = descriptor->role == PlayerBotSpellRole::Healing || descriptor->role == PlayerBotSpellRole::Support;
	if (healingGroup ? snapshot.healingExhausted : snapshot.combatExhausted) { command.reason = "cooldown"; return command; }
	if ((descriptor->role == PlayerBotSpellRole::MeleeOffense || descriptor->role == PlayerBotSpellRole::RangedOffense) &&
	    !snapshot.target.valid) { command.reason = "lost_target"; return command; }
	if (!spell->targetReachable) { command.reason = "target_unreachable"; return command; }
	const uint32_t manaCost = spell->manaCost;
	const uint32_t reserve = descriptor->role == PlayerBotSpellRole::Healing ? 0 : higherPriorityRecoveryManaReserve;
	if (snapshot.mana < manaCost + reserve) { command.reason = "insufficient_mana_reserve"; return command; }
	PlayerBotSpellPendingCast pending;
	pending.name = descriptor->name;
	pending.role = descriptor->role;
	pending.need = need;
	pending.manaBefore = snapshot.mana;
	pending.manaReserve = reserve;
	pending.healthBefore = snapshot.health;
	pending.targetId = snapshot.target.id;
	pending.targetHealthBefore = snapshot.target.health;
	pending.missingHealth = snapshot.healthMaximum - snapshot.health;
	pending.hasteTicksBefore = snapshot.hasteTicks;
	pending.envelope = spell->envelope;
	pending.targetClass = snapshot.target.targetClass;
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
	const PlayerBotSurvivalSpellObservation* berserk = spellObservation(snapshot, "Berserk");
	if (berserk && berserk->learned && snapshot.level >= 35) {
		const PlayerBotSpellDescriptor* ranged = playerBotSpellDescriptor("Whirlwind Throw");
		const PlayerBotSpellDescriptor* melee = playerBotSpellDescriptor("Berserk");
		const PlayerBotSurvivalSpellObservation* rangedSpell = spellObservation(snapshot, "Whirlwind Throw");
		const std::string& kind = snapshot.target.targetClass;
		if (ranged && melee && rangedSpell) {
			const PlayerBotSpellProfile* profile = calibration.find(ranged->name, kind);
			if (profile && profile->confidence >= 1.0 &&
			    calibration.ranking(ranged->name, kind, rangedSpell->envelope) >
			        calibration.ranking(melee->name, kind, berserk->envelope)) {
				return decideSpell(snapshot, ranged->name, "offense", now);
			}
		}
		return decideSpell(snapshot, "Berserk", "offense", now);
	}
	return decideSpell(snapshot, "Whirlwind Throw", "offense", now);
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
	if (const PlayerBotSpellProfile* profile = calibration.find(pending.name, pending.targetClass)) return *profile;
	return std::nullopt;
}
double PlayerBotSurvivalRuntime::calibrationRanking(const PlayerBotSpellPendingCast& pending) const { return calibration.ranking(pending.name, pending.targetClass, pending.envelope); }
size_t PlayerBotSurvivalRuntime::calibrationSize() const { return calibration.size(); }

const char* PlayerBotSurvivalRuntime::magicTrainingReason(const PlayerBotSurvivalSnapshot& snapshot) const
{
	if (snapshot.hunting) return "hunting";
	if (snapshot.progressionActive) return "progression_objective";
	if (snapshot.combatOrPursuit) return "combat_or_pursuit";
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
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		const PlayerBotSurvivalSpellObservation* spell = spellObservation(snapshot, descriptor.name);
		if (!spell || !spell->magicTrainingEligible || spell->manaCost == 0 ||
		    spell->manaCost > static_cast<uint64_t>(snapshot.mana) - std::min<uint64_t>(snapshot.mana, magicTrainingEmergencyReserve)) continue;
		PlayerBotMagicTrainingCommand candidate{descriptor.name, descriptor.words, descriptor.magicTrainingPriority, spell->manaCost,
		    false, snapshot.mana, snapshot.manaSpent, snapshot.magicLevel, snapshot.regenerationManaGain,
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
