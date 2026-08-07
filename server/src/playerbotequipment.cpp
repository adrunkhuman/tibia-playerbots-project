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
	constexpr size_t maximumEquipmentUniqueItems = 16;

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

bool PlayerBotController::applyEquipmentOffer(const Player& player, EquipmentLoadout& loadout, uint16_t itemId,
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

	slots_t slot = CONST_SLOT_WHEREEVER;
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
	       << ",\"provider_position\":{\"x\":" << evaluation.npcPosition.x << ",\"y\":" << evaluation.npcPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(evaluation.npcPosition.z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("equipment_offer_candidate", position, fields.str());
}

void PlayerBotController::evaluateEquipmentOffers(Player& player, const Position& position)
{
	if (!requiresKnightCombatReadiness(player)) {
		return;
	}
	const uint64_t reserve = spellTrainingReserve(player);
	const uint64_t totalMoney = player.getMoney() + player.getBankBalance();
	const EquipmentLoadout currentLoadout = equipmentLoadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentCombatProfile(player, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	const bool currentReady = equipmentLoadoutReady(player, currentLoadout);
	std::map<uint16_t, EquipmentOfferEvaluation> evaluatedItems;
	std::map<uint32_t, std::optional<uint32_t>> providerRoutes;
	std::set<uint32_t> providerRouteNodeLimits;
	std::optional<EquipmentOfferEvaluation> selected;
	uint32_t feasibleCandidates = 0;
	bool providerRouteBudgetExhausted = false;
	size_t catalogOffers = 0;
	bool catalogTruncated = false;

	auto providerRoute = [&](Npc& npc) -> std::optional<uint32_t> {
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
		for (size_t approachIndex = 0; approachIndex < approaches.size() && approachIndex < maximumEquipmentProviderApproaches;
		     ++approachIndex) {
			const Position& approach = approaches[approachIndex];
			Tile* tile = g_game.map.getTile(approach);
			if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
				continue;
			}
			std::deque<PlayerBotNavigationStep> steps;
			uint64_t expandedNodes = 0;
			++counters.pathfindingCalls;
			const PlayerBotNavigationResult result = approach == position ? PlayerBotNavigationResult::Reached :
			                                        navigator.plan(player, approach, {}, steps, expandedNodes,
			                                                       maximumEquipmentProviderPathNodes);
			if (result == PlayerBotNavigationResult::Reached) {
				return providerRoutes.emplace(npc.getID(), static_cast<uint32_t>(steps.size())).first->second;
			}
			if (result == PlayerBotNavigationResult::NodeLimit) {
				providerRouteNodeLimits.insert(npc.getID());
			}
			++counters.pathfindingFailures;
		}
		return providerRoutes.emplace(npc.getID(), std::nullopt).first->second;
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
			evaluation.npcId = npc->getID();
			evaluation.npcPosition = npc->getPosition();
			evaluation.itemId = offer.itemId;
			evaluation.price = offer.buyPrice;
			evaluation.currentReady = currentReady;
			if (auto item = evaluatedItems.find(offer.itemId); item != evaluatedItems.end()) {
				evaluation = item->second;
				evaluation.npcId = npc->getID();
				evaluation.npcPosition = npc->getPosition();
				evaluation.price = offer.buyPrice;
			} else {
				if (evaluatedItems.size() >= maximumEquipmentUniqueItems) {
					evaluation.profile = currentProfile;
					evaluation.hunts = currentHunts;
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
					                   "unique_item_evaluation_budget_exhausted");
					continue;
				}
				EquipmentLoadout candidateLoadout = currentLoadout;
				std::string rejection;
				if (!applyEquipmentOffer(player, candidateLoadout, offer.itemId, evaluation.replacedItemId,
				                         evaluation.displacedLeftItemId,
				                         evaluation.displacedRightItemId, rejection)) {
					evaluation.profile = currentProfile;
					evaluation.hunts = currentHunts;
					evaluation.rejection = rejection;
					evaluatedItems.emplace(offer.itemId, evaluation);
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", rejection.c_str());
					continue;
				}
				if (Item::items[offer.itemId].weight > player.getFreeCapacity()) {
					evaluation.profile = currentProfile;
					evaluation.hunts = currentHunts;
					evaluation.rejection = "insufficient_capacity";
					evaluatedItems.emplace(offer.itemId, evaluation);
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "insufficient_capacity");
					continue;
				}
				evaluation.profile = equipmentCombatProfile(player, candidateLoadout);
				evaluation.hunts = equipmentHuntSummary(player, evaluation.profile);
				evaluation.candidateReady = equipmentLoadoutReady(player, candidateLoadout, Item::items[offer.itemId].weight);
				const int32_t currentMaximumDamage = Weapons::getMaxWeaponDamage(currentProfile.level, currentProfile.attackSkill,
				                                                                   currentProfile.attack, currentProfile.attackFactor);
				const int32_t candidateMaximumDamage = Weapons::getMaxWeaponDamage(evaluation.profile.level,
				                                                                     evaluation.profile.attackSkill,
				                                                                     evaluation.profile.attack,
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
					evaluatedItems.emplace(offer.itemId, evaluation);
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "regresses_readiness");
					continue;
				}
				if (!noWorse) {
					evaluation.rejection = better ? "ambiguous_tradeoff" : "non_improving";
					evaluatedItems.emplace(offer.itemId, evaluation);
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
					                   better ? "ambiguous_tradeoff" : "non_improving");
					continue;
				}
				if (!better) {
					evaluation.rejection = "non_improving";
					evaluatedItems.emplace(offer.itemId, evaluation);
					emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", "non_improving");
					continue;
				}
				evaluation.rule = !currentReady && evaluation.candidateReady ? EquipmentDecisionRule::ReadinessRepair :
				                  evaluation.hunts.suitableRegions > currentHunts.suitableRegions ? EquipmentDecisionRule::UnlocksHunt :
				                  EquipmentDecisionRule::ParetoImprovement;
				evaluatedItems.emplace(offer.itemId, evaluation);
			}
			if (!evaluation.rejection.empty()) {
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected",
				                   evaluation.rejection.c_str());
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
			const std::optional<uint32_t> travelSteps = providerRoute(*npc);
			if (!travelSteps) {
				const char* reason = providerRouteBudgetExhausted ? "provider_evaluation_budget_exhausted" :
				                     providerRouteNodeLimits.find(npc->getID()) != providerRouteNodeLimits.end() ?
				                     "provider_route_node_budget_exhausted" : "provider_unreachable";
				emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "rejected", reason);
				continue;
			}
			evaluation.travelSteps = *travelSteps;
			emitEquipmentOffer(player, evaluation, currentProfile, currentHunts, reserve, position, "feasible", nullptr);
			++feasibleCandidates;
			if (!selected || evaluation.rule > selected->rule ||
			    (evaluation.rule == selected->rule && (evaluation.price < selected->price ||
			     (evaluation.price == selected->price && (evaluation.travelSteps < selected->travelSteps ||
			      (evaluation.travelSteps == selected->travelSteps && (evaluation.itemId < selected->itemId ||
			       (evaluation.itemId == selected->itemId && evaluation.npcId < selected->npcId)))))))) {
				selected = evaluation;
			}
		}
	}
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(selected ? "would_buy" : "no_decision")
	       << ",\"feasible_candidates\":" << feasibleCandidates
	       << ",\"catalog_offers_evaluated\":" << catalogOffers
	       << ",\"catalog_truncated\":" << (catalogTruncated ? "true" : "false")
	       << ",\"reason\":" << jsonString(selected ? equipmentDecisionRuleName(selected->rule) : "no_justified_offer");
	if (selected) {
		fields << ",\"npc_id\":" << selected->npcId << ",\"item_id\":" << selected->itemId
		       << ",\"price\":" << selected->price << ",\"travel_steps\":" << selected->travelSteps;
	}
	emit("equipment_offer_shadow", position, fields.str());
}
