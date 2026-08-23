/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotcontroller.h"
#include "playerbotarea.h"

// Goal arbitration and reward discovery, claiming, and equipment.
using namespace playerbot;

void PlayerBotController::emitCombatReadiness(const Player& player, const Position& position, const char* result,
                                              const std::string& recovery, const std::string& terminalReason) const
{
	Item* left = player.getInventoryItem(CONST_SLOT_LEFT);
	Item* right = player.getInventoryItem(CONST_SLOT_RIGHT);
	Item* armor = player.getInventoryItem(CONST_SLOT_ARMOR);
	Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const PlayerBotFoodInventory food = inventoryPolicy.foodInventory(player);
	const uint32_t usableCapacity = inventoryPolicy.effectiveFreeCapacity(player);
	const bool weaponReady = (left && equipmentPolicy.isKnightMeleeWeapon(player, *left)) ||
	                         (right && equipmentPolicy.isKnightMeleeWeapon(player, *right));
	const bool armorReady = armor && equipmentPolicy.isLegalEquipmentItem(player, *armor) && armor->getArmor() > 0;
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(result)
	       << ",\"vocation_id\":" << player.getVocationId()
	       << ",\"requirements\":[{\"name\":\"legal_melee_weapon\",\"ready\":" << (weaponReady ? "true" : "false")
	       << ",\"left_item_id\":" << (left ? std::to_string(left->getID()) : "null")
	       << ",\"right_item_id\":" << (right ? std::to_string(right->getID()) : "null") << '}'
	       << ",{\"name\":\"armor_loadout\",\"ready\":" << (armorReady && backpack && backpack->getContainer() ? "true" : "false")
	       << ",\"armor_item_id\":" << (armor ? std::to_string(armor->getID()) : "null")
	       << ",\"armor\":" << (armor ? armor->getArmor() : 0) << '}'
	       << ",{\"name\":\"small_health_potions\",\"ready\":" <<
	          (inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold ? "true" : "false")
	       << ",\"count\":" << inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId)
	       << ",\"return_threshold\":" << smallHealthPotionReturnThreshold
	       << ",\"restock_target\":" << smallHealthPotionRestockTarget << '}'
	       << ",{\"name\":\"food\",\"required\":false,\"ready\":true"
	       << ",\"count\":" << food.count << ",\"preferred\":" << preferredFoodCount
	       << ",\"reclaimable_weight\":" << food.weight
	       << ",\"preference_utility\":" <<
	          std::max<int32_t>(0, static_cast<int32_t>(preferredFoodCount -
	              std::min<uint32_t>(preferredFoodCount, food.count))) * foodPreferenceUtility << '}'
	       << ",{\"name\":\"free_capacity\",\"ready\":" << (usableCapacity >= returnCapacityThreshold ? "true" : "false")
	       << ",\"current\":" << player.getFreeCapacity() << ",\"reclaimable_food\":" << food.weight
	       << ",\"effective\":" << usableCapacity << ",\"minimum\":" << returnCapacityThreshold << "}]"
	       << ",\"selected_recovery\":" << (recovery.empty() ? "null" : jsonString(recovery))
	       << ",\"terminal_reason\":" << (terminalReason.empty() ? "null" : jsonString(terminalReason));
	emit("combat_readiness", position, fields.str());
}

PlayerBotEquipmentReadinessInput PlayerBotController::equipmentReadinessInput(const Player& player) const
{
	const Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	return {backpack && backpack->getContainer(),
	        inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold,
	        inventoryPolicy.effectiveFreeCapacity(player), returnCapacityThreshold};
}

bool PlayerBotController::beginReadinessEquipment(Player* player, const Position& position, const char* reason)
{
	Item* item = nullptr;
	EquipmentUpgrade upgrade{};
	if (!player || !equipmentPolicy.findCarriedUpgrade(*player, item, upgrade) || !player->canDoAction()) {
		return false;
	}
	Position source;
	uint8_t sourceIndex = 0;
	g_game.internalGetPosition(item, source, sourceIndex);
	Container* sourceContainer = dynamic_cast<Container*>(item->getParent());
	if (sourceContainer && player->getContainerID(sourceContainer) < 0) {
		Container* containerToOpen = sourceContainer;
		while (Container* parent = dynamic_cast<Container*>(containerToOpen->getParent())) {
			if (player->getContainerID(parent) >= 0) {
				break;
			}
			containerToOpen = parent;
		}
		Position containerPosition;
		uint8_t containerIndex = 0;
		Item* containerItem = static_cast<Item*>(containerToOpen);
		g_game.internalGetPosition(containerItem, containerPosition, containerIndex);
		uint8_t containerId = rewardContainerIdBase;
		while (containerId <= maximumContainerId && player->getContainerByID(containerId)) {
			++containerId;
		}
		++pendingReadinessAttempts;
		if (containerId > maximumContainerId || containerPosition.x != 0xFFFF ||
		    pendingReadinessAttempts > maximumProgressionAttempts) {
			return false;
		}
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, containerPosition, containerIndex, containerId, containerItem->getClientID());
		emit("action_result", position, "\"action\":\"open_readiness_container\",\"result\":\"requested\",\"item_id\":" +
		     std::to_string(containerItem->getID()) + ",\"container_id\":" + std::to_string(containerId));
		schedule(navigationDecisionDelay(*player));
		return true;
	}
	pendingReadinessItemId = item->getID();
	pendingReadinessSlot = upgrade.slot;
	readinessEquipmentPending = true;
	pendingReadinessAttempts = 0;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, source, item->getClientID(), sourceIndex,
	                      Position(0xFFFF, upgrade.slot, 0), item->getItemCount(), item, nullptr);
	emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"requested\",\"item_id\":" +
	     std::to_string(item->getID()) + ",\"slot\":" + std::to_string(upgrade.slot) +
	     ",\"reason\":" + jsonString(reason));
	schedule(navigationDecisionDelay(*player));
	return true;
}

void PlayerBotController::processReadinessEquipment(Player* player, const Position& position)
{
	Item* equipped = pendingReadinessSlot == CONST_SLOT_WHEREEVER ? nullptr : player->getInventoryItem(pendingReadinessSlot);
	if (equipped && equipped->getID() == pendingReadinessItemId) {
		Item* carriedUpgrade = nullptr;
		EquipmentUpgrade upgrade{};
		const bool hasCarriedUpgrade = equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade);
		const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
			*player, hasCarriedUpgrade, equipmentReadinessInput(*player));
		const bool ready = readiness.ready;
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"success\",\"item_id\":" +
		     std::to_string(pendingReadinessItemId) + ",\"slot\":" + std::to_string(pendingReadinessSlot));
		emitCombatReadiness(*player, position, ready ? "ready" : "recovery", readiness.recovery, readiness.terminalReason);
		readinessEquipmentPending = false;
		pendingReadinessAttempts = 0;
		if (readinessResumeService) {
			readinessResumeService = false;
			depotSession.setStage(PlayerBotDepotStage::Deposit);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (ready) {
			if (activeHuntRegion) {
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			startHunt(player, position, "readiness_carried_upgrade");
			return;
		}
		ensureCombatReady(player, position, "readiness_upgrade_incomplete");
		return;
	}
	if (++pendingReadinessAttempts >= maximumProgressionAttempts) {
		Item* carriedUpgrade = nullptr;
		EquipmentUpgrade upgrade{};
		const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
			*player, equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade), equipmentReadinessInput(*player));
		readinessEquipmentPending = false;
		readinessResumeService = false;
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"failed\",\"reason\":\"move_not_verified\"");
		emitCombatReadiness(*player, position, "failed", readiness.recovery, readiness.terminalReason);
		if (!readiness.terminalReason.empty()) {
			stop(("combat_readiness_" + readiness.terminalReason).c_str(), position);
		} else {
			beginService(player, position, "readiness_equipment_move_failed");
			schedule(navigationInterval);
		}
		return;
	}
	readinessEquipmentPending = false;
	if (!beginReadinessEquipment(player, position, "readiness_retry")) {
		if (pendingReadinessAttempts >= maximumProgressionAttempts) {
			emit("action_result", position,
			     "\"action\":\"open_readiness_container\",\"result\":\"failed\",\"reason\":\"access_attempts_exhausted\"");
			pendingReadinessAttempts = 0;
			beginService(player, position, "readiness_container_access_failed");
			schedule(navigationInterval);
		} else {
			schedule(navigationDecisionDelay(*player));
		}
	}
}

