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

// Goal arbitration and reward discovery, claiming, and equipment.
using namespace playerbot;

std::optional<PlayerBotController::EquipmentUpgrade> PlayerBotController::evaluateEquipmentUpgrade(const Player& player, const Item& candidate) const
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
		}
	}
	if (candidateValue <= currentValue) {
		return std::nullopt;
	}
	return EquipmentUpgrade{slot, candidateValue - currentValue, metric, currentValue, candidateValue};
}

bool PlayerBotController::requiresKnightCombatReadiness(const Player& player) const
{
	return player.getVocationId() == oracleVocationId;
}

bool PlayerBotController::isLegalEquipmentItem(const Player& player, const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	if (!item.isPickupable() || player.getLevel() < type.minReqLevel ||
	    player.getMagicLevel() < type.minReqMagicLevel ||
	    ((type.wieldInfo & WIELDINFO_PREMIUM) != 0 && !player.isPremium())) {
		return false;
	}
	return type.vocationIds.empty() || type.vocationIds.find(player.getVocationId()) != type.vocationIds.end();
}

bool PlayerBotController::isKnightMeleeWeapon(const Player& player, const Item& item) const
{
	const WeaponType_t weaponType = item.getWeaponType();
	const int32_t slots = item.getSlotPosition();
	return isLegalEquipmentItem(player, item) && item.getAttack() > 0 &&
	       (weaponType == WEAPON_SWORD || weaponType == WEAPON_CLUB || weaponType == WEAPON_AXE) &&
	       (slots & (SLOTP_LEFT | SLOTP_RIGHT)) != 0;
}

bool PlayerBotController::isCombatEquipment(const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	return (type.slotPosition & (SLOTP_HEAD | SLOTP_ARMOR | SLOTP_LEGS | SLOTP_FEET)) != 0 ||
	       type.weaponType != WEAPON_NONE || item.getArmor() > 0 || item.getDefense() > 0;
}

bool PlayerBotController::isProtectedInventoryItem(const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	return type.isContainer() || isCombatEquipment(item) || item.getID() == ropeItemId || item.getID() == 2554 ||
	       item.getID() == meatItemId || item.getID() == smallHealthPotionItemId || item.getWorth() != 0 ||
	       itemSellValues.find(item.getID()) == itemSellValues.end();
}

bool PlayerBotController::isCombatReady(const Player& player, std::string& recovery, std::string& terminalReason) const
{
	recovery.clear();
	terminalReason.clear();
	if (!requiresKnightCombatReadiness(player)) {
		return true;
	}
	Item* left = player.getInventoryItem(CONST_SLOT_LEFT);
	Item* right = player.getInventoryItem(CONST_SLOT_RIGHT);
	Item* armor = player.getInventoryItem(CONST_SLOT_ARMOR);
	Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const bool weaponReady = (left && isKnightMeleeWeapon(player, *left)) || (right && isKnightMeleeWeapon(player, *right));
	const bool armorReady = armor && isLegalEquipmentItem(player, *armor) &&
	                        (armor->getSlotPosition() & SLOTP_ARMOR) != 0 && armor->getArmor() > 0;
	const bool loadoutReady = backpack && backpack->getContainer();
	const bool suppliesReady = getInventoryItemCount(player, smallHealthPotionItemId) >= minimumSmallHealthPotions &&
	                           getInventoryItemCount(player, meatItemId) >= minimumMeat;
	const bool capacityReady = player.getFreeCapacity() >= returnCapacityThreshold;
	Item* upgrade = nullptr;
	EquipmentUpgrade upgradeInfo{};
	if (findCarriedEquipmentUpgrade(const_cast<Player&>(player), upgrade, upgradeInfo)) {
		recovery = "equip_carried";
		return false;
	}
	if (weaponReady && armorReady && loadoutReady && suppliesReady && capacityReady) {
		return true;
	}
	if (!weaponReady) {
		terminalReason = "missing_legal_melee_weapon";
	} else if (!armorReady) {
		terminalReason = "missing_legal_armor";
	} else if (!loadoutReady) {
		terminalReason = "missing_backpack";
	} else {
		recovery = "service";
	}
	return false;
}

