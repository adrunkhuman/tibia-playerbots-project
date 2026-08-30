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

#include "player.h"
#include "playerbotinventorypolicy.h"

#include "container.h"

using namespace playerbot;

uint32_t playerbot::recoveryPotionRouteReserve(uint16_t vocationId, int32_t maximumHealth,
	                                            uint32_t routeDangerCost, uint32_t healthLossCost)
{
	if (maximumHealth <= 0 || routeDangerCost == 0 || healthLossCost == 0) return healthPotionReturnThreshold;
	const uint64_t expectedHealthLoss =
		(static_cast<uint64_t>(maximumHealth) * routeDangerCost + healthLossCost - 1) / healthLossCost;
	const uint32_t minimumHealing = static_cast<uint32_t>(std::max(1, recoveryPotionMinimumHealing(vocationId)));
	const uint64_t routePotions = (expectedHealthLoss + minimumHealing - 1) / minimumHealing;
	return static_cast<uint32_t>(std::min<uint64_t>(
		std::numeric_limits<uint32_t>::max(), std::max<uint64_t>(healthPotionReturnThreshold, routePotions)));
}

uint32_t playerbot::recoveryPotionRestockTargetForReserve(uint32_t returnReserve)
{
	return std::max(healthPotionRestockTarget,
	                returnReserve == std::numeric_limits<uint32_t>::max() ? returnReserve : returnReserve + 1);
}

PlayerBotInventoryPolicy::PlayerBotInventoryPolicy(const PlayerBotInventoryPolicy::SellValues& sellValues,
                                                    EquipmentUpgradePredicate equipmentUpgrade) :
	sellValues(sellValues), equipmentUpgrade(std::move(equipmentUpgrade))
{
}

uint32_t PlayerBotInventoryPolicy::inventoryItemCount(const Player& player, uint16_t itemId) const
{
	return static_cast<const Cylinder&>(player).getItemTypeCount(itemId);
}

uint64_t PlayerBotInventoryPolicy::desiredCarriedGold(const Player& player) const
{
	return std::min<uint64_t>(carriedGoldReserve, player.getMoney() + player.getBankBalance());
}

bool PlayerBotInventoryPolicy::isFoodItem(uint16_t itemId)
{
	return itemId == 2362 || (itemId >= 2666 && itemId <= 2691) || (itemId >= 2695 && itemId <= 2696) ||
	       (itemId >= 2787 && itemId <= 2796) || itemId == 5097 || itemId == 6125 ||
	       (itemId >= 6278 && itemId <= 6279) || (itemId >= 6393 && itemId <= 6394) || itemId == 6501 ||
	       (itemId >= 6541 && itemId <= 6545) || itemId == 6569 || itemId == 6574 ||
	       (itemId >= 7158 && itemId <= 7159) || (itemId >= 7372 && itemId <= 7377) ||
	       (itemId >= 7909 && itemId <= 7910) || itemId == 7963 || itemId == 8112 ||
	       (itemId >= 8838 && itemId <= 8845) || itemId == 8847 || itemId == 9005 || itemId == 9114 ||
	       itemId == 11246 || itemId == 11370 || itemId == 11429 ||
	       (itemId >= 12415 && itemId <= 12418) || (itemId >= 12637 && itemId <= 12639);
}

bool PlayerBotInventoryPolicy::isCurrencyItem(uint16_t itemId)
{
	return Item::items[itemId].worth != 0;
}

PlayerBotFoodInventory PlayerBotInventoryPolicy::foodInventory(const Player& player) const
{
	uint64_t count = 0;
	uint64_t weight = 0;
	std::function<void(const Item&)> inspect = [&](const Item& item) {
		if (isFoodItem(item.getID())) {
			count += item.getItemCount();
			weight += static_cast<uint64_t>(item.getItemCount()) * item.getBaseWeight();
		}
		if (const Container* container = item.getContainer()) {
			for (const Item* child : container->getItemList()) {
				inspect(*child);
			}
		}
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			inspect(*item);
		}
	}
	return {static_cast<uint32_t>(std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max())),
	        static_cast<uint32_t>(std::min<uint64_t>(weight, std::numeric_limits<uint32_t>::max()))};
}

uint32_t PlayerBotInventoryPolicy::effectiveFreeCapacity(const Player& player) const
{
	return static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(player.getFreeCapacity()) + foodInventory(player).weight,
	    std::numeric_limits<uint32_t>::max()));
}

uint32_t PlayerBotInventoryPolicy::currencyInventoryWeight(const Player& player) const
{
	uint64_t weight = 0;
	std::function<void(const Item&)> inspect = [&](const Item& item) {
		if (isCurrencyItem(item.getID())) {
			weight += static_cast<uint64_t>(item.getItemCount()) * item.getBaseWeight();
		}
		if (const Container* container = item.getContainer()) {
			for (const Item* child : container->getItemList()) inspect(*child);
		}
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) inspect(*item);
	}
	return static_cast<uint32_t>(std::min<uint64_t>(weight, std::numeric_limits<uint32_t>::max()));
}

uint32_t PlayerBotInventoryPolicy::huntFreeCapacity(const Player& player) const
{
	return static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(effectiveFreeCapacity(player)) + currencyInventoryWeight(player),
	    std::numeric_limits<uint32_t>::max()));
}

