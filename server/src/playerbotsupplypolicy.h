/** Pure hunt-duration consumption and recovery-spending policy.
 * This preference never replaces immediate combat or navigation safety gates.
 */
#ifndef FS_PLAYERBOTSUPPLYPOLICY_H
#define FS_PLAYERBOTSUPPLYPOLICY_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

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

inline constexpr std::size_t playerBotSupplyEquipmentSlotCount = 11;

enum PlayerBotSupplyCapabilityField : uint64_t {
	PlayerBotSupplyCapabilityLevel = 1ULL << 0,
	PlayerBotSupplyCapabilityMaximumHealth = 1ULL << 1,
	PlayerBotSupplyCapabilityArmor = 1ULL << 2,
	PlayerBotSupplyCapabilityDefense = 1ULL << 3,
	PlayerBotSupplyCapabilityAttack = 1ULL << 4,
	PlayerBotSupplyCapabilityAttackSkill = 1ULL << 5,
	PlayerBotSupplyCapabilityAttackFactor = 1ULL << 6,
	PlayerBotSupplyCapabilityMagicLevel = 1ULL << 7,
	PlayerBotSupplyCapabilityMaximumMana = 1ULL << 8,
	PlayerBotSupplyCapabilitySpellLegal = 1ULL << 9,
	PlayerBotSupplyCapabilitySpellHealing = 1ULL << 10,
	PlayerBotSupplyCapabilitySpellMana = 1ULL << 11,
	PlayerBotSupplyCapabilitySpellInterval = 1ULL << 12,
	PlayerBotSupplyCapabilityPotionHealing = 1ULL << 13,
	PlayerBotSupplyCapabilityFoodState = 1ULL << 14,
	PlayerBotSupplyCapabilityFoodHealth = 1ULL << 15,
	PlayerBotSupplyCapabilityFoodMana = 1ULL << 16,
	PlayerBotSupplyCapabilityEquipment = 1ULL << 17,
};

struct PlayerBotSupplyCapabilitySnapshot {
	uint32_t level = 0;
	int32_t maximumHealth = 0;
	int32_t armor = 0;
	int32_t defense = 0;
	int32_t attack = 0;
	int32_t attackSkill = 0;
	int32_t attackFactorMilli = 1000;
	uint32_t magicLevel = 0;
	uint32_t maximumMana = 0;
	bool spellLegal = false;
	uint32_t spellHealing = 0;
	uint32_t spellMana = 0;
	uint32_t spellIntervalMilliseconds = 0;
	uint32_t potionHealing = 0;
	// Availability is separate from the stable vocation recovery contract.
	// A condition may expire while carried food can immediately renew it.
	bool foodAvailable = false;
	uint32_t foodHealthGain = 0;
	uint32_t foodHealthIntervalMilliseconds = 0;
	uint32_t foodManaGain = 0;
	uint32_t foodManaIntervalMilliseconds = 0;
	// Type IDs for combat-bearing slots. Backpack and slot zero stay empty.
	std::array<uint16_t, playerBotSupplyEquipmentSlotCount> equipmentItemIds{};

	bool operator==(const PlayerBotSupplyCapabilitySnapshot& other) const
	{
		return level == other.level && maximumHealth == other.maximumHealth && armor == other.armor &&
		       defense == other.defense && attack == other.attack && attackSkill == other.attackSkill &&
		       attackFactorMilli == other.attackFactorMilli && magicLevel == other.magicLevel &&
		       maximumMana == other.maximumMana && spellLegal == other.spellLegal &&
		       spellHealing == other.spellHealing && spellMana == other.spellMana &&
		       spellIntervalMilliseconds == other.spellIntervalMilliseconds &&
		       potionHealing == other.potionHealing && foodAvailable == other.foodAvailable &&
		       foodHealthGain == other.foodHealthGain &&
		       foodHealthIntervalMilliseconds == other.foodHealthIntervalMilliseconds &&
		       foodManaGain == other.foodManaGain &&
		       foodManaIntervalMilliseconds == other.foodManaIntervalMilliseconds &&
		       equipmentItemIds == other.equipmentItemIds;
	}
	bool operator!=(const PlayerBotSupplyCapabilitySnapshot& other) const { return !(*this == other); }
};

enum class PlayerBotSupplyCapabilityDirection : uint8_t {
	Unchanged,
	Improved,
	Regressed,
	Mixed,
};

inline const char* playerBotSupplyCapabilityDirectionName(PlayerBotSupplyCapabilityDirection direction)
{
	switch (direction) {
		case PlayerBotSupplyCapabilityDirection::Improved: return "improved";
		case PlayerBotSupplyCapabilityDirection::Regressed: return "regressed";
		case PlayerBotSupplyCapabilityDirection::Mixed: return "mixed";
		default: return "unchanged";
	}
}

