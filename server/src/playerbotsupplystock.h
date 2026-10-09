/** Typed consumable stock: per-kind rules, mandatory floors, and learned hunt demand.
 * Health potions keep their damage-model budget, route reserve, and stock target
 * in the controller; this layer gives every kind the same floor/target/demand
 * contract so later vocations add values rather than structure.
 */
#ifndef FS_PLAYERBOTSUPPLYSTOCK_H
#define FS_PLAYERBOTSUPPLYSTOCK_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class PlayerBotSupplyKind : uint8_t {
	HealthPotion,
	ManaPotion,
	Ammunition,
	// Units of the wielded throwing weapon, such as spears, beyond the one in use.
	ThrowingWeapon,
};

// Survival priority: deficits are reported and floors are bought in this order.
inline constexpr std::array<PlayerBotSupplyKind, 4> playerBotSupplyKinds{
	PlayerBotSupplyKind::HealthPotion, PlayerBotSupplyKind::ManaPotion, PlayerBotSupplyKind::Ammunition,
	PlayerBotSupplyKind::ThrowingWeapon,
};

inline const char* playerBotSupplyKindName(PlayerBotSupplyKind kind)
{
	switch (kind) {
		case PlayerBotSupplyKind::ManaPotion: return "mana_potion";
		case PlayerBotSupplyKind::Ammunition: return "ammunition";
		case PlayerBotSupplyKind::ThrowingWeapon: return "throwing_weapon";
		default: return "health_potion";
	}
}

// Mandatory-service reason; the health name predates typed supplies.
inline const char* playerBotSupplyReserveReason(PlayerBotSupplyKind kind)
{
	switch (kind) {
		case PlayerBotSupplyKind::ManaPotion: return "mana_potion_reserve";
		case PlayerBotSupplyKind::Ammunition: return "ammunition_reserve";
		case PlayerBotSupplyKind::ThrowingWeapon: return "throwing_weapon_reserve";
		default: return "healing_reserve";
	}
}

// Hunt-end reason when a kind falls to its return threshold during a hunt.
// Health potions keep their healing-triggered interruption instead.
inline const char* playerBotSupplyExhaustedReason(PlayerBotSupplyKind kind)
{
	switch (kind) {
		case PlayerBotSupplyKind::ManaPotion: return "mana_potion_exhausted";
		case PlayerBotSupplyKind::Ammunition: return "ammunition_exhausted";
		case PlayerBotSupplyKind::ThrowingWeapon: return "throwing_weapon_exhausted";
		default: return "healing_reserve";
	}
}

struct PlayerBotSupplyRule {
	PlayerBotSupplyKind kind = PlayerBotSupplyKind::HealthPotion;
	uint16_t itemId = 0;
	// Below the floor, service is mandatory before the next hunt.
	uint32_t safetyFloor = 0;
	// Stock at or below this is hunt reserve, not routine supply.
	uint32_t returnThreshold = 0;
	uint32_t target = 0;

	bool active() const { return itemId != 0 && target != 0; }
};

inline constexpr uint16_t playerBotManaPotionItemId = 7620;
// Lower bound of the datapack's mana potion roll (actions/scripts/other/potions.lua).
inline constexpr uint32_t playerBotManaPotionMinimumMana = 75;

// Base rule for kinds without a specialized controller policy. Paladins stock
// mana potions for spell use; other vocations still drink carried mana potions
// without a shopping target. Weapon-matched kinds have no item until the
// loadout selects one.
inline PlayerBotSupplyRule playerBotSupplyRule(PlayerBotSupplyKind kind, uint16_t vocationId)
{
	PlayerBotSupplyRule rule;
	rule.kind = kind;
	if (kind == PlayerBotSupplyKind::ManaPotion) {
		rule.itemId = playerBotManaPotionItemId;
		if (vocationId == 3 || vocationId == 7) {
			rule.safetyFloor = 2;
			rule.returnThreshold = 1;
			rule.target = 20;
		}
	}
	return rule;
}

// Throwing weapons stack: a break takes one unit off the stack in hand, and
// purchases join that stack. Counts therefore include the wielded stack and
// match carried item counts in service, deposit, and demand accounting. Spares
// in containers (looted, or left over when the hand stack is full) are counted
// too and equipped once the hand empties. A spear weighs 20 oz: start with
// three at level 8, then add one per two levels up to seven. This uses only
// half the Paladin's added capacity, leaving room for potions and loot.
// A hunt ends with one spear left so the last break need not leave it unarmed.
inline constexpr uint32_t playerBotThrowingWeaponReturnThreshold = 1;
inline constexpr uint32_t playerBotThrowingWeaponSafetyFloor = 3;

