/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotcontroller.h"
#include "weapons.h"

using namespace playerbot;

namespace {
	constexpr uint32_t maximumEquipmentProviderDistance = 200;
	constexpr size_t maximumEquipmentHuntRegions = 32;
	constexpr size_t maximumEquipmentProviderRoutes = 4;
	constexpr size_t maximumEquipmentProviderApproaches = 4;
	constexpr uint64_t maximumEquipmentProviderPathNodes = 5000;
	constexpr size_t maximumEquipmentCatalogOffers = 64;

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

PlayerBotController::EquipmentLoadout PlayerBotController::equipmentLoadout(const Player& player) const
{
	EquipmentLoadout loadout;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			loadout.itemIds[slot] = item->getID();
		}
	}
	return loadout;
}

bool PlayerBotController::applyEquipmentOffer(const Player& player, EquipmentLoadout& loadout, uint16_t itemId, slots_t& slot,
                                              uint16_t& replacedItemId, uint16_t& displacedLeftItemId,
                                              uint16_t& displacedRightItemId,
                                              std::string& rejection) const
{
	const ItemType& type = Item::items[itemId];
	if (!type.isPickupable()) {
		rejection = "not_pickupable";
		return false;
	}
	if (player.getLevel() < type.minReqLevel) {
		rejection = "level_ineligible";
		return false;
	}
	if (player.getMagicLevel() < type.minReqMagicLevel) {
		rejection = "magic_level_ineligible";
		return false;
	}
	if ((type.wieldInfo & WIELDINFO_PREMIUM) != 0 && !player.isPremium()) {
		rejection = "premium_ineligible";
		return false;
	}
	if (!type.vocationIds.empty() && type.vocationIds.find(player.getVocationId()) == type.vocationIds.end()) {
		rejection = "vocation_ineligible";
		return false;
	}

	auto itemTypeAt = [&loadout](slots_t slot) -> const ItemType* {
		const uint16_t equippedItemId = loadout.itemIds[slot];
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
	if (type.slotPosition & SLOTP_HEAD) {
		slot = CONST_SLOT_HEAD;
	} else if (type.slotPosition & SLOTP_ARMOR) {
		slot = CONST_SLOT_ARMOR;
	} else if (type.slotPosition & SLOTP_LEGS) {
		slot = CONST_SLOT_LEGS;
	} else if (type.slotPosition & SLOTP_FEET) {
		slot = CONST_SLOT_FEET;
	} else if (type.weaponType == WEAPON_SHIELD) {
		slot = isWeapon(CONST_SLOT_LEFT) && !isTwoHanded(CONST_SLOT_LEFT) ? CONST_SLOT_RIGHT :
		       isWeapon(CONST_SLOT_RIGHT) && !isTwoHanded(CONST_SLOT_RIGHT) ? CONST_SLOT_LEFT : CONST_SLOT_RIGHT;
	} else if (type.weaponType != WEAPON_NONE && type.weaponType != WEAPON_AMMO &&
	           (type.slotPosition & (SLOTP_LEFT | SLOTP_RIGHT)) != 0) {
		if (requiresKnightCombatReadiness(player) &&
		    type.weaponType != WEAPON_SWORD && type.weaponType != WEAPON_CLUB && type.weaponType != WEAPON_AXE) {
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
		if (slot == CONST_SLOT_LEFT) {
			displacedLeftItemId = loadout.itemIds[slot];
		} else if (slot == CONST_SLOT_RIGHT) {
			displacedRightItemId = loadout.itemIds[slot];
		}
		loadout.itemIds[slot] = itemId;
	}
	return true;
}

PlayerBotCombatProfile PlayerBotController::equipmentCombatProfile(const Player& player, const EquipmentLoadout& loadout) const
{
	auto itemTypeAt = [&loadout](slots_t slot) -> const ItemType* {
		const uint16_t itemId = loadout.itemIds[slot];
		return itemId == 0 ? nullptr : &Item::items[itemId];
	};

	int32_t armor = 0;
	for (slots_t slot : {CONST_SLOT_HEAD, CONST_SLOT_NECKLACE, CONST_SLOT_ARMOR, CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_RING}) {
		if (const ItemType* type = itemTypeAt(slot)) {
			armor += type->armor;
		}
	}

	const ItemType* weapon = nullptr;
	const ItemType* shield = nullptr;
	for (slots_t slot : {CONST_SLOT_RIGHT, CONST_SLOT_LEFT}) {
		const ItemType* type = itemTypeAt(slot);
		if (!type || type->weaponType == WEAPON_NONE) {
			continue;
		}
		if (type->weaponType == WEAPON_SHIELD) {
			if (!shield || type->defense > shield->defense) {
				shield = type;
			}
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

bool PlayerBotController::equipmentLoadoutReady(const Player& player, const EquipmentLoadout& loadout,
                                                uint32_t additionalWeight) const
{
	const auto isKnightWeapon = [this, &player](uint16_t itemId) {
		if (itemId == 0) {
			return false;
		}
		const ItemType& type = Item::items[itemId];
		return isLegalEquipmentType(player, type) && type.attack > 0 &&
		       (type.weaponType == WEAPON_SWORD || type.weaponType == WEAPON_CLUB ||
		                           type.weaponType == WEAPON_AXE) &&
		       (type.slotPosition & (SLOTP_LEFT | SLOTP_RIGHT)) != 0;
	};
	const uint16_t armorItemId = loadout.itemIds[CONST_SLOT_ARMOR];
	const bool armorReady = armorItemId != 0 && isLegalEquipmentType(player, Item::items[armorItemId]) &&
	                        (Item::items[armorItemId].slotPosition & SLOTP_ARMOR) != 0 && Item::items[armorItemId].armor > 0;
	const Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const bool suppliesReady = getInventoryItemCount(player, smallHealthPotionItemId) >= minimumSmallHealthPotions &&
	                           getInventoryItemCount(player, meatItemId) >= minimumMeat;
	const bool capacityReady = player.getFreeCapacity() >= returnCapacityThreshold + additionalWeight;
	return (isKnightWeapon(loadout.itemIds[CONST_SLOT_LEFT]) || isKnightWeapon(loadout.itemIds[CONST_SLOT_RIGHT])) && armorReady &&
	       backpack && backpack->getContainer() && suppliesReady && capacityReady;
}

PlayerBotController::EquipmentHuntSummary PlayerBotController::equipmentHuntSummary(Player& player,
	                                                                                   const PlayerBotCombatProfile& profile) const
{
	EquipmentHuntSummary summary;
	summary.lowestThreatRatio = std::numeric_limits<double>::max();
	std::set<Position> excludedRegions;
	const auto now = std::chrono::steady_clock::now();
	for (const auto& [center, cooldown] : huntRegionCooldowns) {
		if (cooldown > now) {
			excludedRegions.insert(center);
		}
	}
	const PlayerBotHuntRegionScan scan = huntRegionPlanner.beginScan(player);
	const uint32_t huntDurationSeconds = static_cast<uint32_t>(std::max<int32_t>(1,
		g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	for (size_t candidateIndex : scan.candidateIndices) {
		if (summary.evaluatedRegions >= maximumEquipmentHuntRegions) {
			summary.truncated = true;
			break;
		}
		PlayerBotHuntRegion region;
		if (!huntRegionPlanner.score(player, profile, scan.revision, candidateIndex, excludedRegions,
		                            huntRegionPerformance, huntDurationSeconds, region)) {
			continue;
		}
		++summary.evaluatedRegions;
		summary.lowestThreatRatio = std::min(summary.lowestThreatRatio, region.threatRatio);
		if (region.suitable) {
			++summary.suitableRegions;
			summary.bestProjectedExperience = std::max(summary.bestProjectedExperience, region.projectedExperience);
		}
	}
	if (summary.lowestThreatRatio == std::numeric_limits<double>::max()) {
		summary.lowestThreatRatio = 0;
	}
	return summary;
}

PlayerBotController::EquipmentOfferEvaluation PlayerBotController::evaluateEquipmentCandidate(
	Player& player, uint16_t itemId, const EquipmentLoadout& currentLoadout,
	const PlayerBotCombatProfile& currentProfile, const EquipmentHuntSummary& currentHunts,
	bool currentReady, uint32_t additionalWeight, bool allowSimulation) const
{
	EquipmentOfferEvaluation evaluation;
	evaluation.itemId = itemId;
	evaluation.currentReady = currentReady;
	EquipmentLoadout candidateLoadout = currentLoadout;
	if (!applyEquipmentOffer(player, candidateLoadout, itemId, evaluation.slot, evaluation.replacedItemId,
	                         evaluation.displacedLeftItemId, evaluation.displacedRightItemId,
	                         evaluation.rejection)) {
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
	evaluation.profile = equipmentCombatProfile(player, candidateLoadout);
	evaluation.hunts = equipmentHuntSummary(player, evaluation.profile);
	evaluation.candidateReady = equipmentLoadoutReady(player, candidateLoadout, additionalWeight);
	const int32_t currentMaximumDamage = Weapons::getMaxWeaponDamage(
		currentProfile.level, currentProfile.attackSkill, currentProfile.attack, currentProfile.attackFactor);
	const int32_t candidateMaximumDamage = Weapons::getMaxWeaponDamage(
		evaluation.profile.level, evaluation.profile.attackSkill, evaluation.profile.attack,
		evaluation.profile.attackFactor);
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
	if (currentReady && !evaluation.candidateReady) {
		evaluation.rejection = "regresses_readiness";
	} else if (!noWorse) {
		evaluation.rejection = better ? "ambiguous_tradeoff" : "non_improving";
	} else if (!better) {
		evaluation.rejection = "non_improving";
	} else {
		evaluation.rule = !currentReady && evaluation.candidateReady ? EquipmentDecisionRule::ReadinessRepair :
		                  evaluation.hunts.suitableRegions > currentHunts.suitableRegions ? EquipmentDecisionRule::UnlocksHunt :
		                  EquipmentDecisionRule::ParetoImprovement;
	}
	return evaluation;
}

const char* PlayerBotController::equipmentDecisionRuleName(EquipmentDecisionRule rule) const
{
	switch (rule) {
		case EquipmentDecisionRule::ParetoImprovement: return "pareto_improvement";
		case EquipmentDecisionRule::UnlocksHunt: return "unlocks_suitable_hunt";
		case EquipmentDecisionRule::ReadinessRepair: return "fills_readiness_gap";
		case EquipmentDecisionRule::None: return "none";
	}
	return "none";
}

void PlayerBotController::emitEquipmentOffer(const Player& player, const EquipmentOfferEvaluation& evaluation,
	                                            const PlayerBotCombatProfile& currentProfile,
	                                            const EquipmentHuntSummary& currentHunts, uint64_t reserve,
	                                            const Position& position, const char* result, const char* reason) const
{
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2)
	       << "\"result\":" << jsonString(result) << ",\"npc_id\":" << evaluation.npcId
	       << ",\"item_id\":" << evaluation.itemId << ",\"price\":" << evaluation.price
	       << ",\"replaced_item_id\":" << (evaluation.replacedItemId == 0 ? "null" : std::to_string(evaluation.replacedItemId))
	       << ",\"carried_money\":" << player.getMoney() << ",\"bank_balance\":" << player.getBankBalance()
	       << ",\"reserve\":" << reserve << ",\"travel_steps\":" << evaluation.travelSteps
	       << ",\"displaced_left_item_id\":" << (evaluation.displacedLeftItemId == 0 ? "null" : std::to_string(evaluation.displacedLeftItemId))
	       << ",\"displaced_right_item_id\":" << (evaluation.displacedRightItemId == 0 ? "null" : std::to_string(evaluation.displacedRightItemId))
	       << ",\"current\":{\"armor\":" << currentProfile.armor << ",\"defense\":" << currentProfile.defense
	       << ",\"attack\":" << currentProfile.attack << ",\"suitable_regions\":" << currentHunts.suitableRegions
	       << ",\"evaluated_regions\":" << currentHunts.evaluatedRegions
	       << ",\"hunt_evaluation_truncated\":" << (currentHunts.truncated ? "true" : "false")
	       << ",\"best_projected_experience\":" << currentHunts.bestProjectedExperience
	       << ",\"lowest_threat_ratio\":" << currentHunts.lowestThreatRatio
	       << ",\"combat_ready\":" << (evaluation.currentReady ? "true" : "false") << '}'
	       << ",\"candidate\":{\"armor\":" << evaluation.profile.armor << ",\"defense\":" << evaluation.profile.defense
	       << ",\"attack\":" << evaluation.profile.attack << ",\"suitable_regions\":" << evaluation.hunts.suitableRegions
	       << ",\"evaluated_regions\":" << evaluation.hunts.evaluatedRegions
	       << ",\"hunt_evaluation_truncated\":" << (evaluation.hunts.truncated ? "true" : "false")
	       << ",\"best_projected_experience\":" << evaluation.hunts.bestProjectedExperience
	       << ",\"lowest_threat_ratio\":" << evaluation.hunts.lowestThreatRatio
	       << ",\"combat_ready\":" << (evaluation.candidateReady ? "true" : "false") << '}'
	       << ",\"rule\":" << jsonString(equipmentDecisionRuleName(evaluation.rule))
	       << ",\"carried\":" << (evaluation.carried ? "true" : "false")
	       << ",\"provider_position\":{\"x\":" << evaluation.npcPosition.x << ",\"y\":" << evaluation.npcPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(evaluation.npcPosition.z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("equipment_offer_candidate", position, fields.str());
}

std::optional<PlayerBotController::EquipmentOfferEvaluation> PlayerBotController::evaluateEquipmentOffers(
	Player& player, const Position& position)
{
	if (!requiresKnightCombatReadiness(player)) {
		return std::nullopt;
	}
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const EquipmentLoadout currentLoadout = equipmentLoadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentCombatProfile(player, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	const bool currentReady = equipmentLoadoutReady(player, currentLoadout);
	std::map<uint16_t, EquipmentOfferEvaluation> evaluatedItems;
	std::map<uint32_t, std::optional<std::pair<Position, uint32_t>>> providerRoutes;
	std::set<uint32_t> providerRouteNodeLimits;
	std::optional<EquipmentOfferEvaluation> selected;
	uint32_t feasibleCandidates = 0;
	size_t simulatedItems = 0;
	bool providerRouteBudgetExhausted = false;
	size_t catalogOffers = 0;
	bool catalogTruncated = false;

	auto providerRoute = [&](Npc& npc) -> std::optional<std::pair<Position, uint32_t>> {
		if (auto route = providerRoutes.find(npc.getID()); route != providerRoutes.end()) {
			return route->second;
		}
		if (providerRoutes.size() >= maximumEquipmentProviderRoutes) {
			providerRouteBudgetExhausted = true;
			return std::nullopt;
		}
		std::vector<Position> approaches;
		for (int32_t x = -3; x <= 3; ++x) {
			for (int32_t y = -3; y <= 3; ++y) {
				if (x != 0 || y != 0) {
					approaches.emplace_back(npc.getPosition().x + x, npc.getPosition().y + y, npc.getPosition().z);
				}
			}
		}
		std::sort(approaches.begin(), approaches.end(), [&position](const Position& left, const Position& right) {
			const int32_t leftDistance = std::max(Position::getDistanceX(position, left), Position::getDistanceY(position, left));
			const int32_t rightDistance = std::max(Position::getDistanceX(position, right), Position::getDistanceY(position, right));
			return leftDistance == rightDistance ? left < right : leftDistance < rightDistance;
		});
		size_t evaluatedApproaches = 0;
		for (size_t approachIndex = 0; approachIndex < approaches.size(); ++approachIndex) {
			const Position& approach = approaches[approachIndex];
			Tile* tile = g_game.map.getTile(approach);
			if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
				continue;
			}
			if (evaluatedApproaches >= maximumEquipmentProviderApproaches) {
				break;
			}
			++evaluatedApproaches;
			std::deque<PlayerBotNavigationStep> steps;
			uint64_t expandedNodes = 0;
			++counters.pathfindingCalls;
			const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached :
			                                        navigator.plan(player, approach, {}, steps, expandedNodes,
			                                                       maximumEquipmentProviderPathNodes);
			if (result == PlayerBotNavigationResult::Reached) {
				return providerRoutes.emplace(npc.getID(), std::make_pair(approach, static_cast<uint32_t>(steps.size()))).first->second;
			}
			if (result == PlayerBotNavigationResult::NodeLimit) {
				providerRouteNodeLimits.insert(npc.getID());
			}
			++counters.pathfindingFailures;
		}
		return providerRoutes.emplace(npc.getID(), std::nullopt).first->second;
	};
	auto preferCandidate = [](const EquipmentOfferEvaluation& candidate, const EquipmentOfferEvaluation& current) {
		return candidate.rule > current.rule ||
		       (candidate.rule == current.rule && (candidate.carried != current.carried ? candidate.carried :
		        candidate.price < current.price ||
		        (candidate.price == current.price && (candidate.travelSteps < current.travelSteps ||
		         (candidate.travelSteps == current.travelSteps && (candidate.itemId < current.itemId ||
		          (candidate.itemId == current.itemId && candidate.npcId < current.npcId)))))));
	};

	for (const auto& entry : g_game.getNpcs()) {
		if (catalogTruncated) {
			break;
		}
		Npc* npc = entry.second;
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!npc || !capability || *capability != "shop" ||
		    serviceDistance(player.getTemplePosition(), {npc->getID(), npc->getPosition()}) > maximumEquipmentProviderDistance) {
			continue;
		}
		for (const ShopInfo& offer : npc->getShopOffers()) {
			if (offer.buyPrice == 0) {
				continue;
			}
			if (catalogOffers >= maximumEquipmentCatalogOffers) {
				catalogTruncated = true;
				break;
			}
			++catalogOffers;
			EquipmentOfferEvaluation evaluation;
			const bool carried = g_game.findItemOfType(&player, offer.itemId, true) != nullptr;
			if (auto item = evaluatedItems.find(offer.itemId); item != evaluatedItems.end()) {
				evaluation = item->second;
			} else {
				evaluation = evaluateEquipmentCandidate(player, offer.itemId, currentLoadout, currentProfile,
				                                        currentHunts, currentReady,
				                                        carried ? 0 : Item::items[offer.itemId].weight,
				                                        simulatedItems < maximumEquipmentCandidateSimulations);
				if (evaluation.simulated) {
					++simulatedItems;
				}
				evaluatedItems.emplace(offer.itemId, evaluation);
			}
			evaluation.npcId = npc->getID();
			evaluation.npcPosition = npc->getPosition();
			evaluation.price = offer.buyPrice;
			evaluation.carried = carried;
			if (!evaluation.rejection.empty()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   evaluation.rejection.c_str());
				continue;
			}
			if (!evaluation.carried && Item::items[offer.itemId].weight > player.getFreeCapacity()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   "insufficient_capacity");
				continue;
			}
			Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
			Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
			const uint32_t freeBackpackSlots = backpack ? backpack->capacity() -
			                                  std::min<uint32_t>(backpack->capacity(), backpack->size()) : 0;
			uint32_t displacedSlots = 0;
			std::set<slots_t> countedSlots;
			for (const auto& displaced : {std::pair<slots_t, uint16_t>{evaluation.slot, evaluation.replacedItemId},
			                              {CONST_SLOT_LEFT, evaluation.displacedLeftItemId},
			                              {CONST_SLOT_RIGHT, evaluation.displacedRightItemId}}) {
				if (displaced.second != 0 && countedSlots.insert(displaced.first).second) {
					++displacedSlots;
				}
			}
			const uint32_t requiredBackpackSlots = displacedSlots + (evaluation.carried ? 0 : 1);
			if (!backpack || freeBackpackSlots < requiredBackpackSlots) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   "insufficient_displaced_item_space");
				continue;
			}
			if (evaluation.carried) {
				evaluation.travelSteps = 0;
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "feasible", "carried_upgrade");
				++feasibleCandidates;
				if (!selected || preferCandidate(evaluation, *selected)) {
					selected = evaluation;
				}
				continue;
			}
			if (reserve == std::numeric_limits<uint64_t>::max()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "recovery_reserve_unavailable");
				continue;
			}
			if (totalMoney < reserve + evaluation.price) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "unaffordable_after_reserves");
				continue;
			}
			const std::optional<std::pair<Position, uint32_t>> route = providerRoute(*npc);
			if (!route) {
				const char* reason = providerRouteBudgetExhausted ? "provider_evaluation_budget_exhausted" :
				                     providerRouteNodeLimits.find(npc->getID()) != providerRouteNodeLimits.end() ?
				                     "provider_route_node_budget_exhausted" : "provider_unreachable";
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", reason);
				continue;
			}
			evaluation.approachPosition = route->first;
			evaluation.travelSteps = route->second;
			emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "feasible", nullptr);
			++feasibleCandidates;
			if (!selected || preferCandidate(evaluation, *selected)) {
				selected = evaluation;
			}
		}
	}
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(selected ? (selected->carried ? "would_equip" : "would_buy") : "no_decision")
	       << ",\"feasible_candidates\":" << feasibleCandidates
	       << ",\"catalog_offers_evaluated\":" << catalogOffers
	       << ",\"catalog_truncated\":" << (catalogTruncated ? "true" : "false")
	       << ",\"reason\":" << jsonString(selected ? equipmentDecisionRuleName(selected->rule) : "no_justified_offer");
	if (selected) {
		fields << ",\"npc_id\":" << selected->npcId << ",\"item_id\":" << selected->itemId
		       << ",\"price\":" << selected->price << ",\"travel_steps\":" << selected->travelSteps;
	}
	emit("equipment_offer_shadow", position, fields.str());
	return selected;
}

