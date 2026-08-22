#include "otpch.h"

#include "playerbotequipmentpolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
	constexpr slots_t slot(uint8_t value) { return static_cast<slots_t>(value); }
	constexpr slots_t head = slot(1);
	constexpr slots_t necklace = slot(2);
	constexpr slots_t armor = slot(4);
	constexpr slots_t right = slot(5);
	constexpr slots_t left = slot(6);
	constexpr slots_t legs = slot(7);
	constexpr slots_t feet = slot(8);
	constexpr slots_t ring = slot(9);

	bool melee(PlayerBotEquipmentWeaponType type)
	{
		return type == PlayerBotEquipmentWeaponType::Sword || type == PlayerBotEquipmentWeaponType::Club ||
		       type == PlayerBotEquipmentWeaponType::Axe;
	}

	int32_t skill(const PlayerBotEquipmentPlayerSnapshot& player, PlayerBotEquipmentWeaponType type)
	{
		switch (type) {
			case PlayerBotEquipmentWeaponType::Sword: return player.swordSkill;
			case PlayerBotEquipmentWeaponType::Club: return player.clubSkill;
			case PlayerBotEquipmentWeaponType::Axe: return player.axeSkill;
			case PlayerBotEquipmentWeaponType::Distance: case PlayerBotEquipmentWeaponType::Ammo: return player.distanceSkill;
			default: return player.fistSkill;
		}
	}

	int32_t maximumDamage(uint32_t level, int32_t skillLevel, int32_t attack, float factor)
	{
		return static_cast<int32_t>(std::round((level / 5) + (((((skillLevel / 4.) + 1) * (attack / 3.)) * 1.03) / factor)));
	}
}

PlayerBotEquipmentPolicy::PlayerBotEquipmentPolicy(uint16_t combatReadinessVocationId) :
	combatReadinessVocationId(combatReadinessVocationId)
{}

bool PlayerBotEquipmentPolicy::requiresKnightCombatReadiness(const PlayerBotEquipmentPlayerSnapshot& player) const
{
	return player.vocationId == combatReadinessVocationId;
}

bool PlayerBotEquipmentPolicy::isLegalEquipmentItem(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& item) const
{
	return !item.removed && item.pickupable && player.level >= item.minimumLevel &&
	       player.magicLevel >= item.minimumMagicLevel && (!item.premiumRequired || player.premium) &&
	       (item.vocationIds.empty() || std::find(item.vocationIds.begin(), item.vocationIds.end(), player.vocationId) != item.vocationIds.end());
}

bool PlayerBotEquipmentPolicy::isKnightMeleeWeapon(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& item) const
{
	return isLegalEquipmentItem(player, item) && item.attack > 0 && melee(item.weaponType) && (item.left || item.right);
}

bool PlayerBotEquipmentPolicy::isCombatEquipment(const PlayerBotEquipmentItemSnapshot& item) const
{
	return item.head || item.armorSlot || item.legs || item.feet || item.weaponType != PlayerBotEquipmentWeaponType::None ||
	       item.armor > 0 || item.defense > 0;
}

