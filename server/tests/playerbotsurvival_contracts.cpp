// Real survival runtime policy checks; see playerbotsurvival_contracts.sh.
#include "playerbotsurvivalruntime.h"
#include "playerbotsupplyrecovery.h"
#include "playerbotsupplypolicy.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace {
using Command = PlayerBotSurvivalCommandType;
using Clock = std::chrono::steady_clock;
const auto now = Clock::time_point{} + std::chrono::seconds(10);
constexpr uint16_t healthPotion = 7618;
constexpr uint16_t manaPotion = 7620;
constexpr uint32_t healingCost = 20;

PlayerBotSurvivalSnapshot snapshot()
{
	PlayerBotSurvivalSnapshot result;
	result.health = 50;
	result.healthMaximum = 100;
	result.mana = healingCost;
	result.manaMaximum = 200;
	result.potionItemId = healthPotion;
	result.potionCount = 2;
	result.manaPotionItemId = manaPotion;
	result.manaPotionCount = 2;
	result.canDoAction = true;
	PlayerBotSurvivalSpellObservation healing;
	healing.name = "Light Healing";
	healing.metadataMatches = true;
	healing.learned = true;
	healing.targetReachable = true;
	healing.vocationAllowed = true;
	healing.requirementsMet = true;
	healing.manaCost = healingCost;
	healing.envelope = {10, 20, 0};
	result.spells.push_back(healing);
	return result;
}

void expectPotion(const PlayerBotSurvivalCommand& command, uint16_t itemId)
{
	assert(command.type == Command::UsePotion);
	assert(command.itemId == itemId);
	assert(!command.spell);
}

void expectHealingSpell(const PlayerBotSurvivalRuntime& runtime, const PlayerBotSurvivalCommand& command)
{
	assert(command.type == Command::CastSpell);
	assert(command.spell);
	assert(command.spell->name == "Light Healing");
	assert(command.spell->words == "exura");
	assert(command.spell->role == PlayerBotSpellRole::Healing);
	assert(command.itemId == 0);
	assert(runtime.pendingSpell());
	assert(runtime.pendingSpell()->name == "Light Healing");
}

