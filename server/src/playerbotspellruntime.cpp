/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotspellruntime.h"

namespace {
	constexpr uint32_t maximumSpellObservationValue = 10000;
	constexpr int32_t maximumHasteObservationDelay = 2000;
}

bool PlayerBotSpellRuntime::hasPending() const
{
	return !pendingCast.name.empty();
}

const PlayerBotSpellPendingCast* PlayerBotSpellRuntime::pending() const
{
	return hasPending() ? &pendingCast : nullptr;
}

void PlayerBotSpellRuntime::begin(PlayerBotSpellPendingCast cast)
{
	pendingCast = std::move(cast);
}

void PlayerBotSpellRuntime::beginEngineCast()
{
	engineCastExecuting = true;
}

void PlayerBotSpellRuntime::endEngineCast()
{
	engineCastExecuting = false;
}

void PlayerBotSpellRuntime::observeHasteAfterCast(int32_t ticks, int64_t endTime)
{
	if (hasPending()) {
		pendingCast.hasteTicksAfterCast = ticks;
		pendingCast.hasteEndTimeAfterCast = endTime;
	}
}

void PlayerBotSpellRuntime::observeHealthDrain(bool controlledPlayer)
{
	if (controlledPlayer && hasPending()) {
		pendingCast.concurrentDamage = true;
	}
}

void PlayerBotSpellRuntime::observeCombatDamage(uint32_t attackerId, uint32_t targetId, uint32_t playerId, uint32_t damage)
{
	if (!hasPending() || !engineCastExecuting) {
		return;
	}
	if (attackerId != playerId) {
		if (targetId == pendingCast.targetId) {
			pendingCast.otherAttacker = true;
		}
		return;
	}
	for (uint8_t index = 0; index < pendingCast.spellVictimCount; ++index) {
		if (pendingCast.spellVictimIds[index] == targetId) {
			if (targetId == pendingCast.targetId) {
				pendingCast.observedSpellDamage = std::min<uint32_t>(maximumSpellObservationValue,
					pendingCast.observedSpellDamage + damage);
			}
			return;
		}
	}
	if (pendingCast.spellVictimCount < pendingCast.spellVictimIds.size()) {
		pendingCast.spellVictimIds[pendingCast.spellVictimCount++] = targetId;
	} else {
		pendingCast.spellVictimOverflow = true;
	}
	if (targetId == pendingCast.targetId) {
		pendingCast.observedSpellDamage = std::min<uint32_t>(maximumSpellObservationValue,
			pendingCast.observedSpellDamage + damage);
	}
}

void PlayerBotSpellRuntime::observeHealthGain(bool controlledHealer, bool controlledTarget, uint32_t gain)
{
	if (!hasPending() || !controlledTarget || pendingCast.role != PlayerBotSpellRole::Healing) {
		return;
	}
	if (controlledHealer && engineCastExecuting) {
		pendingCast.observedSpellHealing = std::min<uint32_t>(maximumSpellObservationValue,
			pendingCast.observedSpellHealing + gain);
	} else {
		pendingCast.otherRecovery = true;
	}
}

std::optional<PlayerBotSpellVerification> PlayerBotSpellRuntime::verify(const PlayerBotSpellVerificationInput& input)
{
	if (!hasPending()) {
		return std::nullopt;
	}
	PlayerBotSpellVerification verification;
	verification.pending = pendingCast;
	verification.manaSpent = input.mana < pendingCast.manaBefore;
	verification.observation.manaSpent = verification.manaSpent;
	verification.observation.concurrentDamage = pendingCast.concurrentDamage;
	verification.observation.otherRecovery = pendingCast.otherRecovery;
	verification.observation.otherAttacker = pendingCast.otherAttacker;
	verification.observation.meleeOrOtherBotDamage = pendingCast.meleeOrOtherBotDamage;
	if (pendingCast.role == PlayerBotSpellRole::Healing) {
		verification.observation.value = pendingCast.observedSpellHealing;
		verification.observed = input.health > pendingCast.healthBefore;
		if (!pendingCast.concurrentDamage && input.health - pendingCast.healthBefore !=
		    static_cast<int32_t>(pendingCast.observedSpellHealing)) {
			verification.observation.otherRecovery = true;
		}
	} else if (pendingCast.role == PlayerBotSpellRole::Support) {
		const int32_t elapsed = static_cast<int32_t>(std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			input.observedAt - pendingCast.observedAt).count(), 0, maximumHasteObservationDelay));
		verification.pending.hasteTicksObserved = input.hasteTicks;
		verification.pending.hasteDurationMeasured = input.hasteTicks + elapsed;
		const bool newlyApplied = input.hasteEndTime != 0 && pendingCast.hasteTicksBefore == 0 &&
		                          pendingCast.hasteTicksAfterCast > 0 &&
		                          pendingCast.hasteEndTimeAfterCast != 0 &&
		                          input.hasteEndTime == pendingCast.hasteEndTimeAfterCast;
		verification.observation.value = newlyApplied ? verification.pending.hasteDurationMeasured : 0;
		verification.observed = verification.observation.value > 0;
	} else {
		verification.observation.targetStable = input.targetStable;
		verification.observation.value = pendingCast.observedSpellDamage;
		verification.observation.multiTarget = pendingCast.spellVictimOverflow || pendingCast.spellVictimCount > 1;
		verification.observed = verification.observation.value > 0;
	}
	verification.evidence = playerBotClassifySpellObservation(pendingCast.role, verification.observation,
	                                                          pendingCast.missingHealth, pendingCast.envelope);
	verification.success = verification.manaSpent && verification.observed;
	pendingCast = PlayerBotSpellPendingCast{};
	return verification;
}

void PlayerBotSpellRuntime::deferRetry(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration delay)
{
	retryAfter = now + delay;
}

bool PlayerBotSpellRuntime::canRetry(std::chrono::steady_clock::time_point now) const
{
	return now >= retryAfter;
}
