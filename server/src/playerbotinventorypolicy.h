/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTINVENTORYPOLICY_H
#define FS_PLAYERBOTINVENTORYPOLICY_H

#include <cstdint>
#include <functional>
#include <map>

class Player;
class Item;
enum slots_t : uint8_t;

namespace playerbot {
	inline constexpr uint16_t smallHealthPotionItemId = 8704;
	inline constexpr uint16_t healthPotionItemId = 7618;
	inline constexpr uint32_t healthPotionReturnThreshold = 1;
	inline constexpr uint32_t healthPotionRestockTarget = 10;
	inline constexpr uint32_t preferredFoodCount = 2;
	inline constexpr uint16_t ropeItemId = 2120;
	inline constexpr uint32_t carriedGoldReserve = 100;

	constexpr uint16_t recoveryPotionItemId(uint16_t vocationId)
	{
		return vocationId == 0 ? smallHealthPotionItemId : healthPotionItemId;
	}

	constexpr int32_t recoveryPotionMinimumHealing(uint16_t vocationId)
	{
		return vocationId == 0 ? 60 : 125;
	}

	constexpr int32_t recoveryPotionMaximumHealing(uint16_t vocationId)
	{
		return vocationId == 0 ? 90 : 175;
	}

	uint32_t recoveryPotionRouteReserve(uint16_t vocationId, int32_t maximumHealth,
	                                   uint32_t routeDangerCost, uint32_t healthLossCost = 1000);
	uint32_t recoveryPotionRestockTargetForReserve(uint32_t returnReserve);

	struct PlayerBotFoodInventory {
		uint32_t count = 0;
		uint32_t weight = 0;
	};

	// Item ownership and disposition rules; it has no scheduling or goal state.
	class PlayerBotInventoryPolicy
	{
		public:
			using SellValues = std::map<uint16_t, uint32_t>;
			using EquipmentUpgradePredicate = std::function<bool(const Player&, const Item&)>;

			PlayerBotInventoryPolicy(const SellValues& sellValues, EquipmentUpgradePredicate equipmentUpgrade);

			uint32_t inventoryItemCount(const Player& player, uint16_t itemId) const;
			uint64_t desiredCarriedGold(const Player& player) const;
			static bool isFoodItem(uint16_t itemId);
			PlayerBotFoodInventory foodInventory(const Player& player) const;
			uint32_t effectiveFreeCapacity(const Player& player) const;
			uint32_t itemUnitValue(uint16_t itemId) const;
			uint32_t protectedItemReserve(const Player& player, uint16_t itemId) const;
			bool isProtectedInventoryItem(const Item& item) const;
			bool isProtectedDepositItem(const Player& player, const Item& item) const;
			uint32_t backpackSaleItemCount(const Player& player, uint16_t itemId) const;
			static bool isItemValidForSlot(const Item& item, slots_t slot);
			bool isActionableSlottedItem(const Player& player, const Item& item, slots_t slot,
			                             uint16_t itemId = 0) const;

		private:
			const SellValues& sellValues;
			EquipmentUpgradePredicate equipmentUpgrade;
	};
}

#endif