bool PlayerBotController::ensureCombatReady(Player* player, const Position& position, const char* reason)
{
	if (!player || !equipmentPolicy.requiresKnightCombatReadiness(*player)) {
		return true;
	}
	Item* carriedUpgrade = nullptr;
	EquipmentUpgrade upgrade{};
	const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
		*player, equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade), equipmentReadinessInput(*player));
	if (readiness.ready) {
		if (std::strcmp(reason, "readiness_continuous_check") != 0) {
			emitCombatReadiness(*player, position, "ready", {}, {});
		}
		return true;
	}
	emitCombatReadiness(*player, position, "recovery", readiness.recovery, readiness.terminalReason);
	if (readiness.recovery == "equip_carried") {
		if (!beginReadinessEquipment(player, position, reason)) {
			if (pendingReadinessAttempts >= maximumProgressionAttempts) {
				emit("action_result", position,
				     "\"action\":\"open_readiness_container\",\"result\":\"failed\",\"reason\":\"access_attempts_exhausted\"");
				pendingReadinessAttempts = 0;
				beginService(player, position, "readiness_container_access_failed");
				schedule(navigationInterval);
			} else {
				schedule(navigationDecisionDelay(*player));
			}
		}
		return false;
	}
	if (readiness.recovery == "service") {
		beginService(player, position, "combat_readiness_service");
		schedule(navigationInterval);
		return false;
	}
	stop(("combat_readiness_" + readiness.terminalReason).c_str(), position);
	return false;
}

std::string PlayerBotController::rewardItemSignature(const Item& item) const
{
	std::ostringstream signature;
	signature << item.getID() << ':' << item.getSubType();
	if (const Container* container = item.getContainer()) {
		signature << '[';
		bool first = true;
		for (const Item* child : container->getItemList()) {
			if (!first) {
				signature << ',';
			}
			first = false;
			signature << rewardItemSignature(*child);
		}
		signature << ']';
	}
	return signature.str();
}

void PlayerBotController::inspectRewardItem(Player& player, const Item& item, uint16_t rootOrdinal,
                       std::vector<uint16_t>& path, const std::string& rootSignature,
                       const EquipmentLoadout& currentLoadout,
                       const PlayerBotCombatProfile& currentProfile,
                       const EquipmentHuntSummary& currentHunts, bool currentReady,
                       uint32_t additionalWeight,
					   std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
                       size_t& simulatedItems, RewardInspection& inspection) const
{
	RewardItemInspection inspected{item.getID(), item.getItemCount(), static_cast<uint32_t>(path.size()),
	                              rootOrdinal, path};
	++inspection.itemCount;
	if (item.getContainer()) {
		inspected.classes.emplace_back("container");
		++inspection.containerCount;
	}
	EquipmentOfferEvaluation equipment;
	bool evaluatedEquipment = false;
	if (equipmentPolicy.isCombatEquipment(item)) {
		evaluatedEquipment = true;
		const auto cacheKey = std::make_pair(item.getID(), additionalWeight);
		if (auto cached = equipmentEvaluations.find(cacheKey); cached != equipmentEvaluations.end()) {
			equipment = cached->second;
		} else {
			equipment = equipmentPolicy.evaluateCandidate(
				player, item.getID(), currentLoadout, currentProfile, currentHunts, currentReady,
				equipmentReadinessInput(player), additionalWeight,
				simulatedItems < maximumEquipmentCandidateSimulations,
				[this](Player& candidate, const PlayerBotCombatProfile& profile) {
					return equipmentHuntSummary(candidate, profile);
				});
			if (equipment.simulated) {
				++simulatedItems;
			}
			equipmentEvaluations.emplace(cacheKey, equipment);
		}
	}
	if (evaluatedEquipment && equipment.rejection.empty()) {
		const ItemType& candidateType = Item::items[item.getID()];
		const uint16_t currentItemId = currentLoadout.itemIds[equipment.slot];
		const ItemType* currentType = currentItemId == 0 ? nullptr : &Item::items[currentItemId];
		const bool armorSlot = equipment.slot == CONST_SLOT_HEAD || equipment.slot == CONST_SLOT_ARMOR ||
		                       equipment.slot == CONST_SLOT_LEGS || equipment.slot == CONST_SLOT_FEET;
		const bool shield = candidateType.weaponType == WEAPON_SHIELD;
		const char* metric = armorSlot ? "armor" : shield ? "defense" : "attack";
		const int32_t candidateValue = armorSlot ? candidateType.armor :
		                               shield ? candidateType.defense : candidateType.attack;
		const int32_t currentValue = !currentType ? 0 : armorSlot ? currentType->armor :
		                             shield ? currentType->defense : currentType->attack;
		EquipmentUpgrade upgrade{equipment.slot, std::max(1, candidateValue - currentValue), metric,
		                         currentValue, candidateValue};
		inspected.classes.emplace_back("equipment_upgrade");
		++inspection.equipmentUpgradeCount;
		if (!inspection.bestEquipment || equipment.rule > inspection.bestEquipment->rule ||
		    (equipment.rule == inspection.bestEquipment->rule &&
		     upgrade.benefit > inspection.bestUpgrade->benefit)) {
			inspection.bestUpgrade = upgrade;
			inspection.bestEquipment = equipment;
			inspection.bestItemId = item.getID();
			inspection.bestRootOrdinal = rootOrdinal;
			inspection.bestItemPath = path;
			inspection.bestRootSignature = rootSignature;
		}
	} else if (evaluatedEquipment && inspection.equipmentRejection.empty()) {
		inspection.equipmentRejection = equipment.rejection;
	}
	inspected.worth = item.getWorth();
	if (inspected.worth != 0) {
		inspected.classes.emplace_back("currency");
		inspection.currencyValue += inspected.worth;
	}
	if (item.getID() == smallHealthPotionItemId) {
		inspected.classes.emplace_back("required_supply");
		inspection.potionCount += item.getItemCount();
	} else if (PlayerBotInventoryPolicy::isFoodItem(item.getID())) {
		inspected.classes.emplace_back("food");
		inspection.foodCount += item.getItemCount();
	} else if (item.getID() == ropeItemId) {
		inspected.classes.emplace_back("tool");
		inspection.ropeCount += item.getItemCount();
	} else if (item.getID() == 2554) {
		inspected.classes.emplace_back("tool");
		inspection.shovelCount += item.getItemCount();
	}
	const auto sellIt = itemSellValues.find(item.getID());
	const ItemType& itemType = Item::items[item.getID()];
	const bool unsupportedTwoHandedWeapon = (itemType.slotPosition & SLOTP_TWO_HAND) != 0 &&
	                                      itemType.weaponType != WEAPON_NONE;
	const uint32_t sellPrice = inspected.worth == 0 && !unsupportedTwoHandedWeapon &&
	                           sellIt != itemSellValues.end() ? sellIt->second : 0;
	if (sellPrice != 0) {
		inspected.classes.emplace_back("sellable");
		inspected.sellValue = sellPrice * item.getItemCount();
		inspection.sellValue += inspected.sellValue;
	}
	uint32_t itemUtility = inspected.worth + inspected.sellValue;
	if (item.getID() == smallHealthPotionItemId) {
		itemUtility += missingPotionUtility * item.getItemCount();
	} else if (PlayerBotInventoryPolicy::isFoodItem(item.getID())) {
		itemUtility += foodPreferenceUtility * item.getItemCount();
	} else if (item.getID() == ropeItemId || item.getID() == 2554) {
		itemUtility += 100;
	}
	if (itemUtility > inspection.primaryKnownItemUtility) {
		inspection.primaryKnownItemId = item.getID();
		inspection.primaryKnownRootOrdinal = rootOrdinal;
		inspection.primaryKnownRootSignature = rootSignature;
		inspection.primaryKnownItemUtility = itemUtility;
	}
	if (inspected.classes.empty()) {
		inspected.classes.emplace_back("unknown_keep");
		++inspection.unknownCount;
	}
	inspection.items.push_back(std::move(inspected));

	if (const Container* container = item.getContainer()) {
		uint16_t childOrdinal = 0;
		for (const Item* child : container->getItemList()) {
			path.push_back(childOrdinal++);
			inspectRewardItem(player, *child, rootOrdinal, path, rootSignature, currentLoadout,
			                  currentProfile, currentHunts, currentReady, additionalWeight,
			                  equipmentEvaluations, simulatedItems, inspection);
			path.pop_back();
		}
	}
}

PlayerBotController::RewardInspection PlayerBotController::inspectRewardBundle(
	Player& player, const Container& contents, const EquipmentLoadout& currentLoadout,
	const PlayerBotCombatProfile& currentProfile, const EquipmentHuntSummary& currentHunts, bool currentReady,
	uint32_t additionalWeight, std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
	size_t& simulatedItems) const
{
	RewardInspection inspection;
	uint16_t rootOrdinal = 0;
	for (const Item* root : contents.getItemList()) {
		const std::string signature = rewardItemSignature(*root);
		inspection.rootSignatures.push_back(signature);
		if (root->isStackable()) {
			inspection.stackableRootCounts[root->getID()] += root->getItemCount();
		} else {
			inspection.nonStackableRootSignatures.push_back(signature);
		}
		std::vector<uint16_t> path;
		inspectRewardItem(player, *root, rootOrdinal++, path, signature, currentLoadout, currentProfile,
		                  currentHunts, currentReady, additionalWeight, equipmentEvaluations, simulatedItems,
		                  inspection);
	}
	finalizeRewardInspection(player, inspection);
	return inspection;
}

