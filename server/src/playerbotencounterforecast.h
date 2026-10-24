#ifndef FS_PLAYERBOTENCOUNTERFORECAST_H
#define FS_PLAYERBOTENCOUNTERFORECAST_H

#include "playerbotdamagemodel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

struct PlayerBotEncounterAttacker {
	PlayerBotDamageRates damage;
	double fightSeconds = 0;
	// Maximum loaded immediate volley after minimum armor, without claiming
	// shield blocks. Unknown scripted effects cannot authorize healing credit.
	double volley = 0;
	bool audited = true;
	// Optional exact weighted isolated cost for mutually exclusive types.
	double isolatedCost = 0;
};

struct PlayerBotEncounterPhase {
	double seconds = 0;
	double damagePerSecond = 0;
	double volley = 0;
};

struct PlayerBotEncounterForecast {
	double damage = 0;
	double seconds = 0;
	double conditionTail = 0;
	bool audited = true;
	std::vector<PlayerBotEncounterPhase> phases;
};

// Strongest potential sources first, as in the conservative encounter gate.
// The caller chooses the neighborhood; this function never turns a worst
// neighborhood into a region-wide consumption multiplier.
inline PlayerBotEncounterForecast playerBotEncounterForecast(std::vector<PlayerBotEncounterAttacker> attackers)
{
	std::stable_sort(attackers.begin(), attackers.end(), [](const auto& left, const auto& right) {
		return left.damage.unshieldedTotal() > right.damage.unshieldedTotal();
	});
	PlayerBotEncounterForecast result;
	PlayerBotDamageRates lingering;
	for (const auto& attacker : attackers) {
		lingering.merge(attacker.damage);
		result.audited = result.audited && attacker.audited && std::isfinite(attacker.fightSeconds) && attacker.fightSeconds >= 0;
	}
	lingering.clearDirect();
	result.conditionTail = lingering.conditionTail();
	for (size_t index = 0; index < attackers.size(); ++index) {
		PlayerBotDamageRates remaining = lingering;
		double volley = 0;
		for (size_t alive = index; alive < attackers.size(); ++alive) {
			remaining.merge(attackers[alive].damage);
			volley += attackers[alive].volley;
		}
		const double seconds = std::max(0.0, attackers[index].fightSeconds);
		result.phases.push_back({seconds, remaining.total(), volley});
		result.damage += remaining.total() * seconds;
		result.seconds += seconds;
	}
	return result;
}

// Unequal respawn rates must not let common dangerous targets borrow a rare
// harmless neighbor's cheap share. Never discount the anchor's isolated cost.
inline double playerBotRoutineEncounterCost(const PlayerBotEncounterForecast& pack,
                                            const PlayerBotEncounterAttacker& anchor, size_t members)
{
	const double isolated = std::max(anchor.isolatedCost,
	    anchor.damage.total() * anchor.fightSeconds + anchor.damage.conditionTail());
	return std::max(isolated, (pack.damage + pack.conditionTail) / std::max<size_t>(1, members));
}

struct PlayerBotEncounterRecovery {
	double health = 0;
	double maximumHealth = 0;
	uint32_t mana = 0;
	uint32_t spellMana = 0;
	double spellHealing = 0;
	double spellCooldownSeconds = 1;
	bool spellLegal = false;
	uint32_t potions = 0; // Only stock above the protected return reserve.
	double potionHealing = 0;
};

struct PlayerBotEncounterSurvival {
	bool survived = false;
	bool completed = false;
	double usefulHealing = 0;
	double finalHealth = 0;
	uint32_t spellCasts = 0;
	uint32_t potionUses = 0;
};

// Expected kill/damage phases with a maximum-volley guard. This is not a
// promise against arbitrary RNG. Recovery uses audited minima, actual MP and
// unreserved carried potions only: no future food, loot or mana refills.
inline PlayerBotEncounterSurvival playerBotEncounterSurvival(const PlayerBotEncounterForecast& forecast,
                                                            PlayerBotEncounterRecovery recovery)
{
	PlayerBotEncounterSurvival result;
	result.finalHealth = std::min(recovery.health, recovery.maximumHealth);
	if (!forecast.audited || recovery.maximumHealth <= 0 || result.finalHealth <= 0) return result;
	const double trigger = recovery.maximumHealth * .6;
	const double emergency = recovery.maximumHealth * .3;
	double time = 0;
	// Reserve a full second for an in-flight action / scheduler response.
	double nextRecovery = 1;
	size_t events = 0;
	for (const auto& phase : forecast.phases) {
		if (!std::isfinite(phase.seconds) || !std::isfinite(phase.damagePerSecond) ||
		    !std::isfinite(phase.volley) || phase.seconds < 0 || phase.damagePerSecond < 0 || phase.volley < 0)
			return result;
		const double end = time + phase.seconds;
		while (time < end) {
			// Bounded pure work per candidate, never a dispatcher-blocking loop.
			// An unresolved forecast cannot authorize a formerly lethal outing.
			if (++events > 128) return result;
			if (result.finalHealth <= phase.volley) { result.completed = true; return result; }
			const bool spell = recovery.spellLegal && recovery.spellMana > 0 &&
			    recovery.mana >= recovery.spellMana && recovery.spellHealing > 0;
			const bool potion = recovery.potions > 0 && recovery.potionHealing > 0;
			if (result.finalHealth <= trigger && time >= nextRecovery && (spell || potion)) {
				const bool usePotion = potion && (result.finalHealth <= emergency || !spell);
				const double healing = std::min(recovery.maximumHealth - result.finalHealth,
				    usePotion ? recovery.potionHealing : recovery.spellHealing);
				result.finalHealth += healing;
				result.usefulHealing += healing;
				if (usePotion) { --recovery.potions; ++result.potionUses; }
				else { recovery.mana -= recovery.spellMana; ++result.spellCasts; }
				nextRecovery = time + (usePotion ? 1 : std::max(1.0, recovery.spellCooldownSeconds));
				continue;
			}
			double advance = end - time;
			if (phase.damagePerSecond > 0 && (spell || potion)) {
				if (result.finalHealth > trigger)
					advance = std::min(advance, (result.finalHealth - trigger) / phase.damagePerSecond);
				else if (nextRecovery > time) advance = std::min(advance, nextRecovery - time);
			}
			if (advance <= 0) return result;
			result.finalHealth -= phase.damagePerSecond * advance;
			time += advance;
			// The next possible volley can arrive just before a kill/response,
			// not only at healing events. Check the whole no-action interval.
			if (result.finalHealth <= phase.volley) { result.completed = true; return result; }
		}
	}
	// A possible fresh condition tail after the last attacker must also fit
	// without inventing recovery after the forecast. Same-key tails merge.
	result.finalHealth -= forecast.conditionTail;
	result.completed = true;
	result.survived = result.finalHealth > 0;
	return result;
}

#endif
