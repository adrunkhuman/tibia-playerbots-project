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

// Corpse access, loot selection, and capacity-aware cargo replacement.
using namespace playerbot;

void PlayerBotController::logLootSuccess(uint16_t itemId, uint32_t count, uint32_t inventoryCount, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"loot\",\"result\":\"success\",\"item_id\":" << itemId
	       << ",\"count\":" << count << ",\"inventory_count\":" << inventoryCount
	       << ",\"unit_value\":" << inventoryPolicy.itemUnitValue(itemId)
	       << ",\"total_value\":" << static_cast<uint64_t>(inventoryPolicy.itemUnitValue(itemId)) * count
	       << ",\"unit_weight\":" << Item::items[itemId].weight;
	emit("action_result", position, fields.str());
}

void PlayerBotController::beginLoot(Player* player, const Position& currentPosition)
{
	if (activeHuntRegion) {
		++huntRegionKills;
	}
	lootTargetId = ratId;
	corpseDeathPosition = ratPosition;
	clearRatTarget(currentPosition, "target_defeated");
	lootPosition = corpseDeathPosition;
	corpseSearchAttempts = 0;
	corpseOpenAttempts = 0;
	corpseNavigationFailures = 0;
	consecutiveCorpseNavigationFailures = 0;
	corpseNavigationSuspensions = 0;
	corpseNavigationFailurePosition = currentPosition;
	corpseLootStarted = std::chrono::steady_clock::now();
	corpseNavigationRetryAt = {};
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	lootedCurrentCorpse = false;
	corpseObserved = false;
	corpseNavigationSuspended = false;
	unavailableLootItemIds.clear();
	if (!expectedCorpseLootable) {
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_not_lootable\""
		       << ",\"expected_corpse_item_id\":" << expectedCorpseItemId;
		emit("action_result", currentPosition, fields.str());
		finishLoot(player, currentPosition);
		return;
	}
	setStage(ScenarioStage::LootCorpse, currentPosition);
}

void PlayerBotController::finishLoot(Player* player, const Position& currentPosition)
{
	player->closeContainer(corpseContainerId);
	clearNavigation();
	pendingLootItemId = 0;
	pendingDiscardItemId = 0;
	expectedCorpseItemId = 0;
	expectedCorpseLootable = false;
	lootTargetId = 0;
	corpseNavigationSuspended = false;
	corpseObserved = false;
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::finishLootFailure(Player* player, const Position& currentPosition, const char* reason)
{
	++counters.actionsFailed;
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - corpseLootStarted).count();
	std::ostringstream fields;
	fields << "\"action\":\"loot\",\"result\":\"failed\",\"reason\":" << jsonString(reason)
	       << ",\"target_id\":" << lootTargetId
	       << ",\"expected_corpse_item_id\":" << expectedCorpseItemId
	       << ",\"last_known_death_position\":{\"x\":" << corpseDeathPosition.x << ",\"y\":" << corpseDeathPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(corpseDeathPosition.z) << '}'
	       << ",\"corpse_position\":";
	if (corpseObserved) {
		fields << "{\"x\":" << lootPosition.x << ",\"y\":" << lootPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(lootPosition.z) << '}';
	} else {
		fields << "null";
	}
	fields << ",\"search_attempts\":" << corpseSearchAttempts
	       << ",\"navigation_failures\":" << corpseNavigationFailures
	       << ",\"navigation_suspensions\":" << corpseNavigationSuspensions
	       << ",\"elapsed_ms\":" << elapsed;
	emit("action_result", currentPosition, fields.str());
	finishLoot(player, currentPosition);
}

