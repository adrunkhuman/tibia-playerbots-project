#include "playerbotencounterforecast.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
	PlayerBotDamageRates weak;
	weak.addPhysicalAttack(10, 0, 100, 2000);
	const auto single = playerBotEncounterForecast({{weak, 10}});
	assert(single.damage == 0);
	const auto pack = playerBotEncounterForecast({{weak, 10}, {weak, 10}, {weak, 10}});
	assert(pack.damage > 0 && pack.phases.size() == 3 && pack.seconds == 30);
	assert(pack.phases.front().damagePerSecond == 5);
	assert(pack.phases.back().damagePerSecond == 0);
	assert(playerBotRoutineEncounterCost(pack, {weak, 10}, 3) > 0); // Zero isolated damage, positive contention.
	PlayerBotDamageRates dangerous;
	dangerous.direct = 100;
	const auto mixed = playerBotEncounterForecast({{dangerous, 100}, {{}, 1}});
	assert(playerBotRoutineEncounterCost(mixed, {dangerous, 100}, 2) == 10000);
	PlayerBotDamageRates poison;
	poison.addCondition({1, 0, 0, 20, 2, 2000});
	const auto poisoned = playerBotEncounterForecast({{poison, 10}, {poison, 10}});
	assert(poisoned.conditionTail == 20); // Same condition does not stack.
	assert(poisoned.damage == 20);
	PlayerBotDamageRates alternative = weak;
	PlayerBotDamageRates stronger;
	stronger.addPhysicalAttack(20, 10, 100, 1000);
	alternative.boundAlternative(stronger);
	assert(alternative.total() >= weak.total() && alternative.total() >= stronger.total());
	assert(alternative.direct == stronger.direct); // Alternatives do not become two attackers.

	PlayerBotEncounterForecast fight;
	fight.phases = {{30, 4, 10}};
	fight.damage = 120;
	fight.seconds = 30;
	PlayerBotEncounterRecovery actor;
	actor.health = actor.maximumHealth = 100;
	actor.potions = 2;
	actor.potionHealing = 50;
	const auto healed = playerBotEncounterSurvival(fight, actor);
	assert(healed.completed && healed.survived && healed.potionUses == 2);
	assert(healed.finalHealth > 0 && healed.usefulHealing > 0);
	actor.potions = 0;
	assert(!playerBotEncounterSurvival(fight, actor).survived);
	actor.mana = 50;
	actor.spellMana = 20;
	actor.spellHealing = 50;
	actor.spellLegal = true;
	const auto spells = playerBotEncounterSurvival(fight, actor);
	assert(spells.completed && spells.survived && spells.spellCasts == 2);
	actor.spellLegal = false;
	assert(!playerBotEncounterSurvival(fight, actor).survived);
	actor.potions = 0;
	fight.phases = {{30, 2, 50}};
	assert(!playerBotEncounterSurvival(fight, actor).survived); // A volley between responses matters even when mean damage alone fits.
	actor.potions = 20;
	fight.phases = {{1, 200, 0}};
	assert(!playerBotEncounterSurvival(fight, actor).survived); // Healing after lethal damage does not count.
	fight.phases = {{30, 1, 100}};
	assert(!playerBotEncounterSurvival(fight, actor).survived); // Immediate possible lethal volley.
	fight.phases = {{30, 4, 10}};
	fight.audited = false;
	assert(!playerBotEncounterSurvival(fight, actor).completed);
	fight.audited = true;
	fight.conditionTail = 200;
	assert(!playerBotEncounterSurvival(fight, actor).survived);
	fight.conditionTail = 0;
	fight.phases = {{10000, 4, 10}};
	actor.potions = 10000;
	assert(!playerBotEncounterSurvival(fight, actor).completed); // Bounded work, no optimistic timeout.
	std::cout << "encounter forecast contracts passed\n";
}
