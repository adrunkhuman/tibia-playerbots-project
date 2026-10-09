#include "playerbotdamagemodel.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {
void close(double actual, double expected) { assert(std::abs(actual - expected) < 1e-10); }

void delayedDecayAndMerge()
{
	// Same loaded semantics as built-in poison melee: amount is total damage,
	// default start=ceil(amount/20), delayed first tick after 4000ms.
	const PlayerBotDamageCondition poison{1, 0, 0, 30, 0, 4000};
	close(poison.damagePerSecond(), 0.5);
	close((PlayerBotDamageCondition{1, 0, 0, 20, 0, 4000}).damagePerSecond(), 0.25);
	close((PlayerBotDamageCondition{1, 0, 0, 21, 0, 4000}).damagePerSecond(), 0.5);
	close((PlayerBotDamageCondition{1, 0, 0, 30, 6, 4000}).damagePerSecond(), 1.5);
	close((PlayerBotDamageCondition{1, 0, 0, 30, 90, 4000}).damagePerSecond(), 7.5);
	close((PlayerBotDamageCondition{1, 0, 0, 0, 0, 4000}).damagePerSecond(), 0);
	close((PlayerBotDamageCondition{1, 0, 0, 30, 0, 10000}).damagePerSecond(), 0.2);

	PlayerBotDamageRates attacker;
	attacker.addAttack(10, 100, 2000, false, poison);
	close(attacker.direct, 5);
	close(attacker.total(), 5.5);
	PlayerBotDamageRates crowd;
	for (unsigned i = 0; i < 5; ++i) crowd.merge(attacker);
	close(crowd.direct, 25); // Direct hits still stack, without a species exception.
	close(crowd.total(), 25.5);
	assert(crowd.conditions.size() == 1);

	PlayerBotDamageRates stronger;
	stronger.addCondition({1, 0, 0, 120, 0, 4000});
	crowd.merge(stronger);
	close(crowd.total(), 26.5);
	crowd.merge(attacker);
	close(crowd.total(), 31.5); // Weaker applications do not add another condition.

	for (const auto& independent : {PlayerBotDamageCondition{2, 0, 0, 30, 0, 4000},
	                               PlayerBotDamageCondition{1, 1, 0, 30, 0, 4000},
	                               PlayerBotDamageCondition{1, 0, 1, 30, 0, 4000}}) {
		crowd.addCondition(independent);
	}
	assert(crowd.conditions.size() == 4);
	close(crowd.total(), 33);

	PlayerBotDamageRates conditionOnly;
	conditionOnly.addAttack(30, 10, 2000, true, poison);
	close(conditionOnly.direct, 0); // min/max are NOT direct combat damage.
	close(conditionOnly.total(), 0.5); // Conservative persistent uptime, not chance/10s.
	conditionOnly.addAttack(30, 0, 2000, false, {2, 0, 0, 30, 0, 4000});
	conditionOnly.addAttack(30, 100, 0, false, {2, 0, 0, 30, 0, 4000});
	close(conditionOnly.total(), 0.5);
}

void spatialAndAlternativeSources()
{
	PlayerBotDamageRates attacker;
	attacker.addAttack(10, 50, 2000, false, {1, 0, 0, 30, 0, 4000});
	PlayerBotDamageRates crowd;
	for (double reach : {6.0 / 9, 8.0 / 9, 7.0 / 9, 8.0 / 9}) crowd.merge(attacker, reach);
	close(crowd.total(), 2.5 * 29 / 9 + 0.5 * 8 / 9);
	PlayerBotDamageRates spawn;
	spawn.addAlternative(attacker, 0.1);
	spawn.addAlternative(attacker, 0.9);
	close(spawn.direct, attacker.direct);
	close(spawn.total(), attacker.total());
	PlayerBotDamageRates strong;
	strong.addCondition({1, 0, 0, 120, 0, 4000});
	spawn.addAlternative(strong, 0);
	close(spawn.total(), attacker.total());
	spawn.addAlternative(strong, 0.01);
	close(spawn.total(), attacker.direct + 1.5); // Envelope over possible persistent conditions.
}
}

int main()
{
	delayedDecayAndMerge();
	spatialAndAlternativeSources();
	std::cout << "playerbot damage contracts passed\n";
}
