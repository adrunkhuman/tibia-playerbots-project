/** Healing economy only. Callers supply eligible spells and authoritative mana costs. */
#ifndef FS_PLAYERBOTHEALINGPOLICY_H
#define FS_PLAYERBOTHEALINGPOLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

struct PlayerBotHealingSpellValue {
	std::string_view name;
	uint32_t manaCost = 0;
	double expectedHealing = 0;
};

inline double playerBotHealingEfficiency(const PlayerBotHealingSpellValue& spell,
                                        double missingHealth = std::numeric_limits<double>::max())
{
	if (!std::isfinite(spell.expectedHealing) || spell.expectedHealing <= 0 || missingHealth <= 0) return 0;
	const double useful = std::min(spell.expectedHealing, missingHealth);
	return spell.manaCost == 0 ? std::numeric_limits<double>::infinity() : useful / spell.manaCost;
}

inline bool playerBotPreferHealingSpell(const PlayerBotHealingSpellValue& candidate,
                                       const PlayerBotHealingSpellValue& current,
                                       double missingHealth = std::numeric_limits<double>::max())
{
	const double candidateEfficiency = playerBotHealingEfficiency(candidate, missingHealth);
	const double currentEfficiency = playerBotHealingEfficiency(current, missingHealth);
	if (candidateEfficiency != currentEfficiency) return candidateEfficiency > currentEfficiency;
	if (candidate.manaCost != current.manaCost) return candidate.manaCost < current.manaCost;
	return candidate.name < current.name;
}

// A purchase must improve economy, not merely win the deterministic name tie-break.
inline bool playerBotHealingPurchaseImproves(const PlayerBotHealingSpellValue& candidate,
                                            const PlayerBotHealingSpellValue& learned)
{
	const double candidateEfficiency = playerBotHealingEfficiency(candidate);
	const double learnedEfficiency = playerBotHealingEfficiency(learned);
	return candidateEfficiency > learnedEfficiency ||
	       (candidateEfficiency == learnedEfficiency && candidate.manaCost < learned.manaCost);
}

#endif