void typedHealthPreparation()
{
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.health = 75;
	PlayerBotPreparationRequirement requirement;
	requirement.kind = PlayerBotRequirementKind::HealthRegeneration;
	requirement.health = 80;
	auto options = runtime.healthPreparations(state, requirement, false, false);
	assert(options == std::vector<PlayerBotPreparationOption>{PlayerBotPreparationOption::RestoreHealth});
	state.departureHealthTarget = 80;
	expectHealingSpell(runtime, runtime.decideHealing(state, now));
	// Admission must include the executor's mana-potion -> re-evaluated heal,
	// even above the ordinary healing trigger and without a health potion.
	auto assisted = snapshot();
	assisted.health = 75; assisted.departureHealthTarget = 80;
	assisted.mana = 0; assisted.potionCount = 0; assisted.manaPotionCount = 1;
	PlayerBotSurvivalRuntime manaAssist;
	assert(manaAssist.healthPreparations(assisted, requirement, false, false) ==
	       std::vector<PlayerBotPreparationOption>{PlayerBotPreparationOption::RestoreHealth});
	auto assistedCommand = manaAssist.decideHealing(assisted, now);
	expectPotion(assistedCommand, manaPotion);
	assert(assistedCommand.reason == "healing_spell_mana");
	manaAssist.beginPotion(assisted, manaPotion, assistedCommand.reason);
	assisted.mana = 20; assisted.manaPotionCount = 0;
	assistedCommand = manaAssist.decideHealing(assisted, now + std::chrono::seconds(3));
	assert(assistedCommand.potionVerification &&
	       assistedCommand.potionVerification->result == PlayerBotPotionVerificationResult::Success);
	expectHealingSpell(manaAssist, assistedCommand);
	assisted.spells.front().learned = false;
	assert(!manaAssist.canPrepareHealth(assisted));
	assisted.spells.front().learned = true; assisted.mana = 0;
	assert(!manaAssist.canPrepareHealth(assisted)); // No carried mana left.
	state.spells.front().learned = false;
	PlayerBotSurvivalRuntime potionRuntime;
	assert(potionRuntime.canPrepareHealth(state));
	expectPotion(potionRuntime.decideHealing(state, now), healthPotion);
	state.potionCount = 0;
	assert(runtime.healthPreparations(state, requirement, false, false).empty());
	state.regenerationActive = true;
	state.foodTicks = 180000;
	assert(runtime.healthPreparations(state, requirement, true, false) ==
	       std::vector<PlayerBotPreparationOption>{PlayerBotPreparationOption::Regenerate});
	state.protectionZone = true;
	requirement.observeRegeneration = true;
	state.health = 80;
	assert(!runtime.healthPreparationSatisfied(state, requirement, true)); // Engine pauses in PZ.
	state.protectionZone = false;
	assert(!runtime.healthPreparationSatisfied(state, requirement, false)); // Rest still needs live safety proof.
	assert(runtime.healthPreparationSatisfied(state, requirement, true));
	state.regenerationActive = false;
	state.foodTicks = 0;
	state.foodClientId = 100;
	state.foodItemId = 2666;
	state.foodCount = 1;
	assert(runtime.healthPreparations(state, requirement, true, false) ==
	       std::vector<PlayerBotPreparationOption>{PlayerBotPreparationOption::Regenerate});
	assert(!runtime.healthPreparationSatisfied(state, requirement, true)); // Carried food is not an effect.
	PlayerBotSurvivalRuntime eating;
	state.lootMovePending = true; // Unobserved neutral purchase receipt: do not consume it.
	auto command = eating.decideFood(state, now);
	assert(command.type == Command::None);
	state.lootMovePending = false;
	command = eating.decideFood(state, now);
	assert(command.type == Command::UseFood);
	state.foodClientId = 0; state.foodCount = 0; state.pendingFoodCount = 0;
	state.regenerationActive = true; state.foodTicks = 180000;
	command = eating.decideFood(state, now + std::chrono::seconds(1));
	assert(command.foodVerification && command.foodVerification->result == PlayerBotFoodVerificationResult::Success);
	assert(runtime.healthPreparationSatisfied(state, requirement, true)); // Observed, then hunt must re-evaluate.
	state.regenerationActive = false; state.foodTicks = 0;
	assert(runtime.healthPreparations(state, requirement, true, true) ==
	       std::vector<PlayerBotPreparationOption>{PlayerBotPreparationOption::BuyFood});
	assert(runtime.healthPreparations(state, requirement, true, false).empty()); // Unaffordable/no offer.
	assert(runtime.healthPreparations(state, requirement, false, true).empty()); // No safe reachable rest.

	// Food service derives buyingPotions=false; healing preempts preparation,
	// independently of the receipt's consumption guard.
	auto emergency = snapshot();
	emergency.health = 25; emergency.combatActive = true;
	emergency.buyingPotions = false; emergency.lootMovePending = true;
	PlayerBotSurvivalRuntime criticalPotion;
	expectPotion(criticalPotion.decideHealing(emergency, now), healthPotion);
	emergency.potionCount = 0;
	PlayerBotSurvivalRuntime criticalSpell;
	expectHealingSpell(criticalSpell, criticalSpell.decideHealing(emergency, now));
}