uint32_t PlayerBotInventoryPolicy::itemUnitValue(uint16_t itemId) const
{
	const ItemType& type = Item::items[itemId];
	if (type.worth != 0) {
		return type.worth;
	}
	auto it = sellValues.find(itemId);
	return it == sellValues.end() ? 0 : it->second;
}

uint32_t PlayerBotInventoryPolicy::protectedItemReserve(const Player& player, uint16_t itemId) const
{
	if (itemId == ropeItemId || itemId == 2554) {
		return 1;
	}
	if (isFoodItem(itemId)) {
		return preferredFoodCount;
	}
	if (itemId == recoveryPotionItemId(player.getVocationId())) {
		return healthPotionRestockTarget;
	}
	return 0;
}

bool PlayerBotInventoryPolicy::isProtectedInventoryItem(const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	return type.isContainer() || item.getID() == ropeItemId || item.getID() == 2554 ||
	       ((type.slotPosition & SLOTP_TWO_HAND) != 0 && type.weaponType != WEAPON_NONE) ||
	       isFoodItem(item.getID()) || item.getID() == smallHealthPotionItemId || item.getID() == healthPotionItemId || item.getWorth() != 0 ||
	       sellValues.find(item.getID()) == sellValues.end();
}

bool PlayerBotInventoryPolicy::isProtectedDepositItem(const Player& player, const Item& item) const
{
	if (item.getID() == smallHealthPotionItemId || item.getID() == healthPotionItemId) {
		return false;
	}
	const ItemType& type = Item::items[item.getID()];
	return (type.isContainer() && type.corpseType == RACE_NONE) || item.getID() == ropeItemId || item.getID() == 2554 ||
	       ((type.slotPosition & SLOTP_TWO_HAND) != 0 && type.weaponType != WEAPON_NONE) ||
	       equipmentUpgrade(player, item) || item.getWorth() != 0 || sellValues.find(item.getID()) == sellValues.end();
}

uint32_t PlayerBotInventoryPolicy::backpackSaleItemCount(const Player& player, uint16_t itemId) const
{
	if (isFoodItem(itemId)) {
		return 0;
	}
	const ItemType& type = Item::items[itemId];
	if ((type.isContainer() && type.corpseType == RACE_NONE) || type.isFluidContainer() || type.isSplash()) {
		return 0;
	}
	if ((type.slotPosition & SLOTP_TWO_HAND) != 0 && type.weaponType != WEAPON_NONE) {
		return 0;
	}
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		return 0;
	}
	uint32_t count = 0;
	for (Item* item : backpack->getItemList()) {
		if (item->getID() == itemId) {
			if (equipmentUpgrade(player, *item)) {
				return 0;
			}
			const Container* container = item->getContainer();
			if (container && !container->empty()) {
				return 0;
			}
			count += item->getItemCount();
		}
	}
	uint32_t removableCount = 0;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* inventoryItem = player.getInventoryItem(static_cast<slots_t>(slot));
		Container* container = inventoryItem ? inventoryItem->getContainer() : nullptr;
		if (!container) {
			continue;
		}
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			Item* nestedItem = *it;
			if (nestedItem->getID() == itemId) {
				if (equipmentUpgrade(player, *nestedItem)) {
					return 0;
				}
				removableCount += nestedItem->getItemCount();
			}
		}
	}
	if (removableCount != count) {
		return 0;
	}
	const uint32_t reserve = protectedItemReserve(player, itemId);
	return count > reserve ? count - reserve : 0;
}

bool PlayerBotInventoryPolicy::isItemValidForSlot(const Item& item, slots_t slot)
{
	uint32_t slotPosition = 0;
	switch (slot) {
		case CONST_SLOT_HEAD: slotPosition = SLOTP_HEAD; break;
		case CONST_SLOT_NECKLACE: slotPosition = SLOTP_NECKLACE; break;
		case CONST_SLOT_BACKPACK: slotPosition = SLOTP_BACKPACK; break;
		case CONST_SLOT_ARMOR: slotPosition = SLOTP_ARMOR; break;
		case CONST_SLOT_RIGHT: slotPosition = SLOTP_RIGHT; break;
		case CONST_SLOT_LEFT: slotPosition = SLOTP_LEFT; break;
		case CONST_SLOT_LEGS: slotPosition = SLOTP_LEGS; break;
		case CONST_SLOT_FEET: slotPosition = SLOTP_FEET; break;
		case CONST_SLOT_RING: slotPosition = SLOTP_RING; break;
		case CONST_SLOT_AMMO: slotPosition = SLOTP_AMMO; break;
		default: return false;
	}
	return (item.getSlotPosition() & slotPosition) != 0;
}

bool PlayerBotInventoryPolicy::isActionableSlottedItem(const Player& player, const Item& item, slots_t slot,
	                                                     uint16_t itemId) const
{
	return slot != CONST_SLOT_BACKPACK && (itemId == 0 || item.getID() == itemId) &&
	       !isItemValidForSlot(item, slot) && !isProtectedInventoryItem(item) &&
	       !isProtectedDepositItem(player, item);
}
