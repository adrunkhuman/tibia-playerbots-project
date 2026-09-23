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

	std::optional<CorpseDiscovery> findOwnedCorpse(Player& player, const Position& searchPosition, uint16_t expectedItemId)
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
					if (!corpse || item->getID() != expectedItemId || Item::items[item->getID()].corpseType == RACE_NONE) continue;
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
		return !inventoryPolicy.isProtectedInventoryItem(item) && type.corpseType == RACE_NONE &&
		       inventoryPolicy.itemUnitValue(item.getID()) != 0 && item.getBaseWeight() != 0;
	}

	constexpr size_t maximumLootContainers = 14;
	constexpr size_t maximumLootCargoItems = 256;
	constexpr size_t maximumLootContainerAccess = 32;

	uint8_t availableLootContainerId(Player& player)
	{
		for (uint8_t id = rewardContainerIdBase; id <= maximumContainerId; ++id) {
			if (!player.getContainerByID(id)) return id;
		}
		return UINT8_MAX;
	}
}

void PlayerBotController::logLootSuccess(const PlayerBotLootMoveVerification& verification, const Position& position)
{
	const PlayerBotLootMove& move = verification.move;
	const uint64_t coinGold = Item::items[move.itemId].worth * verification.movedCount;
	huntCoordinator.observeCoinAcquisition(coinGold);
	std::ostringstream fields;
	fields << "\"action\":\"loot\",\"result\":\"success\",\"item_id\":" << move.itemId
	       << ",\"count\":" << verification.movedCount << ",\"inventory_count\":" << verification.inventoryCount
	       << ",\"unit_value\":" << inventoryPolicy.itemUnitValue(move.itemId)
	       << ",\"total_value\":" << static_cast<uint64_t>(inventoryPolicy.itemUnitValue(move.itemId)) * verification.movedCount
	       << ",\"coin_gold_acquired\":" << coinGold
	       << ",\"unit_weight\":" << Item::items[move.itemId].weight
	       << ",\"destination_container_item_id\":" << move.destinationContainerItemId
	       << ",\"destination_container_id\":" << static_cast<int32_t>(move.destinationContainerId);
	emit("action_result", position, fields.str());
}

void PlayerBotController::beginLoot(Player* player, const Position& currentPosition, const PlayerBotCombatDecision& defeatedTarget)
{
	if (defeatedTarget.target.id == 0) {
		setStage(ScenarioStage::Traverse, currentPosition);
		return;
	}
	emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(defeatedTarget.target.id) +
	     ",\"target_id\":null,\"reason\":\"target_defeated\"");
	const PlayerBotLootCommand command = huntCoordinator.beginLoot(defeatedTarget, currentPosition, std::chrono::steady_clock::now());
	if (command.outcome == PlayerBotLootOutcome::CorpseNotLootable) {
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_not_lootable\""
		       << ",\"expected_corpse_item_id\":" << huntCoordinator.expectedCorpse().itemId;
		emit("action_result", currentPosition, fields.str());
		finishLoot(player, currentPosition);
		return;
	}
	setStage(ScenarioStage::LootCorpse, currentPosition);
}

