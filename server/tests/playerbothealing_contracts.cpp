#include "playerbothealingpolicy.h"

#include <cassert>
#include <iostream>
#include <limits>

int main()
{
	const PlayerBotHealingSpellValue light{"Light", 20, 19.9};
	const PlayerBotHealingSpellValue intense{"Intense", 70, 51.2};
	const PlayerBotHealingSpellValue ultimate{"Ultimate", 160, 109.4};
	assert(playerBotPreferHealingSpell(light, intense));
	assert(playerBotPreferHealingSpell(light, ultimate));
	assert(!playerBotHealingPurchaseImproves(intense, light));
	assert(playerBotHealingPurchaseImproves(light, ultimate));
	// Efficiency changes can come from loaded mana or calibration, not tier names.
	const PlayerBotHealingSpellValue efficient{"Future Heal", 10, 30};
	assert(playerBotPreferHealingSpell(efficient, light));
	assert(playerBotHealingPurchaseImproves(efficient, light));
	// Do not credit overheal. A cheaper small heal can beat a normally efficient large one.
	assert(playerBotPreferHealingSpell(light, efficient, 5) == false);
	const PlayerBotHealingSpellValue large{"Large", 100, 300};
	assert(playerBotPreferHealingSpell(large, light));
	assert(playerBotPreferHealingSpell(light, large, 10));
	const PlayerBotHealingSpellValue tie{"Tie", 40, 39.8};
	assert(playerBotPreferHealingSpell(light, tie));
	assert(playerBotHealingPurchaseImproves(light, tie));
	assert(!playerBotHealingPurchaseImproves({"Alphabetic", 20, 19.9}, light));
	const PlayerBotHealingSpellValue free{"Free", 0, 1};
	assert(playerBotPreferHealingSpell(free, light));
	assert(playerBotHealingEfficiency({"Zero", 0, 0}) == 0);
	assert(playerBotHealingEfficiency({"Invalid", 1, std::numeric_limits<double>::quiet_NaN()}) == 0);
	assert(playerBotHealingEfficiency(light, 0) == 0);
	std::cout << "playerbot healing contracts passed\n";
}