std::optional<PlayerBotEquipmentUpgrade> PlayerBotEquipmentPolicy::evaluateUpgrade(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout, const PlayerBotEquipmentItemSnapshot& candidate) const
{
	if (!isLegalEquipmentItem(player, candidate)) return std::nullopt;
	slots_t target = slot(0);
	const char* metric = nullptr;
	int32_t candidateValue = 0;
	if (candidate.head) { target = head; metric = "armor"; candidateValue = candidate.armor; }
	else if (candidate.armorSlot) { target = armor; metric = "armor"; candidateValue = candidate.armor; }
	else if (candidate.legs) { target = legs; metric = "armor"; candidateValue = candidate.armor; }
	else if (candidate.feet) { target = feet; metric = "armor"; candidateValue = candidate.armor; }
	else if (candidate.weaponType == PlayerBotEquipmentWeaponType::Shield) { target = right; metric = "defense"; candidateValue = candidate.defense; }
	else if (!candidate.twoHanded && candidate.weaponType != PlayerBotEquipmentWeaponType::None && candidate.weaponType != PlayerBotEquipmentWeaponType::Ammo) {
		target = left; metric = "attack"; candidateValue = candidate.attack;
	}
	if (target == slot(0) || candidateValue <= 0) return std::nullopt;
	const PlayerBotEquipmentItemSnapshot& equipped = loadout.items[static_cast<uint8_t>(target)];
	int32_t currentValue = 0;
	if (equipped.itemId != 0) {
		currentValue = std::strcmp(metric, "armor") == 0 ? equipped.armor : std::strcmp(metric, "defense") == 0 ? equipped.defense : equipped.attack;
		if (std::strcmp(metric, "attack") == 0 && maximumDamage(player.level, skill(player, candidate.weaponType), candidate.attack, player.attackFactor) <=
		    maximumDamage(player.level, skill(player, equipped.weaponType), equipped.attack, player.attackFactor)) return std::nullopt;
	}
	if (candidateValue <= currentValue) return std::nullopt;
	return PlayerBotEquipmentUpgrade{target, candidateValue - currentValue, metric, currentValue, candidateValue};
}

std::optional<PlayerBotEquipmentCarriedUpgrade> PlayerBotEquipmentPolicy::findCarriedUpgrade(
	const PlayerBotEquipmentPlayerSnapshot& player, const PlayerBotEquipmentLoadout& loadout,
	const std::vector<PlayerBotEquipmentCarriedCandidate>& candidates) const
{
	std::optional<PlayerBotEquipmentCarriedUpgrade> selected;
	for (size_t index = 0; index < candidates.size(); ++index) {
		const auto& candidate = candidates[index];
		auto upgrade = evaluateUpgrade(player, loadout, candidate.item);
		if (!candidate.actionable || !candidate.item.inContainer || !upgrade || (requiresKnightCombatReadiness(player) &&
		    candidate.item.weaponType != PlayerBotEquipmentWeaponType::None && !isKnightMeleeWeapon(player, candidate.item)) ||
		    (selected && upgrade->benefit <= selected->upgrade.benefit)) continue;
		selected = PlayerBotEquipmentCarriedUpgrade{index, *upgrade};
	}
	return selected;
}

