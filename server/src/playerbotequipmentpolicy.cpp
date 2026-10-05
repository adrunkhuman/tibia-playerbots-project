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
	constexpr slots_t ammo = slot(10);
	// Combat closes to an adjacent tile; ranged positioning (#235) will supply
	// the held distance. Flat-chance items ignore it.
	constexpr uint32_t engagementDistance = 1;

	bool melee(PlayerBotEquipmentWeaponType type)
	{
		return type == PlayerBotEquipmentWeaponType::Sword || type == PlayerBotEquipmentWeaponType::Club ||
		       type == PlayerBotEquipmentWeaponType::Axe;
	}

	bool inFamily(PlayerBotWeaponFamily family, PlayerBotEquipmentWeaponType type)
	{
		switch (family) {
			case PlayerBotWeaponFamily::Melee: return melee(type);
			case PlayerBotWeaponFamily::Distance: return type == PlayerBotEquipmentWeaponType::Distance;
			case PlayerBotWeaponFamily::None: break;
		}
		return false;
	}

	bool launcher(const PlayerBotEquipmentItemSnapshot& item)
	{
		return item.weaponType == PlayerBotEquipmentWeaponType::Distance && item.ammoType != 0;
	}

	bool feeds(const PlayerBotEquipmentItemSnapshot& weapon, const PlayerBotEquipmentItemSnapshot& ammunition)
	{
		return ammunition.itemId != 0 && ammunition.weaponType == PlayerBotEquipmentWeaponType::Ammo &&
		       ammunition.ammoType == weapon.ammoType;
	}

	// Mirrors WeaponDistance::useWeapon. The fired item is the throwing weapon
	// itself, or a launcher's ammunition plus the launcher's hit bonus.
	int32_t distanceHitChance(const PlayerBotEquipmentItemSnapshot& fired, int32_t launcherBonus, int32_t skillLevel,
	                          uint32_t distance)
	{
		const uint32_t skill = static_cast<uint32_t>(std::max(0, skillLevel));
		int32_t chance = fired.hitChance;
		if (chance == 0) {
			const int32_t maximum = fired.maxHitChance != -1 ? fired.maxHitChance : fired.ammoType != 0 ? 90 : 75;
			if (maximum == 75) {
				switch (distance) {
					case 1: case 5: chance = std::min<uint32_t>(skill, 74) + 1; break;
					case 2: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 28) * 2.40f) + 8; break;
					case 3: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 45) * 1.55f) + 6; break;
					case 4: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 58) * 1.25f) + 3; break;
					case 6: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 90) * 0.80f) + 3; break;
					case 7: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 104) * 0.70f) + 2; break;
					default: chance = fired.hitChance; break;
				}
			} else if (maximum == 90) {
				switch (distance) {
					case 1: case 5: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 74) * 1.20f) + 1; break;
					case 2: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 28) * 3.20f); break;
					case 3: chance = std::min<uint32_t>(skill, 45) * 2; break;
					case 4: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 58) * 1.55f); break;
					case 6: case 7: chance = std::min<uint32_t>(skill, 90); break;
					default: chance = fired.hitChance; break;
				}
			} else if (maximum == 100) {
				switch (distance) {
					case 1: case 5: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 73) * 1.35f) + 1; break;
					case 2: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 30) * 3.20f) + 4; break;
					case 3: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 48) * 2.05f) + 2; break;
					case 4: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 65) * 1.50f) + 2; break;
					case 6: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 87) * 1.20f) - 4; break;
					case 7: chance = static_cast<int32_t>(std::min<uint32_t>(skill, 90) * 1.10f) + 1; break;
					default: chance = fired.hitChance; break;
				}
			} else {
				chance = maximum;
			}
		}
		return std::clamp(chance + launcherBonus, 0, 100);
	}

	// A launcher alone fires nothing; ammunition is judged with its launcher.
	int32_t handHitChance(const PlayerBotEquipmentPlayerSnapshot& player, const PlayerBotEquipmentItemSnapshot& item)
	{
		if (item.weaponType != PlayerBotEquipmentWeaponType::Distance) return 100;
		return launcher(item) ? 0 : distanceHitChance(item, 0, player.distanceSkill, engagementDistance);
	}

	// Consumed items per 100 attacks: a launcher spends one ammunition per shot,
	// a throwing weapon breaks at its break chance, melee consumes nothing.
	int32_t consumption(const PlayerBotEquipmentLoadout& loadout)
	{
		for (slots_t hand : {left, right}) {
			const auto& item = loadout.items[static_cast<uint8_t>(hand)];
			if (item.itemId == 0 || item.weaponType != PlayerBotEquipmentWeaponType::Distance) continue;
			return launcher(item) ? 100 : item.breakChance;
		}
		return 0;
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

	// Maximum damage scaled by hit chance, in hundredths.
	int64_t expectedDamage(const PlayerBotCombatProfile& profile)
	{
		return static_cast<int64_t>(maximumDamage(profile.level, profile.attackSkill, profile.attack, profile.attackFactor)) *
		       profile.hitChance;
	}
}

