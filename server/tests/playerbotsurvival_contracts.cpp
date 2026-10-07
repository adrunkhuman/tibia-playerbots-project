// Real survival runtime policy checks; see playerbotsurvival_contracts.sh.
#include "playerbotsurvivalruntime.h"

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
} // namespace

int main()
{
	spellFirstAndCriticalOverride();
	percentageBoundaries();
	manaAndHealthFallbacks();
	manaThenReevaluate();
	spellBlockersAndMissingSupplies();
	actionAndRetryDelays();
	buyingPotionsAndHealthyTopups();
	std::cout << "playerbot survival contracts passed\n";
}
