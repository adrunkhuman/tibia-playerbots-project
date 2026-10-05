/** Immutable combat values shared by hunt and equipment decisions. */
#ifndef FS_PLAYERBOTCOMBATPROFILE_H
#define FS_PLAYERBOTCOMBATPROFILE_H

#include <cstdint>

struct PlayerBotCombatProfile {
	uint32_t level = 0;
	int32_t maximumHealth = 0;
	int32_t armor = 0;
	int32_t defense = 0;
	int32_t attack = 0;
	int32_t attackSkill = 0;
	float attackFactor = 1.0f;
	// Percent of attacks that land. Melee always connects; distance attacks
	// roll the fired item's hit chance first.
	int32_t hitChance = 100;
	// Distance attacks bypass the target's shield defense; only armor applies.
	bool blockedByShield = true;
	uint8_t attackRange = 1;
};

#endif