bool PlayerBotEquipmentPolicy::managesEquipment(const PlayerBotEquipmentPlayerSnapshot& player) const
{
	return playerBotCombatStyle(player.vocationId).managed;
}

bool PlayerBotEquipmentPolicy::isLegalEquipmentItem(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& item) const
{
	return !item.removed && item.pickupable && player.level >= item.minimumLevel &&
	       player.magicLevel >= item.minimumMagicLevel && (!item.premiumRequired || player.premium) &&
	       (item.vocationIds.empty() || std::find(item.vocationIds.begin(), item.vocationIds.end(), player.vocationId) != item.vocationIds.end());
}

bool PlayerBotEquipmentPolicy::isStyleWeapon(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& item) const
{
	return isLegalEquipmentItem(player, item) && (item.attack > 0 || launcher(item)) &&
	       inFamily(playerBotCombatStyle(player.vocationId).weapons, item.weaponType) && (item.left || item.right);
}

bool PlayerBotEquipmentPolicy::isThrowingWeapon(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentItemSnapshot& item) const
{
	return item.weaponType == PlayerBotEquipmentWeaponType::Distance && !launcher(item) && item.breakChance != 0 &&
	       isStyleWeapon(player, item);
}

uint16_t PlayerBotEquipmentPolicy::throwingWeaponSupplyItem(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout, const std::vector<PlayerBotEquipmentItemSnapshot>& carried) const
{
	for (slots_t hand : {left, right}) {
		const auto& item = loadout.items[static_cast<uint8_t>(hand)];
		if (item.itemId != 0 && isThrowingWeapon(player, item)) return item.itemId;
	}
	if (weaponReady(player, loadout)) return 0;
	uint16_t selected = 0;
	int32_t selectedBenefit = 0;
	for (const auto& item : carried) {
		if (!isThrowingWeapon(player, item)) continue;
		const auto upgrade = evaluateUpgrade(player, loadout, item);
		if (upgrade && upgrade->benefit > selectedBenefit) {
			selected = item.itemId;
			selectedBenefit = upgrade->benefit;
		}
	}
	return selected;
}

bool PlayerBotEquipmentPolicy::weaponReady(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout) const
{
	const auto& ammunition = loadout.items[static_cast<uint8_t>(ammo)];
	for (slots_t hand : {left, right}) {
		const auto& item = loadout.items[static_cast<uint8_t>(hand)];
		if (isStyleWeapon(player, item) && (!launcher(item) || (feeds(item, ammunition) && isLegalEquipmentItem(player, ammunition)))) {
			return true;
		}
	}
	return false;
}

bool PlayerBotEquipmentPolicy::armorReady(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout) const
{
	const auto& item = loadout.items[static_cast<uint8_t>(armor)];
	return item.itemId != 0 && isLegalEquipmentItem(player, item) && item.armorSlot && item.armor > 0;
}