inline PlayerBotSupplyRule playerBotThrowingWeaponRule(uint16_t weaponItemId, uint32_t level)
{
	const uint32_t growth = level > 8 ? std::min<uint32_t>((level - 8) / 2, 4) : 0;
	return {PlayerBotSupplyKind::ThrowingWeapon, weaponItemId, playerBotThrowingWeaponSafetyFloor,
	        playerBotThrowingWeaponReturnThreshold,
	        weaponItemId == 0 ? 0 : playerBotThrowingWeaponSafetyFloor + growth};
}

struct PlayerBotSupplyStock {
	PlayerBotSupplyRule rule;
	uint32_t count = 0;
};

// Ordered by playerBotSupplyKinds; inactive kinds may be omitted.
using PlayerBotSupplyStocks = std::vector<PlayerBotSupplyStock>;

struct PlayerBotSupplyDeficit {
	const PlayerBotSupplyStock* first = nullptr;
	uint32_t missing = 0;
};

inline PlayerBotSupplyDeficit playerBotMandatorySupplyDeficit(const PlayerBotSupplyStocks& stocks, bool recovery)
{
	PlayerBotSupplyDeficit deficit;
	if (recovery) return deficit;
	for (const PlayerBotSupplyStock& stock : stocks) {
		if (!stock.rule.active() || stock.count >= stock.rule.safetyFloor) continue;
		if (!deficit.first) deficit.first = &stock;
		deficit.missing += stock.rule.safetyFloor - stock.count;
	}
	return deficit;
}

inline const PlayerBotSupplyStock* playerBotSupplyStock(const PlayerBotSupplyStocks& stocks, PlayerBotSupplyKind kind)
{
	const auto found = std::find_if(stocks.begin(), stocks.end(), [kind](const auto& stock) { return stock.rule.kind == kind; });
	return found == stocks.end() ? nullptr : &*found;
}

// Base exit requirements before candidate demand is added. Keep the legacy
// health-supplier requirement plus active kinds below their mandatory floor.
// Each item may have its own provider; no universal shop is required.
inline std::vector<uint16_t> playerBotSupplyExitShopItems(const PlayerBotSupplyStocks& stocks, uint16_t healthItemId)
{
	std::vector<uint16_t> items{healthItemId};
	for (const PlayerBotSupplyStock& stock : stocks) {
		if (stock.rule.kind != PlayerBotSupplyKind::HealthPotion && stock.rule.active() &&
		    stock.count < stock.rule.safetyFloor) items.push_back(stock.rule.itemId);
	}
	return items;
}

// First active non-health kind at or below its return threshold. Health
// potions return through the healing interruption, not this check.
inline const PlayerBotSupplyStock* playerBotExhaustedSupply(const PlayerBotSupplyStocks& stocks)
{
	const auto found = std::find_if(stocks.begin(), stocks.end(), [](const PlayerBotSupplyStock& stock) {
		return stock.rule.kind != PlayerBotSupplyKind::HealthPotion && stock.rule.active() &&
		       stock.count <= stock.rule.returnThreshold;
	});
	return found == stocks.end() ? nullptr : &*found;
}

// Identifies a deferred restock: any count change re-enables the attempt.
inline uint64_t playerBotSupplyStockKey(const PlayerBotSupplyStocks& stocks)
{
	constexpr uint32_t bits = 64 / playerBotSupplyKinds.size();
	uint64_t key = 0;
	for (const PlayerBotSupplyStock& stock : stocks) {
		key |= static_cast<uint64_t>(std::min<uint32_t>(stock.count, (1U << bits) - 1))
		       << (bits * static_cast<uint8_t>(stock.rule.kind));
	}
	return key;
}

// Non-throwing supplies retain the health calibration correction shape:
// higher demand corrects immediately; cheaper evidence needs a full outing.
inline constexpr double playerBotSupplyDemandDownwardBlend = 0.20;

struct PlayerBotSupplyDemand {
	double unitsPerCombatSecond = 0;
	uint32_t samples = 0;
	// Throwing-weapon breaks are stochastic. Pool valid outings, including
	// interrupted ones, rather than retaining the largest short-sample rate.
	double accumulatedCombatSeconds = 0;
	double accumulatedConsumed = 0;
};