Container* PlayerBotController::findCorpse(Player* player, const Position& searchPosition)
{
	Container* fallback = nullptr;
	Position fallbackPosition;
	for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
		for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
			Position position(searchPosition.x + offsetX, searchPosition.y + offsetY, searchPosition.z);
			Tile* tile = g_game.map.getTile(position);
			TileItemVector* items = tile ? tile->getItemList() : nullptr;
			if (!items) {
				continue;
			}

			for (auto it = items->getBeginDownItem(); it != items->getEndDownItem(); ++it) {
				Item* item = *it;
				Container* corpse = item->getContainer();
				if (!corpse || Item::items[item->getID()].corpseType == RACE_NONE) {
					continue;
				}
				const uint32_t corpseOwner = corpse->getCorpseOwner();
				if (corpseOwner != 0 && !player->canOpenCorpse(corpseOwner)) {
					continue;
				}
				if (position == searchPosition) {
					lootPosition = position;
					return corpse;
				}
				if (!fallback) {
					fallback = corpse;
					fallbackPosition = position;
				}
			}
		}
	}
	if (fallback) {
		lootPosition = fallbackPosition;
	}
	return fallback;
}

uint8_t PlayerBotController::backpackDestinationIndex(const Container& backpack, const Item& item) const
{
	if (item.isStackable()) {
		const ItemDeque& items = backpack.getItemList();
		for (size_t index = 0; index < items.size(); ++index) {
			if (items[index]->getID() == item.getID() && items[index]->getItemCount() < 100) {
				return static_cast<uint8_t>(index);
			}
		}
	}
	return static_cast<uint8_t>(backpack.size());
}

bool PlayerBotController::isReplaceableCargo(const Item& item) const
{
	const ItemType& type = Item::items[item.getID()];
	const bool food = PlayerBotInventoryPolicy::isFoodItem(item.getID());
	const bool protectedItem = inventoryPolicy.isProtectedInventoryItem(item) && !food;
	return !protectedItem && type.corpseType == RACE_NONE && (food || inventoryPolicy.itemUnitValue(item.getID()) != 0) &&
	       item.getBaseWeight() != 0;
}

bool PlayerBotController::chooseCargoReplacement(const Container& backpack, const Item& incoming, uint8_t incomingCount,
                            uint32_t freeCapacity,
                            CargoCandidate& replacement, uint8_t& replacementCount) const
{
	const uint32_t incomingWeight = incoming.getBaseWeight() * incomingCount;
	if (incomingWeight <= freeCapacity || incomingWeight == 0) {
		return false;
	}

	std::vector<CargoCandidate> candidates;
	std::function<void(Container&)> collect = [&](Container& source) {
		const ItemDeque& items = source.getItemList();
		for (size_t index = 0; index < items.size() && index <= UINT8_MAX; ++index) {
			Item* item = items[index];
			if (isReplaceableCargo(*item)) {
				candidates.push_back({item, &source, static_cast<uint8_t>(index), inventoryPolicy.itemUnitValue(item->getID()),
				                      item->getBaseWeight(), item->getItemCount()});
			}
			if (Container* nested = item->getContainer()) {
				collect(*nested);
			}
		}
	};
	collect(const_cast<Container&>(backpack));
	std::sort(candidates.begin(), candidates.end(), [](const CargoCandidate& left, const CargoCandidate& right) {
		const uint64_t leftDensity = static_cast<uint64_t>(left.unitValue) * right.unitWeight;
		const uint64_t rightDensity = static_cast<uint64_t>(right.unitValue) * left.unitWeight;
		return leftDensity == rightDensity ? left.item->getID() < right.item->getID() : leftDensity < rightDensity;
	});

	uint32_t requiredWeight = incomingWeight - freeCapacity;
	uint64_t totalDiscardedValue = 0;
	bool selected = false;
	for (const CargoCandidate& candidate : candidates) {
		const uint32_t count = std::min(candidate.availableCount,
		                                (requiredWeight + candidate.unitWeight - 1) / candidate.unitWeight);
		if (count == 0) {
			continue;
		}
		if (!selected) {
			replacement = candidate;
			replacementCount = static_cast<uint8_t>(count);
			selected = true;
		}
		totalDiscardedValue += static_cast<uint64_t>(count) * candidate.unitValue;
		const uint32_t releasedWeight = count * candidate.unitWeight;
		if (releasedWeight >= requiredWeight) {
			requiredWeight = 0;
			break;
		}
		requiredWeight -= releasedWeight;
	}

	const uint64_t incomingValue = static_cast<uint64_t>(inventoryPolicy.itemUnitValue(incoming.getID())) * incomingCount;
	if (!selected || requiredWeight != 0 || incomingValue <= totalDiscardedValue) {
		return false;
	}
	return true;
}