void PlayerBotController::finishLoot(Player* player, const Position& currentPosition)
{
	player->closeContainer(corpseContainerId);
	resetNavigation();
	huntCoordinator.resetLoot();
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::finishLootFailure(Player* player, const Position& currentPosition, const char* reason)
{
	if (huntCoordinator.hasDefensiveCombat()) {
		finishDefensiveCombat(player, currentPosition, "skipped", reason);
	}
	telemetry.recordActionFailure();
	const auto elapsed = huntCoordinator.lootElapsedMilliseconds(std::chrono::steady_clock::now());
	std::ostringstream fields;
	fields << "\"action\":\"loot\",\"result\":\"failed\",\"reason\":" << jsonString(reason)
	       << ",\"target_id\":" << huntCoordinator.lootTargetId()
	       << ",\"expected_corpse_item_id\":" << huntCoordinator.expectedCorpse().itemId
	       << ",\"last_known_death_position\":{\"x\":" << huntCoordinator.lootDeathPosition().x << ",\"y\":" << huntCoordinator.lootDeathPosition().y
	       << ",\"z\":" << static_cast<uint16_t>(huntCoordinator.lootDeathPosition().z) << '}' << ",\"corpse_position\":";
	if (huntCoordinator.corpseObserved()) {
		fields << "{\"x\":" << huntCoordinator.corpsePosition().x << ",\"y\":" << huntCoordinator.corpsePosition().y
		       << ",\"z\":" << static_cast<uint16_t>(huntCoordinator.corpsePosition().z) << '}';
	} else fields << "null";
	fields << ",\"search_attempts\":" << huntCoordinator.lootSearchAttempts()
	       << ",\"navigation_failures\":" << huntCoordinator.lootNavigationFailures()
	       << ",\"navigation_suspensions\":" << huntCoordinator.lootNavigationSuspensions()
	       << ",\"elapsed_ms\":" << elapsed;
	emit("action_result", currentPosition, fields.str());
	finishLoot(player, currentPosition);
}

void PlayerBotController::lootCorpse(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	if (huntCoordinator.lootNavigationSuspended()) {
		if (huntCoordinator.lootTimedOut(now)) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		if (huntCoordinator.resumeLootNavigation(currentPosition, now) == PlayerBotLootNavigationTransition::Resumed) {
			resetNavigation();
			emit("navigation_progress", currentPosition, "\"result\":\"resumed\",\"reason\":\"corpse_retry\",\"navigation_failures\":" +
			     std::to_string(huntCoordinator.lootNavigationFailures()));
		} else {
			const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(huntCoordinator.lootNavigationRetryAt() - now).count();
			schedule(static_cast<uint32_t>(std::max<int64_t>(SCHEDULER_MINTICKS, delay)));
			return;
		}
	}

	Container* openedCorpse = player->getContainerByID(corpseContainerId);
	Tile* openedCorpseTile = openedCorpse ? openedCorpse->getTile() : nullptr;
	if (openedCorpse && (openedCorpse->getID() != huntCoordinator.expectedCorpse().itemId ||
	    Item::items[openedCorpse->getID()].corpseType == RACE_NONE || !openedCorpseTile ||
	    !Position::areInRange<1, 1, 0>(openedCorpseTile->getPosition(), huntCoordinator.corpsePosition()))) openedCorpse = nullptr;
	std::optional<CorpseDiscovery> discovery;
	if (openedCorpse) discovery = {{openedCorpse, openedCorpseTile->getPosition()}};
	else discovery = findOwnedCorpse(*player, huntCoordinator.corpsePosition(), huntCoordinator.expectedCorpse().itemId);

	PlayerBotLootWorkflowSnapshot snapshot;
	snapshot.currentPosition = currentPosition;
	snapshot.now = now;
	snapshot.canDoAction = player->canDoAction();
	snapshot.inventory.freeCapacity = player->getFreeCapacity();
	snapshot.inventory.heldFood = inventoryPolicy.foodInventory(*player).count;
	if (Tile* tile = g_game.map.getTile(currentPosition)) {
		if (TileItemVector* items = tile->getItemList()) {
			for (const Item* item : *items) {
				snapshot.groundItemCounts[item->getID()] += item->getItemCount();
				const int32_t index = tile->getThingIndex(item);
				if (index >= 0 && index <= UINT8_MAX) {
					snapshot.groundItems.push_back({item, item->getID(), item->getClientID(),
					                                static_cast<uint8_t>(item->getItemCount()),
					                                static_cast<uint8_t>(index), currentPosition});
				}
			}
		}
	}
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
				                                PlayerBotInventoryPolicy::isFoodItem(item->getID()),
				                                PlayerBotInventoryPolicy::isCurrencyItem(item->getID()),
				                                discovery->corpse, item, item->isStackable()});
			}
		}
	}
	Item* backpackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
	snapshot.backpackAvailable = backpack != nullptr;
	snapshot.backpackContainerOpen = backpack && player->getContainerByID(backpackContainerId) == backpack;
	if (backpack && snapshot.backpackContainerOpen) {
		std::set<const Container*> directlyObservedContainers;
		for (uint8_t containerId = backpackContainerId; containerId <= maximumContainerId; ++containerId) {
			Container* container = player->getContainerByID(containerId);
			if (!container || !directlyObservedContainers.insert(container).second) continue;
			const ItemDeque& items = container->getItemList();
			for (size_t index = 0; index < items.size() && index <= UINT8_MAX; ++index) {
				Item* item = items[index];
				snapshot.inventoryItems.push_back({container, item, item->getID(), item->getClientID(),
				                                   static_cast<uint8_t>(item->getItemCount()),
				                                   static_cast<uint8_t>(index), static_cast<int8_t>(containerId)});
			}
		}
		uint32_t replaceableFood = snapshot.inventory.heldFood > preferredFoodCount ?
			snapshot.inventory.heldFood - preferredFoodCount : 0;
		std::set<const Container*> observedContainers;
		std::function<void(Container&)> collectCargo = [&](Container& source) {
			if (snapshot.inventory.containers.size() >= maximumLootContainers || !observedContainers.insert(&source).second ||
			    source.size() > UINT8_MAX) return;
			const int8_t sourceId = player->getContainerID(&source);
			if (sourceId < 0) return;
			const size_t containerSnapshotIndex = snapshot.inventory.containers.size();
			snapshot.inventory.containers.push_back({&source, source.getID(), source.getClientID(),
				static_cast<uint8_t>(source.size()),
				static_cast<uint8_t>(std::min<uint32_t>(source.capacity(), UINT8_MAX)), sourceId, true});
			const ItemDeque& items = source.getItemList();
			for (size_t index = 0; index < items.size(); ++index) {
				if (snapshot.inventory.cargo.size() >= maximumLootCargoItems) {
					snapshot.inventory.containers[containerSnapshotIndex].contentsComplete = false;
					return;
				}
				Item* item = items[index];
				const uint8_t itemCount = static_cast<uint8_t>(item->getItemCount());
				uint8_t replaceableCount = itemCount;
				bool replaceable = isReplaceableCargo(inventoryPolicy, *item);
				if (PlayerBotInventoryPolicy::isFoodItem(item->getID())) {
					replaceableCount = static_cast<uint8_t>(std::min<uint32_t>(itemCount, replaceableFood));
					replaceableFood -= replaceableCount;
					replaceable = replaceableCount != 0 && item->getBaseWeight() != 0;
				}
				snapshot.inventory.itemCounts[item->getID()] = inventoryPolicy.inventoryItemCount(*player, item->getID());
				snapshot.inventory.cargo.push_back({&source, item->getID(), item->getClientID(), replaceableCount,
				                                  static_cast<uint8_t>(index), item->getBaseWeight(), inventoryPolicy.itemUnitValue(item->getID()),
				                                  replaceable, sourceId, item, item->isStackable(),
				                                  replaceableCount == itemCount, itemCount});
				if (Container* nested = item->getContainer()) {
					const int8_t nestedId = player->getContainerID(nested);
					if (nestedId >= 0) collectCargo(*nested);
					else if (snapshot.inventory.containerAccess.size() < maximumLootContainerAccess) {
						snapshot.inventory.containerAccess.push_back({nested, &source, item, item->getID(), item->getClientID(),
						                                              static_cast<uint8_t>(index), sourceId});
					}
				}
			}
		};
		collectCargo(*backpack);
	}

	const PlayerBotLootDecision decision = huntCoordinator.advanceLoot(snapshot);
	if (decision.lootVerification) {
		if (decision.lootVerification->moved) logLootSuccess(*decision.lootVerification, currentPosition);
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
	if (decision.recoveryVerification) {
		std::ostringstream fields;
		fields << "\"action\":\"loot_replace_recovery\",\"result\":"
		       << jsonString(decision.recoveryVerification->recovered ? "success" : "failed")
		       << ",\"reason\":" << jsonString(decision.recoveryVerification->recovered ? "cargo_recovered" : "recovery_move_not_verified")
		       << ",\"item_id\":" << decision.recoveryVerification->move.itemId
		       << ",\"count\":" << decision.recoveryVerification->recoveredCount;
		emit("action_result", currentPosition, fields.str());
	}

	const PlayerBotLootCommand& command = decision.command;
	if (command.type == PlayerBotLootCommandType::Navigate) {
		const uint32_t blockedStepsBefore = navigationRuntime.stepFailureCount();
		PlayerBotNavigationRuntimeOutcome navigation;
		processNavigation(player, currentPosition, PlayerBotNavigationGoal::withinRange(command.destination, 1, 1), &navigation);
		if (navigation.routeUnavailable || navigation.stepFailureCount > blockedStepsBefore) {
			const auto transition = huntCoordinator.observeLootNavigationFailure(currentPosition, now);
			if (transition == PlayerBotLootNavigationTransition::Failed) {
				finishLootFailure(player, currentPosition, "corpse_inaccessible");
			} else if (transition == PlayerBotLootNavigationTransition::Suspended) {
				resetNavigation();
				emit("navigation_progress", currentPosition, "\"result\":\"suspended\",\"reason\":\"corpse_route_unchanged\",\"navigation_failures\":" +
				     std::to_string(huntCoordinator.lootNavigationFailures()));
			}
		}
		return;
	}

	if (command.outcome == PlayerBotLootOutcome::CargoRecoveryImpossible && !decision.recoveryVerification) {
		emit("action_result", currentPosition,
		     "\"action\":\"loot_replace_recovery\",\"result\":\"failed\",\"reason\":\"cargo_recovery_impossible\",\"item_id\":" +
		         std::to_string(command.cargo.itemId) + ",\"count\":" + std::to_string(command.count));
	}
	if (command.outcome == PlayerBotLootOutcome::NoCapacity || command.outcome == PlayerBotLootOutcome::NoSlot) {
		uint32_t freeSlots = 0;
		uint32_t mergeRoom = 0;
		for (const auto& container : snapshot.inventory.containers) freeSlots += container.capacity - container.size;
		for (const auto& cargo : snapshot.inventory.cargo) {
			if (cargo.stackable && cargo.itemId == command.item.itemId && cargo.sourceCount < 100) {
				mergeRoom += 100 - cargo.sourceCount;
			}
		}
		std::ostringstream fields;
		fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":"
		       << jsonString(command.outcome == PlayerBotLootOutcome::NoSlot ? "no_slot" : "no_capacity")
		       << ",\"item_id\":" << command.item.itemId << ",\"count\":" << static_cast<uint32_t>(command.item.availableCount)
		       << ",\"unit_value\":" << command.item.unitValue << ",\"weight\":" << command.item.unitWeight * command.item.availableCount
		       << ",\"free_capacity\":" << player->getFreeCapacity()
		       << ",\"free_slots\":" << freeSlots << ",\"merge_room\":" << mergeRoom
		       << ",\"open_inventory_containers\":" << snapshot.inventory.containers.size();
		emit("action_result", currentPosition, fields.str());
	}
	schedule(navigationInterval);
	if (command.type == PlayerBotLootCommandType::Fail) {
		finishLootFailure(player, currentPosition, command.outcome == PlayerBotLootOutcome::CorpseExpired ? "corpse_expired" : "corpse_inaccessible");
		return;
	}
	if (command.type == PlayerBotLootCommandType::Finish) {
		if (command.outcome == PlayerBotLootOutcome::OwnedCorpseUnavailable) logActionFailure("loot", "owned_corpse_unavailable", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::CorpseOpenFailed) logActionFailure("loot", "corpse_open_failed", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::BackpackUnavailable) logActionFailure("loot", "backpack_unavailable", currentPosition);
		if (command.outcome == PlayerBotLootOutcome::CorpseEmpty && !huntCoordinator.lootedCurrentCorpse() && discovery) {
			std::ostringstream fields;
			fields << "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"corpse_empty\",\"corpse_item_id\":"
			       << discovery->corpse->getID() << ",\"corpse_owner_id\":" << discovery->corpse->getCorpseOwner()
			       << ",\"corpse_position\":{\"x\":" << discovery->position.x << ",\"y\":" << discovery->position.y
			       << ",\"z\":" << static_cast<uint16_t>(discovery->position.z) << '}';
			emit("action_result", currentPosition, fields.str());
		}
		if (command.outcome == PlayerBotLootOutcome::FoodPreferenceSatisfied && !huntCoordinator.lootedCurrentCorpse()) {
			emit("action_result", currentPosition, "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"food_preference_satisfied\",\"item_id\":" +
			     std::to_string(command.item.itemId) + ",\"carried\":" + std::to_string(snapshot.inventory.heldFood) +
			     ",\"preferred\":" + std::to_string(preferredFoodCount));
		}
		if (command.outcome == PlayerBotLootOutcome::NoEligibleLoot && !huntCoordinator.lootedCurrentCorpse())
			emit("action_result", currentPosition, "\"action\":\"loot\",\"result\":\"skipped\",\"reason\":\"no_eligible_loot\"");
		finishLoot(player, currentPosition);
		return;
	}

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
		const auto& access = command.containerAccess;
		Container* container = const_cast<Container*>(static_cast<const Container*>(access.container));
		Container* parent = const_cast<Container*>(static_cast<const Container*>(access.parent));
		const uint8_t containerId = availableLootContainerId(*player);
		if (!container || !parent || containerId == UINT8_MAX || access.parentContainerId < 0 ||
		    player->getContainerByID(static_cast<uint8_t>(access.parentContainerId)) != parent ||
		    access.index >= parent->getItemList().size() || parent->getItemList()[access.index] != access.item ||
		    static_cast<const Item*>(access.item) != static_cast<Item*>(container) || container->getID() != access.itemId || container->getClientID() != access.clientId) return;
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId,
		                   Position(0xFFFF, 0x40 | static_cast<uint8_t>(access.parentContainerId), access.index),
		                   access.index, containerId, container->getClientID());
		return;
	}
	auto rejectStaleLootMove = [&](bool discard) {
		if (discard) huntCoordinator.cancelPendingDiscardMove();
		else huntCoordinator.cancelPendingLootMove();
		emit("action_result", currentPosition,
		     std::string("\"action\":\"") + (discard ? "loot_replace" : "loot") +
		     "\",\"result\":\"skipped\",\"reason\":\"stale_move_plan\",\"item_id\":" +
		     std::to_string(discard ? command.cargo.itemId : command.item.itemId));
	};
	if (command.type == PlayerBotLootCommandType::MoveItem) {
		Container* source = const_cast<Container*>(static_cast<const Container*>(command.item.source));
		Container* destination = const_cast<Container*>(static_cast<const Container*>(command.itemDestination.container));
		if (!discovery || discovery->corpse != source || !destination || command.itemDestination.containerId < 0 ||
		    player->getContainerByID(static_cast<uint8_t>(command.itemDestination.containerId)) != destination ||
		    destination->getID() != command.itemDestination.containerItemId ||
		    destination->getClientID() != command.itemDestination.containerClientId ||
		    command.item.index >= source->getItemList().size()) {
			rejectStaleLootMove(false);
			return;
		}
		Item* item = source->getItemList()[command.item.index];
		bool destinationValid = destination->size() == command.itemDestination.index &&
		                        destination->size() < destination->capacity();
		if (command.itemDestination.merge) {
			destinationValid = command.itemDestination.index < destination->getItemList().size() &&
			                   destination->getItemList()[command.itemDestination.index] == command.itemDestination.item &&
			                   command.itemDestination.itemId == item->getID() &&
			                   command.itemDestination.count == static_cast<const Item*>(command.itemDestination.item)->getItemCount();
		}
		if (item != command.item.item || item->getID() != command.item.itemId || item->getClientID() != command.item.clientId ||
		    item->getItemCount() != command.item.availableCount || command.count == 0 ||
		    command.count > command.itemDestination.availableCount || !destinationValid ||
		    destination->queryAdd(command.itemDestination.index, *item, command.count, 0, player) != RETURNVALUE_NOERROR) {
			rejectStaleLootMove(false);
			return;
		}
		telemetry.recordActionAttempt();
		g_game.playerMoveItem(player, Position(0xFFFF, 0x40 | corpseContainerId, command.item.index), item->getClientID(), command.item.index,
		                   Position(0xFFFF, 0x40 | static_cast<uint8_t>(command.itemDestination.containerId), command.itemDestination.index),
		                   command.count, item, destination);
		return;
	}
	if (command.type == PlayerBotLootCommandType::RecoverCargo) {
		Tile* source = g_game.map.getTile(command.groundItem.position);
		Item* item = const_cast<Item*>(static_cast<const Item*>(command.groundItem.item));
		Container* destination = const_cast<Container*>(static_cast<const Container*>(command.itemDestination.container));
		if (!source || !item || !destination || currentPosition != command.groundItem.position ||
		    source->getThingIndex(item) != command.groundItem.index || item->getID() != command.groundItem.itemId ||
		    item->getClientID() != command.groundItem.clientId || item->getItemCount() != command.groundItem.count ||
		    command.itemDestination.containerId < 0 ||
		    player->getContainerByID(static_cast<uint8_t>(command.itemDestination.containerId)) != destination ||
		    destination->getID() != command.itemDestination.containerItemId ||
		    destination->getClientID() != command.itemDestination.containerClientId) return;
		bool destinationValid = destination->size() == command.itemDestination.index &&
		                        destination->size() < destination->capacity();
		if (command.itemDestination.merge) {
			destinationValid = command.itemDestination.index < destination->getItemList().size() &&
			                   destination->getItemList()[command.itemDestination.index] == command.itemDestination.item &&
			                   command.itemDestination.itemId == item->getID() &&
			                   command.itemDestination.count == static_cast<const Item*>(command.itemDestination.item)->getItemCount();
		}
		uint32_t maximumMoveCount = 0;
		destination->queryMaxCount(command.itemDestination.index, *item, command.count, maximumMoveCount, 0);
		if (!destinationValid || command.count == 0 || command.count > command.itemDestination.availableCount ||
		    maximumMoveCount < command.count ||
		    destination->queryAdd(command.itemDestination.index, *item, command.count, 0, player) != RETURNVALUE_NOERROR) return;
		telemetry.recordActionAttempt();
		g_game.playerMoveItem(player, command.groundItem.position, item->getClientID(), command.groundItem.index,
		                   Position(0xFFFF, 0x40 | static_cast<uint8_t>(command.itemDestination.containerId),
		                            command.itemDestination.index),
		                   command.count, item, destination);
		return;
	}
	if (command.type == PlayerBotLootCommandType::DiscardCargo) {
		Container* source = const_cast<Container*>(static_cast<const Container*>(command.cargo.source));
		Tile* destination = g_game.map.getTile(currentPosition);
		if (!source || !destination || command.cargo.containerId < 0 ||
		    player->getContainerByID(static_cast<uint8_t>(command.cargo.containerId)) != source ||
		    command.cargo.index >= source->getItemList().size()) {
			rejectStaleLootMove(true);
			return;
		}
		Item* item = source->getItemList()[command.cargo.index];
		if (item != command.cargo.item || item->getID() != command.cargo.itemId ||
		    item->getClientID() != command.cargo.clientId || item->getItemCount() != command.cargo.sourceCount ||
		    command.count == 0 || command.count > command.cargo.count) {
			rejectStaleLootMove(true);
			return;
		}
		if (item->isStackable()) {
			if (TileItemVector* groundItems = destination->getItemList()) {
				for (const Item* groundItem : *groundItems) {
					if (groundItem->getID() == item->getID() && groundItem->getClientID() == item->getClientID() &&
					    groundItem->getItemCount() < 100 && command.count > 100 - groundItem->getItemCount()) {
						rejectStaleLootMove(true);
						return;
					}
				}
			}
		}
		telemetry.recordActionAttempt();
		g_game.playerMoveItem(player, Position(0xFFFF, 0x40 | static_cast<uint8_t>(command.cargo.containerId), command.cargo.index),
		                   item->getClientID(), command.cargo.index, currentPosition, command.count, item, destination);
	}
}