void PlayerBotController::emitCombatReadiness(const Player& player, const Position& position, const char* result,
                                              const std::string& recovery, const std::string& terminalReason) const
{
	Item* left = player.getInventoryItem(CONST_SLOT_LEFT);
	Item* right = player.getInventoryItem(CONST_SLOT_RIGHT);
	Item* armor = player.getInventoryItem(CONST_SLOT_ARMOR);
	Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const bool weaponReady = (left && isKnightMeleeWeapon(player, *left)) || (right && isKnightMeleeWeapon(player, *right));
	const bool armorReady = armor && isLegalEquipmentItem(player, *armor) && armor->getArmor() > 0;
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
	          (getInventoryItemCount(player, smallHealthPotionItemId) >= minimumSmallHealthPotions ? "true" : "false")
	       << ",\"count\":" << getInventoryItemCount(player, smallHealthPotionItemId)
	       << ",\"minimum\":" << minimumSmallHealthPotions << '}'
	       << ",{\"name\":\"food\",\"ready\":" << (getInventoryItemCount(player, meatItemId) >= minimumMeat ? "true" : "false")
	       << ",\"count\":" << getInventoryItemCount(player, meatItemId) << ",\"minimum\":" << minimumMeat << '}'
	       << ",{\"name\":\"free_capacity\",\"ready\":" << (player.getFreeCapacity() >= returnCapacityThreshold ? "true" : "false")
	       << ",\"current\":" << player.getFreeCapacity() << ",\"minimum\":" << returnCapacityThreshold << "}]"
	       << ",\"selected_recovery\":" << (recovery.empty() ? "null" : jsonString(recovery))
	       << ",\"terminal_reason\":" << (terminalReason.empty() ? "null" : jsonString(terminalReason));
	emit("combat_readiness", position, fields.str());
}

bool PlayerBotController::findCarriedEquipmentUpgrade(Player& player, Item*& selectedItem, EquipmentUpgrade& selectedUpgrade) const
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
			std::optional<EquipmentUpgrade> upgrade = evaluateEquipmentUpgrade(player, *candidate);
			if (!upgrade || (requiresKnightCombatReadiness(player) &&
			                 candidate->getWeaponType() != WEAPON_NONE && !isKnightMeleeWeapon(player, *candidate))) {
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

bool PlayerBotController::beginReadinessEquipment(Player* player, const Position& position, const char* reason)
{
	Item* item = nullptr;
	EquipmentUpgrade upgrade{};
	if (!player || !findCarriedEquipmentUpgrade(*player, item, upgrade) || !player->canDoAction()) {
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
		++counters.actionsAttempted;
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
	++counters.actionsAttempted;
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
		std::string recovery;
		std::string terminalReason;
		const bool ready = isCombatReady(*player, recovery, terminalReason);
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"success\",\"item_id\":" +
		     std::to_string(pendingReadinessItemId) + ",\"slot\":" + std::to_string(pendingReadinessSlot));
		emitCombatReadiness(*player, position, ready ? "ready" : "recovery", recovery, terminalReason);
		readinessEquipmentPending = false;
		pendingReadinessAttempts = 0;
		if (ready) {
			startHunt(player, position, "readiness_carried_upgrade");
			return;
		}
		ensureCombatReady(player, position, "readiness_upgrade_incomplete");
		return;
	}
	if (++pendingReadinessAttempts >= maximumProgressionAttempts) {
		std::string recovery;
		std::string terminalReason;
		isCombatReady(*player, recovery, terminalReason);
		readinessEquipmentPending = false;
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"failed\",\"reason\":\"move_not_verified\"");
		emitCombatReadiness(*player, position, "failed", recovery, terminalReason);
		if (!terminalReason.empty()) {
			stop(("combat_readiness_" + terminalReason).c_str(), position);
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
	if (!player || !requiresKnightCombatReadiness(*player)) {
		return true;
	}
	std::string recovery;
	std::string terminalReason;
	if (isCombatReady(*player, recovery, terminalReason)) {
		if (std::strcmp(reason, "readiness_continuous_check") != 0) {
			emitCombatReadiness(*player, position, "ready", {}, {});
		}
		return true;
	}
	emitCombatReadiness(*player, position, "recovery", recovery, terminalReason);
	if (recovery == "equip_carried") {
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
	if (recovery == "service") {
		beginService(player, position, "combat_readiness_service");
		schedule(navigationInterval);
		return false;
	}
	stop(("combat_readiness_" + terminalReason).c_str(), position);
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

void PlayerBotController::inspectRewardItem(const Player& player, const Item& item, uint16_t rootOrdinal,
                       std::vector<uint16_t>& path, const std::string& rootSignature,
                       RewardInspection& inspection) const
{
	RewardItemInspection inspected{item.getID(), item.getItemCount(), static_cast<uint32_t>(path.size()),
	                              rootOrdinal, path};
	++inspection.itemCount;
	if (item.getContainer()) {
		inspected.classes.emplace_back("container");
		++inspection.containerCount;
	}
	if (std::optional<EquipmentUpgrade> upgrade = evaluateEquipmentUpgrade(player, item)) {
		inspected.classes.emplace_back("equipment_upgrade");
		++inspection.equipmentUpgradeCount;
		if (!inspection.bestUpgrade || upgrade->benefit > inspection.bestUpgrade->benefit) {
			inspection.bestUpgrade = upgrade;
			inspection.bestItemId = item.getID();
			inspection.bestRootOrdinal = rootOrdinal;
			inspection.bestItemPath = path;
			inspection.bestRootSignature = rootSignature;
		}
	}
	inspected.worth = item.getWorth();
	if (inspected.worth != 0) {
		inspected.classes.emplace_back("currency");
		inspection.currencyValue += inspected.worth;
	}
	if (item.getID() == smallHealthPotionItemId) {
		inspected.classes.emplace_back("required_supply");
		inspection.potionCount += item.getItemCount();
	} else if (item.getID() == meatItemId) {
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
	const uint32_t sellPrice = inspected.worth == 0 && sellIt != itemSellValues.end() ? sellIt->second : 0;
	if (sellPrice != 0) {
		inspected.classes.emplace_back("sellable");
		inspected.sellValue = sellPrice * item.getItemCount();
		inspection.sellValue += inspected.sellValue;
	}
	uint32_t itemUtility = inspected.worth + inspected.sellValue;
	if (item.getID() == smallHealthPotionItemId) {
		itemUtility += missingPotionUtility * item.getItemCount();
	} else if (item.getID() == meatItemId) {
		itemUtility += missingFoodUtility * item.getItemCount();
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
			inspectRewardItem(player, *child, rootOrdinal, path, rootSignature, inspection);
			path.pop_back();
		}
	}
}

PlayerBotController::RewardInspection PlayerBotController::inspectRewardBundle(Player& player, const Container& contents) const
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
		inspectRewardItem(player, *root, rootOrdinal++, path, signature, inspection);
	}
	finalizeRewardInspection(player, inspection);
	return inspection;
}

PlayerBotController::RewardInspection PlayerBotController::inspectKnownReward(Player& player, const Item& item) const
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
	inspectRewardItem(player, item, 0, path, signature, inspection);
	finalizeRewardInspection(player, inspection);
	return inspection;
}

