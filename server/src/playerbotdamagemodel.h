#ifndef FS_PLAYERBOTDAMAGEMODEL_H
#define FS_PLAYERBOTDAMAGEMODEL_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

// Metadata for the delayed, decaying conditions built by Monsters::getDamageCondition.
// Scripted spells and fields are not inferred from their names.
struct PlayerBotDamageCondition {
	uint32_t type = 0;
	int32_t id = 0;
	uint32_t subId = 0;
	uint32_t maximumTotal = 0;
	uint32_t start = 0;
	uint32_t tickMs = 0;

	double damagePerSecond() const {
		if (!type || !maximumTotal || !tickMs) return 0;
		// ConditionDamage::init chooses ceil(amount/20) when start is zero;
		// generateDamageList then decreases it. Assume continuous refresh at
		// the strongest tick, not total damage delivered in an arbitrary 10s.
		const double first = start ? std::min(start, maximumTotal) : std::ceil(maximumTotal / 20.0);
		return first * 1000.0 / tickMs;
	}
};

struct PlayerBotDamageRates {
	using ConditionKey = std::tuple<uint32_t, int32_t, uint32_t>;
	double direct = 0;
	std::map<ConditionKey, double> conditions;

	void addCondition(const PlayerBotDamageCondition& condition) {
		const double rate = condition.damagePerSecond();
		if (rate > 0) {
			auto& current = conditions[{condition.type, condition.id, condition.subId}];
			current = std::max(current, rate);
		}
	}
	void addAttack(double directDamage, uint32_t chance, uint32_t intervalMs,
	               bool conditionOnly, const PlayerBotDamageCondition& condition) {
		if (!chance || !intervalMs) return;
		if (!conditionOnly) direct += std::max(0.0, directDamage) * (chance / 100.0) * (1000.0 / intervalMs);
		// Possible applications may persist between attacks. Full uptime is a
		// conservative envelope; applying chance again would discount that tail.
		addCondition(condition);
	}
	// Concurrent sources add direct damage, but Creature::addCondition merges
	// the same (type,id,subId). A delayed refresh preserves the next tick time.
	void merge(const PlayerBotDamageRates& other, double reach = 1) {
		direct += other.direct * reach;
		for (const auto& [key, rate] : other.conditions) {
			auto& current = conditions[key];
			current = std::max(current, rate * reach);
		}
	}
	// Direct damage is an expectation over mutually exclusive alternatives.
	// Keep possible conditions at full uptime: max(expected rates) across
	// concurrent spawns would underestimate the expected strongest condition.
	void addAlternative(const PlayerBotDamageRates& other, double probability) {
		if (probability <= 0) return;
		direct += other.direct * probability;
		for (const auto& [key, rate] : other.conditions) {
			auto& current = conditions[key];
			current = std::max(current, rate);
		}
	}
	double total() const {
		double result = direct;
		for (const auto& entry : conditions) result += entry.second;
		return result;
	}
};

#endif
