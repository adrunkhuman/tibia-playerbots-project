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

// This file is the engine adapter: it discovers owned corpses, snapshots opened containers, and dispatches workflow commands.
using namespace playerbot;

namespace {
	struct CorpseDiscovery {
		Container* corpse = nullptr;
		Position position;
	};

	std::optional<CorpseDiscovery> findOwnedCorpse(Player& player, const Position& searchPosition)
	{
		std::optional<CorpseDiscovery> fallback;
		for (int32_t offsetX = -1; offsetX <= 1; ++offsetX) {
			for (int32_t offsetY = -1; offsetY <= 1; ++offsetY) {
				Position position(searchPosition.x + offsetX, searchPosition.y + offsetY, searchPosition.z);
				Tile* tile = g_game.map.getTile(position);
				TileItemVector* items = tile ? tile->getItemList() : nullptr;
				if (!items) continue;
				for (auto it = items->getBeginDownItem(); it != items->getEndDownItem(); ++it) {
					Item* item = *it;
					Container* corpse = item->getContainer();
					if (!corpse || Item::items[item->getID()].corpseType == RACE_NONE) continue;
					const uint32_t owner = corpse->getCorpseOwner();
					if (owner != 0 && !player.canOpenCorpse(owner)) continue;
					if (position == searchPosition) return {{corpse, position}};
					if (!fallback) fallback = {{corpse, position}};
				}
			}
		}
		return fallback;
	}

	bool isReplaceableCargo(const PlayerBotInventoryPolicy& inventoryPolicy, const Item& item)
	{
		const ItemType& type = Item::items[item.getID()];
		const bool food = PlayerBotInventoryPolicy::isFoodItem(item.getID());
		return !(inventoryPolicy.isProtectedInventoryItem(item) && !food) && type.corpseType == RACE_NONE &&
		       (food || inventoryPolicy.itemUnitValue(item.getID()) != 0) && item.getBaseWeight() != 0;
	}

	uint8_t backpackDestinationIndex(const Container& backpack, const Item& item)
	{
		if (item.isStackable()) {
			const ItemDeque& items = backpack.getItemList();
			for (size_t index = 0; index < items.size(); ++index) {
				if (items[index]->getID() == item.getID() && items[index]->getItemCount() < 100) return static_cast<uint8_t>(index);
			}
		}
		return static_cast<uint8_t>(backpack.size());
	}
}

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