void departureHealthPreparation()
{
	assert(playerBotSupplyRecoveryRequiredHealth(185) == 148);
	assert(playerBotSupplyRecoveryRequiredHealth(186) == 149);
	PlayerBotDepartureHealthPreparation preparation;
	assert(preparation.advance(128, 185, 148, now) == PlayerBotDepartureHealthResult::Recovering);
	assert(preparation.active() && preparation.target() == 148);
	assert(preparation.advance(128, 185, 148, now + std::chrono::seconds(59)) == PlayerBotDepartureHealthResult::Recovering);
	assert(preparation.advance(128, 185, 148, now + std::chrono::seconds(60)) == PlayerBotDepartureHealthResult::TimedOut);
	assert(preparation.advance(148, 185, 148, now + std::chrono::seconds(61)) == PlayerBotDepartureHealthResult::Ready);
	assert(!preparation.active());
	assert(preparation.advance(128, 185, 0, now) == PlayerBotDepartureHealthResult::Ready);
	auto state = snapshot();
	state.health = 128; state.healthMaximum = 185;
	state.potionCount = 20; state.spells.clear();
	PlayerBotSurvivalRuntime runtime;
	assert(!runtime.needsHealing(state) && runtime.decideHealing(state, now).type == Command::None);
	state.departureHealthTarget = 148;
	assert(runtime.needsHealing(state));
	expectPotion(runtime.decideHealing(state, now), healthPotion);
	state.health = 148;
	assert(!runtime.needsHealing(state));
	state.health = 128; state.combatActive = true;
	assert(!runtime.needsHealing(state)); // No global change to emergency healing.
	state.health = 111;
	assert(runtime.needsHealing(state));
	state.combatActive = false; state.health = 128; state.potionCount = 0;
	assert(runtime.decideHealing(state, now).type == Command::InterruptForService);
	state = snapshot(); state.health = 128; state.healthMaximum = 185; state.departureHealthTarget = 148;
	PlayerBotSurvivalRuntime spellRuntime;
	expectHealingSpell(spellRuntime, spellRuntime.decideHealing(state, now));
}

void percentageBoundaries()
{
	// Large maxima also catch 32-bit percentage multiplication overflow.
	for (int32_t maximum : {100, 1000, 2000000000}) {
		for (int offset : {-1, 0, 1}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.healthMaximum = maximum;
			state.health = static_cast<int32_t>(static_cast<int64_t>(maximum) * 60 / 100) + offset;
			assert(runtime.needsHealing(state) == (offset <= 0));
			const auto command = runtime.decideHealing(state, now);
			if (offset <= 0) expectHealingSpell(runtime, command);
			else assert(command.type == Command::None && !runtime.pendingSpell());
		}
		for (int offset : {-1, 0, 1}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.healthMaximum = maximum;
			state.health = static_cast<int32_t>(static_cast<int64_t>(maximum) * 30 / 100) + offset;
			const auto command = runtime.decideHealing(state, now);
			if (offset <= 0) {
				expectPotion(command, healthPotion);
				assert(!runtime.pendingSpell());
			} else expectHealingSpell(runtime, command);
		}
	}
	// A level-8 Knight's 185 HP does not have an integral 30% threshold.
	for (int32_t health : {55, 56, 110, 111, 112}) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.healthMaximum = 185;
		state.health = health;
		assert(runtime.needsHealing(state) == (health <= 111));
		const auto command = runtime.decideHealing(state, now);
		if (health == 55) expectPotion(command, healthPotion);
		else if (health <= 111) expectHealingSpell(runtime, command);
		else assert(command.type == Command::None);
	}
}

void spellFirstAndCriticalOverride()
{
	for (bool combat : {false, true}) {
		for (int32_t health : {31, 50, 60}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.healthMaximum = 1000;
			state.health = health * 10;
			state.combatActive = combat;
			assert(state.healthMaximum - state.health > 175);
			const auto command = runtime.decideHealing(state, now);
			expectHealingSpell(runtime, command);
			assert(command.spell->pending.missingHealth == state.healthMaximum - state.health);
			assert(command.spell->pending.manaReserve == 0);
		}
		for (uint32_t mana : {0u, healingCost}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.health = 30;
			state.mana = mana;
			state.combatActive = combat;
			expectPotion(runtime.decideHealing(state, now), healthPotion);
			assert(!runtime.pendingSpell()); // Do not begin a spell before the critical override.
		}
	}
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.health = 30;
	state.potionCount = 0;
	expectHealingSpell(runtime, runtime.decideHealing(state, now));
}