PlayerBotController::RewardInspection PlayerBotController::inspectKnownReward(
	Player& player, const Item& item, const EquipmentLoadout& currentLoadout,
	const PlayerBotCombatProfile& currentProfile, const EquipmentHuntSummary& currentHunts, bool currentReady,
	uint32_t additionalWeight, std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
	size_t& simulatedItems) const
{
	RewardInspection inspection;
	const std::string signature = rewardItemSignature(item);
	inspection.rootSignatures.push_back(signature);
	if (item.isStackable()) {
		inspection.stackableRootCounts[item.getID()] += item.getItemCount();
	} else {
		inspection.nonStackableRootSignatures.push_back(signature);
	}
	std::vector<uint16_t> path;
	inspectRewardItem(player, item, 0, path, signature, currentLoadout, currentProfile, currentHunts,
	                  currentReady, additionalWeight, equipmentEvaluations, simulatedItems, inspection);
	finalizeRewardInspection(player, inspection);
	return inspection;
}

void PlayerBotController::finalizeRewardInspection(Player& player, RewardInspection& inspection) const
{
	if (inspection.bestUpgrade) {
		inspection.knownUtility += inspection.bestUpgrade->benefit * 20;
	}
	inspection.knownUtility += static_cast<int32_t>(inspection.currencyValue + inspection.sellValue);
	const uint32_t heldPotions = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t heldFood = inventoryPolicy.foodInventory(player).count;
	const uint32_t potionNeed = heldPotions < smallHealthPotionRestockTarget ? smallHealthPotionRestockTarget - heldPotions : 0;
	const uint32_t foodNeed = heldFood < preferredFoodCount ? preferredFoodCount - heldFood : 0;
	inspection.knownUtility += static_cast<int32_t>(std::min(inspection.potionCount, potionNeed) * missingPotionUtility +
	                                                  std::min(inspection.foodCount, foodNeed) * foodPreferenceUtility);
	if (inspection.ropeCount != 0 && !g_game.findItemOfType(&player, ropeItemId, true)) {
		inspection.knownUtility += 100;
	}
	if (inspection.shovelCount != 0 && !g_game.findItemOfType(&player, 2554, true)) {
		inspection.knownUtility += 100;
	}
}

std::string PlayerBotController::rewardInspectionItemsJson(const RewardInspection& inspection) const
{
	std::ostringstream json;
	json << '[';
	bool firstItem = true;
	for (const RewardItemInspection& item : inspection.items) {
		if (!firstItem) {
			json << ',';
		}
		firstItem = false;
		json << "{\"item_id\":" << item.itemId << ",\"name\":" << jsonString(Item::items[item.itemId].name)
		     << ",\"count\":" << item.count << ",\"depth\":" << item.depth
		     << ",\"root_ordinal\":" << item.rootOrdinal << ",\"path\":[";
		for (size_t index = 0; index < item.path.size(); ++index) {
			if (index != 0) {
				json << ',';
			}
			json << item.path[index];
		}
		json << "],\"classes\":[";
		for (size_t index = 0; index < item.classes.size(); ++index) {
			if (index != 0) {
				json << ',';
			}
			json << jsonString(item.classes[index]);
		}
		json << "],\"worth\":" << item.worth << ",\"sell_value\":" << item.sellValue << '}';
	}
	json << ']';
	return json.str();
}

int32_t PlayerBotController::estimatedPickupUtility(const PlayerBotRewardPlan& reward) const
{
	const int32_t base = reward.equipmentUpgradeCount != 0 ? pickupRewardBaseUtility : economicPickupBaseUtility;
	return std::max<int32_t>(0, base + static_cast<int32_t>(reward.knownUtility) -
	                               static_cast<int32_t>(reward.estimatedDistance));
}

Container* PlayerBotController::playerBackpack(Player& player) const
{
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	return backpackItem ? backpackItem->getContainer() : nullptr;
}

uint32_t PlayerBotController::matchingRewardRootCount(Player& player, const std::string& signature) const
{
	uint32_t count = 0;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player.getInventoryItem(static_cast<slots_t>(slot));
		if (item && rewardItemSignature(*item) == signature) {
			++count;
		}
	}
	if (Container* backpack = playerBackpack(player)) {
		count += static_cast<uint32_t>(std::count_if(backpack->getItemList().begin(), backpack->getItemList().end(),
		                                            [this, &signature](const Item* item) {
			                                            return rewardItemSignature(*item) == signature;
		                                            }));
	}
	return count;
}

bool PlayerBotController::allRewardRootsAdded(Player& player) const
{
	std::map<std::string, uint32_t> expectedCounts;
	for (const std::string& signature : rewardSession.plan().nonStackableRootSignatures) {
		++expectedCounts[signature];
	}
	for (const auto& entry : expectedCounts) {
		auto beforeIt = rewardSession.claimSnapshot().roots.find(entry.first);
		const uint32_t before = beforeIt == rewardSession.claimSnapshot().roots.end() ? 0 : beforeIt->second;
		if (matchingRewardRootCount(player, entry.first) < before + entry.second) {
			return false;
		}
	}
	for (const auto& entry : rewardSession.plan().stackableRootCounts) {
		auto beforeIt = rewardSession.claimSnapshot().stackables.find(entry.first);
		const uint32_t before = beforeIt == rewardSession.claimSnapshot().stackables.end() ? 0 : beforeIt->second;
		if (inventoryPolicy.inventoryItemCount(player, entry.first) < before + entry.second) {
			return false;
		}
	}
	return true;
}

Item* PlayerBotController::findMatchingRewardRoot(Player& player, const std::string& signature) const
{
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player.getInventoryItem(static_cast<slots_t>(slot));
		if (item && rewardItemSignature(*item) == signature) {
			return item;
		}
	}
	Container* backpack = playerBackpack(player);
	if (!backpack) {
		return nullptr;
	}
	for (Item* item : backpack->getItemList()) {
		if (rewardItemSignature(*item) == signature) {
			return item;
		}
	}
	return nullptr;
}

Item* PlayerBotController::resolveRewardPath(Item* root, const std::vector<uint16_t>& path, size_t length) const
{
	Item* current = root;
	for (size_t depth = 0; depth < length; ++depth) {
		Container* container = current ? current->getContainer() : nullptr;
		if (!container || path[depth] >= container->size()) {
			return nullptr;
		}
		current = container->getItemByIndex(path[depth]);
	}
	return current;
}

bool PlayerBotController::prepareRewardItemAccess(Player& player, const Position& position, Item*& selectedItem, std::string& failure)
{
	Container* backpack = playerBackpack(player);
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	if (!backpack || !backpackItem) {
		failure = "backpack_unavailable";
		return false;
	}
	if (player.getContainerByID(backpackContainerId) != backpack) {
		if (!player.canDoAction()) {
			schedule(navigationDecisionDelay(player));
			return false;
		}
		player.closeContainer(backpackContainerId);
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0,
		                     backpackContainerId, backpackItem->getClientID());
		schedule(navigationDecisionDelay(player));
		return false;
	}

	const auto& reward = rewardSession.plan();
	Item* root = findMatchingRewardRoot(player, reward.rootSignature);
	if (!root) {
		Item* extracted = g_game.findItemOfType(&player, reward.itemId, true);
		if (extracted && isRewardClaimed(player, reward.uniqueId)) {
			selectedItem = extracted;
			return true;
		}
		failure = "reward_bundle_unavailable";
		return false;
	}
	if (reward.selectedItemPath.size() > maximumContainerId - rewardContainerIdBase + 1) {
		failure = "reward_container_depth_unsupported";
		return false;
	}
	for (size_t depth = 0; depth < reward.selectedItemPath.size(); ++depth) {
		Item* ancestor = resolveRewardPath(root, reward.selectedItemPath, depth);
		Container* container = ancestor ? ancestor->getContainer() : nullptr;
		if (!container) {
			failure = "reward_container_path_invalid";
			return false;
		}
		const uint8_t containerId = static_cast<uint8_t>(rewardContainerIdBase + depth);
		if (player.getContainerByID(containerId) == container) {
			if (rewardSession.containerAccess().depth == depth) {
				rewardSession.containerAccess() = {};
			}
			continue;
		}
		if (!player.canDoAction()) {
			schedule(navigationDecisionDelay(player));
			return false;
		}
		player.closeContainer(containerId);
		Position sourcePosition;
		uint8_t sourceIndex = 0;
		g_game.internalGetPosition(ancestor, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF) {
			failure = "reward_container_position_unavailable";
			return false;
		}
		if (rewardSession.containerAccess().depth != depth) {
			rewardSession.containerAccess().depth = depth;
			rewardSession.containerAccess().openAttempts = 0;
		}
		if (++rewardSession.containerAccess().openAttempts >= maximumProgressionAttempts) {
			failure = "reward_container_open_failed";
			return false;
		}
		g_game.playerUseItem(playerId, sourcePosition, sourceIndex, containerId, ancestor->getClientID());
		emit("action_result", position,
		     "\"action\":\"open_reward_container\",\"result\":\"requested\",\"container_id\":" +
		         std::to_string(containerId) + ",\"depth\":" + std::to_string(depth) +
		         ",\"item_id\":" + std::to_string(ancestor->getID()));
		schedule(navigationDecisionDelay(player));
		return false;
	}
	selectedItem = resolveRewardPath(root, reward.selectedItemPath, reward.selectedItemPath.size());
	if (!selectedItem || selectedItem->getID() != reward.itemId) {
		failure = "reward_item_path_invalid";
		return false;
	}
	return true;
}