void PlayerBotController::beginEquipmentPurchase(Player& player, const Position& position,
	EquipmentOfferEvaluation evaluation)
{
	equipmentPurchase = std::move(evaluation);
	progressionObjective = ProgressionObjective::BuyEquipment;
	equipmentPurchaseStage = equipmentPurchase.carried ? EquipmentPurchaseStage::Equip : EquipmentPurchaseStage::Travel;
	progressionAttempts = 0;
	pendingEquipmentDisplacedCounts.clear();
	if (!equipmentPurchase.carried) {
		resetConversation(equipmentPurchase.npcId);
	}
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"reason\":"
	       << jsonString(equipmentDecisionRuleName(equipmentPurchase.rule))
	       << ",\"npc_id\":" << equipmentPurchase.npcId << ",\"item_id\":" << equipmentPurchase.itemId
	       << ",\"price\":" << equipmentPurchase.price << ",\"travel_steps\":" << equipmentPurchase.travelSteps
	       << ",\"acquisition\":" << jsonString(equipmentPurchase.carried ? "carried" : "purchase");
	emit("strategy_selection", position, fields.str());
	say(player, equipmentPurchase.carried ? "Equipping a carried equipment upgrade." :
	                                      "Going to buy a justified equipment upgrade.");
}

void PlayerBotController::finishEquipmentPurchase(Player* player, const Position& position, const char* result,
	const char* reason)
{
	std::ostringstream fields;
	fields << "\"goal\":\"buy_equipment\",\"npc_id\":" << equipmentPurchase.npcId
	       << ",\"item_id\":" << equipmentPurchase.itemId << ",\"price\":" << equipmentPurchase.price
	       << ",\"rule\":" << jsonString(equipmentDecisionRuleName(equipmentPurchase.rule))
	       << ",\"result\":" << jsonString(result) << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) +
	         ",\"goal\":\"buy_equipment\",\"result\":" + jsonString(result) +
	         ",\"reason\":" + jsonString(reason));
	if (player) {
		player->closeShopWindow();
		say(*player, std::string("Equipment purchase ") + result + ": " + reason + '.');
	}
	const bool succeeded = std::strcmp(result, "success") == 0;
	equipmentPurchaseCooldownUntil = std::chrono::steady_clock::now() +
	                                 (succeeded ? equipmentPurchaseSuccessCooldown : equipmentPurchaseFailureCooldown);
	progressionObjective = ProgressionObjective::None;
	equipmentPurchaseStage = EquipmentPurchaseStage::Travel;
	equipmentPurchase = EquipmentOfferEvaluation{};
	progressionAttempts = 0;
	pendingEquipmentDisplacedCounts.clear();
	serviceTargetId = 0;
	conversationStep = ConversationStep::Greet;
	clearNavigation();
	cyclePhase = CyclePhase::Service;
	if (testPolicy.progressionEnabled && player) {
		selectTopLevelGoal(*player, position, succeeded ? "equipment_purchase_complete" : "equipment_purchase_failed");
	} else {
		activeGoal = TopLevelGoal::Service;
	}
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::processEquipmentPurchase(Player* player, const Position& position)
{
	Npc* npc = nullptr;
	const ShopInfo* offer = nullptr;
	ServiceNpc provider;
	if (!equipmentPurchase.carried) {
		npc = g_game.getNpcByID(equipmentPurchase.npcId);
		const std::string* capability = npc && !npc->isRemoved() ? npc->getParameter("playerbot_service") : nullptr;
		if (!npc || !capability || *capability != "shop") {
			finishEquipmentPurchase(player, position, "failed", "provider_unavailable");
			return;
		}
		provider = {equipmentPurchase.npcId, npc->getPosition()};
		offer = findOffer(provider, equipmentPurchase.itemId, true);
		if (!offer || offer->buyPrice != equipmentPurchase.price) {
			finishEquipmentPurchase(player, position, "failed", "offer_changed");
			return;
		}
	}

	if (equipmentPurchaseStage == EquipmentPurchaseStage::Travel) {
		if (!processNavigation(player, position, equipmentPurchase.approachPosition)) {
			if (fixedTargetRouteFailureCount >= maximumProgressionAttempts ||
			    blockedStepCount >= maximumRepeatedNavigationStepFailures) {
				finishEquipmentPurchase(player, position, "failed", "route_unavailable");
			}
			return;
		}
		equipmentPurchaseStage = EquipmentPurchaseStage::Purchase;
		clearNavigation();
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (equipmentPurchaseStage == EquipmentPurchaseStage::Purchase) {
		if (!Position::areInRange<3, 3, 0>(position, npc->getPosition())) {
			finishEquipmentPurchase(player, position, "failed", "provider_moved");
			return;
		}
		if (!openServiceShop(player, provider, position)) {
			if (serviceAttempts >= maximumServiceAttempts) {
				finishEquipmentPurchase(player, position, "failed", "shop_window_unavailable");
			}
			return;
		}
		const uint64_t reserve = spellTrainingReserve(*player);
		const uint64_t totalMoney = player->getMoney() + player->getBankBalance();
		if (reserve == std::numeric_limits<uint64_t>::max() || totalMoney < equipmentPurchase.price ||
		    totalMoney - equipmentPurchase.price < reserve) {
			finishEquipmentPurchase(player, position, "failed", "reserve_changed");
			return;
		}
		serviceBeforeItemCount = getInventoryItemCount(*player, equipmentPurchase.itemId);
		serviceBeforeMoney = player->getMoney();
		serviceBeforeBalance = player->getBankBalance();
		equipmentPurchaseStage = EquipmentPurchaseStage::VerifyPurchase;
		++counters.actionsAttempted;
		if (!testPolicy.forceEquipmentPurchaseRejected) {
			g_game.playerPurchaseItem(playerId, Item::items[equipmentPurchase.itemId].clientId,
			                          static_cast<uint8_t>(offer->subType), 1, false, false);
		}
		schedule(navigationDecisionDelay(*player));
		return;
	}

	if (equipmentPurchaseStage == EquipmentPurchaseStage::VerifyPurchase) {
		const uint32_t currentCount = getInventoryItemCount(*player, equipmentPurchase.itemId);
		const uint64_t expectedMoney = serviceBeforeMoney > equipmentPurchase.price ?
		                               serviceBeforeMoney - equipmentPurchase.price : 0;
		const uint64_t expectedBalance = equipmentPurchase.price > serviceBeforeMoney ?
		                                 serviceBeforeBalance - (equipmentPurchase.price - serviceBeforeMoney) :
		                                 serviceBeforeBalance;
		const bool itemChanged = currentCount == serviceBeforeItemCount + 1;
		const bool economyChanged = player->getMoney() == expectedMoney && player->getBankBalance() == expectedBalance;
		if (itemChanged && economyChanged) {
			emit("action_result", position,
			     "\"action\":\"buy_equipment\",\"result\":\"success\",\"item_id\":" +
			         std::to_string(equipmentPurchase.itemId) + ",\"price\":" + std::to_string(equipmentPurchase.price) +
			         ",\"carried_before\":" + std::to_string(serviceBeforeMoney) +
			         ",\"carried_after\":" + std::to_string(player->getMoney()) +
			         ",\"bank_before\":" + std::to_string(serviceBeforeBalance) +
			         ",\"bank_after\":" + std::to_string(player->getBankBalance()));
			progressionAttempts = 0;
			equipmentPurchaseStage = EquipmentPurchaseStage::Equip;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (currentCount != serviceBeforeItemCount || player->getMoney() != serviceBeforeMoney ||
		    player->getBankBalance() != serviceBeforeBalance) {
			logActionFailure("buy_equipment", "transaction_delta_mismatch", position);
			stop("equipment_purchase_delta_mismatch", position);
			return;
		}
		if (++progressionAttempts >= maximumProgressionAttempts) {
			logActionFailure("buy_equipment", "transaction_rejected", position);
			finishEquipmentPurchase(player, position, "failed", "transaction_rejected");
			return;
		}
		equipmentPurchaseStage = EquipmentPurchaseStage::Purchase;
		conversationStep = ConversationStep::Ready;
		schedule(navigationDecisionDelay(*player));
		return;
	}

	if (equipmentPurchaseStage == EquipmentPurchaseStage::Equip) {
		Item* equipped = player->getInventoryItem(equipmentPurchase.slot);
		if (equipped && equipped->getID() == equipmentPurchase.itemId) {
			equipmentPurchaseStage = EquipmentPurchaseStage::VerifyEquipment;
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		Item* purchased = g_game.findItemOfType(player, equipmentPurchase.itemId, true);
		if (!purchased) {
			if (++progressionAttempts >= maximumProgressionAttempts) {
				finishEquipmentPurchase(player, position, "failed", "purchased_item_unavailable");
			} else {
				schedule(navigationDecisionDelay(*player));
			}
			return;
		}
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Item* displaced = nullptr;
		slots_t displacedSlot = CONST_SLOT_WHEREEVER;
		for (const auto& entry : {std::pair<slots_t, uint16_t>{equipmentPurchase.slot, equipmentPurchase.replacedItemId},
		                         {CONST_SLOT_LEFT, equipmentPurchase.displacedLeftItemId},
		                         {CONST_SLOT_RIGHT, equipmentPurchase.displacedRightItemId}}) {
			Item* equippedItem = entry.second == 0 ? nullptr : player->getInventoryItem(entry.first);
			if (equippedItem && equippedItem->getID() == entry.second && equippedItem != purchased) {
				displaced = equippedItem;
				displacedSlot = entry.first;
				break;
			}
		}
		if (displaced) {
			if (progressionAttempts >= maximumProgressionAttempts) {
				finishEquipmentPurchase(player, position, "failed", "displaced_item_move_not_verified");
				return;
			}
			Position displacedPosition;
			uint8_t displacedIndex = 0;
			g_game.internalGetPosition(displaced, displacedPosition, displacedIndex);
			++progressionAttempts;
			++counters.actionsAttempted;
			emit("action_result", position,
			     "\"action\":\"preserve_displaced_equipment\",\"result\":\"requested\",\"item_id\":" +
			         std::to_string(displaced->getID()) + ",\"slot\":" + std::to_string(displacedSlot));
			g_game.playerMoveItem(player, displacedPosition, displaced->getClientID(), displacedIndex,
			                      Position(0xFFFF, 0, 0), displaced->getItemCount(), displaced, nullptr);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Container* sourceContainer = dynamic_cast<Container*>(purchased->getParent());
		if (sourceContainer && player->getContainerID(sourceContainer) < 0) {
			if (progressionAttempts >= maximumProgressionAttempts) {
				finishEquipmentPurchase(player, position, "failed", "purchased_item_container_unavailable");
				return;
			}
			Container* containerToOpen = sourceContainer;
			while (Container* parent = dynamic_cast<Container*>(containerToOpen->getParent())) {
				if (player->getContainerID(parent) >= 0) {
					break;
				}
				containerToOpen = parent;
			}
			uint8_t containerId = rewardContainerIdBase;
			while (containerId <= maximumContainerId && player->getContainerByID(containerId)) {
				++containerId;
			}
			Position containerPosition;
			uint8_t containerIndex = 0;
			Item* containerItem = static_cast<Item*>(containerToOpen);
			g_game.internalGetPosition(containerItem, containerPosition, containerIndex);
			if (containerId > maximumContainerId || containerPosition.x != 0xFFFF) {
				finishEquipmentPurchase(player, position, "failed", "purchased_item_container_unavailable");
				return;
			}
			++progressionAttempts;
			++counters.actionsAttempted;
			g_game.playerUseItem(playerId, containerPosition, containerIndex, containerId, containerItem->getClientID());
			emit("action_result", position,
			     "\"action\":\"open_equipment_container\",\"result\":\"requested\",\"item_id\":" +
			         std::to_string(containerItem->getID()) + ",\"container_id\":" + std::to_string(containerId));
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Position sourcePosition;
		uint8_t sourceIndex = 0;
		g_game.internalGetPosition(purchased, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF) {
			finishEquipmentPurchase(player, position, "failed", "purchased_item_position_unavailable");
			return;
		}
		pendingEquipmentDisplacedCounts.clear();
		for (uint16_t itemId : {equipmentPurchase.replacedItemId, equipmentPurchase.displacedLeftItemId,
		                        equipmentPurchase.displacedRightItemId}) {
			if (itemId != 0) {
				pendingEquipmentDisplacedCounts[itemId] = getInventoryItemCount(*player, itemId);
			}
		}
		++counters.actionsAttempted;
		emit("action_result", position,
		     "\"action\":\"equip_equipment\",\"result\":\"requested\",\"item_id\":" +
		         std::to_string(purchased->getID()) + ",\"slot\":" + std::to_string(equipmentPurchase.slot) +
		         ",\"source_y\":" + std::to_string(sourcePosition.y) +
		         ",\"source_z\":" + std::to_string(sourcePosition.z) +
		         ",\"source_container\":" + (sourceContainer ? "true" : "false"));
		g_game.playerMoveItem(player, sourcePosition, purchased->getClientID(), sourceIndex,
		                      Position(0xFFFF, equipmentPurchase.slot, 0), purchased->getItemCount(), purchased, nullptr);
		equipmentPurchaseStage = EquipmentPurchaseStage::VerifyEquipment;
		schedule(navigationDecisionDelay(*player));
		return;
	}

	Item* equipped = player->getInventoryItem(equipmentPurchase.slot);
	const bool displacedPreserved = std::all_of(pendingEquipmentDisplacedCounts.begin(),
	                                           pendingEquipmentDisplacedCounts.end(),
	                                           [this, player](const auto& entry) {
		                                           return getInventoryItemCount(*player, entry.first) >= entry.second;
	                                           });
	if (!equipped || equipped->getID() != equipmentPurchase.itemId || !displacedPreserved) {
		if (++progressionAttempts >= maximumProgressionAttempts) {
			finishEquipmentPurchase(player, position, "failed",
			                        displacedPreserved ? "equip_not_verified" : "displaced_item_lost");
			return;
		}
		equipmentPurchaseStage = EquipmentPurchaseStage::Equip;
		schedule(navigationDecisionDelay(*player));
		return;
	}
	const EquipmentLoadout actualLoadout = equipmentLoadout(*player);
	const PlayerBotCombatProfile actualProfile = equipmentCombatProfile(*player, actualLoadout);
	const EquipmentHuntSummary actualHunts = equipmentHuntSummary(*player, actualProfile);
	emit("action_result", position,
	     "\"action\":\"equip_equipment\",\"result\":\"success\",\"item_id\":" +
	         std::to_string(equipmentPurchase.itemId) + ",\"slot\":" + std::to_string(equipmentPurchase.slot) +
	         ",\"combat_ready\":" + (equipmentLoadoutReady(*player, actualLoadout) ? "true" : "false") +
	         ",\"suitable_regions\":" + std::to_string(actualHunts.suitableRegions) +
	         ",\"displaced_items_preserved\":true");
	finishEquipmentPurchase(player, position, "success", "upgrade_equipped");
}