bool PlayerBotEquipmentPolicy::applyOffer(const PlayerBotEquipmentPlayerSnapshot& player, PlayerBotEquipmentLoadout& loadout,
	const PlayerBotEquipmentItemSnapshot& candidate, slots_t& target, uint16_t& replacedItemId,
	uint16_t& displacedLeftItemId, uint16_t& displacedRightItemId, std::string& rejection) const
{
	if (!candidate.pickupable) { rejection = "not_pickupable"; return false; }
	if (player.level < candidate.minimumLevel) { rejection = "level_ineligible"; return false; }
	if (player.magicLevel < candidate.minimumMagicLevel) { rejection = "magic_level_ineligible"; return false; }
	if (candidate.premiumRequired && !player.premium) { rejection = "premium_ineligible"; return false; }
	if (!candidate.vocationIds.empty() && std::find(candidate.vocationIds.begin(), candidate.vocationIds.end(), player.vocationId) == candidate.vocationIds.end()) {
		rejection = "vocation_ineligible"; return false;
	}
	auto itemAt = [&loadout](slots_t value) -> const PlayerBotEquipmentItemSnapshot& { return loadout.items[static_cast<uint8_t>(value)]; };
	auto twoHanded = [&itemAt](slots_t value) { return itemAt(value).itemId != 0 && itemAt(value).twoHanded; };
	auto weapon = [&itemAt](slots_t value) { const auto type = itemAt(value).weaponType; return itemAt(value).itemId != 0 && type != PlayerBotEquipmentWeaponType::None && type != PlayerBotEquipmentWeaponType::Shield; };
	auto shield = [&itemAt](slots_t value) { return itemAt(value).itemId != 0 && itemAt(value).weaponType == PlayerBotEquipmentWeaponType::Shield; };
	target = slot(0);
	if (candidate.head) target = head;
	else if (candidate.armorSlot) target = armor;
	else if (candidate.legs) target = legs;
	else if (candidate.feet) target = feet;
	else if (candidate.weaponType == PlayerBotEquipmentWeaponType::Shield) target = weapon(left) && !twoHanded(left) ? right : weapon(right) && !twoHanded(right) ? left : right;
	else if (candidate.weaponType != PlayerBotEquipmentWeaponType::None && candidate.weaponType != PlayerBotEquipmentWeaponType::Ammo && (candidate.left || candidate.right)) {
		if (requiresKnightCombatReadiness(player) && !melee(candidate.weaponType)) { rejection = "unsupported_weapon_type"; return false; }
		target = shield(left) ? right : shield(right) ? left : candidate.left ? left : right;
	} else { rejection = "unsupported_slot"; return false; }
	replacedItemId = candidate.twoHanded ? loadout.itemIds[static_cast<uint8_t>(left)] : loadout.itemIds[static_cast<uint8_t>(target)];
	displacedLeftItemId = 0;
	displacedRightItemId = 0;
	if (candidate.twoHanded) {
		displacedLeftItemId = loadout.itemIds[static_cast<uint8_t>(left)]; displacedRightItemId = loadout.itemIds[static_cast<uint8_t>(right)];
		loadout.itemIds[static_cast<uint8_t>(left)] = candidate.itemId; loadout.items[static_cast<uint8_t>(left)] = candidate;
		loadout.itemIds[static_cast<uint8_t>(right)] = 0; loadout.items[static_cast<uint8_t>(right)] = {};
	} else if (twoHanded(left) || twoHanded(right)) {
		displacedLeftItemId = loadout.itemIds[static_cast<uint8_t>(left)]; displacedRightItemId = loadout.itemIds[static_cast<uint8_t>(right)];
		loadout.itemIds[static_cast<uint8_t>(left)] = 0; loadout.items[static_cast<uint8_t>(left)] = {};
		loadout.itemIds[static_cast<uint8_t>(right)] = 0; loadout.items[static_cast<uint8_t>(right)] = {};
		loadout.itemIds[static_cast<uint8_t>(target)] = candidate.itemId; loadout.items[static_cast<uint8_t>(target)] = candidate;
	} else {
		if (target == left) displacedLeftItemId = loadout.itemIds[static_cast<uint8_t>(target)];
		else if (target == right) displacedRightItemId = loadout.itemIds[static_cast<uint8_t>(target)];
		loadout.itemIds[static_cast<uint8_t>(target)] = candidate.itemId; loadout.items[static_cast<uint8_t>(target)] = candidate;
	}
	return true;
}

PlayerBotCombatProfile PlayerBotEquipmentPolicy::combatProfile(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout) const
{
	auto itemAt = [&loadout](slots_t value) -> const PlayerBotEquipmentItemSnapshot& { return loadout.items[static_cast<uint8_t>(value)]; };
	int32_t armorValue = 0;
	for (slots_t value : {head, necklace, armor, legs, feet, ring}) armorValue += itemAt(value).armor;
	const PlayerBotEquipmentItemSnapshot* weapon = nullptr;
	const PlayerBotEquipmentItemSnapshot* shield = nullptr;
	for (slots_t value : {right, left}) {
		const auto& item = itemAt(value);
		if (item.itemId == 0 || item.weaponType == PlayerBotEquipmentWeaponType::None) continue;
		if (item.weaponType == PlayerBotEquipmentWeaponType::Shield) { if (!shield || item.defense > shield->defense) shield = &item; }
		else weapon = &item;
	}
	int32_t defenseValue = 7;
	int32_t defenseSkill = player.fistSkill;
	if (weapon) { defenseValue = weapon->defense + weapon->extraDefense; defenseSkill = skill(player, weapon->weaponType); }
	if (shield) { defenseValue = weapon ? shield->defense + weapon->extraDefense : shield->defense; defenseSkill = player.shieldSkill; }
	const int32_t defense = defenseSkill == 0 ? 1 : static_cast<int32_t>((defenseSkill / 4.0 + 2.23) * defenseValue * 0.15 * player.defenseFactor * player.defenseMultiplier);
	return {player.level, player.maximumHealth, static_cast<int32_t>(armorValue * player.armorMultiplier), defense,
	        weapon ? weapon->attack : 7, skill(player, weapon ? weapon->weaponType : PlayerBotEquipmentWeaponType::None), player.attackFactor};
}