struct PlayerBotSupplyCapabilityComparison {
	uint64_t changedFields = 0;
	PlayerBotSupplyCapabilityDirection direction = PlayerBotSupplyCapabilityDirection::Unchanged;
	bool compatible = true;
	bool materialChange = false;
	bool recoveryContextChanged = false;
};

inline PlayerBotSupplyCapabilityComparison playerBotCompareSupplyCapabilities(
    const PlayerBotSupplyCapabilitySnapshot& before, const PlayerBotSupplyCapabilitySnapshot& after)
{
	PlayerBotSupplyCapabilityComparison result;
	bool improved = false;
	bool regressed = false;
	auto monotonic = [&](auto left, auto right, uint64_t field, bool lowerIsBetter = false) {
		if (left == right) return;
		result.changedFields |= field;
		const bool better = lowerIsBetter ? right < left : right > left;
		improved = improved || better;
		regressed = regressed || !better;
	};
	auto material = [&](const auto& left, const auto& right, uint64_t field) {
		if (left == right) return;
		result.changedFields |= field;
		result.materialChange = true;
		improved = regressed = true;
	};

	monotonic(before.level, after.level, PlayerBotSupplyCapabilityLevel);
	monotonic(before.maximumHealth, after.maximumHealth, PlayerBotSupplyCapabilityMaximumHealth);
	material(before.armor, after.armor, PlayerBotSupplyCapabilityArmor);
	monotonic(before.defense, after.defense, PlayerBotSupplyCapabilityDefense);
	material(before.attack, after.attack, PlayerBotSupplyCapabilityAttack);
	monotonic(before.attackSkill, after.attackSkill, PlayerBotSupplyCapabilityAttackSkill);
	material(before.attackFactorMilli, after.attackFactorMilli, PlayerBotSupplyCapabilityAttackFactor);
	monotonic(before.magicLevel, after.magicLevel, PlayerBotSupplyCapabilityMagicLevel);
	monotonic(before.maximumMana, after.maximumMana, PlayerBotSupplyCapabilityMaximumMana);
	monotonic(before.spellLegal, after.spellLegal, PlayerBotSupplyCapabilitySpellLegal);
	if (before.spellHealing != after.spellHealing) result.changedFields |= PlayerBotSupplyCapabilitySpellHealing;
	if (before.spellMana != after.spellMana) result.changedFields |= PlayerBotSupplyCapabilitySpellMana;
	if (before.spellIntervalMilliseconds != after.spellIntervalMilliseconds)
		result.changedFields |= PlayerBotSupplyCapabilitySpellInterval;
	if (before.spellLegal != after.spellLegal || before.spellHealing != after.spellHealing ||
	    before.spellMana != after.spellMana || before.spellIntervalMilliseconds != after.spellIntervalMilliseconds) {
		result.recoveryContextChanged = true;
	}
	if (before.spellLegal && after.spellLegal) {
		monotonic(before.spellHealing, after.spellHealing, PlayerBotSupplyCapabilitySpellHealing);
		monotonic(before.spellMana, after.spellMana, PlayerBotSupplyCapabilitySpellMana, true);
		monotonic(before.spellIntervalMilliseconds, after.spellIntervalMilliseconds,
		          PlayerBotSupplyCapabilitySpellInterval, true);
	}
	material(before.potionHealing, after.potionHealing, PlayerBotSupplyCapabilityPotionHealing);
	material(before.equipmentItemIds, after.equipmentItemIds, PlayerBotSupplyCapabilityEquipment);

	if (before.foodAvailable != after.foodAvailable) {
		result.changedFields |= PlayerBotSupplyCapabilityFoodState;
		result.recoveryContextChanged = true;
		improved = improved || after.foodAvailable;
		regressed = regressed || !after.foodAvailable;
	}
	if (before.foodHealthGain != after.foodHealthGain ||
	    before.foodHealthIntervalMilliseconds != after.foodHealthIntervalMilliseconds) {
		result.changedFields |= PlayerBotSupplyCapabilityFoodHealth;
		result.recoveryContextChanged = true;
		improved = regressed = true;
	}
	if (before.foodManaGain != after.foodManaGain ||
	    before.foodManaIntervalMilliseconds != after.foodManaIntervalMilliseconds) {
		result.changedFields |= PlayerBotSupplyCapabilityFoodMana;
		result.recoveryContextChanged = true;
		improved = regressed = true;
	}

	result.direction = improved && regressed ? PlayerBotSupplyCapabilityDirection::Mixed :
	                   improved ? PlayerBotSupplyCapabilityDirection::Improved :
	                   regressed ? PlayerBotSupplyCapabilityDirection::Regressed :
	                               PlayerBotSupplyCapabilityDirection::Unchanged;
	result.compatible = !result.materialChange && !regressed;
	return result;
}