void PlayerBotController::finalizeRewardInspection(Player& player, RewardInspection& inspection) const
{
	if (inspection.bestUpgrade) {
		inspection.knownUtility += inspection.bestUpgrade->benefit * 20;
	}
	inspection.knownUtility += static_cast<int32_t>(inspection.currencyValue + inspection.sellValue);
	const uint32_t heldPotions = getInventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t heldFood = getInventoryItemCount(player, meatItemId);
	const uint32_t potionNeed = heldPotions < minimumSmallHealthPotions ? minimumSmallHealthPotions - heldPotions : 0;
	const uint32_t foodNeed = heldFood < minimumMeat ? minimumMeat - heldFood : 0;
	inspection.knownUtility += static_cast<int32_t>(std::min(inspection.potionCount, potionNeed) * missingPotionUtility +
	                                                  std::min(inspection.foodCount, foodNeed) * missingFoodUtility);
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

int32_t PlayerBotController::estimatedPickupUtility(const PickupReward& reward) const
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
	for (const std::string& signature : pickupReward.nonStackableRootSignatures) {
		++expectedCounts[signature];
	}
	for (const auto& entry : expectedCounts) {
		auto beforeIt = pendingRewardRootCounts.find(entry.first);
		const uint32_t before = beforeIt == pendingRewardRootCounts.end() ? 0 : beforeIt->second;
		if (matchingRewardRootCount(player, entry.first) < before + entry.second) {
			return false;
		}
	}
	for (const auto& entry : pickupReward.stackableRootCounts) {
		auto beforeIt = pendingRewardStackableCounts.find(entry.first);
		const uint32_t before = beforeIt == pendingRewardStackableCounts.end() ? 0 : beforeIt->second;
		if (getInventoryItemCount(player, entry.first) < before + entry.second) {
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

	Item* root = findMatchingRewardRoot(player, pickupReward.rootSignature);
	if (!root) {
		failure = "reward_bundle_unavailable";
		return false;
	}
	if (pickupReward.selectedItemPath.size() > maximumContainerId - rewardContainerIdBase + 1) {
		failure = "reward_container_depth_unsupported";
		return false;
	}
	for (size_t depth = 0; depth < pickupReward.selectedItemPath.size(); ++depth) {
		Item* ancestor = resolveRewardPath(root, pickupReward.selectedItemPath, depth);
		Container* container = ancestor ? ancestor->getContainer() : nullptr;
		if (!container) {
			failure = "reward_container_path_invalid";
			return false;
		}
		const uint8_t containerId = static_cast<uint8_t>(rewardContainerIdBase + depth);
		if (player.getContainerByID(containerId) == container) {
			if (pendingRewardContainerDepth == depth) {
				pendingRewardContainerDepth = SIZE_MAX;
				pendingRewardContainerOpenAttempts = 0;
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
		if (sourcePosition.x != 0xFFFF || (sourcePosition.y & 0x40) == 0) {
			failure = "reward_container_position_unavailable";
			return false;
		}
		if (pendingRewardContainerDepth != depth) {
			pendingRewardContainerDepth = depth;
			pendingRewardContainerOpenAttempts = 0;
		}
		if (++pendingRewardContainerOpenAttempts >= maximumProgressionAttempts) {
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
	selectedItem = resolveRewardPath(root, pickupReward.selectedItemPath, pickupReward.selectedItemPath.size());
	if (!selectedItem || selectedItem->getID() != pickupReward.itemId) {
		failure = "reward_item_path_invalid";
		return false;
	}
	return true;
}

bool PlayerBotController::isRookgaardRewardPosition(const Position& position) const
{
	return position.z < MAP_MAX_LAYERS &&
	       Position::getDistanceX(position, rookgaardTemplePosition) <= rookgaardRewardRadius &&
	       Position::getDistanceY(position, rookgaardTemplePosition) <= rookgaardRewardRadius;
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
			++counters.pathfindingCalls;
			const auto startedAt = std::chrono::steady_clock::now();
			const PlayerBotNavigationResult planResult = candidate == currentPosition ? PlayerBotNavigationResult::Reached :
				navigator.plan(player, candidate, {}, steps, candidateExpandedNodes);
			const bool planned = planResult == PlayerBotNavigationResult::Reached;
			counters.pathfindingTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - startedAt).count();
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

void PlayerBotController::emitRewardCandidate(const PickupReward& candidate, const Position& position, const char* result,
                         const char* reason) const
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << candidate.uniqueId
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
	       << ",\"result\":\"inspected\",\"recursive\":true"
	       << ",\"known_utility\":" << inspection.knownUtility
	       << ",\"item_count\":" << inspection.itemCount
	       << ",\"container_count\":" << inspection.containerCount
	       << ",\"unknown_count\":" << inspection.unknownCount
	       << ",\"currency_value\":" << inspection.currencyValue
	       << ",\"sell_value\":" << inspection.sellValue
	       << ",\"equipment_upgrade_count\":" << inspection.equipmentUpgradeCount
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

bool PlayerBotController::findPickupReward(Player& player, const Position& position, PickupReward& reward,
                      std::deque<PlayerBotNavigationStep>& rewardSteps)
{
	std::optional<PickupReward> claimedUpgrade;
	std::vector<PickupReward> unclaimedCandidates;
	std::deque<PlayerBotNavigationStep> selectedSteps;
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
		if (!tile || !isRookgaardRewardPosition(tile->getPosition()) ||
		    (containerReward && (!contents || contents->empty()))) {
			continue;
		}

		std::unique_ptr<Item> knownReward;
		if (doubletReward) {
			knownReward.reset(Item::CreateItem(doubletItemId));
			if (!knownReward) {
				continue;
			}
		}
		RewardInspection inspection = containerReward ? inspectRewardBundle(player, *contents) :
		                                                inspectKnownReward(player, *knownReward);
		if (inspection.itemCount != 0) {
			emitRewardInspection(rewardObject->getUniqueId(), tile->getPosition(), inspection, position);
		}
		if (inspection.knownUtility <= 0 || inspection.rootSignatures.empty()) {
			continue;
		}
		uint32_t totalWeight = 0;
		if (containerReward) {
			for (Item* reward : contents->getItemList()) {
				totalWeight += reward->getWeight();
			}
		} else {
			totalWeight = knownReward->getWeight();
		}

		PickupReward candidate;
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
		candidate.estimatedDistance = navigationDistance(position, candidate.itemPosition);
		candidate.requiredBackpackSlots = (containerReward ? static_cast<uint32_t>(contents->size()) : 1) +
		                                  (inspection.bestUpgrade && player.getInventoryItem(candidate.slot) ? 1 : 0);
		if (isRewardClaimed(player, candidate.uniqueId)) {
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

	std::optional<PickupReward> selected;
	bool resumeEquipment = false;
	if (claimedUpgrade) {
		selected = claimedUpgrade;
		resumeEquipment = true;
	} else {
		std::sort(unclaimedCandidates.begin(), unclaimedCandidates.end(), [this](const PickupReward& left,
		                                                                        const PickupReward& right) {
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
		for (PickupReward candidate : unclaimedCandidates) {
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

const char* PlayerBotController::topLevelGoalName(TopLevelGoal goal) const
{
	switch (goal) {
		case TopLevelGoal::Departure: return "oracle_departure";
		case TopLevelGoal::Service: return "service";
		case TopLevelGoal::PickupReward: return "pickup_reward";
		case TopLevelGoal::LearnSpell: return "learn_spell";
		case TopLevelGoal::Hunt: return "hunt";
	}
	return "unknown";
}

uint32_t PlayerBotController::saleableItemCount(const Player& player) const
{
	uint32_t count = 0;
	for (const auto& entry : itemSellValues) {
		count += getSaleItemCount(player, entry.first);
	}
	return count;
}

PlayerBotController::GoalCandidate PlayerBotController::serviceGoalCandidate(const Player& player) const
{
	const uint32_t potionCount = getInventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t meatCount = getInventoryItemCount(player, meatItemId);
	const uint32_t missingPotions = potionCount < minimumSmallHealthPotions ?
	                                  minimumSmallHealthPotions - potionCount : 0;
	const uint32_t missingMeat = meatCount < minimumMeat ? minimumMeat - meatCount : 0;
	const uint32_t sellable = saleableItemCount(player);
	const bool lowCapacity = player.getFreeCapacity() < returnCapacityThreshold;
	const bool criticalHealing = needsHealing(player) && missingPotions != 0;
	const bool cashAdjustment = player.getMoney() != carriedGoldReserve;
	const bool feasible = lowCapacity || missingPotions != 0 || missingMeat != 0 || sellable != 0 || cashAdjustment;
	int32_t utility = feasible ? serviceGoalBaseUtility : 0;
	utility += static_cast<int32_t>(missingPotions * missingPotionUtility + missingMeat * missingFoodUtility +
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
	                     missingMeat != 0 ? "food_reserve" : sellable != 0 ? "sellable_inventory" :
	                     cashAdjustment ? "cash_reserve" : "no_service_need";
	return GoalCandidate{TopLevelGoal::Service, feasible, utility, reason};
}

void PlayerBotController::emitGoalCandidate(const Player& player, const GoalCandidate& candidate, const Position& position, const char* decisionReason,
	                       const PickupReward* reward, const DeparturePlan* departure) const
{
	std::ostringstream fields;
	const bool evaluated = candidate.reason != "deferred_lower_utility";
	fields << "\"decision_id\":" << goalDecisionId << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"goal\":" << jsonString(topLevelGoalName(candidate.goal))
	       << ",\"evaluated\":" << (evaluated ? "true" : "false")
	       << ",\"feasible\":" << (candidate.feasible ? "true" : "false")
	       << ",\"utility\":" << candidate.utility << ",\"reason\":" << jsonString(candidate.reason);
	if (candidate.goal == TopLevelGoal::Service) {
		fields << ",\"potion_count\":" << getInventoryItemCount(player, smallHealthPotionItemId)
		       << ",\"meat_count\":" << getInventoryItemCount(player, meatItemId)
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
	emit("goal_candidate", position, fields.str());
}

void PlayerBotController::beginPickupReward(Player& player, const Position& position, PickupReward reward,
                       std::deque<PlayerBotNavigationStep> rewardSteps)
{
	pickupReward = std::move(reward);
	progressionObjective = ProgressionObjective::PickupReward;
	progressionStage = pickupReward.resumeEquipment ? ProgressionStage::EquipReward : ProgressionStage::Travel;
	progressionAttempts = 0;
	pendingRewardContainerDepth = SIZE_MAX;
	pendingRewardContainerOpenAttempts = 0;
	if (!pickupReward.resumeEquipment) {
		navigationTarget = pickupReward.approachPosition;
		navigationSteps = std::move(rewardSteps);
	}
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << pickupReward.uniqueId
	       << ",\"reason\":" << jsonString(pickupReward.resumeEquipment ? "resume_claimed_upgrade" :
	                                                                    "highest_known_utility_reachable_reward")
	       << ",\"item_id\":" << pickupReward.itemId << ",\"root_item_id\":" << pickupReward.rootItemId
	       << ",\"benefit\":" << pickupReward.benefit << ",\"known_utility\":" << pickupReward.knownUtility
	       << ",\"travel_steps\":" << pickupReward.travelSteps;
	emit("strategy_selection", position, fields.str());
	say(player, pickupReward.resumeEquipment ? "Equipping a previously claimed reward." :
	                                         "Going to claim a useful equipment reward.");
}

bool PlayerBotController::selectTopLevelGoal(Player& player, const Position& position, const char* decisionReason)
{
	++goalDecisionId;
	if (requiresRookgaardDeparture(player)) {
		return forceOracleDeparture(player, position, decisionReason);
	}
	const GoalCandidate service = serviceGoalCandidate(player);
	DeparturePlan departure;
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
	PickupReward reward;
	std::deque<PlayerBotNavigationStep> rewardSteps;
	const auto now = std::chrono::steady_clock::now();
	const bool pickupCoolingDown = pickupRewardCooldownUntil > now;
	const bool pickupFound = !pickupCoolingDown && findPickupReward(player, position, reward, rewardSteps);
	const int32_t pickupUtility = pickupFound ?
		std::max<int32_t>(0, (reward.equipmentUpgradeCount != 0 ? pickupRewardBaseUtility : economicPickupBaseUtility) +
		                         static_cast<int32_t>(reward.knownUtility) - static_cast<int32_t>(reward.travelSteps)) : 0;
	const GoalCandidate pickup{TopLevelGoal::PickupReward, pickupFound, pickupUtility,
	                           pickupCoolingDown ? "cooldown" : pickupFound ? "useful_reachable_reward" : "no_useful_reward"};
	SpellTrainingPlan spellTraining;
	std::deque<PlayerBotNavigationStep> spellTrainingSteps;
	const bool spellTrainingCoolingDown = spellTrainingCooldownUntil > now;
	const bool spellTrainingFound = !spellTrainingCoolingDown && findSpellTraining(player, position, spellTraining, spellTrainingSteps);
	const GoalCandidate learnSpell{TopLevelGoal::LearnSpell, spellTrainingFound,
	                               spellTrainingFound ? spellTrainingGoalUtility : 0,
	                               spellTrainingCoolingDown ? "cooldown" : spellTrainingFound ? "eligible_reachable_spell" :
	                                                          "no_eligible_spell"};
	const bool higherUtilityGoal = (departureCandidate.feasible && departureCandidate.utility > huntGoalUtility) ||
	                               (service.feasible && service.utility > huntGoalUtility) ||
	                               (pickup.feasible && pickup.utility > huntGoalUtility) ||
	                               (learnSpell.feasible && learnSpell.utility > huntGoalUtility);
	const bool huntFeasible = !higherUtilityGoal;
	const GoalCandidate hunt{TopLevelGoal::Hunt, huntFeasible, huntGoalUtility,
	                         higherUtilityGoal ? "deferred_lower_utility" :
	                         huntFeasible ? "autonomous_hunting_available" : "no_suitable_reachable_region"};
	emitGoalCandidate(player, departureCandidate, position, decisionReason, nullptr,
	                  departureFound ? &departure : nullptr);
	emitGoalCandidate(player, service, position, decisionReason);
	emitGoalCandidate(player, pickup, position, decisionReason, pickupFound ? &reward : nullptr);
	emitGoalCandidate(player, learnSpell, position, decisionReason);
	emitGoalCandidate(player, hunt, position, decisionReason);

	const GoalCandidate* selected = nullptr;
	const std::array<const GoalCandidate*, 5> candidates = {&departureCandidate, &service, &pickup, &learnSpell, &hunt};
	for (const GoalCandidate* candidate : candidates) {
		if (candidate->feasible && (!selected || candidate->utility > selected->utility)) {
			selected = candidate;
		}
	}
	if (!selected) {
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(goalDecisionId) + ",\"decision_reason\":" +
		         jsonString(decisionReason) + ",\"result\":\"failed\",\"reason\":\"no_feasible_goal\"");
		stop("no_feasible_goal", position);
		return false;
	}
	const TopLevelGoal previousGoal = activeGoal;
	activeGoal = selected->goal;
	std::ostringstream fields;
	fields << "\"decision_id\":" << goalDecisionId << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"from_goal\":" << jsonString(topLevelGoalName(previousGoal))
	       << ",\"to_goal\":" << jsonString(topLevelGoalName(activeGoal))
	       << ",\"utility\":" << selected->utility << ",\"reason\":" << jsonString(selected->reason);
	if (selected->goal == TopLevelGoal::Departure) {
		fields << ",\"npc_id\":" << departure.npcId << ",\"town_id\":" << oracleTownId
		       << ",\"vocation_id\":" << oracleVocationId;
	} else if (selected->goal == TopLevelGoal::PickupReward) {
		fields << ",\"candidate_id\":" << reward.uniqueId << ",\"item_id\":" << reward.itemId;
	} else if (selected->goal == TopLevelGoal::LearnSpell) {
		fields << ",\"npc_id\":" << spellTraining.npcId << ",\"spell\":" << jsonString(spellTraining.spellName)
		       << ",\"price\":" << spellTraining.price;
	}
	emit("goal_selection", position, fields.str());
	if (selected->goal == TopLevelGoal::Departure) {
		beginOracleDeparture(player, position, std::move(departure), std::move(departureRoute));
	} else if (selected->goal == TopLevelGoal::PickupReward) {
		beginPickupReward(player, position, std::move(reward), std::move(rewardSteps));
	} else if (selected->goal == TopLevelGoal::LearnSpell) {
		beginSpellTraining(player, position, std::move(spellTraining), std::move(spellTrainingSteps));
	} else if (selected->goal == TopLevelGoal::Service) {
		beginService(&player, position, "goal_selected");
	} else {
		startHunt(&player, position, "goal_selected");
	}
	return true;
}

const char* PlayerBotController::objectiveName() const
{
	return progressionObjective == ProgressionObjective::OracleDeparture ? "oracle_departure" :
	       progressionObjective == ProgressionObjective::PickupReward ? "pickup_reward" :
	       progressionObjective == ProgressionObjective::LearnSpell ? "learn_spell" : cyclePhaseName();
}

void PlayerBotController::finishProgressionObjective(Player* player, const Position& position, const char* result, const char* reason,
                                bool scheduleNext)
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << pickupReward.uniqueId
	       << ",\"item_id\":" << pickupReward.itemId << ",\"result\":" << jsonString(result)
	       << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalDecisionId) + ",\"goal\":\"pickup_reward\",\"result\":" +
	         jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, std::string("Equipment reward objective ") + result + ": " + reason + '.');
	}
	progressionObjective = ProgressionObjective::None;
	progressionStage = ProgressionStage::Travel;
	pickupReward = PickupReward{};
	progressionAttempts = 0;
	clearNavigation();
	serviceStage = ServiceStage::Discover;
	conversationStep = ConversationStep::Greet;
	serviceTargetId = 0;
	cyclePhase = CyclePhase::Service;
	pickupRewardCooldownUntil = std::chrono::steady_clock::now() +
	                            (std::strcmp(result, "success") == 0 ? pickupRewardSuccessCooldown :
	                                                                    pickupRewardFailureCooldown);
	if (testPolicy.progressionEnabled && player) {
		const char* decisionReason = std::strcmp(result, "success") == 0 ? "pickup_complete" :
		                             std::strcmp(result, "interrupted") == 0 ? "pickup_interrupted" : "pickup_failed";
		selectTopLevelGoal(*player, position, decisionReason);
	} else {
		activeGoal = TopLevelGoal::Service;
		emit("objective_transition", position,
		     "\"from\":\"pickup_reward\",\"to\":\"service\",\"reason\":" + jsonString(reason));
	}
	if (scheduleNext) {
		schedule(SCHEDULER_MINTICKS);
	}
}

void PlayerBotController::processPickupReward(Player* player, const Position& currentPosition)
{
	if (progressionStage == ProgressionStage::Travel) {
		if (!processNavigation(player, currentPosition, pickupReward.approachPosition)) {
			if (fixedTargetRouteFailureCount >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "route_unavailable");
			}
			return;
		}
		progressionStage = ProgressionStage::UseReward;
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (progressionStage == ProgressionStage::UseReward) {
		Item* rewardObject = g_game.getUniqueItem(pickupReward.uniqueId);
		Tile* tile = rewardObject ? rewardObject->getTile() : nullptr;
		const int32_t stackPosition = tile ? tile->getThingIndex(rewardObject) : -1;
		if (!rewardObject || !tile || tile->getPosition() != pickupReward.itemPosition ||
		    stackPosition < 0 || stackPosition > UINT8_MAX) {
			finishProgressionObjective(player, currentPosition, "failed", "reward_object_unavailable");
			return;
		}
		if (!Position::areInRange<1, 1, 0>(currentPosition, pickupReward.itemPosition)) {
			progressionStage = ProgressionStage::Travel;
			clearNavigation();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (!player->canDoAction()) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		pendingRewardItemCount = getInventoryItemCount(*player, pickupReward.itemId);
		pendingRewardRootCount = matchingRewardRootCount(*player, pickupReward.rootSignature);
		pendingRewardRootCounts.clear();
		for (const std::string& signature : pickupReward.nonStackableRootSignatures) {
			pendingRewardRootCounts.emplace(signature, matchingRewardRootCount(*player, signature));
		}
		pendingRewardStackableCounts.clear();
		for (const auto& entry : pickupReward.stackableRootCounts) {
			pendingRewardStackableCounts.emplace(entry.first, getInventoryItemCount(*player, entry.first));
		}
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, pickupReward.itemPosition, static_cast<uint8_t>(stackPosition), 0,
		                     rewardObject->getClientID());
		progressionStage = ProgressionStage::VerifyReward;
		schedule(navigationDecisionDelay(*player));
		return;
	}

	if (progressionStage == ProgressionStage::VerifyReward) {
		const uint32_t itemCount = getInventoryItemCount(*player, pickupReward.itemId);
		const uint32_t rootCount = matchingRewardRootCount(*player, pickupReward.rootSignature);
		const bool bundleAdded = allRewardRootsAdded(*player);
		if (!isRewardClaimed(*player, pickupReward.uniqueId) || itemCount <= pendingRewardItemCount || !bundleAdded) {
			if (++progressionAttempts >= maximumProgressionAttempts) {
				finishProgressionObjective(player, currentPosition, "failed", "claim_not_verified");
				return;
			}
			progressionStage = ProgressionStage::UseReward;
			schedule(navigationDecisionDelay(*player));
			return;
		}
		emit("action_result", currentPosition,
		     "\"action\":\"claim_reward\",\"result\":\"success\",\"candidate_id\":" +
		         std::to_string(pickupReward.uniqueId) + ",\"item_id\":" + std::to_string(pickupReward.itemId) +
		         ",\"root_item_id\":" + std::to_string(pickupReward.rootItemId) +
		         ",\"inventory_before\":" + std::to_string(pendingRewardItemCount) +
		         ",\"inventory_after\":" + std::to_string(itemCount) +
		         ",\"root_count_before\":" + std::to_string(pendingRewardRootCount) +
		         ",\"root_count_after\":" + std::to_string(rootCount) +
		         ",\"top_level_root_count\":" + std::to_string(pickupReward.rootSignatures.size()) +
		         ",\"all_roots_verified\":true");
		progressionAttempts = 0;
		if (pickupReward.slot == CONST_SLOT_WHEREEVER) {
			finishProgressionObjective(player, currentPosition, "success", "reward_bundle_claimed");
			return;
		}
		progressionStage = ProgressionStage::EquipReward;
		schedule(SCHEDULER_MINTICKS);
		return;
	}

	if (progressionStage == ProgressionStage::EquipReward) {
		Item* equipped = player->getInventoryItem(pickupReward.slot);
		if (equipped && equipped->getID() == pickupReward.itemId) {
			finishProgressionObjective(player, currentPosition, "success", "reward_equipped");
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

		Position sourcePosition;
		uint8_t sourceIndex = 0;
		g_game.internalGetPosition(reward, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF) {
			finishProgressionObjective(player, currentPosition, "failed", "reward_inventory_position_unavailable");
			return;
		}
		Item* previous = player->getInventoryItem(pickupReward.slot);
		pendingEquipmentItemId = previous ? previous->getID() : 0;
		pendingEquipmentItemCount = pendingEquipmentItemId == 0 ? 0 :
		                            getInventoryItemCount(*player, pendingEquipmentItemId);
		++counters.actionsAttempted;
		g_game.playerMoveItem(player, sourcePosition, reward->getClientID(), sourceIndex,
		                      Position(0xFFFF, pickupReward.slot, 0), reward->getItemCount(), reward, nullptr);
		progressionStage = ProgressionStage::VerifyEquipment;
		schedule(navigationDecisionDelay(*player));
		return;
	}

	Item* equipped = player->getInventoryItem(pickupReward.slot);
	const bool displacedPreserved = pendingEquipmentItemId == 0 ||
	                                 getInventoryItemCount(*player, pendingEquipmentItemId) >= pendingEquipmentItemCount;
	if (!equipped || equipped->getID() != pickupReward.itemId || !displacedPreserved) {
		if (++progressionAttempts >= maximumProgressionAttempts) {
			finishProgressionObjective(player, currentPosition, "failed",
			                           displacedPreserved ? "equip_not_verified" : "displaced_item_lost");
			return;
		}
		progressionStage = ProgressionStage::EquipReward;
		schedule(navigationDecisionDelay(*player));
		return;
	}
	std::ostringstream fields;
	fields << "\"action\":\"equip\",\"result\":\"success\",\"item_id\":" << pickupReward.itemId
	       << ",\"slot\":" << static_cast<int32_t>(pickupReward.slot)
	       << ",\"displaced_item_id\":" << pendingEquipmentItemId
	       << ",\"metric\":" << jsonString(pickupReward.metric)
	       << ",\"value_before\":" << pickupReward.currentValue
	       << ",\"value_after\":" << pickupReward.candidateValue;
	emit("action_result", currentPosition, fields.str());
	finishProgressionObjective(player, currentPosition, "success", "reward_equipped");
}

void PlayerBotController::processProgression(Player* player, const Position& currentPosition)
{
	if (progressionObjective == ProgressionObjective::OracleDeparture) {
		processOracleDeparture(player, currentPosition);
	} else if (progressionObjective == ProgressionObjective::PickupReward) {
		processPickupReward(player, currentPosition);
	} else if (progressionObjective == ProgressionObjective::LearnSpell) {
		processSpellTraining(player, currentPosition);
	}
}
