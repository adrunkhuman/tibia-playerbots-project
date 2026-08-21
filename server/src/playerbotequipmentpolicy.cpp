/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotequipmentpolicy.h"

#include "game.h"
#include "item.h"
#include "player.h"
#include "weapons.h"

namespace {
	skills_t skillForWeapon(WeaponType_t weaponType)
	{
		switch (weaponType) {
			case WEAPON_SWORD: return SKILL_SWORD;
			case WEAPON_CLUB: return SKILL_CLUB;
			case WEAPON_AXE: return SKILL_AXE;
			case WEAPON_DISTANCE: case WEAPON_AMMO: return SKILL_DISTANCE;
			default: return SKILL_FIST;
		}
	}
}

PlayerBotEquipmentPolicy::PlayerBotEquipmentPolicy(uint16_t combatReadinessVocationId) :
	combatReadinessVocationId(combatReadinessVocationId)
{
}

bool PlayerBotEquipmentPolicy::requiresKnightCombatReadiness(const Player& player) const
{
	return player.getVocationId() == combatReadinessVocationId;
}

bool PlayerBotEquipmentPolicy::isLegalEquipmentType(const Player& player, const ItemType& type) const
{
	if (!type.isPickupable() || player.getLevel() < type.minReqLevel ||
	    player.getMagicLevel() < type.minReqMagicLevel ||
	    ((type.wieldInfo & WIELDINFO_PREMIUM) != 0 && !player.isPremium())) {
		return false;
	}
	return type.vocationIds.empty() || type.vocationIds.find(player.getVocationId()) != type.vocationIds.end();
}

bool PlayerBotEquipmentPolicy::isLegalEquipmentItem(const Player& player, const Item& item) const
{
	return !item.isRemoved() && isLegalEquipmentType(player, Item::items[item.getID()]);
}

bool PlayerBotEquipmentPolicy::isKnightMeleeWeapon(const Player& player, const Item& item) const
{
	const WeaponType_t weaponType = item.getWeaponType();
	return isLegalEquipmentItem(player, item) && item.getAttack() > 0 &&
	       (weaponType == WEAPON_SWORD || weaponType == WEAPON_CLUB || weaponType == WEAPON_AXE) &&
	       (item.getSlotPosition() & (SLOTP_LEFT | SLOTP_RIGHT)) != 0;
}

bool PlayerBotEquipmentPolicy::isCombatEquipment(const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	return (type.slotPosition & (SLOTP_HEAD | SLOTP_ARMOR | SLOTP_LEGS | SLOTP_FEET)) != 0 ||
	       type.weaponType != WEAPON_NONE || item.getArmor() > 0 || item.getDefense() > 0;
}

std::optional<PlayerBotEquipmentUpgrade> PlayerBotEquipmentPolicy::evaluateUpgrade(const Player& player, const Item& candidate) const
{
	const ItemType& type = Item::items[candidate.getID()];
	if (!isLegalEquipmentItem(player, candidate)) {
		return std::nullopt;
	}
	slots_t slot = CONST_SLOT_WHEREEVER;
	const char* metric = nullptr;
	int32_t candidateValue = 0;
	if (type.slotPosition & SLOTP_HEAD) {
		slot = CONST_SLOT_HEAD;
		metric = "armor";
		candidateValue = candidate.getArmor();
	} else if (type.slotPosition & SLOTP_ARMOR) {
		slot = CONST_SLOT_ARMOR;
		metric = "armor";
		candidateValue = candidate.getArmor();
	} else if (type.slotPosition & SLOTP_LEGS) {
		slot = CONST_SLOT_LEGS;
		metric = "armor";
		candidateValue = candidate.getArmor();
	} else if (type.slotPosition & SLOTP_FEET) {
		slot = CONST_SLOT_FEET;
		metric = "armor";
		candidateValue = candidate.getArmor();
	} else if (candidate.getWeaponType() == WEAPON_SHIELD) {
		slot = CONST_SLOT_RIGHT;
		metric = "defense";
		candidateValue = candidate.getDefense();
	} else if (!(type.slotPosition & SLOTP_TWO_HAND) && candidate.getWeaponType() != WEAPON_NONE &&
	           candidate.getWeaponType() != WEAPON_AMMO) {
		slot = CONST_SLOT_LEFT;
		metric = "attack";
		candidateValue = candidate.getAttack();
	}
	if (slot == CONST_SLOT_WHEREEVER || candidateValue <= 0) {
		return std::nullopt;
	}

	const Item* equipped = player.getInventoryItem(slot);
	int32_t currentValue = 0;
	if (equipped) {
		if (std::strcmp(metric, "armor") == 0) {
			currentValue = equipped->getArmor();
		} else if (std::strcmp(metric, "defense") == 0) {
			currentValue = equipped->getDefense();
		} else {
			currentValue = equipped->getAttack();
			const int32_t candidateMaximumDamage = Weapons::getMaxWeaponDamage(
				player.getLevel(), player.getWeaponSkill(&candidate), candidate.getAttack(), player.getAttackFactor());
			const int32_t currentMaximumDamage = Weapons::getMaxWeaponDamage(
				player.getLevel(), player.getWeaponSkill(equipped), equipped->getAttack(), player.getAttackFactor());
			if (candidateMaximumDamage <= currentMaximumDamage) {
				return std::nullopt;
			}
		}
	}
	if (candidateValue <= currentValue) {
		return std::nullopt;
	}
	return PlayerBotEquipmentUpgrade{slot, candidateValue - currentValue, metric, currentValue, candidateValue};
}

