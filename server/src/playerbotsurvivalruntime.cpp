/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotsurvivalruntime.h"

#include "condition.h"
#include "player.h"
#include "playerbotinventorypolicy.h"
#include "spells.h"

extern Spells* g_spells;

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

	std::string targetClass(const Creature* target)
	{
		if (!target) return "self";
		if (!target->getMonster()) return "creature";
		std::string name = target->getName();
		name.resize(std::min<size_t>(name.size(), 48));
		return "monster:" + name;
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
		    descriptor.magicTrainingEffect == PlayerBotTrainingEffect::None || !player.hasLearnedInstantSpell(descriptor.name)) return false;
		spell = g_spells ? g_spells->getInstantSpellByName(descriptor.name) : nullptr;
		if (!spell || spell->getWords() != descriptor.words || !spell->isLearnable() || !spell->isEnabled() ||
		    player.getLevel() < spell->getLevel() || player.getMagicLevel() < spell->getMagicLevel() ||
		    player.getSoul() < spell->getSoulCost() || (spell->isPremium() && !player.isPremium()) ||
		    (spell->getNeedWeapon() && !player.getWeapon(true)) || player.hasCondition(CONDITION_EXHAUST_HEAL) ||
		    spell->getAggressive() || !spell->getSelfTarget() || spell->getNeedTarget() || spell->getHasParam() ||
		    spell->getHasPlayerNameParam() || spell->getNeedDirection() || spell->getNeedCasterTargetOrDirection()) return false;
		cost = spell->getManaCost(&player);
		return cost != 0 && cost <= static_cast<uint64_t>(player.getMana()) -
		       std::min<uint64_t>(player.getMana(), magicTrainingEmergencyReserve);
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

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideHealing(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
	const Position& position, std::chrono::steady_clock::time_point now)
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
		spellAttempt = decideSpell(player, snapshot, position, "Light Healing", "recovery", nullptr, now);
		if (spellAttempt.type == PlayerBotSurvivalCommandType::CastSpell) return spellAttempt;
	}
	if (snapshot.potionCount == 0) {
		command.type = PlayerBotSurvivalCommandType::InterruptForService;
		command.reason = "healing_supply_missing";
		return command;
	}
	command.type = PlayerBotSurvivalCommandType::UsePotion;
	command.candidate = spellAttempt.candidate;
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
	command.itemClientId = snapshot.foodClientId;
	return command;
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
	const Position& position, const char* spellName, const char* need, Creature* target, std::chrono::steady_clock::time_point now)
{
	PlayerBotSurvivalCommand command;
	command.need = need;
	const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor(spellName);
	if (!descriptor) { command.reason = "unsupported_descriptor"; return command; }
	command.candidate = descriptor;
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName(descriptor->name) : nullptr;
	if (!spell || spell->getWords() != descriptor->words || !spell->isLearnable()) { command.reason = "unsupported_metadata"; return command; }
	if (!player.hasLearnedInstantSpell(descriptor->name)) { command.reason = "unlearned"; return command; }
	if (!snapshot.canDoAction || spells.hasPending() || !spells.canRetry(now)) return command;
	const bool healingGroup = descriptor->role == PlayerBotSpellRole::Healing || descriptor->role == PlayerBotSpellRole::Support;
	if (player.hasCondition(healingGroup ? CONDITION_EXHAUST_HEAL : CONDITION_EXHAUST_COMBAT)) { command.reason = "cooldown"; return command; }
	if ((descriptor->role == PlayerBotSpellRole::MeleeOffense || descriptor->role == PlayerBotSpellRole::RangedOffense) &&
	    (!target || target->isRemoved() || target->isDead() || player.getAttackedCreature() != target || !player.canSeeCreature(target) ||
	     !player.canSee(target->getPosition()) || !Position::areInRange<1, 1, 0>(position, target->getPosition()))) { command.reason = "lost_target"; return command; }
	if (spell->getNeedTarget() && !spell->canThrowSpell(&player, target)) { command.reason = "target_unreachable"; return command; }
	const uint32_t manaCost = spell->getManaCost(&player);
	const uint32_t reserve = descriptor->role == PlayerBotSpellRole::Healing ? 0 : higherPriorityRecoveryManaReserve;
	if (player.getMana() < manaCost + reserve) { command.reason = "insufficient_mana_reserve"; return command; }
	PlayerBotSpellPendingCast pending;
	pending.name = descriptor->name;
	pending.role = descriptor->role;
	pending.need = need;
	pending.manaBefore = player.getMana();
	pending.manaReserve = reserve;
	pending.healthBefore = player.getHealth();
	pending.targetId = target ? target->getID() : 0;
	pending.targetHealthBefore = target ? target->getHealth() : 0;
	pending.missingHealth = player.getMaxHealth() - player.getHealth();
	pending.hasteTicksBefore = snapshot.hasteActive ? player.getCondition(CONDITION_HASTE)->getTicks() : 0;
	pending.envelope = playerBotSpellEnvelope(player, *descriptor);
	pending.targetClass = targetClass(target);
	pending.otherRecovery = descriptor->role == PlayerBotSpellRole::Healing && player.hasCondition(CONDITION_REGENERATION);
	pending.observedAt = now;
	spells.begin(pending);
	command.type = PlayerBotSurvivalCommandType::CastSpell;
	command.spell = PlayerBotSurvivalSpellCommand{descriptor, spell, target, std::move(pending)};
	return command;
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideSupportSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
	const Position& position, std::chrono::steady_clock::time_point now)
{
	if (snapshot.hasteActive || snapshot.routeSteps < minimumHasteRouteSteps || needsHealing(snapshot)) return {};
	return decideSpell(player, snapshot, position, "Haste", "safe_route", nullptr, now);
}