bool PlayerBotController::isRewardPosition(const Player& player, const Position& position) const
{
	if (position.z >= MAP_MAX_LAYERS || !player.getTown()) {
		return false;
	}
	if (player.getTown()->getID() == rookgaardTownId) {
		return Position::getDistanceX(position, rookgaardTemplePosition) <= rookgaardRewardRadius &&
		       Position::getDistanceY(position, rookgaardTemplePosition) <= rookgaardRewardRadius;
	}
	return player.getTown()->getID() == oracleTownId &&
	       playerbot::isInsideLocalPlanningArea(player.getTemplePosition(), position);
}

bool PlayerBotController::isRewardClaimed(const Player& player, uint16_t uniqueId) const
{
	int32_t storageValue = -1;
	return player.getStorageValue(uniqueId, storageValue) && storageValue != -1;
}

bool PlayerBotController::planSimpleRewardApproach(Player& player, const Position& rewardPosition, Position& approachPosition,
                              std::deque<PlayerBotNavigationStep>& approachSteps, uint64_t& expandedNodes)
{
	const Position currentPosition = player.getPosition();
	std::vector<Position> candidates;
	candidates.reserve(8);
	for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) {
		for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
			if (xOffset == 0 && yOffset == 0) {
				continue;
			}
			const Position candidate(rewardPosition.x + xOffset, rewardPosition.y + yOffset, rewardPosition.z);
			Tile* tile = g_game.map.getTile(candidate);
			if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
				continue;
			}
			candidates.push_back(candidate);
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const Position& left, const Position& right) {
		return Position::getDistanceX(currentPosition, left) + Position::getDistanceY(currentPosition, left) <
		       Position::getDistanceX(currentPosition, right) + Position::getDistanceY(currentPosition, right);
	});
	for (const Position& candidate : candidates) {
			std::deque<PlayerBotNavigationStep> steps;
			uint64_t candidateExpandedNodes = 0;
			const auto startedAt = std::chrono::steady_clock::now();
			const PlayerBotNavigationRoutePlan routePlan = candidate == currentPosition ? PlayerBotNavigationRoutePlan{} :
				navigationRuntime.plan(player, candidate);
			const PlayerBotNavigationResult planResult = candidate == currentPosition ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
			if (candidate != currentPosition) {
				steps = routePlan.steps;
				candidateExpandedNodes = routePlan.metrics.expandedNodes;
			}
			const bool planned = planResult == PlayerBotNavigationResult::Reached;
			telemetry.recordPathfindingAttempt(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - startedAt));
			if (!planned || (candidate != currentPosition && steps.empty())) {
				continue;
			}
			const bool simple = std::none_of(steps.begin(), steps.end(), [](const PlayerBotNavigationStep& step) {
				return step.action == PlayerBotNavigationAction::UseDoor ||
				       step.action == PlayerBotNavigationAction::UseRope ||
				       step.action == PlayerBotNavigationAction::UseShovel;
			});
			if (!simple || steps.size() > 120) {
				continue;
			}
			approachPosition = candidate;
			approachSteps = std::move(steps);
			expandedNodes = candidateExpandedNodes;
			return true;
	}
	return false;
}

void PlayerBotController::emitRewardCandidate(const PlayerBotRewardPlan& candidate, const Position& position, const char* result,
                         const char* reason) const
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << candidate.uniqueId
	       << ",\"acquisition_source\":\"map_reward\""
	       << ",\"result\":" << jsonString(result)
	       << ",\"item_id\":" << candidate.itemId
	       << ",\"root_item_id\":" << candidate.rootItemId
	       << ",\"slot\":" << static_cast<int32_t>(candidate.slot)
	       << ",\"metric\":" << jsonString(candidate.metric)
	       << ",\"current_value\":" << candidate.currentValue
	       << ",\"candidate_value\":" << candidate.candidateValue
	       << ",\"benefit\":" << candidate.benefit
	       << ",\"known_utility\":" << candidate.knownUtility
	       << ",\"item_count\":" << candidate.itemCount
	       << ",\"container_count\":" << candidate.containerCount
	       << ",\"unknown_count\":" << candidate.unknownCount
	       << ",\"currency_value\":" << candidate.currencyValue
	       << ",\"sell_value\":" << candidate.sellValue
	       << ",\"equipment_upgrade_count\":" << candidate.equipmentUpgradeCount
	       << ",\"travel_steps\":" << candidate.travelSteps
	       << ",\"destination\":{\"x\":" << candidate.itemPosition.x
	       << ",\"y\":" << candidate.itemPosition.y << ",\"z\":"
	       << static_cast<uint16_t>(candidate.itemPosition.z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("strategy_candidate", position, fields.str());
}

void PlayerBotController::emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
                          const RewardInspection& inspection, const Position& position)
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << uniqueId
	       << ",\"acquisition_source\":\"map_reward\""
	       << ",\"result\":\"inspected\",\"recursive\":true"
	       << ",\"known_utility\":" << inspection.knownUtility
	       << ",\"item_count\":" << inspection.itemCount
	       << ",\"container_count\":" << inspection.containerCount
	       << ",\"unknown_count\":" << inspection.unknownCount
	       << ",\"currency_value\":" << inspection.currencyValue
	       << ",\"sell_value\":" << inspection.sellValue
	       << ",\"equipment_upgrade_count\":" << inspection.equipmentUpgradeCount
	       << ",\"equipment_rule\":" << (inspection.bestEquipment ?
	              jsonString(PlayerBotEquipmentPolicy::decisionRuleName(inspection.bestEquipment->rule)) : "null")
	       << ",\"equipment_rejection\":" << (inspection.equipmentRejection.empty() ?
	              "null" : jsonString(inspection.equipmentRejection))
	       << ",\"destination\":{\"x\":" << rewardPosition.x << ",\"y\":" << rewardPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(rewardPosition.z) << '}'
	       << ",\"items\":" << rewardInspectionItemsJson(inspection);
	const std::string fingerprint = fields.str();
	auto previous = rewardInspectionFingerprints.find(uniqueId);
	if (previous != rewardInspectionFingerprints.end() && previous->second == fingerprint) {
		return;
	}
	rewardInspectionFingerprints[uniqueId] = fingerprint;
	emit("reward_inspection", position, fingerprint);
}

