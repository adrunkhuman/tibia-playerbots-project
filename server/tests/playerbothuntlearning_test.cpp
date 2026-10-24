#include "playerbothuntpolicy.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {
PlayerBotHuntRegion region(uint64_t variant = 1)
{
	PlayerBotHuntRegion value;
	value.atlasVariantId = variant;
	value.atlasRevision = 1;
	value.currentHealth = value.maximumHealth = 205;
	value.supplyProfile.potions = 3;
	value.supplyProfile.potionHealing = 125;
	value.supplyProfile.mana = value.supplyProfile.maximumMana = 65;
	value.supplyStaticPotionsPerCombatSecond = 2.35 / 60;
	return value;
}

PlayerBotSupplyObservation outing(PlayerBotHuntPolicy& policy, const PlayerBotHuntRegion& area,
    double activeSeconds, uint32_t kills, uint32_t potions = 0, int32_t health = 205,
    bool interrupted = true, uint64_t duration = 39)
{
	policy.resetCombatEvidence();
	policy.observeCombat({true, activeSeconds, health, 205, 65, 65, 1});
	for (uint32_t i = 0; i < kills; ++i) policy.observeKill();
	for (uint32_t i = 0; i < potions; ++i) policy.observeRecovery(true);
	return policy.observeSupplies(area, area.supplyCapability, duration, health, 205, 65, 3 - potions, interrupted);
}

void shortPotionSampleDoesNotRatchetCosts()
{
	PlayerBotHuntPolicy policy;
	const auto area = region();
	const auto spike = outing(policy, area, 3.5, 1, 1, 169);
	assert(!spike.accepted && !spike.globalUpdated && !spike.localUpdated);
	assert(spike.globalMultiplierAfter == 1);
	PlayerBotSupplyObservation update;
	for (int i = 0; i < 32; ++i) update = outing(policy, area, 30, 12, 0, 205, true, 100);
	assert(update.globalMultiplierAfter < 1);
	assert(update.calibration.samples > 0);
	assert(update.calibration.potionsPerCombatSecond < area.supplyStaticPotionsPerCombatSecond);
}

void globalEvidencePoolsAcrossAreasButLocalEvidenceDoesNot()
{
	PlayerBotHuntPolicy policy;
	auto firstArea = region(1);
	auto secondArea = region(2);
	firstArea.supplyStaticPotionsPerCombatSecond = 1.0 / 60;
	secondArea.supplyStaticPotionsPerCombatSecond = 2.0 / 60;
	const auto first = outing(policy, firstArea, 30, 4, 1, 205, true, 60);
	assert(!first.accepted);
	const auto second = outing(policy, secondArea, 30, 4, 0, 205, true, 60);
	assert(second.globalUpdated && !second.localUpdated);
	// One observed potion versus 0.5 + 1 predicted, not an unweighted
	// average of the two areas' separate ratios.
	assert(std::abs(second.globalMultiplierAfter - (0.9 + 0.1 / 1.5)) < 1e-12);
	assert(policy.regionPerformance().at(1).supply.samples == 0);
	assert(policy.regionPerformance().at(2).supply.samples == 0);
}

void evidenceDoesNotCrossMaterialCapabilityOrAtlasChanges()
{
	for (int change = 0; change < 3; ++change) {
		PlayerBotHuntPolicy policy;
		auto area = region();
		assert(!outing(policy, area, 30, 4, 0, 205, true, 60).accepted);
		if (change == 0) ++area.atlasRevision;
		else if (change == 1) area.supplyCapability.equipmentItemIds[5] = 2395;
		else area.supplyCapability.spellLegal = true;
		assert(!outing(policy, area, 30, 4, 0, 205, true, 60).accepted);
		assert(outing(policy, area, 30, 4, 0, 205, true, 60).globalUpdated);
	}
}