bool PlayerBotEquipmentPolicy::findCarriedUpgrade(Player& player, Item*& selectedItem,
	                                                PlayerBotEquipmentUpgrade& selectedUpgrade) const
{
	selectedItem = nullptr;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* root = player.getInventoryItem(static_cast<slots_t>(slot));
		Container* container = root ? root->getContainer() : nullptr;
		if (!container) {
			continue;
		}
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			Item* candidate = *it;
			std::optional<PlayerBotEquipmentUpgrade> upgrade = evaluateUpgrade(player, *candidate);
			if (!upgrade || (requiresKnightCombatReadiness(player) && candidate->getWeaponType() != WEAPON_NONE &&
			                 !isKnightMeleeWeapon(player, *candidate))) {
				continue;
			}
			Position source;
			uint8_t index = 0;
			g_game.internalGetPosition(candidate, source, index);
			if (source.x != 0xFFFF || (source.y & 0x40) == 0 ||
			    (selectedItem && upgrade->benefit <= selectedUpgrade.benefit)) {
				continue;
			}
			selectedItem = candidate;
			selectedUpgrade = *upgrade;
		}
	}
	return selectedItem != nullptr;
}

PlayerBotEquipmentLoadout PlayerBotEquipmentPolicy::loadout(const Player& player) const
{
	PlayerBotEquipmentLoadout loadout;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			loadout.itemIds[slot] = item->getID();
		}
	}
	return loadout;
}