bool PlayerBotController::findPickupReward(Player& player, const Position& position, PlayerBotRewardPlan& reward,
                      std::deque<PlayerBotNavigationStep>& rewardSteps)
{
	std::optional<PlayerBotRewardPlan> claimedUpgrade;
	std::vector<PlayerBotRewardPlan> unclaimedCandidates;
	std::deque<PlayerBotNavigationStep> selectedSteps;
	const EquipmentLoadout currentLoadout = equipmentPolicy.loadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentPolicy.combatProfile(player, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	const bool currentReady = equipmentPolicy.loadoutReady(player, currentLoadout, equipmentReadinessInput(player));
	std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation> equipmentEvaluations;
	size_t simulatedItems = 0;
	for (const auto& entry : g_game.getUniqueItems()) {
		Item* rewardObject = entry.second;
		if (!rewardObject) {
			continue;
		}
		const bool containerReward = rewardObject->getActionId() == genericQuestChestActionId;
		const bool doubletReward = rewardObject->getActionId() == nonContainerQuestActionId &&
		                           rewardObject->getUniqueId() == doubletQuestUniqueId;
		if (!containerReward && !doubletReward) {
			continue;
		}
		if (!rewardObject->isLoadedFromMap() && !doubletReward) {
			continue;
		}
		Tile* tile = rewardObject->getTile();
		Container* contents = rewardObject->getContainer();
		if (!tile || !isRewardPosition(player, tile->getPosition())) {
			continue;
		}
		PlayerBotRewardPlan rejected;
		rejected.uniqueId = rewardObject->getUniqueId();
		rejected.rootItemId = rewardObject->getID();
		rejected.itemPosition = tile->getPosition();
		if (containerReward && !contents) {
			emitRewardCandidate(rejected, position, "rejected", "malformed_reward_container");
			continue;
		}
		if (containerReward && contents->empty()) {
			emitRewardCandidate(rejected, position, "rejected", "empty_reward_bundle");
			continue;
		}

		std::unique_ptr<Item> knownReward;
		if (doubletReward) {
			knownReward.reset(Item::CreateItem(doubletItemId));
			if (!knownReward) {
				continue;
			}
		}
		uint32_t totalWeight = 0;
		if (containerReward) {
			for (Item* reward : contents->getItemList()) {
				totalWeight += reward->getWeight();
			}
		} else {
			totalWeight = knownReward->getWeight();
		}
		const bool claimed = isRewardClaimed(player, rewardObject->getUniqueId());
		const uint32_t acquisitionWeight = claimed ? 0 : totalWeight;
		RewardInspection inspection = containerReward ?
			inspectRewardBundle(player, *contents, currentLoadout, currentProfile, currentHunts, currentReady,
			                    acquisitionWeight, equipmentEvaluations, simulatedItems) :
			inspectKnownReward(player, *knownReward, currentLoadout, currentProfile, currentHunts, currentReady,
			                   acquisitionWeight, equipmentEvaluations, simulatedItems);
		if (inspection.itemCount != 0) {
			emitRewardInspection(rewardObject->getUniqueId(), tile->getPosition(), inspection, position);
		}
		if (inspection.knownUtility <= 0 || inspection.rootSignatures.empty()) {
			rejected.itemCount = inspection.itemCount;
			rejected.containerCount = inspection.containerCount;
			rejected.unknownCount = inspection.unknownCount;
			rejected.equipmentUpgradeCount = inspection.equipmentUpgradeCount;
			emitRewardCandidate(rejected, position, "rejected",
			                    inspection.equipmentRejection.empty() ? "unsupported_reward_bundle" :
			                                                            inspection.equipmentRejection.c_str());
			continue;
		}
		PlayerBotRewardPlan candidate;
		candidate.uniqueId = rewardObject->getUniqueId();
		candidate.rootOrdinal = inspection.bestUpgrade ? inspection.bestRootOrdinal : inspection.primaryKnownRootOrdinal;
		Item* selectedRoot = containerReward ? contents->getItemByIndex(candidate.rootOrdinal) : knownReward.get();
		candidate.rootItemId = selectedRoot->getID();
		candidate.itemId = inspection.bestUpgrade ? inspection.bestItemId : inspection.primaryKnownItemId;
		candidate.itemPosition = tile->getPosition();
		if (inspection.bestUpgrade) {
			candidate.slot = inspection.bestUpgrade->slot;
			candidate.benefit = inspection.bestUpgrade->benefit;
			candidate.metric = inspection.bestUpgrade->metric;
			candidate.currentValue = inspection.bestUpgrade->currentValue;
			candidate.candidateValue = inspection.bestUpgrade->candidateValue;
			candidate.replacedItemId = inspection.bestEquipment->replacedItemId;
			candidate.displacedLeftItemId = inspection.bestEquipment->displacedLeftItemId;
			candidate.displacedRightItemId = inspection.bestEquipment->displacedRightItemId;
		}
		candidate.knownUtility = inspection.knownUtility;
		candidate.itemCount = inspection.itemCount;
		candidate.containerCount = inspection.containerCount;
		candidate.unknownCount = inspection.unknownCount;
		candidate.currencyValue = inspection.currencyValue;
		candidate.sellValue = inspection.sellValue;
		candidate.equipmentUpgradeCount = inspection.equipmentUpgradeCount;
		candidate.selectedItemPath = inspection.bestItemPath;
		candidate.rootSignature = inspection.bestUpgrade ? inspection.bestRootSignature : inspection.primaryKnownRootSignature;
		candidate.rootSignatures = inspection.rootSignatures;
		candidate.nonStackableRootSignatures = inspection.nonStackableRootSignatures;
		candidate.stackableRootCounts = inspection.stackableRootCounts;
		candidate.estimatedDistance = playerBotNavigationDistance(position, candidate.itemPosition);
		std::set<slots_t> displacedSlots;
		if (inspection.bestEquipment) {
			for (const auto& displaced : {std::pair<slots_t, uint16_t>{candidate.slot, candidate.replacedItemId},
			                              {CONST_SLOT_LEFT, candidate.displacedLeftItemId},
			                              {CONST_SLOT_RIGHT, candidate.displacedRightItemId}}) {
				if (displaced.second != 0) {
					displacedSlots.insert(displaced.first);
				}
			}
		}
		candidate.requiredBackpackSlots = (containerReward ? static_cast<uint32_t>(contents->size()) : 1) +
		                                  static_cast<uint32_t>(displacedSlots.size());
		if (claimed) {
			const bool ownedUpgrade = inspection.bestUpgrade &&
			                          (candidate.selectedItemPath.empty() ?
			                               g_game.findItemOfType(&player, candidate.itemId, true) != nullptr :
			                               findMatchingRewardRoot(player, candidate.rootSignature) != nullptr);
			if (ownedUpgrade) {
				candidate.travelSteps = 0;
				emitRewardCandidate(candidate, position, "feasible", "claimed_reward_owned");
				if (!claimedUpgrade || candidate.benefit > claimedUpgrade->benefit ||
				    (candidate.benefit == claimedUpgrade->benefit && candidate.uniqueId < claimedUpgrade->uniqueId)) {
					claimedUpgrade = candidate;
				}
			} else {
				emitRewardCandidate(candidate, position, "rejected", "claimed_reward_missing");
			}
			continue;
		}
		if (player.getFreeCapacity() < totalWeight) {
			emitRewardCandidate(candidate, position, "rejected", "insufficient_capacity");
			continue;
		}
		Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
		Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
		const uint32_t freeBackpackSlots = backpack ? backpack->capacity() -
		                                  std::min<uint32_t>(backpack->capacity(), backpack->size()) : 0;
		if (!backpack || freeBackpackSlots < candidate.requiredBackpackSlots) {
			emitRewardCandidate(candidate, position, "rejected", "insufficient_inventory_space");
			continue;
		}
		unclaimedCandidates.push_back(candidate);
	}

	std::optional<PlayerBotRewardPlan> selected;
	bool resumeEquipment = false;
	if (claimedUpgrade) {
		selected = claimedUpgrade;
		resumeEquipment = true;
	} else {
		std::sort(unclaimedCandidates.begin(), unclaimedCandidates.end(), [this](const PlayerBotRewardPlan& left,
		                                                                        const PlayerBotRewardPlan& right) {
			const int32_t leftUtility = estimatedPickupUtility(left);
			const int32_t rightUtility = estimatedPickupUtility(right);
			if (leftUtility != rightUtility) {
				return leftUtility > rightUtility;
			}
			if (left.estimatedDistance != right.estimatedDistance) {
				return left.estimatedDistance < right.estimatedDistance;
			}
			return left.uniqueId < right.uniqueId;
		});
		for (PlayerBotRewardPlan candidate : unclaimedCandidates) {
			if (estimatedPickupUtility(candidate) <= huntGoalUtility) {
				emitRewardCandidate(candidate, position, "rejected", "utility_below_hunt");
				continue;
			}
			std::deque<PlayerBotNavigationStep> steps;
			if (!planSimpleRewardApproach(player, candidate.itemPosition, candidate.approachPosition,
			                              steps, candidate.expandedNodes)) {
				emitRewardCandidate(candidate, position, "rejected", "simple_route_unavailable");
				continue;
			}
			candidate.travelSteps = static_cast<uint32_t>(steps.size());
			const int32_t actualUtility = std::max<int32_t>(
			    0, (candidate.equipmentUpgradeCount != 0 ? pickupRewardBaseUtility : economicPickupBaseUtility) +
			           static_cast<int32_t>(candidate.knownUtility) - static_cast<int32_t>(candidate.travelSteps));
			if (actualUtility <= huntGoalUtility) {
				emitRewardCandidate(candidate, position, "rejected", "actual_utility_below_hunt");
				continue;
			}
			emitRewardCandidate(candidate, position, "feasible");
			selected = candidate;
			selectedSteps = std::move(steps);
			break;
		}
	}
	if (!selected) {
		return false;
	}
	selected->resumeEquipment = resumeEquipment;
	reward = *selected;
	rewardSteps = std::move(selectedSteps);
	return true;
}

uint32_t PlayerBotController::saleableItemCount(const Player& player) const
{
	uint32_t count = 0;
	for (const auto& [itemId, value] : itemSellValues) {
		if (value != 0) {
			count += getSaleItemCount(player, itemId);
		}
	}
	return count;
}

PlayerBotController::GoalCandidate PlayerBotController::serviceGoalCandidate(const Player& player) const
{
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t missingPotions = potionCount <= smallHealthPotionReturnThreshold ?
	                                  smallHealthPotionRestockTarget - potionCount : 0;
	const uint32_t sellable = saleableItemCount(player);
	const bool lowCapacity = inventoryPolicy.effectiveFreeCapacity(player) < returnCapacityThreshold;
	const bool criticalHealing = survivalRuntime.needsHealing(survivalSnapshot(player)) && missingPotions != 0;
	const bool cashAdjustment = player.getMoney() != inventoryPolicy.desiredCarriedGold(player);
	const bool feasible = lowCapacity || missingPotions != 0 || sellable != 0 || cashAdjustment;
	int32_t utility = feasible ? serviceGoalBaseUtility : 0;
	utility += static_cast<int32_t>(missingPotions * missingPotionUtility +
	                                std::min<uint32_t>(sellable, 20) * sellableItemUtility);
	utility += cashAdjustment ? 10 : 0;
	if (lowCapacity) {
		utility = std::max<int32_t>(utility, capacityServiceUtility);
	}
	if (criticalHealing) {
		utility = std::max<int32_t>(utility, criticalHealingServiceUtility);
	}
	const char* reason = criticalHealing ? "critical_healing" : lowCapacity ? "capacity" :
	                     missingPotions != 0 ? "healing_reserve" :
	                     sellable != 0 ? "sellable_inventory" : cashAdjustment ? "cash_reserve" : "no_service_need";
	return GoalCandidate{TopLevelGoal::Service, feasible, utility, reason};
}

void PlayerBotController::emitGoalCandidate(const Player& player, const GoalCandidate& candidate, uint64_t decisionId, const Position& position, const char* decisionReason,
	                       const PlayerBotRewardPlan* reward, const PlayerBotOracleDeparturePlan* departure,
	                       const EquipmentOfferEvaluation* equipment) const
{
	std::ostringstream fields;
	const bool evaluated = candidate.reason != "deferred_lower_utility";
	fields << "\"decision_id\":" << decisionId << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"goal\":" << jsonString(PlayerBotGoalArbiter::goalName(candidate.goal))
	       << ",\"evaluated\":" << (evaluated ? "true" : "false")
	       << ",\"feasible\":" << (candidate.feasible ? "true" : "false")
	       << ",\"utility\":" << candidate.utility << ",\"reason\":" << jsonString(candidate.reason);
	if (candidate.goal == TopLevelGoal::Service) {
		const PlayerBotFoodInventory food = inventoryPolicy.foodInventory(player);
		const uint32_t foodGap = food.count < preferredFoodCount ? preferredFoodCount - food.count : 0;
		fields << ",\"potion_count\":" << inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId)
		       << ",\"potion_return_threshold\":" << smallHealthPotionReturnThreshold
		       << ",\"potion_restock_target\":" << smallHealthPotionRestockTarget
		       << ",\"food_count\":" << food.count << ",\"food_weight\":" << food.weight
		       << ",\"food_preferred\":" << preferredFoodCount << ",\"food_gap\":" << foodGap
		       << ",\"food_utility\":" << foodGap * foodPreferenceUtility
		       << ",\"sellable_count\":" << saleableItemCount(player);
	}
	if (reward) {
		fields << ",\"candidate_id\":" << reward->uniqueId << ",\"item_id\":" << reward->itemId
		       << ",\"benefit\":" << reward->benefit << ",\"travel_steps\":" << reward->travelSteps;
	}
	if (departure) {
		fields << ",\"npc_id\":" << departure->npcId << ",\"town\":\"thais\",\"town_id\":" << oracleTownId
		       << ",\"vocation\":\"knight\",\"vocation_id\":" << oracleVocationId
		       << ",\"travel_steps\":" << departure->travelSteps;
	}
	if (equipment) {
		fields << ",\"npc_id\":" << equipment->npcId << ",\"item_id\":" << equipment->itemId
		       << ",\"price\":" << equipment->price << ",\"rule\":"
	       << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule))
		       << ",\"travel_steps\":" << equipment->travelSteps;
	}
	if (candidate.goal == TopLevelGoal::MagicTraining) {
		const std::optional<ManaRegenerationForecast> forecast = player.getManaRegenerationForecast();
		fields << ",\"mana\":" << player.getMana() << ",\"mana_max\":" << player.getMaxMana();
		if (forecast) {
			fields << ",\"mana_gain\":" << forecast->gain << ",\"mana_tick_interval\":" << forecast->interval
			       << ",\"mana_tick_remaining\":" << forecast->remaining;
		}
	}
	emit("goal_candidate", position, fields.str());
}