void manaAndHealthFallbacks()
{
	for (bool combat : {false, true}) {
		for (uint32_t healthCount : {0u, 2u}) {
			for (int32_t health : {30, 31, 60}) {
				PlayerBotSurvivalRuntime runtime;
				auto state = snapshot();
				state.health = health;
				state.mana = healingCost - 1;
				state.combatActive = combat;
				state.potionCount = healthCount;
				const auto command = runtime.decideHealing(state, now);
				const bool useMana = healthCount == 0 || (!combat && health > 30);
				expectPotion(command, useMana ? manaPotion : healthPotion);
				assert(!runtime.pendingSpell());
				if (useMana) {
					assert(command.reason == "healing_spell_mana");
					assert(command.need == "mana");
					assert(command.manaReserve == healingCost);
				} else if (health > 30) {
					assert(command.reason == "insufficient_mana_reserve");
					assert(command.candidateName == "Light Healing");
				}
			}
		}
	}
}

void manaThenReevaluate()
{
	// The runtime must reconsider the next snapshot, not promise a spell after mana use.
	for (int change = 0; change < 4; ++change) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.mana = 0;
		state.hunting = true; // Hunting alone is not active combat.
		const auto first = runtime.decideHealing(state, now);
		expectPotion(first, manaPotion);
		runtime.beginPotion(state, first.itemId, first.reason);
		assert(runtime.hasPendingDefensiveWork());
		--state.manaPotionCount;
		state.mana = healingCost;
		if (change == 1) state.combatActive = true;
		if (change == 2) { state.combatActive = true; state.mana = healingCost - 1; }
		if (change == 3) state.health = 30;
		const auto next = runtime.decideHealing(state, now + std::chrono::seconds(1));
		assert(next.potionVerification);
		assert(next.potionVerification->result == PlayerBotPotionVerificationResult::Success);
		if (change <= 1) expectHealingSpell(runtime, next);
		else {
			expectPotion(next, healthPotion);
			assert(!runtime.pendingSpell());
		}
	}
}

void spellBlockersAndMissingSupplies()
{
	// Mana potions are useful only for the exact mana blocker, not other spell failures.
	for (int blocker = 0; blocker < 5; ++blocker) {
		for (uint32_t healthCount : {0u, 2u}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.mana = 0;
			state.potionCount = healthCount;
			const char* reason = nullptr;
			switch (blocker) {
				case 0: state.healingExhausted = true; reason = "cooldown"; break;
				case 1: state.spells.front().learned = false; reason = "unlearned"; break;
				case 2: state.spells.front().metadataMatches = false; reason = "unsupported_metadata"; break;
				case 3: state.spells.clear(); reason = "unsupported_metadata"; break;
				case 4: state.spells.front().targetReachable = false; reason = "target_unreachable"; break;
			}
			const auto command = runtime.decideHealing(state, now);
			assert(!runtime.pendingSpell());
			if (healthCount != 0) {
				expectPotion(command, healthPotion);
				assert(command.reason == reason);
			} else {
				assert(command.type == Command::InterruptForService);
				assert(command.reason == "healing_supply_missing");
			}
		}
	}
	for (int missing = 0; missing < 3; ++missing) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.mana = 0;
		state.potionCount = 0;
		if (missing == 0) state.manaPotionCount = 0;
		if (missing == 1) state.manaPotionItemId = 0;
		if (missing == 2) { state.manaPotionCount = 0; state.health = 30; }
		const auto command = runtime.decideHealing(state, now);
		assert(command.type == Command::InterruptForService);
		assert(command.reason == "healing_supply_missing");
	}
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.mana = 0;
	state.manaPotionCount = 0;
	expectPotion(runtime.decideHealing(state, now), healthPotion);
}