bool PlayerBotEquipmentPolicy::loadoutReady(const PlayerBotEquipmentPlayerSnapshot& player, const PlayerBotEquipmentLoadout& loadout,
	const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight) const
{
	const auto& leftItem = loadout.items[static_cast<uint8_t>(left)];
	const auto& rightItem = loadout.items[static_cast<uint8_t>(right)];
	const auto& armorItem = loadout.items[static_cast<uint8_t>(armor)];
	const bool weaponReady = isKnightMeleeWeapon(player, leftItem) || isKnightMeleeWeapon(player, rightItem);
	const bool armorReady = armorItem.itemId != 0 && isLegalEquipmentItem(player, armorItem) && armorItem.armorSlot && armorItem.armor > 0;
	return weaponReady && armorReady && readiness.backpackReady && readiness.suppliesReady &&
	       static_cast<uint64_t>(readiness.effectiveFreeCapacity) >= static_cast<uint64_t>(readiness.minimumFreeCapacity) + additionalWeight;
}

PlayerBotEquipmentReadiness PlayerBotEquipmentPolicy::combatReadiness(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout, bool carriedUpgrade, const PlayerBotEquipmentReadinessInput& readiness) const
{
	PlayerBotEquipmentReadiness result;
	if (!requiresKnightCombatReadiness(player)) { result.ready = true; return result; }
	const auto& leftItem = loadout.items[static_cast<uint8_t>(left)];
	const auto& rightItem = loadout.items[static_cast<uint8_t>(right)];
	const auto& armorItem = loadout.items[static_cast<uint8_t>(armor)];
	const bool weaponReady = isKnightMeleeWeapon(player, leftItem) || isKnightMeleeWeapon(player, rightItem);
	const bool armorReady = armorItem.itemId != 0 && isLegalEquipmentItem(player, armorItem) && armorItem.armorSlot && armorItem.armor > 0;
	if (carriedUpgrade) { result.recovery = "equip_carried"; return result; }
	if (weaponReady && armorReady && readiness.backpackReady && readiness.suppliesReady && readiness.effectiveFreeCapacity >= readiness.minimumFreeCapacity) { result.ready = true; return result; }
	if (!weaponReady) result.terminalReason = "missing_legal_melee_weapon";
	else if (!armorReady) result.terminalReason = "missing_legal_armor";
	else if (!readiness.backpackReady) result.terminalReason = "missing_backpack";
	else result.recovery = "service";
	return result;
}