bool PlayerBotEquipmentPolicy::applyOffer(const Player& player, PlayerBotEquipmentLoadout& loadout, uint16_t itemId,
	                                         slots_t& slot, uint16_t& replacedItemId, uint16_t& displacedLeftItemId,
	                                         uint16_t& displacedRightItemId, std::string& rejection) const
{
	const ItemType& type = Item::items[itemId];
	if (!type.isPickupable()) { rejection = "not_pickupable"; return false; }
	if (player.getLevel() < type.minReqLevel) { rejection = "level_ineligible"; return false; }
	if (player.getMagicLevel() < type.minReqMagicLevel) { rejection = "magic_level_ineligible"; return false; }
	if ((type.wieldInfo & WIELDINFO_PREMIUM) != 0 && !player.isPremium()) { rejection = "premium_ineligible"; return false; }
	if (!type.vocationIds.empty() && type.vocationIds.find(player.getVocationId()) == type.vocationIds.end()) {
		rejection = "vocation_ineligible";
		return false;
	}

	auto itemTypeAt = [&loadout](slots_t hand) -> const ItemType* {
		const uint16_t equippedItemId = loadout.itemIds[hand];
		return equippedItemId == 0 ? nullptr : &Item::items[equippedItemId];
	};
	auto isTwoHanded = [&itemTypeAt](slots_t hand) {
		const ItemType* equipped = itemTypeAt(hand);
		return equipped && (equipped->slotPosition & SLOTP_TWO_HAND) != 0;
	};
	auto isWeapon = [&itemTypeAt](slots_t hand) {
		const ItemType* equipped = itemTypeAt(hand);
		return equipped && equipped->weaponType != WEAPON_NONE && equipped->weaponType != WEAPON_SHIELD;
	};
	auto isShield = [&itemTypeAt](slots_t hand) {
		const ItemType* equipped = itemTypeAt(hand);
		return equipped && equipped->weaponType == WEAPON_SHIELD;
	};

	slot = CONST_SLOT_WHEREEVER;
	if (type.slotPosition & SLOTP_HEAD) slot = CONST_SLOT_HEAD;
	else if (type.slotPosition & SLOTP_ARMOR) slot = CONST_SLOT_ARMOR;
	else if (type.slotPosition & SLOTP_LEGS) slot = CONST_SLOT_LEGS;
	else if (type.slotPosition & SLOTP_FEET) slot = CONST_SLOT_FEET;
	else if (type.weaponType == WEAPON_SHIELD) {
		slot = isWeapon(CONST_SLOT_LEFT) && !isTwoHanded(CONST_SLOT_LEFT) ? CONST_SLOT_RIGHT :
		       isWeapon(CONST_SLOT_RIGHT) && !isTwoHanded(CONST_SLOT_RIGHT) ? CONST_SLOT_LEFT : CONST_SLOT_RIGHT;
	} else if (type.weaponType != WEAPON_NONE && type.weaponType != WEAPON_AMMO &&
	           (type.slotPosition & (SLOTP_LEFT | SLOTP_RIGHT)) != 0) {
		if (requiresKnightCombatReadiness(player) && type.weaponType != WEAPON_SWORD &&
		    type.weaponType != WEAPON_CLUB && type.weaponType != WEAPON_AXE) {
			rejection = "unsupported_weapon_type";
			return false;
		}
		slot = isShield(CONST_SLOT_LEFT) ? CONST_SLOT_RIGHT : isShield(CONST_SLOT_RIGHT) ? CONST_SLOT_LEFT :
		       (type.slotPosition & SLOTP_LEFT) != 0 ? CONST_SLOT_LEFT : CONST_SLOT_RIGHT;
	} else {
		rejection = "unsupported_slot";
		return false;
	}

	replacedItemId = (type.slotPosition & SLOTP_TWO_HAND) != 0 ? loadout.itemIds[CONST_SLOT_LEFT] : loadout.itemIds[slot];
	displacedLeftItemId = 0;
	displacedRightItemId = 0;
	if ((type.slotPosition & SLOTP_TWO_HAND) != 0) {
		displacedLeftItemId = loadout.itemIds[CONST_SLOT_LEFT];
		displacedRightItemId = loadout.itemIds[CONST_SLOT_RIGHT];
		loadout.itemIds[CONST_SLOT_LEFT] = itemId;
		loadout.itemIds[CONST_SLOT_RIGHT] = 0;
	} else if (isTwoHanded(CONST_SLOT_LEFT) || isTwoHanded(CONST_SLOT_RIGHT)) {
		displacedLeftItemId = loadout.itemIds[CONST_SLOT_LEFT];
		displacedRightItemId = loadout.itemIds[CONST_SLOT_RIGHT];
		loadout.itemIds[CONST_SLOT_LEFT] = 0;
		loadout.itemIds[CONST_SLOT_RIGHT] = 0;
		loadout.itemIds[slot] = itemId;
	} else {
		if (slot == CONST_SLOT_LEFT) displacedLeftItemId = loadout.itemIds[slot];
		else if (slot == CONST_SLOT_RIGHT) displacedRightItemId = loadout.itemIds[slot];
		loadout.itemIds[slot] = itemId;
	}
	return true;
}