void PlayerBotController::beginPickupReward(Player& player, const Position& position, PlayerBotRewardPlan reward,
                       std::deque<PlayerBotNavigationStep> rewardSteps)
{
	rewardSession.begin(std::move(reward));
	progressionSession.begin(PlayerBotProgressionProcedure::PickupReward);
	const auto& selected = rewardSession.plan();
	for (uint16_t itemId : {selected.replacedItemId, selected.displacedLeftItemId,
	                        selected.displacedRightItemId}) {
		if (itemId != 0) {
			rewardSession.displacedItemCounts()[itemId] = inventoryPolicy.inventoryItemCount(player, itemId);
		}
	}
	if (!selected.resumeEquipment) {
		navigationRuntime.adopt(selected.approachPosition, std::move(rewardSteps));
	}
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"candidate_id\":" << selected.uniqueId
	       << ",\"reason\":" << jsonString(selected.resumeEquipment ? "resume_claimed_upgrade" :
	                                                                    "highest_known_utility_reachable_reward")
	       << ",\"item_id\":" << selected.itemId << ",\"root_item_id\":" << selected.rootItemId
	       << ",\"benefit\":" << selected.benefit << ",\"known_utility\":" << selected.knownUtility
	       << ",\"travel_steps\":" << selected.travelSteps;
	emit("strategy_selection", position, fields.str());
	say(player, selected.resumeEquipment ? "Equipping a previously claimed reward." :
	                                         "Going to claim a useful equipment reward.");
}