inline uint64_t playerBotSupplyCapabilityHash(const PlayerBotSupplyCapabilitySnapshot& capability)
{
	uint64_t key = 14695981039346656037ULL;
	auto append = [&key](uint64_t value) { key = (key ^ value) * 1099511628211ULL; };
	for (uint64_t value : {uint64_t(capability.level), uint64_t(capability.maximumHealth),
	     uint64_t(capability.armor), uint64_t(capability.defense), uint64_t(capability.attack),
	     uint64_t(capability.attackSkill), uint64_t(capability.attackFactorMilli),
	     uint64_t(capability.magicLevel), uint64_t(capability.maximumMana), uint64_t(capability.spellLegal),
	     uint64_t(capability.spellHealing), uint64_t(capability.spellMana),
	     uint64_t(capability.spellIntervalMilliseconds), uint64_t(capability.potionHealing),
	     uint64_t(capability.foodAvailable), uint64_t(capability.foodHealthGain),
	     uint64_t(capability.foodHealthIntervalMilliseconds), uint64_t(capability.foodManaGain),
	     uint64_t(capability.foodManaIntervalMilliseconds)}) append(value);
	for (uint16_t itemId : capability.equipmentItemIds) append(itemId);
	return key;
}

struct PlayerBotSupplyCalibration {
	PlayerBotSupplyCapabilitySnapshot capability;
	uint64_t capabilityHash = 0;
	double potionsPerCombatSecond = 0;
	uint32_t samples = 0;
};

inline std::optional<PlayerBotSupplyCalibration> playerBotSupplyCalibrationForCapability(
    const PlayerBotSupplyCalibration& calibration, const PlayerBotSupplyCapabilitySnapshot& capability)
{
	if (calibration.samples == 0 ||
	    !playerBotCompareSupplyCapabilities(calibration.capability, capability).compatible) return std::nullopt;
	return calibration;
}

enum class PlayerBotSupplyEstimateDirection : uint8_t {
	None,
	Upward,
	Downward,
	Unchanged,
};

inline const char* playerBotSupplyEstimateDirectionName(PlayerBotSupplyEstimateDirection direction)
{
	switch (direction) {
		case PlayerBotSupplyEstimateDirection::Upward: return "upward";
		case PlayerBotSupplyEstimateDirection::Downward: return "downward";
		case PlayerBotSupplyEstimateDirection::Unchanged: return "unchanged";
		default: return "none";
	}
}

struct PlayerBotSupplyObservation {
	PlayerBotSupplyCalibration calibration;
	uint64_t changedFields = 0;
	PlayerBotSupplyCapabilityDirection direction = PlayerBotSupplyCapabilityDirection::Unchanged;
	PlayerBotSupplyEstimateDirection estimateDirection = PlayerBotSupplyEstimateDirection::None;
	uint8_t observedAttackerCoverage = 0;
	double observedAttackerCoverageSeconds = 0;
	uint64_t durationSeconds = 0;
	bool arrivalBaselineObserved = false;
	int32_t startingHealth = 0;
	int32_t startingMaximumHealth = 0;
	uint32_t startingMana = 0;
	uint32_t startingMaximumMana = 0;
	int32_t endingHealth = 0;
	int32_t endingMaximumHealth = 0;
	uint32_t endingMana = 0;
	uint32_t endingMaximumMana = 0;
	double activeCombatSeconds = 0;
	uint32_t kills = 0;
	uint8_t p10HealthPercent = 0;
	uint8_t p10ManaPercent = 100;
	double foodActiveSeconds = 0;
	double foodAvailableSeconds = 0;
	uint64_t levelHealthRestored = 0;
	uint64_t levelManaRestored = 0;
	double levelAdjustedHealthDebt = 0;
	double levelAdjustedManaDebt = 0;
	double potionEquivalentDemand = 0;
	uint64_t minimumDurationSeconds = 120;
	double minimumActiveCombatSeconds = 60;
	uint32_t minimumKills = 3;
	uint8_t unsafeHealthPercent = 70;
	uint8_t minimumDownwardHealthPercent = 80;
	uint8_t minimumDownwardManaPercent = 50;
	bool interrupted = false;
	bool potionsDepleted = false;
	bool manaDepleted = false;
	bool dangerObserved = false;
	bool deathObserved = false;
	const char* reason = "insufficient_evidence";
	bool accepted = false;
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