PlayerBotCombatProfile PlayerBotEquipmentPolicy::combatProfile(const Player& player,
	                                                              const PlayerBotEquipmentLoadout& loadout) const
{
	auto itemTypeAt = [&loadout](slots_t slot) -> const ItemType* {
		const uint16_t itemId = loadout.itemIds[slot];
		return itemId == 0 ? nullptr : &Item::items[itemId];
	};
	int32_t armor = 0;
	for (slots_t slot : {CONST_SLOT_HEAD, CONST_SLOT_NECKLACE, CONST_SLOT_ARMOR, CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_RING}) {
		if (const ItemType* type = itemTypeAt(slot)) armor += type->armor;
	}
	const ItemType* weapon = nullptr;
	const ItemType* shield = nullptr;
	for (slots_t slot : {CONST_SLOT_RIGHT, CONST_SLOT_LEFT}) {
		const ItemType* type = itemTypeAt(slot);
		if (!type || type->weaponType == WEAPON_NONE) continue;
		if (type->weaponType == WEAPON_SHIELD) {
			if (!shield || type->defense > shield->defense) shield = type;
		} else {
			weapon = type;
		}
	}
	int32_t defenseValue = 7;
	int32_t defenseSkill = player.getSkillLevel(SKILL_FIST);
	if (weapon) {
		defenseValue = weapon->defense + weapon->extraDefense;
		defenseSkill = player.getSkillLevel(skillForWeapon(weapon->weaponType));
	}
	if (shield) {
		defenseValue = weapon ? shield->defense + weapon->extraDefense : shield->defense;
		defenseSkill = player.getSkillLevel(SKILL_SHIELD);
	}
	const int32_t defense = defenseSkill == 0 ? 1 : static_cast<int32_t>(
		(defenseSkill / 4.0 + 2.23) * defenseValue * 0.15 * player.getDefenseFactor() * player.getVocation()->defenseMultiplier);
	return {player.getLevel(), player.getMaxHealth(), static_cast<int32_t>(armor * player.getVocation()->armorMultiplier), defense,
	        weapon ? weapon->attack : 7, player.getSkillLevel(skillForWeapon(weapon ? weapon->weaponType : WEAPON_NONE)),
	        player.getAttackFactor()};
}

bool PlayerBotEquipmentPolicy::loadoutReady(const Player& player, const PlayerBotEquipmentLoadout& loadout,
	                                           const PlayerBotEquipmentReadinessInput& readiness,
	                                           uint32_t additionalWeight) const
{
	auto isKnightWeapon = [this, &player](uint16_t itemId) {
		if (itemId == 0) return false;
		const ItemType& type = Item::items[itemId];
		return isLegalEquipmentType(player, type) && type.attack > 0 &&
		       (type.weaponType == WEAPON_SWORD || type.weaponType == WEAPON_CLUB || type.weaponType == WEAPON_AXE) &&
		       (type.slotPosition & (SLOTP_LEFT | SLOTP_RIGHT)) != 0;
	};
	const uint16_t armorItemId = loadout.itemIds[CONST_SLOT_ARMOR];
	const bool armorReady = armorItemId != 0 && isLegalEquipmentType(player, Item::items[armorItemId]) &&
	                        (Item::items[armorItemId].slotPosition & SLOTP_ARMOR) != 0 && Item::items[armorItemId].armor > 0;
	return (isKnightWeapon(loadout.itemIds[CONST_SLOT_LEFT]) || isKnightWeapon(loadout.itemIds[CONST_SLOT_RIGHT])) && armorReady &&
	       readiness.backpackReady && readiness.suppliesReady &&
	       static_cast<uint64_t>(readiness.effectiveFreeCapacity) >=
	           static_cast<uint64_t>(readiness.minimumFreeCapacity) + additionalWeight;
}

PlayerBotEquipmentReadiness PlayerBotEquipmentPolicy::combatReadiness(
	const Player& player, bool carriedUpgrade, const PlayerBotEquipmentReadinessInput& readiness) const
{
	PlayerBotEquipmentReadiness result;
	if (!requiresKnightCombatReadiness(player)) {
		result.ready = true;
		return result;
	}
	const Item* left = player.getInventoryItem(CONST_SLOT_LEFT);
	const Item* right = player.getInventoryItem(CONST_SLOT_RIGHT);
	const Item* armor = player.getInventoryItem(CONST_SLOT_ARMOR);
	const bool weaponReady = (left && isKnightMeleeWeapon(player, *left)) || (right && isKnightMeleeWeapon(player, *right));
	const bool armorReady = armor && isLegalEquipmentItem(player, *armor) &&
	                        (armor->getSlotPosition() & SLOTP_ARMOR) != 0 && armor->getArmor() > 0;
	if (carriedUpgrade) {
		result.recovery = "equip_carried";
		return result;
	}
	if (weaponReady && armorReady && readiness.backpackReady && readiness.suppliesReady &&
	    readiness.effectiveFreeCapacity >= readiness.minimumFreeCapacity) {
		result.ready = true;
		return result;
	}
	if (!weaponReady) result.terminalReason = "missing_legal_melee_weapon";
	else if (!armorReady) result.terminalReason = "missing_legal_armor";
	else if (!readiness.backpackReady) result.terminalReason = "missing_backpack";
	else result.recovery = "service";
	return result;
}