void actionAndRetryDelays()
{
	for (int32_t health : {30, 50}) {
		for (uint32_t mana : {0u, healingCost}) {
			PlayerBotSurvivalRuntime runtime;
			auto state = snapshot();
			state.health = health;
			state.mana = mana;
			state.canDoAction = false;
			assert(runtime.decideHealing(state, now).type == Command::Wait);
			assert(!runtime.pendingSpell());
			state.canDoAction = true;
			const auto next = runtime.decideHealing(state, now + std::chrono::seconds(1));
			if (health == 30) expectPotion(next, healthPotion);
			else if (mana == 0) expectPotion(next, manaPotion);
			else expectHealingSpell(runtime, next);
		}
	}
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.mana = 0;
	const auto first = runtime.decideHealing(state, now);
	expectPotion(first, manaPotion);
	runtime.beginPotion(state, first.itemId, first.reason);
	const auto unverified = runtime.decideHealing(state, now);
	assert(unverified.type == Command::Wait);
	assert(unverified.potionVerification);
	assert(unverified.potionVerification->result == PlayerBotPotionVerificationResult::UseNotVerified);
	assert(runtime.decideHealing(state, now + std::chrono::milliseconds(1999)).type == Command::Wait);
	expectPotion(runtime.decideHealing(state, now + std::chrono::seconds(2)), manaPotion);
}

void buyingPotionsAndHealthyTopups()
{
	for (int32_t health : {30, 50, 100}) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.health = health;
		state.mana = 0;
		state.hunting = true;
		state.buyingPotions = true;
		assert(runtime.decideHealing(state, now).type == Command::None);
		assert(!runtime.pendingSpell());
	}
	for (bool hunting : {false, true}) {
		for (bool combat : {false, true}) {
			for (uint32_t mana : {healingCost - 1, healingCost}) {
				PlayerBotSurvivalRuntime runtime;
				auto state = snapshot();
				state.health = 61;
				state.mana = mana;
				state.hunting = hunting;
				state.combatActive = combat;
				const auto command = runtime.decideHealing(state, now);
				if ((hunting || combat) && mana < healingCost) {
					expectPotion(command, manaPotion);
					assert(command.reason == "healing_reserve");
					assert(command.manaReserve == healingCost);
				} else assert(command.type == Command::None);
				assert(!runtime.pendingSpell());
			}
		}
	}
	for (int unavailable = 0; unavailable < 5; ++unavailable) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.health = 100;
		state.mana = 0;
		state.combatActive = true;
		if (unavailable == 0) state.canDoAction = false;
		if (unavailable == 1) state.manaPotionCount = 0;
		if (unavailable == 2) state.manaPotionItemId = 0;
		if (unavailable == 3) state.spells.front().learned = false;
		if (unavailable == 4) {
			runtime.beginPotion(state, manaPotion, "healing_reserve");
			const auto unverified = runtime.decideHealing(state, now);
			assert(unverified.potionVerification);
			assert(unverified.type == Command::None); // A failed top-up must not hold combat.
		}
		assert(runtime.decideHealing(state, now + std::chrono::seconds(1)).type == Command::None);
	}
}
void healingFormulaBounds()
{
	for (const auto& expected : {std::make_pair("Light Healing", std::make_pair(17, 22)),
	                           std::make_pair("Intense Healing", std::make_pair(36, 65)),
	                           std::make_pair("Ultimate Healing", std::make_pair(73, 145))}) {
		const auto* descriptor = playerBotSpellDescriptor(expected.first);
		assert(descriptor && descriptor->healingFormula);
		const auto envelope = playerBotHealingEnvelope(20, 4, *descriptor->healingFormula);
		assert(envelope.minimum == expected.second.first && envelope.maximum == expected.second.second);
	}
}