struct PlayerBotSupplyDemandUpdate {
	PlayerBotSupplyKind kind = PlayerBotSupplyKind::ManaPotion;
	uint32_t consumed = 0;
	double debt = 0;
	PlayerBotSupplyDemand demand;
	double observedUnitsPerCombatSecond = 0;
	bool updated = false;
	const char* reason = "insufficient_active_combat";
};

// Debt counts units the outing left unpaid, such as mana below its starting level.
inline PlayerBotSupplyDemandUpdate playerBotObserveSupplyDemand(const PlayerBotSupplyDemand& prior, uint32_t consumed,
    double activeCombatSeconds, bool fullOuting, double debt = 0)
{
	PlayerBotSupplyDemandUpdate update;
	update.consumed = consumed;
	update.debt = std::max(0.0, debt);
	update.demand = prior;
	if (activeCombatSeconds <= 0) return update;
	update.observedUnitsPerCombatSecond = (consumed + update.debt) / activeCombatSeconds;
	if (update.observedUnitsPerCombatSecond > prior.unitsPerCombatSecond) {
		update.demand.unitsPerCombatSecond = update.observedUnitsPerCombatSecond;
		++update.demand.samples;
		update.updated = true;
		update.reason = "higher_observed_demand";
		return update;
	}
	if (!fullOuting) {
		update.reason = "partial_outing";
		return update;
	}
	update.demand.unitsPerCombatSecond = prior.samples == 0 ? update.observedUnitsPerCombatSecond :
	    prior.unitsPerCombatSecond * (1 - playerBotSupplyDemandDownwardBlend) +
	    update.observedUnitsPerCombatSecond * playerBotSupplyDemandDownwardBlend;
	++update.demand.samples;
	update.updated = true;
	update.reason = "safe_combat_evidence";
	return update;
}

inline PlayerBotSupplyDemandUpdate playerBotObserveThrowingWeaponDemand(const PlayerBotSupplyDemand& prior,
    uint32_t consumed, double activeCombatSeconds, double minimumActiveCombatSeconds)
{
	PlayerBotSupplyDemandUpdate update;
	update.kind = PlayerBotSupplyKind::ThrowingWeapon;
	update.consumed = consumed;
	update.demand = prior;
	if (activeCombatSeconds <= 0) return update;
	update.observedUnitsPerCombatSecond = consumed / activeCombatSeconds;
	update.demand.accumulatedCombatSeconds += activeCombatSeconds;
	update.demand.accumulatedConsumed += consumed;
	if (update.demand.accumulatedCombatSeconds < minimumActiveCombatSeconds) return update;
	update.demand.unitsPerCombatSecond = update.demand.accumulatedConsumed / update.demand.accumulatedCombatSeconds;
	++update.demand.samples;
	update.updated = true;
	update.reason = "pooled_combat_evidence";
	return update;
}

// Hunt-planning view of one non-health kind. Health potions keep their own
// damage-model budget in PlayerBotSupplyBudget.
struct PlayerBotSupplyKindProfile {
	PlayerBotSupplyKind kind = PlayerBotSupplyKind::ManaPotion;
	uint16_t itemId = 0;
	uint32_t count = 0;
	uint32_t returnThreshold = 0;
	PlayerBotSupplyDemand demand;
};

struct PlayerBotSupplyKindBudget {
	PlayerBotSupplyKind kind = PlayerBotSupplyKind::ManaPotion;
	uint32_t count = 0;
	uint32_t routine = 0;
	double expected = 0;
	bool fits = true;
};

inline PlayerBotSupplyKindBudget playerBotSupplyKindBudget(const PlayerBotSupplyKindProfile& profile, double combatSeconds)
{
	PlayerBotSupplyKindBudget budget;
	budget.kind = profile.kind;
	budget.count = profile.count;
	budget.routine = profile.count > profile.returnThreshold ? profile.count - profile.returnThreshold : 0;
	budget.expected = std::ceil(std::max(0.0, profile.demand.unitsPerCombatSecond) * std::max(0.0, combatSeconds));
	budget.fits = (profile.count > profile.returnThreshold || profile.returnThreshold == 0) &&
	              budget.expected <= budget.routine;
	return budget;
}

#endif