PlayerBotSurvivalCommand PlayerBotSurvivalRuntime::decideOffensiveSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
	const Position& position, Creature* target, std::chrono::steady_clock::time_point now)
{
	if (needsHealing(snapshot)) return {};
	if (player.hasLearnedInstantSpell("Berserk") && player.getLevel() >= 35) {
		const PlayerBotSpellDescriptor* ranged = playerBotSpellDescriptor("Whirlwind Throw");
		const PlayerBotSpellDescriptor* melee = playerBotSpellDescriptor("Berserk");
		const std::string kind = targetClass(target);
		if (ranged && melee) {
			const PlayerBotSpellProfile* profile = calibration.find(ranged->name, kind);
			if (profile && profile->confidence >= 1.0 &&
			    calibration.ranking(ranged->name, kind, playerBotSpellEnvelope(player, *ranged)) >
			        calibration.ranking(melee->name, kind, playerBotSpellEnvelope(player, *melee))) {
				return decideSpell(player, snapshot, position, ranged->name, "offense", target, now);
			}
		}
		return decideSpell(player, snapshot, position, "Berserk", "offense", target, now);
	}
	return decideSpell(player, snapshot, position, "Whirlwind Throw", "offense", target, now);
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
const PlayerBotSpellPendingCast* PlayerBotSurvivalRuntime::pendingSpell() const { return spells.pending(); }
void PlayerBotSurvivalRuntime::deferSpellRetry(std::chrono::steady_clock::time_point now) { spells.deferRetry(now, retryDelay); }
const PlayerBotSpellProfile* PlayerBotSurvivalRuntime::calibrationProfile(const PlayerBotSpellPendingCast& pending) const { return calibration.find(pending.name, pending.targetClass); }
double PlayerBotSurvivalRuntime::calibrationRanking(const PlayerBotSpellPendingCast& pending) const { return calibration.ranking(pending.name, pending.targetClass, pending.envelope); }
size_t PlayerBotSurvivalRuntime::calibrationSize() const { return calibration.size(); }
const PlayerBotSpellProfile& PlayerBotSurvivalRuntime::observeCalibrationFixture(const std::string& spell, const std::string& targetClass,
	const PlayerBotSpellEnvelope& envelope, PlayerBotSpellEvidence evidence, int32_t value)
{
	return calibration.observe(spell, targetClass, envelope, evidence, value);
}
std::optional<std::string> PlayerBotSurvivalRuntime::takeCalibrationEviction() { return calibration.takeEvictedProfile(); }
void PlayerBotSurvivalRuntime::clearCalibration() { calibration.clear(); }

const char* PlayerBotSurvivalRuntime::magicTrainingReason(const Player& player, const PlayerBotSurvivalSnapshot& snapshot) const
{
	if (snapshot.hunting) return "hunting";
	if (snapshot.progressionActive) return "progression_objective";
	if (snapshot.combatOrPursuit) return "combat_or_pursuit";
	if (snapshot.navigationPending) return "pending_navigation";
	if (hasPendingDefensiveWork() || needsHealing(snapshot)) return "defensive_work";
	if (!snapshot.canDoAction || snapshot.healingExhausted) return "spell_cooldown";
	if (player.getZone() == ZONE_PROTECTION) return "regeneration_paused";
	const auto forecast = player.getManaRegenerationForecast();
	if (!forecast) return "no_active_regeneration_forecast";
	if (static_cast<uint64_t>(player.getMana()) + forecast->gain <= player.getMaxMana()) return "next_tick_not_overflow";
	return decideMagicTraining(player, snapshot) ? nullptr : "no_audited_safe_spell";
}

std::optional<PlayerBotMagicTrainingCommand> PlayerBotSurvivalRuntime::decideMagicTraining(const Player& player,
	const PlayerBotSurvivalSnapshot&) const
{
	const auto forecast = player.getManaRegenerationForecast();
	if (!forecast) return std::nullopt;
	std::optional<PlayerBotMagicTrainingCommand> useful;
	std::optional<PlayerBotMagicTrainingCommand> refresh;
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		InstantSpell* spell = nullptr;
		uint64_t cost = 0;
		if (!magicTrainingSpellLegal(player, descriptor, spell, cost)) continue;
		PlayerBotMagicTrainingCommand candidate{&descriptor, spell, cost, false, player.getMana(), player.getSpentMana(),
		    player.getBaseMagicLevel(), forecast->gain, forecast->interval, forecast->remaining,
		    static_cast<uint64_t>(player.getMana()) + forecast->gain,
		    static_cast<uint64_t>(player.getMana()) + forecast->gain - player.getMaxMana()};
		if (magicTrainingEffectUseful(player, descriptor.magicTrainingEffect)) {
			if (!useful || descriptor.magicTrainingPriority > useful->descriptor->magicTrainingPriority) useful = candidate;
		} else if (descriptor.magicTrainingRefreshSafe && (!refresh || cost < refresh->cost ||
		           (cost == refresh->cost && descriptor.magicTrainingPriority > refresh->descriptor->magicTrainingPriority))) {
			candidate.refresh = true;
			refresh = candidate;
		}
	}
	return useful ? useful : refresh;
}