bool PlayerBotController::selectTopLevelGoal(Player& player, const Position& position, const char* decisionReason)
{
	if (requiresRookgaardDeparture(player)) {
		return forceOracleDeparture(player, position, decisionReason);
	}
	const GoalCandidate service = serviceGoalCandidate(player);
	PlayerBotOracleDeparturePlan departure;
	std::deque<PlayerBotNavigationStep> departureRoute;
	const bool departureEligible = player.getLevel() >= oracleMinimumLevel && player.getLevel() <= oracleMaximumLevel &&
	                               player.getVocation()->getId() == 0;
	const bool departureFound = departureEligible && findOracleDeparture(player, position, departure, departureRoute);
	const GoalCandidate departureCandidate{TopLevelGoal::Departure, departureFound,
	                                       departureFound ? oracleDepartureUtility : 0,
	                                       player.getVocation()->getId() != 0 ? "already_departed" :
	                                       player.getLevel() < oracleMinimumLevel ? "below_minimum_level" :
	                                       player.getLevel() > oracleMaximumLevel ? "above_maximum_level" :
	                                       departureFound ? "oracle_reachable" : "oracle_unreachable"};
	PlayerBotRewardPlan reward;
	std::deque<PlayerBotNavigationStep> rewardSteps;
	const auto now = std::chrono::steady_clock::now();
	const bool pickupCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::PickupReward, now);
	const bool pickupFound = !pickupCoolingDown && findPickupReward(player, position, reward, rewardSteps);
	const int32_t pickupUtility = pickupFound ?
		std::max<int32_t>(0, (reward.equipmentUpgradeCount != 0 ? pickupRewardBaseUtility : economicPickupBaseUtility) +
		                         static_cast<int32_t>(reward.knownUtility) - static_cast<int32_t>(reward.travelSteps)) : 0;
	const GoalCandidate pickup{TopLevelGoal::PickupReward, pickupFound, pickupUtility,
	                           pickupCoolingDown ? "cooldown" : pickupFound ? "useful_reachable_reward" : "no_useful_reward"};
	PlayerBotSpellTrainingPlan spellTraining;
	std::deque<PlayerBotNavigationStep> spellTrainingSteps;
	const bool spellTrainingCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::LearnSpell, now);
	const bool spellTrainingFound = !spellTrainingCoolingDown && findSpellTraining(player, position, spellTraining, spellTrainingSteps);
	const GoalCandidate learnSpell{TopLevelGoal::LearnSpell, spellTrainingFound,
	                               spellTrainingFound ? spellTrainingGoalUtility : 0,
	                               spellTrainingCoolingDown ? "cooldown" : spellTrainingFound ? "eligible_reachable_spell" :
	                                                          "no_eligible_spell"};
	const bool equipmentPurchaseCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::BuyEquipment, now);
	const std::optional<EquipmentOfferEvaluation> equipment = equipmentPurchaseCoolingDown ? std::nullopt :
	                                                        evaluateEquipmentOffers(player, position);
	const bool equipmentFound = fixtureRuntime.equipmentPurchasesEnabled() && equipment.has_value();
	const GoalCandidate buyEquipment{TopLevelGoal::BuyEquipment, equipmentFound,
	                                 equipmentFound ? equipmentPurchaseGoalUtility : 0,
	                                 equipmentPurchaseCoolingDown ? "cooldown" :
	                                 !fixtureRuntime.equipmentPurchasesEnabled() ? "shadow_only" :
	                                 equipmentFound ? PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule) : "no_justified_offer"};
	const bool magicTrainingCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::MagicTraining, now);
	const char* magicTrainingReason = magicTrainingCoolingDown ? "cooldown" :
	                                  survivalRuntime.magicTrainingReason(player, survivalSnapshot(player));
	const GoalCandidate magicTraining{TopLevelGoal::MagicTraining, !magicTrainingCoolingDown && magicTrainingReason == nullptr,
	                                  !magicTrainingCoolingDown && magicTrainingReason == nullptr ? magicTrainingGoalUtility : 0,
	                                  magicTrainingReason ? magicTrainingReason : "next_tick_overflow"};
	const GoalCandidate hunt{TopLevelGoal::Hunt, true, huntGoalUtility, "autonomous_hunting_available"};
	const PlayerBotGoalArbiter::GoalDecision decision = goalArbiter.decide({
		departureCandidate, service, pickup, learnSpell, buyEquipment, magicTraining, hunt,
	});
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Departure), decision.id, position, decisionReason, nullptr,
	                  departureFound ? &departure : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Service), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::PickupReward), decision.id, position, decisionReason, pickupFound ? &reward : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::LearnSpell), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::BuyEquipment), decision.id, position, decisionReason, nullptr, nullptr,
	                  equipmentFound ? &*equipment : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::MagicTraining), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Hunt), decision.id, position, decisionReason);
	if (fixtureRuntime.pauseAfterEquipmentStorageRejection()) {
		return false;
	}

	if (!decision.selected) {
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" +
		         jsonString(decisionReason) + ",\"result\":\"failed\",\"reason\":\"no_feasible_goal\"");
		stop("no_feasible_goal", position);
		return false;
	}
	const GoalCandidate& selected = *decision.selected;
	goalArbiter.apply(decision);
	std::ostringstream fields;
	fields << "\"decision_id\":" << decision.id << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"from_goal\":" << jsonString(PlayerBotGoalArbiter::goalName(decision.previousGoal))
	       << ",\"to_goal\":" << jsonString(PlayerBotGoalArbiter::goalName(selected.goal))
	       << ",\"utility\":" << selected.utility << ",\"reason\":" << jsonString(selected.reason);
	if (selected.goal == TopLevelGoal::Departure) {
		fields << ",\"npc_id\":" << departure.npcId << ",\"town_id\":" << oracleTownId
		       << ",\"vocation_id\":" << oracleVocationId;
	} else if (selected.goal == TopLevelGoal::PickupReward) {
		fields << ",\"candidate_id\":" << reward.uniqueId << ",\"item_id\":" << reward.itemId;
	} else if (selected.goal == TopLevelGoal::LearnSpell) {
		fields << ",\"npc_id\":" << spellTraining.npcId << ",\"spell\":" << jsonString(spellTraining.spellName)
		       << ",\"price\":" << spellTraining.price;
	} else if (selected.goal == TopLevelGoal::BuyEquipment) {
		fields << ",\"npc_id\":" << equipment->npcId << ",\"item_id\":" << equipment->itemId
		       << ",\"price\":" << equipment->price << ",\"rule\":"
	       << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule));
	}
	emit("goal_selection", position, fields.str());
	if (selected.goal == TopLevelGoal::Departure) {
		beginOracleDeparture(player, position, std::move(departure), std::move(departureRoute));
	} else if (selected.goal == TopLevelGoal::PickupReward) {
		beginPickupReward(player, position, std::move(reward), std::move(rewardSteps));
	} else if (selected.goal == TopLevelGoal::LearnSpell) {
		beginSpellTraining(player, position, std::move(spellTraining), std::move(spellTrainingSteps));
	} else if (selected.goal == TopLevelGoal::BuyEquipment) {
		beginEquipmentPurchase(player, position, *equipment);
	} else if (selected.goal == TopLevelGoal::Service) {
		beginService(&player, position, "goal_selected");
	} else if (selected.goal == TopLevelGoal::MagicTraining) {
		schedule(SCHEDULER_MINTICKS);
	} else {
		startHunt(&player, position, "goal_selected");
	}
	return true;
}

const char* PlayerBotController::objectiveName() const
{
	return progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture) ? "oracle_departure" :
	       progressionSession.active(PlayerBotProgressionProcedure::PickupReward) ? "pickup_reward" :
	       progressionSession.active(PlayerBotProgressionProcedure::LearnSpell) ? "learn_spell" :
	       progressionSession.active(PlayerBotProgressionProcedure::BuyEquipment) ? "buy_equipment" : cyclePhaseName();
}

void PlayerBotController::finishProgressionObjective(Player* player, const Position& position, const char* result, const char* reason,
                                 bool scheduleNext)
{
	const auto& reward = rewardSession.plan();
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"candidate_id\":" << reward.uniqueId
	       << ",\"item_id\":" << reward.itemId << ",\"result\":" << jsonString(result)
	       << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
	         ",\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"result\":" +
	         jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, std::string("Equipment reward objective ") + result + ": " + reason + '.');
	}
	progressionSession.reset();
	rewardSession.reset();
	clearNavigation();
	serviceStage = ServiceStage::Discover;
	npcSession.reset();
	cyclePhase = CyclePhase::Service;
	goalArbiter.setCooldown(TopLevelGoal::PickupReward, std::strcmp(result, "success") == 0 ? pickupRewardSuccessCooldown :
	                                                                                              pickupRewardFailureCooldown);
	if (fixtureRuntime.progressionEnabled() && player) {
		const char* decisionReason = std::strcmp(result, "success") == 0 ? "pickup_complete" :
		                             std::strcmp(result, "interrupted") == 0 ? "pickup_interrupted" : "pickup_failed";
		selectTopLevelGoal(*player, position, decisionReason);
	} else {
		goalArbiter.setActiveGoal(TopLevelGoal::Service);
		emit("objective_transition", position,
		     "\"from\":\"pickup_reward\",\"to\":\"service\",\"reason\":" + jsonString(reason));
	}
	if (scheduleNext) {
		schedule(SCHEDULER_MINTICKS);
	}
}