void PlayerBotController::beginLoot(Player* player, const Position& currentPosition, const PlayerBotCombatDecision& defeatedTarget)
{
	if (huntRuntime.active()) huntRuntime.observeKill();
	if (defeatedTarget.target.id == 0) {
		setStage(ScenarioStage::Traverse, currentPosition);
		return;
	}
	if (shouldEmitRepeated("target:clear:target_defeated")) {
		emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(defeatedTarget.target.id) +
		     ",\"target_id\":null,\"reason\":\"target_defeated\"");
	}
	const PlayerBotLootCommand command = lootWorkflow.begin(defeatedTarget.target.id, defeatedTarget.target.position,
	                                                        defeatedTarget.expectedCorpse, currentPosition,
	                                                        std::chrono::steady_clock::now());
	if (command.outcome == PlayerBotLootOutcome::CorpseNotLootable) {
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_not_lootable\""
		       << ",\"expected_corpse_item_id\":" << lootWorkflow.expectedCorpse().itemId;
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
	lootWorkflow.reset();
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::finishLootFailure(Player* player, const Position& currentPosition, const char* reason)
{
	telemetry.recordActionFailure();
	const auto elapsed = lootWorkflow.elapsedMilliseconds(std::chrono::steady_clock::now());
	std::ostringstream fields;
	fields << "\"action\":\"loot\",\"result\":\"failed\",\"reason\":" << jsonString(reason)
	       << ",\"target_id\":" << lootWorkflow.targetId()
	       << ",\"expected_corpse_item_id\":" << lootWorkflow.expectedCorpse().itemId
	       << ",\"last_known_death_position\":{\"x\":" << lootWorkflow.deathPosition().x << ",\"y\":" << lootWorkflow.deathPosition().y
	       << ",\"z\":" << static_cast<uint16_t>(lootWorkflow.deathPosition().z) << '}' << ",\"corpse_position\":";
	if (lootWorkflow.corpseObserved()) {
		fields << "{\"x\":" << lootWorkflow.corpsePosition().x << ",\"y\":" << lootWorkflow.corpsePosition().y
		       << ",\"z\":" << static_cast<uint16_t>(lootWorkflow.corpsePosition().z) << '}';
	} else fields << "null";
	fields << ",\"search_attempts\":" << lootWorkflow.searchAttempts()
	       << ",\"navigation_failures\":" << lootWorkflow.navigationFailures()
	       << ",\"navigation_suspensions\":" << lootWorkflow.navigationSuspensions()
	       << ",\"elapsed_ms\":" << elapsed;
	emit("action_result", currentPosition, fields.str());
	finishLoot(player, currentPosition);
}

void PlayerBotController::lootCorpse(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	if (lootWorkflow.navigationSuspended()) {
		if (lootWorkflow.timedOut(now)) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		if (lootWorkflow.resumeNavigation(currentPosition, now) == PlayerBotLootNavigationTransition::Resumed) {
			clearNavigation();
			emit("navigation_progress", currentPosition, "\"result\":\"resumed\",\"reason\":\"corpse_retry\",\"navigation_failures\":" +
			     std::to_string(lootWorkflow.navigationFailures()));
		} else {
			const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(lootWorkflow.navigationRetryAt() - now).count();
			schedule(static_cast<uint32_t>(std::max<int64_t>(SCHEDULER_MINTICKS, delay)));
			return;
		}
	}

	Container* openedCorpse = player->getContainerByID(corpseContainerId);
	if (openedCorpse && Item::items[openedCorpse->getID()].corpseType == RACE_NONE) openedCorpse = nullptr;
	std::optional<CorpseDiscovery> discovery;
	if (openedCorpse) discovery = {{openedCorpse, lootWorkflow.corpsePosition()}};
	else discovery = findOwnedCorpse(*player, lootWorkflow.corpsePosition());

	PlayerBotLootWorkflowSnapshot snapshot;
	snapshot.currentPosition = currentPosition;
	snapshot.now = now;
	snapshot.canDoAction = player->canDoAction();
	snapshot.inventory.freeCapacity = player->getFreeCapacity();
	snapshot.inventory.heldFood = inventoryPolicy.foodInventory(*player).count;
	if (discovery) {
		snapshot.discoveredCorpse = {{discovery->corpse->getID(), discovery->corpse->getClientID(),
		                             discovery->corpse->getCorpseOwner(), discovery->position}};
		snapshot.corpseContainerOpen = openedCorpse == discovery->corpse;
		if (snapshot.corpseContainerOpen) {
			const ItemDeque& items = discovery->corpse->getItemList();
			for (size_t index = 0; index < items.size() && index <= UINT8_MAX; ++index) {
				Item* item = items[index];
				const uint32_t inventoryCount = inventoryPolicy.inventoryItemCount(*player, item->getID());
				snapshot.inventory.itemCounts[item->getID()] = inventoryCount;
				snapshot.corpseItems.push_back({item->getID(), item->getClientID(), static_cast<uint8_t>(item->getItemCount()),
				                                static_cast<uint8_t>(item->getItemCount()), static_cast<uint8_t>(index),
				                                item->getBaseWeight(), inventoryPolicy.itemUnitValue(item->getID()),
				                                inventoryCount,
				                                PlayerBotInventoryPolicy::isFoodItem(item->getID())});
			}
		}
	}
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	snapshot.backpackAvailable = backpack != nullptr;
	snapshot.backpackContainerOpen = backpack && player->getContainerByID(backpackContainerId) == backpack;
	if (backpack) {
		std::function<void(Container&)> collectCargo = [&](Container& source) {
			const ItemDeque& items = source.getItemList();
			for (size_t index = 0; index < items.size() && index <= UINT8_MAX; ++index) {
				Item* item = items[index];
				snapshot.inventory.itemCounts[item->getID()] = inventoryPolicy.inventoryItemCount(*player, item->getID());
				snapshot.inventory.cargo.push_back({&source, item->getID(), item->getClientID(), static_cast<uint8_t>(item->getItemCount()),
				                                  static_cast<uint8_t>(index), item->getBaseWeight(), inventoryPolicy.itemUnitValue(item->getID()),
				                                  isReplaceableCargo(inventoryPolicy, *item), player->getContainerID(&source)});
				if (Container* nested = item->getContainer()) collectCargo(*nested);
			}
		};
		collectCargo(*backpack);
	}

	const PlayerBotLootDecision decision = lootWorkflow.advance(snapshot);
	if (decision.lootVerification) {
		if (decision.lootVerification->moved) logLootSuccess(decision.lootVerification->move.itemId,
			decision.lootVerification->movedCount, decision.lootVerification->inventoryCount, currentPosition);
		else logActionFailure("loot", "item_move_failed", currentPosition);
	}
	if (decision.discardVerification) {
		if (decision.discardVerification->discarded) {
			std::ostringstream fields;
			fields << "\"action\":\"loot_replace\",\"result\":\"success\",\"discarded_item_id\":"
			       << decision.discardVerification->move.itemId << ",\"discarded_count\":"
			       << static_cast<uint32_t>(decision.discardVerification->move.requestedCount) << ",\"discarded_value\":"
			       << decision.discardVerification->move.value << ",\"incoming_item_id\":" << decision.discardVerification->move.incomingItemId
			       << ",\"incoming_unit_value\":" << inventoryPolicy.itemUnitValue(decision.discardVerification->move.incomingItemId);
			emit("action_result", currentPosition, fields.str());
		} else logActionFailure("loot_replace", "discard_not_verified", currentPosition);
	}

	const PlayerBotLootCommand& command = decision.command;
	if (command.type == PlayerBotLootCommandType::Navigate) {
		const uint32_t blockedStepsBefore = navigationRuntime.stepFailureCount();
		PlayerBotNavigationRuntimeOutcome navigation;
		processNavigation(player, currentPosition, command.destination, &navigation);
		if (navigation.routeUnavailable || navigation.stepFailureCount > blockedStepsBefore) {
			const auto transition = lootWorkflow.observeNavigationFailure(currentPosition, now);
			if (transition == PlayerBotLootNavigationTransition::Failed) {
				finishLootFailure(player, currentPosition, "corpse_inaccessible");
			} else if (transition == PlayerBotLootNavigationTransition::Suspended) {
				clearNavigation();
				emit("navigation_progress", currentPosition, "\"result\":\"suspended\",\"reason\":\"corpse_route_unchanged\",\"navigation_failures\":" +
				     std::to_string(lootWorkflow.navigationFailures()));
			}
		}
		return;
	}

	if (command.outcome == PlayerBotLootOutcome::NoCapacity) {
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_capacity\""
		       << ",\"item_id\":" << command.item.itemId << ",\"count\":" << static_cast<uint32_t>(command.item.availableCount)
		       << ",\"unit_value\":" << command.item.unitValue << ",\"weight\":" << command.item.unitWeight * command.item.availableCount
		       << ",\"free_capacity\":" << player->getFreeCapacity();
		emit("action_result", currentPosition, fields.str());
	}
	if (command.type == PlayerBotLootCommandType::Fail) {
		finishLootFailure(player, currentPosition, command.outcome == PlayerBotLootOutcome::CorpseExpired ? "corpse_expired" : "corpse_inaccessible");
		schedule(navigationInterval);
		return;
	}
	if (command.type == PlayerBotLootCommandType::Finish) {
		if (command.outcome == PlayerBotLootOutcome::OwnedCorpseUnavailable) logActionFailure("loot", "owned_corpse_unavailable", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::CorpseOpenFailed) logActionFailure("loot", "corpse_open_failed", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::BackpackUnavailable) logActionFailure("loot", "backpack_unavailable", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::CorpseEmpty && !lootWorkflow.lootedCurrentCorpse() && discovery) {
			std::ostringstream fields;
			fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_empty\",\"corpse_item_id\":"
			       << discovery->corpse->getID() << ",\"corpse_owner_id\":" << discovery->corpse->getCorpseOwner()
			       << ",\"corpse_position\":{\"x\":" << discovery->position.x << ",\"y\":" << discovery->position.y
			       << ",\"z\":" << static_cast<uint16_t>(discovery->position.z) << '}';
			emit("action_result", currentPosition, fields.str());
		}
		if (command.outcome == PlayerBotLootOutcome::FoodPreferenceSatisfied && !lootWorkflow.lootedCurrentCorpse()) {
			emit("action_result", currentPosition, "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"food_preference_satisfied\",\"item_id\":" +
			     std::to_string(command.item.itemId) + ",\"carried\":" + std::to_string(snapshot.inventory.heldFood) +
			     ",\"preferred\":" + std::to_string(preferredFoodCount));
		}
		if (command.outcome == PlayerBotLootOutcome::NoEligibleLoot && !lootWorkflow.lootedCurrentCorpse())
			emit("action_result", currentPosition, "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_eligible_loot\"");
		finishLoot(player, currentPosition);
		return;
	}

	schedule(navigationInterval);
	if (command.type == PlayerBotLootCommandType::OpenCorpse && discovery) {
		player->closeContainer(corpseContainerId);
		Tile* tile = g_game.map.getTile(discovery->position);
		const int32_t stackPosition = tile ? tile->getThingIndex(discovery->corpse) : -1;
		if (stackPosition >= 0 && stackPosition <= UINT8_MAX) {
			telemetry.recordActionAttempt();
			g_game.playerUseItem(playerId, discovery->position, static_cast<uint8_t>(stackPosition), corpseContainerId,
			                   discovery->corpse->getClientID());
		}
		return;
	}
	if (command.type == PlayerBotLootCommandType::OpenBackpack && backpack) {
		const int8_t existingContainerId = player->getContainerID(backpack);
		if (existingContainerId >= 0) player->closeContainer(static_cast<uint8_t>(existingContainerId));
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, backpackContainerId, backpack->getClientID());
		return;
	}
	if (command.type == PlayerBotLootCommandType::OpenCargo) {
		Container* source = const_cast<Container*>(static_cast<const Container*>(command.cargo.source));
		if (source) openContainer(*player, *source, rewardContainerIdBase, currentPosition);
		return;
	}
	if (command.type == PlayerBotLootCommandType::MoveItem && discovery && backpack) {
		const ItemDeque& items = discovery->corpse->getItemList();
		if (command.item.index < items.size()) {
			Item* item = items[command.item.index];
			telemetry.recordActionAttempt();
			g_game.playerMoveItem(player, Position(0xFFFF, 0x40 | corpseContainerId, command.item.index), item->getClientID(), command.item.index,
			                   Position(0xFFFF, 0x40 | backpackContainerId, backpackDestinationIndex(*backpack, *item)), command.count, item, backpack);
		}
		return;
	}
	if (command.type == PlayerBotLootCommandType::DiscardCargo) {
		Container* source = const_cast<Container*>(static_cast<const Container*>(command.cargo.source));
		Tile* destination = g_game.map.getTile(currentPosition);
		if (source && destination && command.cargo.index < source->getItemList().size()) {
			Item* item = source->getItemList()[command.cargo.index];
			telemetry.recordActionAttempt();
			g_game.playerMoveItem(player, Position(0xFFFF, 0x40 | static_cast<uint8_t>(command.cargo.containerId), command.cargo.index),
			                   item->getClientID(), command.cargo.index, currentPosition, command.count, item, destination);
		}
	}
}