PlayerBotEquipmentOfferEvaluation PlayerBotEquipmentPolicy::evaluateCandidate(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& candidate, const PlayerBotEquipmentLoadout& currentLoadout,
	const PlayerBotCombatProfile& currentProfile, const PlayerBotEquipmentHuntSummary& currentHunts, bool currentReady,
	const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight, bool allowSimulation,
	const HuntSummaryEvaluator& huntSummary) const
{
	PlayerBotEquipmentOfferEvaluation evaluation;
	evaluation.itemId = candidate.itemId;
	evaluation.currentReady = currentReady;
	PlayerBotEquipmentLoadout candidateLoadout = currentLoadout;
	if (!applyOffer(player, candidateLoadout, candidate, evaluation.slot, evaluation.replacedItemId, evaluation.displacedLeftItemId, evaluation.displacedRightItemId, evaluation.rejection)) {
		evaluation.profile = currentProfile; evaluation.hunts = currentHunts; return evaluation;
	}
	if (!allowSimulation) { evaluation.profile = currentProfile; evaluation.hunts = currentHunts; evaluation.rejection = "unique_item_evaluation_budget_exhausted"; return evaluation; }
	evaluation.simulated = true;
	evaluation.profile = combatProfile(player, candidateLoadout);
	evaluation.hunts = huntSummary(evaluation.profile);
	evaluation.candidateReady = loadoutReady(player, candidateLoadout, readiness, additionalWeight);
	const int32_t currentMaximumDamage = maximumDamage(currentProfile.level, currentProfile.attackSkill, currentProfile.attack, currentProfile.attackFactor);
	const int32_t candidateMaximumDamage = maximumDamage(evaluation.profile.level, evaluation.profile.attackSkill, evaluation.profile.attack, evaluation.profile.attackFactor);
	const bool noWorse = evaluation.profile.armor >= currentProfile.armor && evaluation.profile.defense >= currentProfile.defense && candidateMaximumDamage >= currentMaximumDamage && evaluation.hunts.suitableRegions >= currentHunts.suitableRegions && evaluation.hunts.lowestThreatRatio <= currentHunts.lowestThreatRatio && evaluation.hunts.bestProjectedExperience >= currentHunts.bestProjectedExperience;
	const bool better = evaluation.profile.armor > currentProfile.armor || evaluation.profile.defense > currentProfile.defense || candidateMaximumDamage > currentMaximumDamage || evaluation.hunts.suitableRegions > currentHunts.suitableRegions || evaluation.hunts.lowestThreatRatio < currentHunts.lowestThreatRatio || evaluation.hunts.bestProjectedExperience > currentHunts.bestProjectedExperience;
	if (currentReady && !evaluation.candidateReady) evaluation.rejection = "regresses_readiness";
	else if (!noWorse) evaluation.rejection = better ? "ambiguous_tradeoff" : "non_improving";
	else if (!better) evaluation.rejection = "non_improving";
	else evaluation.rule = !currentReady && evaluation.candidateReady ? PlayerBotEquipmentDecisionRule::ReadinessRepair : evaluation.hunts.suitableRegions > currentHunts.suitableRegions ? PlayerBotEquipmentDecisionRule::UnlocksHunt : PlayerBotEquipmentDecisionRule::ParetoImprovement;
	return evaluation;
}

const char* PlayerBotEquipmentPolicy::decisionRuleName(PlayerBotEquipmentDecisionRule rule)
{
	switch (rule) {
		case PlayerBotEquipmentDecisionRule::ParetoImprovement: return "pareto_improvement";
		case PlayerBotEquipmentDecisionRule::UnlocksHunt: return "unlocks_suitable_hunt";
		case PlayerBotEquipmentDecisionRule::ReadinessRepair: return "fills_readiness_gap";
		case PlayerBotEquipmentDecisionRule::None: return "none";
	}
	return "none";
}

bool PlayerBotEquipmentPolicy::prefers(const PlayerBotEquipmentOfferEvaluation& candidate,
	const PlayerBotEquipmentOfferEvaluation& current)
{
	return candidate.rule > current.rule || (candidate.rule == current.rule && (candidate.carried != current.carried ? candidate.carried : candidate.price < current.price || (candidate.price == current.price && (candidate.travelSteps < current.travelSteps || (candidate.travelSteps == current.travelSteps && (candidate.itemId < current.itemId || (candidate.itemId == current.itemId && candidate.npcId < current.npcId)))))));
}