void firstHealingBootstrapEligibility()
{
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.mana = 0;
	state.health = state.healthMaximum;
	auto reserve = [&](bool emergencyOnly) {
		return playerBotSpellTrainingSpendingReserve(100, 100,
		    emergencyOnly && !runtime.preferredHealingSpell(state));
	};
	// A usable learned heal restores the cash buffer even at zero mana/full HP.
	assert(runtime.preferredHealingSpell(state));
	assert(!runtime.preferredHealingSpell(state, true));
	assert(reserve(true) == 100 && reserve(false) == 100);
	for (int gate = 0; gate < 7; ++gate) {
		state = snapshot();
		state.mana = 0;
		auto& heal = state.spells.front();
		if (gate == 0) heal.learned = false;
		if (gate == 1) heal.metadataMatches = false;
		if (gate == 2) heal.vocationAllowed = false;
		if (gate == 3) heal.requirementsMet = false;
		if (gate == 4) heal.targetReachable = false;
		if (gate == 5) heal.envelope = {};
		if (gate == 6) heal.name = "Magic Shield";
		assert(!runtime.preferredHealingSpell(state));
		assert(reserve(true) == 0 && reserve(false) == 100);
	}
}

void economicalHealing()
{
	auto state = snapshot();
	state.mana = 200;
	state.spells.push_back({"Intense Healing", true, true, true, false, true, true, 11, 70, {36, 65, 0}});
	state.spells.push_back({"Ultimate Healing", true, true, true, false, true, true, 20, 160, {73, 145, 0}});
	state.spells.front().envelope = {17, 22, 0};
	{
		PlayerBotSurvivalRuntime runtime;
		assert(runtime.healingManaReserve(state) == 20);
		expectHealingSpell(runtime, runtime.decideHealing(state, now));
		assert(!runtime.healingSpellWorthLearning(state, "Intense Healing"));
		assert(!runtime.healingSpellWorthLearning(state, "Ultimate Healing"));
	}
	// Healthy mana equal to the economic reserve does not trigger a 160-mana top-up.
	{
		PlayerBotSurvivalRuntime runtime;
		auto healthy = state;
		healthy.health = 100;
		healthy.hunting = true;
		healthy.mana = 20;
		assert(runtime.decideHealing(healthy, now).type == Command::None);
	}
	// Loaded cost changes can make a stronger heal economical, but not affordable yet.
	state.spells[1].manaCost = 10;
	{
		PlayerBotSurvivalRuntime runtime;
		assert(runtime.healingManaReserve(state) == 10);
		state.mana = 5;
		expectPotion(runtime.decideHealing(state, now), manaPotion);
	}
	state.spells[1].manaCost = 70;
	state.spells.front().learned = false;
	state.mana = 70;
	{
		PlayerBotSurvivalRuntime runtime;
		assert(runtime.healingManaReserve(state) == 70);
		assert(runtime.healingSpellWorthLearning(state, "Light Healing"));
		const auto command = runtime.decideHealing(state, now);
		assert(command.spell && command.spell->name == "Intense Healing");
	}
	for (int gate = 0; gate < 3; ++gate) {
		PlayerBotSurvivalRuntime runtime;
		auto gated = state;
		if (gate == 0) gated.spells[1].requirementsMet = false;
		if (gate == 1) gated.spells[1].vocationAllowed = false;
		if (gate == 2) gated.spells[1].metadataMatches = false;
		assert(runtime.healingManaReserve(gated) == 160);
		assert(!runtime.healingSpellWorthLearning(gated, "Intense Healing"));
	}
	// Do not refill for an efficient but currently unaffordable heal while another can cast.
	state.spells.front().learned = true;
	state.spells.front().manaCost = 100;
	state.spells.front().envelope = {200, 220, 0};
	state.healthMaximum = 1000;
	state.health = 500;
	{
		PlayerBotSurvivalRuntime runtime;
		const auto command = runtime.decideHealing(state, now);
		assert(command.spell && command.spell->name == "Intense Healing");
	}
}