void PlayerBotController::processPickupReward(Player* player, const Position& currentPosition)
{
	const auto& pickupReward = rewardSession.plan();
	if (rewardSession.stage() == PlayerBotRewardStage::Travel) {
		if (!processNavigation(player, currentPosition, pickupReward.approachPosition)) {
			if (fixedTargetRouteFailureCount >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "route_unavailable");
			}
			return;
		}
		rewardSession.setStage(PlayerBotRewardStage::UseReward);
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (rewardSession.stage() == PlayerBotRewardStage::UseReward) {
		Item* rewardObject = g_game.getUniqueItem(pickupReward.uniqueId);
		Tile* tile = rewardObject ? rewardObject->getTile() : nullptr;
		const int32_t stackPosition = tile ? tile->getThingIndex(rewardObject) : -1;
		if (!rewardObject || !tile || tile->getPosition() != pickupReward.itemPosition ||
		    stackPosition < 0 || stackPosition > UINT8_MAX) {
			finishProgressionObjective(player, currentPosition, "failed", "reward_object_unavailable");
			return;
		}
		if (!Position::areInRange<1, 1, 0>(currentPosition, pickupReward.itemPosition)) {
			rewardSession.setStage(PlayerBotRewardStage::Travel);
			clearNavigation();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		rewardSession.claimSnapshot().itemCount = inventoryPolicy.inventoryItemCount(*player, pickupReward.itemId);
		rewardSession.claimSnapshot().rootCount = matchingRewardRootCount(*player, pickupReward.rootSignature);
		rewardSession.claimSnapshot().roots.clear();
		for (const std::string& signature : pickupReward.nonStackableRootSignatures) {
			rewardSession.claimSnapshot().roots.emplace(signature, matchingRewardRootCount(*player, signature));
		}
		rewardSession.claimSnapshot().stackables.clear();
		for (const auto& entry : pickupReward.stackableRootCounts) {
			rewardSession.claimSnapshot().stackables.emplace(entry.first, inventoryPolicy.inventoryItemCount(*player, entry.first));
		}
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, pickupReward.itemPosition, static_cast<uint8_t>(stackPosition), 0,
		                     rewardObject->getClientID());
		rewardSession.setStage(PlayerBotRewardStage::VerifyReward);
		schedule(navigationDecisionDelay(*player));
		return;
	}

	if (rewardSession.stage() == PlayerBotRewardStage::VerifyReward) {
		const uint32_t itemCount = inventoryPolicy.inventoryItemCount(*player, pickupReward.itemId);
		const uint32_t rootCount = matchingRewardRootCount(*player, pickupReward.rootSignature);
		const bool bundleAdded = allRewardRootsAdded(*player);
		if (!isRewardClaimed(*player, pickupReward.uniqueId) || itemCount <= rewardSession.claimSnapshot().itemCount || !bundleAdded) {
			if (rewardSession.incrementRetries() >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "claim_not_verified");
				return;
			}
			rewardSession.setStage(PlayerBotRewardStage::UseReward);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		emit("action_result", currentPosition,
		     "\"action\":\"claim_reward\",\"result\":\"success\",\"candidate_id\":" +
		         std::to_string(pickupReward.uniqueId) + ",\"item_id\":" + std::to_string(pickupReward.itemId) +
		         ",\"root_item_id\":" + std::to_string(pickupReward.rootItemId) +
		         ",\"inventory_before\":" + std::to_string(rewardSession.claimSnapshot().itemCount) +
		         ",\"inventory_after\":" + std::to_string(itemCount) +
		         ",\"root_count_before\":" + std::to_string(rewardSession.claimSnapshot().rootCount) +
		         ",\"root_count_after\":" + std::to_string(rootCount) +
		         ",\"top_level_root_count\":" + std::to_string(pickupReward.rootSignatures.size()) +
		         ",\"all_roots_verified\":true");
		rewardSession.resetRetries();
		if (pickupReward.slot == CONST_SLOT_WHEREEVER) {
			finishProgressionObjective(player, currentPosition, "success", "reward_bundle_claimed");
			return;
		}
		rewardSession.setStage(PlayerBotRewardStage::EquipReward);
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (rewardSession.stage() == PlayerBotRewardStage::EquipReward) {
		Item* equipped = player->getInventoryItem(pickupReward.slot);
		if (equipped && equipped->getID() == pickupReward.itemId) {
			finishProgressionObjective(player, currentPosition, "success", "reward_equipped");
			return;
		}
		if (equipped && equipped->getID() == pickupReward.rootItemId &&
		    !pickupReward.selectedItemPath.empty() &&
		    resolveRewardPath(equipped, pickupReward.selectedItemPath, pickupReward.selectedItemPath.size())) {
			if (rewardSession.retries() >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "reward_root_move_not_verified");
				return;
			}
			Container* backpack = playerBackpack(*player);
			if (!backpack || backpack->size() >= backpack->capacity()) {
				finishProgressionObjective(player, currentPosition, "failed", "insufficient_reward_root_space");
				return;
			}
			if (!player->canDoAction()) {
				schedule(navigationDecisionDelay(*player));
				return;
			}
			Position rootPosition;
			uint8_t rootIndex = 0;
			g_game.internalGetPosition(equipped, rootPosition, rootIndex);
			rewardSession.incrementRetries();
			telemetry.recordActionAttempt();
			emit("action_result", currentPosition,
			     "\"action\":\"relocate_reward_root\",\"result\":\"requested\",\"item_id\":" +
			         std::to_string(equipped->getID()) + ",\"slot\":" + std::to_string(pickupReward.slot));
			g_game.playerMoveItem(player, rootPosition, equipped->getClientID(), rootIndex,
			                      Position(0xFFFF, 0x40 | backpackContainerId,
			                               static_cast<uint8_t>(backpack->size())),
			                      equipped->getItemCount(), equipped, backpack);
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Item* reward = nullptr;
		std::string accessFailure;
		if (!prepareRewardItemAccess(*player, currentPosition, reward, accessFailure)) {
			if (!accessFailure.empty()) {
				finishProgressionObjective(player, currentPosition, "failed", accessFailure.c_str());
			}
			return;
		}
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		Item* displaced = nullptr;
		slots_t displacedSlot = CONST_SLOT_WHEREEVER;
		for (const auto& entry : {std::pair<slots_t, uint16_t>{pickupReward.slot, pickupReward.replacedItemId},
		                         {CONST_SLOT_LEFT, pickupReward.displacedLeftItemId},
		                         {CONST_SLOT_RIGHT, pickupReward.displacedRightItemId}}) {
			Item* equippedItem = entry.second == 0 ? nullptr : player->getInventoryItem(entry.first);
			if (equippedItem && equippedItem->getID() == entry.second && equippedItem != reward) {
				displaced = equippedItem;
				displacedSlot = entry.first;
				break;
			}
		}
		if (displaced) {
			if (rewardSession.retries() >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "displaced_item_move_not_verified");
				return;
			}
			Position displacedPosition;
			uint8_t displacedIndex = 0;
			g_game.internalGetPosition(displaced, displacedPosition, displacedIndex);
			rewardSession.incrementRetries();
			telemetry.recordActionAttempt();
			emit("action_result", currentPosition,
			     "\"action\":\"preserve_displaced_equipment\",\"result\":\"requested\",\"item_id\":" +
			         std::to_string(displaced->getID()) + ",\"slot\":" + std::to_string(displacedSlot));
			Container* backpack = playerBackpack(*player);
			if (!backpack || backpack->size() >= backpack->capacity()) {
				finishProgressionObjective(player, currentPosition, "failed", "insufficient_displaced_item_space");
				return;
			}
			g_game.playerMoveItem(player, displacedPosition, displaced->getClientID(), displacedIndex,
			                      Position(0xFFFF, 0x40 | backpackContainerId, static_cast<uint8_t>(backpack->size())),
			                      displaced->getItemCount(), displaced, nullptr);
			schedule(navigationDecisionDelay(*player));
			return;
		}

		Position sourcePosition;
		uint8_t sourceIndex = 0;
		g_game.internalGetPosition(reward, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF) {
			finishProgressionObjective(player, currentPosition, "failed", "reward_inventory_position_unavailable");
			return;
		}
		telemetry.recordActionAttempt();
		emit("action_result", currentPosition,
		     "\"action\":\"equip_reward\",\"result\":\"requested\",\"item_id\":" +
		         std::to_string(reward->getID()) + ",\"slot\":" + std::to_string(pickupReward.slot) +
		         ",\"source_y\":" + std::to_string(sourcePosition.y) +
		         ",\"source_z\":" + std::to_string(sourcePosition.z) +
		         ",\"source_index\":" + std::to_string(sourceIndex));
		g_game.playerMoveItem(player, sourcePosition, reward->getClientID(), sourceIndex,
		                      Position(0xFFFF, pickupReward.slot, 0), reward->getItemCount(), reward, nullptr);
		rewardSession.setStage(PlayerBotRewardStage::VerifyEquipment);
		schedule(navigationDecisionDelay(*player));
		return;
	}

	Item* equipped = player->getInventoryItem(pickupReward.slot);
	const bool displacedPreserved = std::all_of(rewardSession.displacedItemCounts().begin(),
	                                           rewardSession.displacedItemCounts().end(),
	                                           [this, player](const auto& entry) {
			                                           return inventoryPolicy.inventoryItemCount(*player, entry.first) >= entry.second;
	                                           });
	if (!equipped || equipped->getID() != pickupReward.itemId || !displacedPreserved) {
		if (rewardSession.incrementRetries() >= maximumProgressionAttempts) {
			finishProgressionObjective(player, currentPosition, "failed",
			                           displacedPreserved ? "equip_not_verified" : "displaced_item_lost");
			return;
		}
		rewardSession.setStage(PlayerBotRewardStage::EquipReward);
		schedule(navigationDecisionDelay(*player));
		return;
	}
	std::ostringstream fields;
	fields << "\"action\":\"equip\",\"result\":\"success\",\"item_id\":" << pickupReward.itemId
	       << ",\"slot\":" << static_cast<int32_t>(pickupReward.slot)
	       << ",\"displaced_item_id\":" << pickupReward.replacedItemId
	       << ",\"metric\":" << jsonString(pickupReward.metric)
	       << ",\"value_before\":" << pickupReward.currentValue
	       << ",\"value_after\":" << pickupReward.candidateValue;
	emit("action_result", currentPosition, fields.str());
	finishProgressionObjective(player, currentPosition, "success", "reward_equipped");
}

void PlayerBotController::processProgression(Player* player, const Position& currentPosition)
{
	if (progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture)) {
		processOracleDeparture(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::PickupReward)) {
		processPickupReward(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::LearnSpell)) {
		processSpellTraining(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::BuyEquipment)) {
		processEquipmentPurchase(player, currentPosition);
	}
}