void PlayerBotController::discardCargoForLoot(Player* player, Container* backpack, Item* incoming, uint8_t incomingCount,
                                               const Position& currentPosition)
{
	CargoCandidate replacement{};
	uint8_t replacementCount = 0;
	if (!chooseCargoReplacement(*backpack, *incoming, incomingCount, player->getFreeCapacity(), replacement,
	                            replacementCount)) {
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_capacity\""
		       << ",\"item_id\":" << incoming->getID() << ",\"count\":" << incoming->getItemCount()
		       << ",\"unit_value\":" << inventoryPolicy.itemUnitValue(incoming->getID())
		       << ",\"weight\":" << incoming->getWeight() << ",\"free_capacity\":" << player->getFreeCapacity();
		emit("action_result", currentPosition, fields.str());
		unavailableLootItemIds.insert(incoming->getID());
		return;
	}

	Tile* destination = g_game.map.getTile(currentPosition);
	if (!destination || !player->canDoAction()) {
		return;
	}
	if (player->getContainerID(replacement.source) < 0 &&
	    !openContainer(*player, *replacement.source, rewardContainerIdBase, currentPosition)) {
		return;
	}
	const int8_t sourceContainerId = player->getContainerID(replacement.source);
	if (sourceContainerId < 0) {
		return;
	}
	pendingDiscardItemId = replacement.item->getID();
	pendingDiscardCount = replacementCount;
	pendingDiscardInventoryCount = inventoryPolicy.inventoryItemCount(*player, pendingDiscardItemId);
	pendingDiscardValue = replacementCount * replacement.unitValue;
	pendingDiscardIncomingItemId = incoming->getID();
	const Position fromPosition(0xFFFF, 0x40 | static_cast<uint8_t>(sourceContainerId), replacement.index);
	++counters.actionsAttempted;
	g_game.playerMoveItem(player, fromPosition, replacement.item->getClientID(), replacement.index,
	                      currentPosition, replacementCount, replacement.item, destination);
}

void PlayerBotController::verifyPendingLootMoves(Player* player, const Position& currentPosition)
{
	if (pendingLootItemId != 0) {
		const uint32_t inventoryCount = inventoryPolicy.inventoryItemCount(*player, pendingLootItemId);
		if (inventoryCount > pendingLootInventoryCount) {
			logLootSuccess(pendingLootItemId, inventoryCount - pendingLootInventoryCount, inventoryCount, currentPosition);
			lootedCurrentCorpse = true;
		} else {
			unavailableLootItemIds.insert(pendingLootItemId);
			logActionFailure("loot", "item_move_failed", currentPosition);
		}
		pendingLootItemId = 0;
	}
	if (pendingDiscardItemId != 0) {
		const uint32_t inventoryCount = inventoryPolicy.inventoryItemCount(*player, pendingDiscardItemId);
		if (inventoryCount + pendingDiscardCount <= pendingDiscardInventoryCount) {
			std::ostringstream fields;
			fields << "\"action\":\"loot_replace\",\"result\":\"success\",\"discarded_item_id\":"
			       << pendingDiscardItemId << ",\"discarded_count\":" << static_cast<uint32_t>(pendingDiscardCount)
			       << ",\"discarded_value\":" << pendingDiscardValue
			       << ",\"incoming_item_id\":" << pendingDiscardIncomingItemId
			       << ",\"incoming_unit_value\":" << inventoryPolicy.itemUnitValue(pendingDiscardIncomingItemId);
			emit("action_result", currentPosition, fields.str());
		} else {
			logActionFailure("loot_replace", "discard_not_verified", currentPosition);
			unavailableLootItemIds.insert(pendingDiscardIncomingItemId);
		}
		pendingDiscardItemId = 0;
	}
}

