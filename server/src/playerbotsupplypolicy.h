/** Pure hunt-duration consumption and recovery-spending policy.
 * This preference never replaces immediate combat or navigation safety gates.
 */
#ifndef FS_PLAYERBOTSUPPLYPOLICY_H
#define FS_PLAYERBOTSUPPLYPOLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Spawn-rate damage already includes every attacker's isolated fight. Only
// concurrent exposure is additional; both totals must describe the same crowd.
inline double playerBotCrowdDamageInflation(double crowdDamage, double isolatedDamage)
{
	return isolatedDamage > 0 ? std::max(1.0, crowdDamage / isolatedDamage) : 1.0;
}

struct PlayerBotSupplyProfile {
	uint32_t potions = 0;
	uint32_t reserve = 1;
	uint32_t potionHealing = 0;
	uint32_t mana = 0;
	uint32_t maximumMana = 0;
	uint32_t spellMana = 0;
	uint32_t spellHealing = 0;
	double spellInterval = 0;
	bool spellLegal = false;
	// Only the currently active food condition; no future eating or loot credit.
	double regenerationSeconds = 0;
	uint32_t healthGain = 0;
	double healthInterval = 0;
	uint32_t manaGain = 0;
	double manaInterval = 0;
};

struct PlayerBotSupplyCalibration {
	uint64_t capability = 0;
	double potionsPerCombatSecond = 0;
	uint32_t samples = 0;
};

struct PlayerBotSupplyBudget {
	double expectedDamage = 0;
	double regenerationHealing = 0;
	double spellHealing = 0;
	double expectedPotions = 0;
	uint32_t reservedPotions = 1;
	uint32_t routinePotions = 0;
	bool fits = true;
};

inline PlayerBotSupplyBudget playerBotSupplyBudget(const PlayerBotSupplyProfile& profile,
    double damagePerSecond, double combatFraction, double huntSeconds, double travelSeconds)
{
	PlayerBotSupplyBudget result;
	result.reservedPotions = profile.reserve;
	result.routinePotions = profile.potions > profile.reserve ? profile.potions - profile.reserve : 0;
	result.expectedDamage = std::max(0.0, damagePerSecond) * std::max(0.0, huntSeconds);
	const double combatSeconds = std::max(0.0, huntSeconds) * std::clamp(combatFraction, 0.0, 1.0);
	// Discard travel regeneration and tick progress. Credit only combat time,
	// capped by the condition's remaining lifetime after travel.
	const double regenerationSeconds = std::min(std::max(0.0, huntSeconds),
	    std::max(0.0, profile.regenerationSeconds - travelSeconds)) * std::clamp(combatFraction, 0.0, 1.0);
	auto regenerated = [regenerationSeconds](uint32_t gain, double interval) {
		return interval > 0 ? gain * std::floor(regenerationSeconds / interval) : 0.0;
	};
	result.regenerationHealing = std::min(result.expectedDamage, regenerated(profile.healthGain, profile.healthInterval));
	const uint64_t manaReserve = static_cast<uint64_t>(profile.spellMana) + 20;
	if (profile.spellLegal && profile.spellMana > 0 && profile.spellInterval > 0 &&
	    profile.maximumMana >= manaReserve + profile.spellMana) {
		// One mana pool only: do not assume repeated refill/cast cycles or that
		// regeneration below the emergency reserve is spendable.
		const double mana = std::max(0.0, std::min<double>(profile.maximumMana,
		    profile.mana + regenerated(profile.manaGain, profile.manaInterval)) - manaReserve);
		const double casts = std::min(std::floor(mana / profile.spellMana),
		                              std::floor(combatSeconds / profile.spellInterval));
		result.spellHealing = std::min(result.expectedDamage - result.regenerationHealing, casts * profile.spellHealing);
	}
	const double deficit = std::max(0.0, result.expectedDamage - result.regenerationHealing - result.spellHealing);
	result.expectedPotions = deficit == 0 ? 0 : profile.potionHealing > 0 ?
	    std::ceil(deficit / profile.potionHealing) : std::numeric_limits<double>::max();
	result.fits = (profile.potions > profile.reserve || profile.reserve == 0) &&
	    result.expectedPotions <= result.routinePotions;
	return result;
}

inline uint64_t playerBotRecoverySpendingReserve(uint32_t potions, uint32_t target, uint32_t price,
                                                uint32_t goldReserve)
{
	return goldReserve + static_cast<uint64_t>(potions < target ? target - potions : 0) * price;
}

inline bool playerBotAffordableAfterReserve(uint64_t money, uint64_t reserve, uint32_t price)
{
	return money >= reserve && money - reserve >= price;
}

#endif