PlayerBotEquipmentOfferEvaluation PlayerBotEquipmentPolicy::evaluateCandidate(
	Player& player, uint16_t itemId, const PlayerBotEquipmentLoadout& currentLoadout,
	const PlayerBotCombatProfile& currentProfile, const PlayerBotEquipmentHuntSummary& currentHunts,
	bool currentReady, const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight,
	bool allowSimulation, const HuntSummaryEvaluator& huntSummary) const
{
	PlayerBotEquipmentOfferEvaluation evaluation;
	evaluation.itemId = itemId;
	evaluation.currentReady = currentReady;
	PlayerBotEquipmentLoadout candidateLoadout = currentLoadout;
	if (!applyOffer(player, candidateLoadout, itemId, evaluation.slot, evaluation.replacedItemId,
	                evaluation.displacedLeftItemId, evaluation.displacedRightItemId, evaluation.rejection)) {
		evaluation.profile = currentProfile;
		evaluation.hunts = currentHunts;
		return evaluation;
	}
	if (!allowSimulation) {
		evaluation.profile = currentProfile;
		evaluation.hunts = currentHunts;
		evaluation.rejection = "unique_item_evaluation_budget_exhausted";
		return evaluation;
	}
	evaluation.simulated = true;
	evaluation.profile = combatProfile(player, candidateLoadout);
	evaluation.hunts = huntSummary(player, evaluation.profile);
	evaluation.candidateReady = loadoutReady(player, candidateLoadout, readiness, additionalWeight);
	const int32_t currentMaximumDamage = Weapons::getMaxWeaponDamage(
		currentProfile.level, currentProfile.attackSkill, currentProfile.attack, currentProfile.attackFactor);
	const int32_t candidateMaximumDamage = Weapons::getMaxWeaponDamage(
		evaluation.profile.level, evaluation.profile.attackSkill, evaluation.profile.attack, evaluation.profile.attackFactor);
	const bool noWorse = evaluation.profile.armor >= currentProfile.armor &&
	                     evaluation.profile.defense >= currentProfile.defense &&
	                     candidateMaximumDamage >= currentMaximumDamage &&
	                     evaluation.hunts.suitableRegions >= currentHunts.suitableRegions &&
	                     evaluation.hunts.lowestThreatRatio <= currentHunts.lowestThreatRatio &&
	                     evaluation.hunts.bestProjectedExperience >= currentHunts.bestProjectedExperience;
	const bool better = evaluation.profile.armor > currentProfile.armor ||
	                    evaluation.profile.defense > currentProfile.defense ||
	                    candidateMaximumDamage > currentMaximumDamage ||
	                    evaluation.hunts.suitableRegions > currentHunts.suitableRegions ||
	                    evaluation.hunts.lowestThreatRatio < currentHunts.lowestThreatRatio ||
	                    evaluation.hunts.bestProjectedExperience > currentHunts.bestProjectedExperience;
	if (currentReady && !evaluation.candidateReady) evaluation.rejection = "regresses_readiness";
	else if (!noWorse) evaluation.rejection = better ? "ambiguous_tradeoff" : "non_improving";
	else if (!better) evaluation.rejection = "non_improving";
	else evaluation.rule = !currentReady && evaluation.candidateReady ? PlayerBotEquipmentDecisionRule::ReadinessRepair :
	                       evaluation.hunts.suitableRegions > currentHunts.suitableRegions ? PlayerBotEquipmentDecisionRule::UnlocksHunt :
	                                                                                PlayerBotEquipmentDecisionRule::ParetoImprovement;
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
	return candidate.rule > current.rule ||
	       (candidate.rule == current.rule && (candidate.carried != current.carried ? candidate.carried :
	        candidate.price < current.price ||
	        (candidate.price == current.price && (candidate.travelSteps < current.travelSteps ||
	         (candidate.travelSteps == current.travelSteps && (candidate.itemId < current.itemId ||
	          (candidate.itemId == current.itemId && candidate.npcId < current.npcId)))))));
}