void PlayerBotController::lootCorpse(Player* player, const Position& currentPosition)
{
	verifyPendingLootMoves(player, currentPosition);
	Container* corpse = player->getContainerByID(corpseContainerId);
	if (!corpse || Item::items[corpse->getID()].corpseType == RACE_NONE) {
		corpse = findCorpse(player, lootPosition);
	}
	if (corpse) {
		corpseObserved = true;
	} else if (corpseObserved) {
		finishLootFailure(player, currentPosition, "corpse_expired");
		schedule(navigationInterval);
		return;
	}
	if (!Position::areInRange<1, 1, 0>(currentPosition, lootPosition)) {
		const auto now = std::chrono::steady_clock::now();
		if (now - corpseLootStarted >= corpseLootTimeout) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		if (corpseNavigationSuspended) {
			if (currentPosition != corpseNavigationFailurePosition || now >= corpseNavigationRetryAt) {
				corpseNavigationSuspended = false;
				consecutiveCorpseNavigationFailures = 0;
				clearNavigation();
				emit("navigation_progress", currentPosition,
				     "\"result\":\"resumed\",\"reason\":\"corpse_retry\",\"navigation_failures\":" +
				         std::to_string(corpseNavigationFailures));
			} else {
				const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(corpseNavigationRetryAt - now).count();
				schedule(static_cast<uint32_t>(std::max<int64_t>(SCHEDULER_MINTICKS, delay)));
				return;
			}
		}
		if (currentPosition != corpseNavigationFailurePosition) {
			corpseNavigationFailurePosition = currentPosition;
			consecutiveCorpseNavigationFailures = 0;
		}
		const uint64_t pathfindingFailuresBefore = counters.pathfindingFailures;
		const uint32_t blockedStepsBefore = navigationSession.stepFailureCount();
		processNavigation(player, currentPosition, lootPosition);
		if (counters.pathfindingFailures > pathfindingFailuresBefore ||
		    navigationSession.stepFailureCount() > blockedStepsBefore) {
			++corpseNavigationFailures;
			++consecutiveCorpseNavigationFailures;
			corpseNavigationFailurePosition = currentPosition;
			if (corpseNavigationFailures >= maximumCorpseNavigationFailures) {
				finishLootFailure(player, currentPosition, "corpse_inaccessible");
				return;
			}
			if (consecutiveCorpseNavigationFailures >= corpseNavigationSuspendThreshold) {
				corpseNavigationSuspended = true;
				corpseNavigationRetryAt = std::chrono::steady_clock::now() +
				                          std::chrono::milliseconds(corpseNavigationRetryInterval);
				++corpseNavigationSuspensions;
				clearNavigation();
				emit("navigation_progress", currentPosition,
				     "\"result\":\"suspended\",\"reason\":\"corpse_route_unchanged\",\"navigation_failures\":" +
				         std::to_string(corpseNavigationFailures));
			}
		}
		return;
	}
	schedule(navigationInterval);

	if (!corpse) {
		if (++corpseSearchAttempts >= maxCorpseSearchAttempts) {
			logActionFailure("loot", "owned_corpse_unavailable", currentPosition);
			finishLoot(player, currentPosition);
		}
		return;
	}

	if (player->getContainerByID(corpseContainerId) != corpse) {
		if (!player->canDoAction()) {
			return;
		}
		if (++corpseOpenAttempts > 2) {
			logActionFailure("loot", "corpse_open_failed", currentPosition);
			finishLoot(player, currentPosition);
			return;
		}
		player->closeContainer(corpseContainerId);
		Tile* tile = g_game.map.getTile(lootPosition);
		const int32_t stackPosition = tile ? tile->getThingIndex(corpse) : -1;
		if (stackPosition < 0 || stackPosition > UINT8_MAX) {
			return;
		}
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, lootPosition, static_cast<uint8_t>(stackPosition), corpseContainerId, corpse->getClientID());
		return;
	}

	if (corpse->empty()) {
		if (!lootedCurrentCorpse) {
			std::ostringstream fields;
			fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_empty\""
			       << ",\"corpse_item_id\":" << corpse->getID() << ",\"corpse_owner_id\":" << corpse->getCorpseOwner()
			       << ",\"corpse_position\":{\"x\":" << lootPosition.x << ",\"y\":" << lootPosition.y
			       << ",\"z\":" << static_cast<uint16_t>(lootPosition.z) << '}';
			emit("action_result", currentPosition, fields.str());
		}
		finishLoot(player, currentPosition);
		return;
	}

	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	if (!backpack) {
		logActionFailure("loot", "backpack_unavailable", currentPosition);
		finishLoot(player, currentPosition);
		return;
	}

	if (player->getContainerByID(backpackContainerId) != backpack) {
		if (!player->canDoAction()) {
			return;
		}
		const int8_t existingContainerId = player->getContainerID(backpack);
		if (existingContainerId >= 0) {
			player->closeContainer(static_cast<uint8_t>(existingContainerId));
		}
		const Position backpackPosition(0xFFFF, CONST_SLOT_BACKPACK, 0);
		++counters.actionsAttempted;
		g_game.playerUseItem(playerId, backpackPosition, 0, backpackContainerId, backpack->getClientID());
		return;
	}

	Item* lootItem = nullptr;
	uint8_t lootIndex = 0;
	const uint32_t heldFood = inventoryPolicy.foodInventory(*player).count;
	bool skippedSurplusFood = false;
	uint16_t skippedFoodItemId = 0;
	const ItemDeque& corpseItems = corpse->getItemList();
	for (size_t index = 0; index < corpseItems.size(); ++index) {
		Item* candidate = corpseItems[index];
		const bool foodCandidate = PlayerBotInventoryPolicy::isFoodItem(candidate->getID());
		const uint32_t candidateValue = std::max<uint32_t>(inventoryPolicy.itemUnitValue(candidate->getID()), foodCandidate ? 1 : 0);
		if (candidateValue == 0 || unavailableLootItemIds.find(candidate->getID()) != unavailableLootItemIds.end()) {
			continue;
		}
		if (foodCandidate && heldFood >= preferredFoodCount) {
			skippedSurplusFood = true;
			skippedFoodItemId = candidate->getID();
			continue;
		}
		if (!lootItem) {
			lootItem = candidate;
			lootIndex = static_cast<uint8_t>(index);
			continue;
		}
		const uint64_t candidateDensity = static_cast<uint64_t>(candidateValue) * lootItem->getBaseWeight();
		const uint64_t selectedDensity = static_cast<uint64_t>(inventoryPolicy.itemUnitValue(lootItem->getID())) * candidate->getBaseWeight();
		if (candidateDensity > selectedDensity ||
		    (candidateDensity == selectedDensity && candidateValue > inventoryPolicy.itemUnitValue(lootItem->getID()))) {
			lootItem = candidate;
			lootIndex = static_cast<uint8_t>(index);
		}
	}

	if (!lootItem) {
		if (!lootedCurrentCorpse) {
			if (skippedSurplusFood) {
				emit("action_result", currentPosition,
				     "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"food_preference_satisfied\",\"item_id\":" +
				         std::to_string(skippedFoodItemId) + ",\"carried\":" + std::to_string(heldFood) +
				         ",\"preferred\":" + std::to_string(preferredFoodCount));
			} else {
				emit("action_result", currentPosition,
				     "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_eligible_loot\"");
			}
		}
		finishLoot(player, currentPosition);
		return;
	}

	const uint32_t inventoryCount = inventoryPolicy.inventoryItemCount(*player, lootItem->getID());
	const uint8_t moveCount = PlayerBotInventoryPolicy::isFoodItem(lootItem->getID()) ?
	                          static_cast<uint8_t>(std::min<uint32_t>(lootItem->getItemCount(), preferredFoodCount - heldFood)) :
	                          static_cast<uint8_t>(lootItem->getItemCount());
	const uint32_t moveWeight = lootItem->getBaseWeight() * moveCount;
	if (moveWeight > player->getFreeCapacity()) {
		discardCargoForLoot(player, backpack, lootItem, moveCount, currentPosition);
		return;
	}
	const Position fromPosition(0xFFFF, 0x40 | corpseContainerId, lootIndex);
	const Position toPosition(0xFFFF, 0x40 | backpackContainerId, backpackDestinationIndex(*backpack, *lootItem));
	if (!player->canDoAction()) {
		return;
	}
	pendingLootItemId = lootItem->getID();
	pendingLootInventoryCount = inventoryCount;
	++counters.actionsAttempted;
	g_game.playerMoveItem(player, fromPosition, lootItem->getClientID(), lootIndex, toPosition, moveCount, lootItem, backpack);
}