bool PlayerBotEquipmentPolicy::fillsReadinessGap(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout, const PlayerBotEquipmentItemSnapshot& candidate) const
{
	const bool weaponGap = !weaponReady(player, loadout);
	const bool armorGap = !armorReady(player, loadout);
	if (!weaponGap && !armorGap) return false;
	PlayerBotEquipmentLoadout candidateLoadout = loadout;
	slots_t target;
	uint16_t replaced, displacedLeft, displacedRight;
	std::string rejection;
	if (!applyOffer(player, candidateLoadout, candidate, target, replaced, displacedLeft, displacedRight, rejection)) return false;
	return (weaponGap && weaponReady(player, candidateLoadout)) || (armorGap && armorReady(player, candidateLoadout));
}

const char* PlayerBotEquipmentPolicy::weaponRequirement(const PlayerBotEquipmentPlayerSnapshot& player)
{
	switch (playerBotCombatStyle(player.vocationId).weapons) {
		case PlayerBotWeaponFamily::Melee: return "legal_melee_weapon";
		case PlayerBotWeaponFamily::Distance: return "legal_distance_weapon";
		case PlayerBotWeaponFamily::None: break;
	}
	return "legal_weapon";
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
		if (std::strcmp(metric, "attack") == 0) {
			const int64_t candidateDamage = static_cast<int64_t>(maximumDamage(player.level, skill(player, candidate.weaponType),
			    candidate.attack, player.attackFactor)) * handHitChance(player, candidate);
			const int64_t currentDamage = static_cast<int64_t>(maximumDamage(player.level, skill(player, equipped.weaponType),
			    equipped.attack, player.attackFactor)) * handHitChance(player, equipped);
			// Faster breakage is a cost trade-off, not a free upgrade.
			if (candidateDamage <= currentDamage || candidate.breakChance > equipped.breakChance) return std::nullopt;
		}
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
		if (!candidate.actionable || !candidate.item.inContainer || !upgrade || (managesEquipment(player) &&
		    candidate.item.weaponType != PlayerBotEquipmentWeaponType::None &&
		    candidate.item.weaponType != PlayerBotEquipmentWeaponType::Shield && !isStyleWeapon(player, candidate.item)) ||
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
		if (managesEquipment(player) && !inFamily(playerBotCombatStyle(player.vocationId).weapons, candidate.weaponType)) { rejection = "unsupported_weapon_type"; return false; }
		// Launchers need ammunition stock and a loadout-switching rule (#255).
		if (managesEquipment(player) && launcher(candidate)) { rejection = "launcher_loadout_deferred"; return false; }
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
	PlayerBotCombatProfile profile{player.level, player.maximumHealth, static_cast<int32_t>(armorValue * player.armorMultiplier), defense,
	        weapon ? weapon->attack : 7, skill(player, weapon ? weapon->weaponType : PlayerBotEquipmentWeaponType::None), player.attackFactor};
	if (weapon && weapon->weaponType == PlayerBotEquipmentWeaponType::Distance) {
		profile.blockedByShield = false;
		profile.attackRange = weapon->shootRange;
		profile.hitChance = handHitChance(player, *weapon);
		const auto& ammunition = itemAt(ammo);
		if (launcher(*weapon) && feeds(*weapon, ammunition)) {
			profile.attack += ammunition.attack;
			profile.hitChance = distanceHitChance(ammunition, weapon->hitChance, player.distanceSkill, engagementDistance);
		}
	}
	return profile;
}

bool PlayerBotEquipmentPolicy::loadoutReady(const PlayerBotEquipmentPlayerSnapshot& player, const PlayerBotEquipmentLoadout& loadout,
	const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight) const
{
	return weaponReady(player, loadout) && armorReady(player, loadout) && readiness.backpackReady && readiness.suppliesReady &&
	       static_cast<uint64_t>(readiness.effectiveFreeCapacity) >= static_cast<uint64_t>(readiness.minimumFreeCapacity) + additionalWeight;
}

PlayerBotBackpackAcquisition PlayerBotEquipmentPolicy::standardBackpackAcquisition(
	const PlayerBotEquipmentPlayerSnapshot& player, uint16_t currentBackItemId, bool currentBackIsContainer,
	uint32_t currentBackItems, uint32_t currentBackCapacity) const
{
	if (!managesEquipment(player)) return {false, false, "unsupported_vocation"};
	if (currentBackItemId == 0) return {true, false, nullptr};
	if (currentBackItemId != 1987 || !currentBackIsContainer || currentBackItems > currentBackCapacity) {
		return {false, false, "back_slot_not_upgradeable"};
	}
	return {true, true, nullptr};
}

PlayerBotEquipmentReadiness PlayerBotEquipmentPolicy::combatReadiness(const PlayerBotEquipmentPlayerSnapshot& player,
	const PlayerBotEquipmentLoadout& loadout, bool carriedUpgrade, const PlayerBotEquipmentReadinessInput& readiness) const
{
	PlayerBotEquipmentReadiness result;
	if (!managesEquipment(player)) { result.ready = true; return result; }
	const bool weaponIsReady = weaponReady(player, loadout);
	const bool armorIsReady = armorReady(player, loadout);
	if (carriedUpgrade) { result.recovery = "equip_carried"; return result; }
	if (weaponIsReady && armorIsReady && readiness.backpackReady && readiness.suppliesReady && readiness.effectiveFreeCapacity >= readiness.minimumFreeCapacity) { result.ready = true; return result; }
	// A missing weapon or armor is bought as readiness repair; the controller
	// stops with the matching missing_* reason only when no offer can fill it.
	if (!weaponIsReady) result.recovery = "acquire_weapon";
	else if (!armorIsReady) result.recovery = "acquire_armor";
	else if (!readiness.backpackReady) result.recovery = "acquire_backpack";
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
	const int64_t currentDamage = expectedDamage(currentProfile);
	const int64_t candidateDamage = expectedDamage(evaluation.profile);
	const int32_t currentConsumption = consumption(currentLoadout);
	const int32_t candidateConsumption = consumption(candidateLoadout);
	const bool noWorse = evaluation.profile.armor >= currentProfile.armor && evaluation.profile.defense >= currentProfile.defense && candidateDamage >= currentDamage && candidateConsumption <= currentConsumption && evaluation.hunts.suitableRegions >= currentHunts.suitableRegions && evaluation.hunts.lowestThreatRatio <= currentHunts.lowestThreatRatio && evaluation.hunts.bestProjectedExperience >= currentHunts.bestProjectedExperience;
	const bool better = evaluation.profile.armor > currentProfile.armor || evaluation.profile.defense > currentProfile.defense || candidateDamage > currentDamage || candidateConsumption < currentConsumption || evaluation.hunts.suitableRegions > currentHunts.suitableRegions || evaluation.hunts.lowestThreatRatio < currentHunts.lowestThreatRatio || evaluation.hunts.bestProjectedExperience > currentHunts.bestProjectedExperience;
	const bool gapFilled = (!weaponReady(player, currentLoadout) && weaponReady(player, candidateLoadout)) ||
	                       (!armorReady(player, currentLoadout) && armorReady(player, candidateLoadout));
	if (currentReady && !evaluation.candidateReady) evaluation.rejection = "regresses_readiness";
	// Filling a missing weapon or armor outranks every trade-off: without it the bot cannot hunt.
	else if (gapFilled) evaluation.rule = PlayerBotEquipmentDecisionRule::ReadinessRepair;
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
	if (candidate.toolAcquisition != current.toolAcquisition) return candidate.toolAcquisition;
	return candidate.rule > current.rule || (candidate.rule == current.rule && (candidate.carried != current.carried ? candidate.carried : candidate.price < current.price || (candidate.price == current.price && (candidate.travelSteps < current.travelSteps || (candidate.travelSteps == current.travelSteps && (candidate.itemId < current.itemId || (candidate.itemId == current.itemId && candidate.npcId < current.npcId)))))));
}