void calibratedHealingEconomy()
{
	PlayerBotSurvivalRuntime runtime;
	auto state = snapshot();
	state.healthMaximum = 1000;
	state.health = 500;
	state.mana = 200;
	state.spells.push_back({"Intense Healing", true, true, true, false, true, true, 11, 70, {50, 100, 0}});
	// Eight uncensored low rolls reverse the initial economy ranking. Learning,
	// reserve, and cast choice must all use the same controller-local evidence.
	for (int i = 0; i < 8; ++i) {
		const auto time = now + std::chrono::seconds(i * 3);
		const auto command = runtime.decideHealing(state, time);
		assert(command.spell && command.spell->name == "Intense Healing");
		assert(command.spell->pending.targetClass == "self");
		runtime.beginEngineSpellCast();
		runtime.observeHealthGain(true, true, 50);
		runtime.endEngineSpellCast();
		const auto verification = runtime.verifySpell({130, 550, 0, 0, true, time + std::chrono::milliseconds(100)});
		assert(verification && verification->verification.success);
	}
	assert(runtime.healingManaReserve(state) == 20);
	expectHealingSpell(runtime, runtime.decideHealing(state, now + std::chrono::seconds(30)));
	state.spells[1].learned = false;
	assert(!runtime.healingSpellWorthLearning(state, "Intense Healing"));
}
void healingCalibrationProgression()
{
	// Old absolute healing rolls must not make stronger spells look economical
	// after level/magic growth, even once the old profile has filled its sample cap.
	for (int samples : {8, 64}) {
		PlayerBotSurvivalRuntime runtime;
		auto state = snapshot();
		state.level = 9;
		state.magicLevel = 1;
		state.healthMaximum = 1000;
		state.health = 500;
		state.mana = 200;
		const auto* light = playerBotSpellDescriptor("Light Healing");
		state.spells.front().envelope = playerBotHealingEnvelope(9, 1, *light->healingFormula);
		PlayerBotSpellPendingCast oldCast;
		for (int i = 0; i < samples; ++i) {
			const auto time = now + std::chrono::seconds(i * 3);
			const auto command = runtime.decideHealing(state, time);
			expectHealingSpell(runtime, command);
			oldCast = command.spell->pending;
			runtime.beginEngineSpellCast();
			runtime.observeHealthGain(true, true, 12);
			runtime.endEngineSpellCast();
			const auto outcome = runtime.verifySpell({180, 512, 0, 0, true, time + std::chrono::milliseconds(100)});
			assert(outcome && outcome->verification.success && outcome->calibration.accepted == i + 1);
		}
		assert(runtime.calibrationRanking(oldCast) == 12);
		state.level = 20;
		state.magicLevel = 4;
		state.spells.front().envelope = playerBotHealingEnvelope(20, 4, *light->healingFormula);
		state.spells.push_back({"Intense Healing", true, false, true, false, true, true, 11, 70, {36, 65, 0}});
		assert(!runtime.healingSpellWorthLearning(state, "Intense Healing"));
		state.spells.back().learned = true;
		assert(runtime.healingManaReserve(state) == 20);
		const auto time = now + std::chrono::seconds(samples * 3 + 5);
		const auto command = runtime.decideHealing(state, time);
		expectHealingSpell(runtime, command);
		assert(!runtime.calibrationProfile(command.spell->pending));
		runtime.beginEngineSpellCast();
		runtime.observeHealthGain(true, true, 19);
		runtime.endEngineSpellCast();
		const auto outcome = runtime.verifySpell({180, 519, 0, 0, true, time + std::chrono::milliseconds(100)});
		assert(outcome && outcome->verification.success && outcome->calibration.accepted == 1);
		assert(outcome->calibration.minimum == 19 && outcome->calibration.maximum == 19);
		assert(outcome->rankingEstimate > 19 && outcome->rankingEstimate < 19.5);
	}
}
} // namespace

int main()
{
	spellFirstAndCriticalOverride();
	percentageBoundaries();
	typedHealthPreparation();
	departureHealthPreparation();
	manaAndHealthFallbacks();
	manaThenReevaluate();
	spellBlockersAndMissingSupplies();
	actionAndRetryDelays();
	buyingPotionsAndHealthyTopups();
	healingFormulaBounds();
	firstHealingBootstrapEligibility();
	economicalHealing();
	calibratedHealingEconomy();
	healingCalibrationProgression();
	std::cout << "playerbot survival contracts passed\n";
}