void dangerStillCorrectsImmediately()
{
	PlayerBotHuntPolicy policy;
	const auto unsafe = outing(policy, region(), 3.5, 1, 1, 20);
	assert(unsafe.accepted && unsafe.globalUpdated);
	assert(unsafe.globalMultiplierAfter > 1);
	assert(unsafe.localEstimateDirection == PlayerBotSupplyEstimateDirection::Upward);
	// Unsafe evidence is not pooled as cheap exposure. A new safe window
	// must qualify before it can lower the conservative correction.
	const auto firstSafe = outing(policy, region(), 30, 4, 0, 205, true, 60);
	assert(!firstSafe.globalUpdated && firstSafe.globalMultiplierAfter == unsafe.globalMultiplierAfter);
	const auto secondSafe = outing(policy, region(), 30, 4, 0, 205, true, 60);
	assert(secondSafe.globalUpdated && secondSafe.globalMultiplierAfter < unsafe.globalMultiplierAfter);
}

void healthPressureDiscardsPendingEasyCombat()
{
	PlayerBotHuntPolicy policy;
	for (int i = 0; i < 4; ++i) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, 30, i == 1 ? 60 : 90, 100, 100, 100, 1});
		for (int k = 0; k < 4; ++k) policy.observeKill();
		const auto update = policy.updateChallengeFrontier({60, 100});
		assert(update.result == (i < 3 ? PlayerBotHuntChallengeResult::InsufficientActiveCombat :
		    PlayerBotHuntChallengeResult::Escalated));
	}
}

void challengeEvidenceDoesNotCrossCapabilities()
{
	PlayerBotHuntPolicy policy;
	PlayerBotHuntChallengeSample sample{60, 100};
	for (int i = 0; i < 3; ++i) {
		policy.resetCombatEvidence();
		policy.observeCombat({true, 30, 90, 100, 100, 100, 1});
		for (int k = 0; k < 4; ++k) policy.observeKill();
		if (i == 1) sample.capability.equipmentItemIds[5] = 2395;
		const auto update = policy.updateChallengeFrontier(sample);
		assert(update.result == (i < 2 ? PlayerBotHuntChallengeResult::InsufficientActiveCombat :
		    PlayerBotHuntChallengeResult::Escalated));
	}
}

void shortSafeFightsCanAdvanceChallenge()
{
	PlayerBotHuntPolicy policy;
	auto safe = [&] {
		policy.resetCombatEvidence();
		policy.observeCombat({true, 30, 90, 100, 100, 100, 1});
		for (int i = 0; i < 4; ++i) policy.observeKill();
		return policy.updateChallengeFrontier({60, 100});
	};
	assert(safe().result == PlayerBotHuntChallengeResult::InsufficientActiveCombat);
	assert(safe().result == PlayerBotHuntChallengeResult::Escalated);
	policy.resetCombatEvidence();
	policy.observeDeath();
	const auto death = policy.updateChallengeFrontier({1, 100});
	assert(death.result == PlayerBotHuntChallengeResult::Backoff);
	assert(death.frontierAfter < death.frontierBefore);
	// Backoff clears pending evidence and still requires two qualified holds.
	assert(safe().result == PlayerBotHuntChallengeResult::InsufficientActiveCombat);
	assert(safe().result == PlayerBotHuntChallengeResult::Hold);
	assert(safe().result == PlayerBotHuntChallengeResult::InsufficientActiveCombat);
	assert(safe().result == PlayerBotHuntChallengeResult::Hold);
	assert(safe().result == PlayerBotHuntChallengeResult::InsufficientActiveCombat);
	assert(safe().result == PlayerBotHuntChallengeResult::Escalated);
}
}

int main()
{
	shortPotionSampleDoesNotRatchetCosts();
	globalEvidencePoolsAcrossAreasButLocalEvidenceDoesNot();
	evidenceDoesNotCrossMaterialCapabilityOrAtlasChanges();
	dangerStillCorrectsImmediately();
	shortSafeFightsCanAdvanceChallenge();
	challengeEvidenceDoesNotCrossCapabilities();
	healthPressureDiscardsPendingEasyCombat();
	std::cout << "playerbot hunt learning contracts passed\n";
}
