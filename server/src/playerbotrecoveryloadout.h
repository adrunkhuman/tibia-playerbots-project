/** Receipt and inventory-space contracts for temporary recovery equipment. */
#ifndef FS_PLAYERBOTRECOVERYLOADOUT_H
#define FS_PLAYERBOTRECOVERYLOADOUT_H

#include <algorithm>
#include <cstdint>
#include "playerbotcombatprofile.h"

inline constexpr uint32_t playerBotRecoveryEquipmentSlots = 2;

enum class PlayerBotDepotUpgradeAction { None, Equip, DepositWorn };

inline PlayerBotDepotUpgradeAction playerBotDepotUpgradeAction(bool recoveryHeld, bool upgradeAvailable,
                                                              bool wornPresent, bool sourceFull, bool wornProtected)
{
	if (recoveryHeld || !upgradeAvailable || wornProtected) return PlayerBotDepotUpgradeAction::None;
	return wornPresent && sourceFull ? PlayerBotDepotUpgradeAction::DepositWorn : PlayerBotDepotUpgradeAction::Equip;
}

inline bool playerBotRecoveryCombatMatches(const PlayerBotCombatProfile& actual, const PlayerBotCombatProfile& expected)
{
	return actual.level == expected.level && actual.maximumHealth == expected.maximumHealth &&
	       actual.armor == expected.armor && actual.defense == expected.defense && actual.attack == expected.attack &&
	       actual.attackSkill == expected.attackSkill && actual.attackFactor == expected.attackFactor &&
	       actual.hitChance == expected.hitChance && actual.blockedByShield == expected.blockedByShield &&
	       actual.attackRange == expected.attackRange;
}

inline bool playerBotRecoveryLoadoutSpace(uint32_t capacity, uint32_t size, uint32_t displaced,
                                        bool departing = true)
{
	return capacity - std::min(capacity, size) >= displaced + (departing ? playerBotRecoveryEquipmentSlots : 0);
}

inline bool playerBotRecoveryMoveVerified(uint16_t requestedItem, uint16_t observedHand,
                                         uint32_t beforeCount, uint32_t afterCount, bool stow)
{
	return requestedItem != 0 && beforeCount != 0 && beforeCount == afterCount &&
	       (stow ? observedHand == 0 : observedHand == requestedItem);
}

#endif
