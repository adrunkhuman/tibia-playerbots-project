/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cstdlib>
#include <iosfwd>

#include "playerbotlootworkflow.h"

#include <algorithm>
#include <utility>

PlayerBotLootWorkflow::PlayerBotLootWorkflow(PlayerBotLootWorkflowConfig config) : config(config), policy(config.preferredFoodCount) {}

PlayerBotLootCommand PlayerBotLootWorkflow::begin(uint32_t targetId, const Position& deathPosition, PlayerBotExpectedCorpse expectedCorpse,
	const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	replacementPlan.reset();
	discardedGroundItem.reset();
	replacementNeedsRecovery = false;
	session.begin(targetId, deathPosition, expectedCorpse, currentPosition, now);
	return {expectedCorpse.lootable ? PlayerBotLootCommandType::None : PlayerBotLootCommandType::Finish,
	        expectedCorpse.lootable ? PlayerBotLootOutcome::None : PlayerBotLootOutcome::CorpseNotLootable};
}

void PlayerBotLootWorkflow::reset()
{
	session.reset();
	replacementPlan.reset();
	discardedGroundItem.reset();
	replacementNeedsRecovery = false;
}

PlayerBotLootNavigationTransition PlayerBotLootWorkflow::observeNavigationFailure(const Position& currentPosition,
	std::chrono::steady_clock::time_point now)
{
	return session.observeNavigationFailure(currentPosition, now, config.maximumNavigationFailures,
	                                        config.navigationSuspendThreshold, config.navigationRetryInterval);
}

PlayerBotLootNavigationTransition PlayerBotLootWorkflow::resumeNavigation(const Position& currentPosition,
	std::chrono::steady_clock::time_point now)
{
	return session.resumeNavigation(currentPosition, now);
}

PlayerBotLootDecision PlayerBotLootWorkflow::advance(const PlayerBotLootWorkflowSnapshot& snapshot)
{
	PlayerBotLootDecision decision;
	auto inventoryCount = [&snapshot](uint16_t itemId) {
		const auto count = snapshot.inventory.itemCounts.find(itemId);
		return count == snapshot.inventory.itemCounts.end() ? 0U : count->second;
	};
	auto groundItemCount = [&snapshot](uint16_t itemId) {
		const auto count = snapshot.groundItemCounts.find(itemId);
		return count == snapshot.groundItemCounts.end() ? 0U : count->second;
	};
	auto containerItemCount = [&snapshot](const void* container, uint16_t itemId) {
		uint32_t count = 0;
		for (const auto& item : snapshot.inventoryItems) {
			if (item.container == container && item.itemId == itemId) count += item.count;
		}
		return count;
	};
	auto observedInventoryItem = [&snapshot](const void* container, const void* item) {
		return std::find_if(snapshot.inventoryItems.begin(), snapshot.inventoryItems.end(),
			[container, item](const auto& observed) {
				return observed.container == container && observed.item == item;
			});
	};
	if (session.hasPendingLootMove()) {
		const PlayerBotLootMove& move = *session.pendingLootMove();
		uint8_t sourceCount = 0;
		for (const auto& item : snapshot.corpseItems) {
			if (item.source == move.source && item.item == move.sourceItem && item.index == move.sourceIndex) {
				sourceCount = item.availableCount;
				break;
			}
		}
		decision.lootVerification = session.verifyLootMove(sourceCount,
			containerItemCount(move.destination, move.itemId), inventoryCount(move.itemId));
		if (decision.lootVerification->moved) {
			session.markLooted();
			replacementPlan.reset();
			discardedGroundItem.reset();
			replacementNeedsRecovery = false;
		} else if (replacementPlan && discardedGroundItem) {
			replacementNeedsRecovery = true;
		}
	}
	if (session.hasPendingDiscardMove()) {
		const PlayerBotLootDiscardMove& move = *session.pendingDiscardMove();
		const auto source = observedInventoryItem(move.source, move.sourceItem);
		const uint8_t sourceCount = source == snapshot.inventoryItems.end() ? 0 : source->count;
		decision.discardVerification = session.verifyDiscardMove(sourceCount, snapshot.groundItems,
		                                                        inventoryCount(move.itemId));
		if (decision.discardVerification->discarded) {
			discardedGroundItem = decision.discardVerification->groundItem;
		} else {
			replacementPlan.reset();
			discardedGroundItem.reset();
			replacementNeedsRecovery = false;
		}
	}
	if (session.hasPendingRecoveryMove()) {
		const PlayerBotLootRecoveryMove& move = *session.pendingRecoveryMove();
		uint8_t sourceCount = 0;
		for (const auto& item : snapshot.groundItems) {
			if (item.position == move.sourcePosition && item.item == move.sourceItem && item.itemId == move.itemId) {
				sourceCount = item.count;
				break;
			}
		}
		decision.recoveryVerification = session.verifyRecoveryMove(sourceCount,
			containerItemCount(move.destination, move.itemId));
		replacementPlan.reset();
		discardedGroundItem.reset();
		replacementNeedsRecovery = false;
		if (!decision.recoveryVerification->recovered) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			decision.command.outcome = PlayerBotLootOutcome::CargoRecoveryImpossible;
			return decision;
		}
	}
	if (snapshot.discoveredCorpse) {
		const auto& corpse = *snapshot.discoveredCorpse;
		session.observeCorpse(corpse.itemId, corpse.ownerId, corpse.position);
	}

	auto issueMove = [&](const PlayerBotLootItemSnapshot& item, const PlayerBotLootDestinationSnapshot& destination,
	                    uint8_t count) {
		session.beginLootMove({item.itemId, count, item.inventoryCount, item.index, item.source, item.item,
		                       item.availableCount, destination.container,
		                       containerItemCount(destination.container, item.itemId),
		                       destination.containerItemId, destination.containerId});
		decision.command.type = PlayerBotLootCommandType::MoveItem;
		decision.command.item = item;
		decision.command.itemDestination = destination;
		decision.command.count = count;
	};

	auto recoverDiscardedCargo = [&]() {
		const PlayerBotLootReplacement replacement = *replacementPlan;
		const auto ground = discardedGroundItem ?
			std::find_if(snapshot.groundItems.begin(), snapshot.groundItems.end(), [this](const auto& item) {
				return item.item == discardedGroundItem->item && item.itemId == discardedGroundItem->itemId &&
				       item.clientId == discardedGroundItem->clientId && item.position == discardedGroundItem->position &&
				       item.count == discardedGroundItem->count;
			}) : snapshot.groundItems.end();
		PlayerBotLootDestinationSnapshot destination;
		bool destinationAvailable = false;
		for (const auto& container : snapshot.inventory.containers) {
			if (container.container != replacement.cargo.source || container.containerId < 0) continue;
			const auto original = observedInventoryItem(container.container, replacement.cargo.item);
			if (original != snapshot.inventoryItems.end() && original->itemId == replacement.cargo.itemId &&
			    original->clientId == replacement.cargo.clientId && replacement.cargo.stackable && original->count < 100 &&
			    replacement.count <= 100 - original->count) {
				destination = {container.container, original->item, container.itemId, container.clientId,
				               original->itemId, original->clientId, original->index, original->count,
				               static_cast<uint8_t>(100 - original->count), container.containerId, true};
				destinationAvailable = true;
			} else if (original == snapshot.inventoryItems.end() && replacement.cargo.stackable) {
				auto autoStack = snapshot.inventoryItems.end();
				for (auto observed = snapshot.inventoryItems.begin(); observed != snapshot.inventoryItems.end(); ++observed) {
					if (observed->container != container.container || observed->itemId != replacement.cargo.itemId ||
					    observed->clientId != replacement.cargo.clientId || observed->count >= 100) continue;
					if (autoStack == snapshot.inventoryItems.end() || observed->index < autoStack->index) autoStack = observed;
				}
				if (autoStack != snapshot.inventoryItems.end()) {
					const uint32_t freeSlots = container.capacity > container.size ? container.capacity - container.size : 0;
					const uint32_t available = 100 - autoStack->count + freeSlots * 100;
					destination = {container.container, autoStack->item, container.itemId, container.clientId,
					               autoStack->itemId, autoStack->clientId, autoStack->index, autoStack->count,
					               static_cast<uint8_t>(std::min<uint32_t>(100, available)), container.containerId, true};
					destinationAvailable = replacement.count <= destination.availableCount;
				} else if (container.size < container.capacity) {
					destination = {container.container, nullptr, container.itemId, container.clientId, 0, 0,
					               container.size, 0, 100, container.containerId, false};
					destinationAvailable = replacement.count <= destination.availableCount;
				}
			} else if (original == snapshot.inventoryItems.end() && container.size < container.capacity) {
				destination = {container.container, nullptr, container.itemId, container.clientId, 0, 0,
				               container.size, 0, 1, container.containerId, false};
				destinationAvailable = replacement.count <= destination.availableCount;
			}
			break;
		}
		const uint64_t recoveryWeight = static_cast<uint64_t>(replacement.count) * replacement.cargo.unitWeight;
		const bool recoveryFeasible = ground != snapshot.groundItems.end() && destinationAvailable &&
		                              recoveryWeight <= snapshot.inventory.freeCapacity;
		if (recoveryFeasible && !snapshot.canDoAction) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			return;
		}
		session.suppressLootItem(replacement.incoming.itemId);
		decision.command.item = replacement.incoming;
		decision.command.cargo = replacement.cargo;
		decision.command.count = replacement.count;
		if (!recoveryFeasible) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			decision.command.outcome = PlayerBotLootOutcome::CargoRecoveryImpossible;
			replacementPlan.reset();
			discardedGroundItem.reset();
			replacementNeedsRecovery = false;
			return;
		}
		session.beginRecoveryMove({ground->itemId, ground->clientId, replacement.count, ground->item,
		                           ground->index, ground->count, ground->position, destination.container,
		                           containerItemCount(destination.container, ground->itemId)});
		decision.command.type = PlayerBotLootCommandType::RecoverCargo;
		decision.command.groundItem = *ground;
		decision.command.itemDestination = destination;
		decision.command.count = replacement.count;
		replacementPlan.reset();
		discardedGroundItem.reset();
		replacementNeedsRecovery = false;
	};

	if (replacementPlan && discardedGroundItem && !session.hasPendingDiscardMove()) {
		if (replacementNeedsRecovery) {
			recoverDiscardedCargo();
			return decision;
		}
		PlayerBotLootItemSnapshot item;
		bool sourceAvailable = false;
		for (const auto& observed : snapshot.corpseItems) {
			if (observed.source == replacementPlan->incoming.source && observed.item == replacementPlan->incoming.item &&
			    observed.index == replacementPlan->incoming.index && observed.availableCount >= replacementPlan->incomingCount) {
				item = observed;
				item.count = replacementPlan->incomingCount;
				sourceAvailable = true;
				break;
			}
		}
		PlayerBotLootDestinationSnapshot destination;
		bool destinationAvailable = false;
		if (sourceAvailable) {
			for (const auto& container : snapshot.inventory.containers) {
				if (container.container != replacementPlan->destination.container || container.containerId < 0 ||
				    !container.contentsComplete) continue;
				if (replacementPlan->destination.merge) {
					const auto merged = observedInventoryItem(container.container, replacementPlan->destination.item);
					if (merged != snapshot.inventoryItems.end() && merged->itemId == item.itemId && merged->count < 100) {
						destination = {container.container, merged->item, container.itemId, container.clientId,
						               merged->itemId, merged->clientId, merged->index, merged->count,
						               static_cast<uint8_t>(100 - merged->count), container.containerId, true};
						destinationAvailable = true;
					}
				} else if (container.size < container.capacity) {
					destination = {container.container, nullptr, container.itemId, container.clientId, 0, 0,
					               container.size, 0, static_cast<uint8_t>(item.stackable ? 100 : 1),
					               container.containerId, false};
					destinationAvailable = true;
				}
				break;
			}
		}
		const uint64_t requiredWeight = static_cast<uint64_t>(replacementPlan->incomingCount) * item.unitWeight;
		if (sourceAvailable && destinationAvailable && destination.availableCount >= replacementPlan->incomingCount &&
		    requiredWeight <= snapshot.inventory.freeCapacity && snapshot.canDoAction) {
			issueMove(item, destination, replacementPlan->incomingCount);
		} else if (!snapshot.canDoAction && sourceAvailable && destinationAvailable &&
		           requiredWeight <= snapshot.inventory.freeCapacity) {
			decision.command.type = PlayerBotLootCommandType::Wait;
		} else {
			recoverDiscardedCargo();
		}
		return decision;
	}
	if (!Position::areInRange<1, 1, 0>(snapshot.currentPosition, session.corpsePosition())) {
		if (session.timedOut(snapshot.now, config.timeout)) {
			decision.command = {PlayerBotLootCommandType::Fail, PlayerBotLootOutcome::CorpseInaccessible};
			return decision;
		}
		if (session.navigationSuspended() &&
		    session.resumeNavigation(snapshot.currentPosition, snapshot.now) != PlayerBotLootNavigationTransition::Resumed) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			return decision;
		}
		decision.command.type = PlayerBotLootCommandType::Navigate;
		decision.command.destination = session.corpsePosition();
		return decision;
	}
	if (!snapshot.discoveredCorpse) {
		decision.command = session.corpseObserved() ?
			PlayerBotLootCommand{PlayerBotLootCommandType::Fail, PlayerBotLootOutcome::CorpseExpired} :
			session.incrementSearchAttempts() >= config.maximumSearchAttempts ?
			PlayerBotLootCommand{PlayerBotLootCommandType::Finish, PlayerBotLootOutcome::OwnedCorpseUnavailable} :
			PlayerBotLootCommand{PlayerBotLootCommandType::Wait};
		return decision;
	}
	if (!snapshot.corpseContainerOpen) {
		decision.command.type = !snapshot.canDoAction ? PlayerBotLootCommandType::Wait :
		                        session.incrementOpenAttempts() > 2 ? PlayerBotLootCommandType::Finish :
		                        PlayerBotLootCommandType::OpenCorpse;
		if (decision.command.type == PlayerBotLootCommandType::Finish) decision.command.outcome = PlayerBotLootOutcome::CorpseOpenFailed;
		return decision;
	}
	if (snapshot.corpseItems.empty()) {
		decision.command = {PlayerBotLootCommandType::Finish, PlayerBotLootOutcome::CorpseEmpty};
		return decision;
	}
	if (!snapshot.backpackAvailable) {
		decision.command = {PlayerBotLootCommandType::Finish, PlayerBotLootOutcome::BackpackUnavailable};
		return decision;
	}
	if (!snapshot.backpackContainerOpen) {
		decision.command.type = snapshot.canDoAction ? PlayerBotLootCommandType::OpenBackpack : PlayerBotLootCommandType::Wait;
		return decision;
	}


	const PlayerBotLootSelection selection = policy.select(snapshot.corpseItems, snapshot.inventory,
	                                                        session.unavailableLootItems());
	if (selection.result != PlayerBotLootSelectionResult::Selected) {
		decision.command.type = PlayerBotLootCommandType::Finish;
		decision.command.outcome = selection.result == PlayerBotLootSelectionResult::FoodPreferenceSatisfied ?
			PlayerBotLootOutcome::FoodPreferenceSatisfied : PlayerBotLootOutcome::NoEligibleLoot;
		decision.command.item = selection.item;
		return decision;
	}
	const PlayerBotLootItemSnapshot& item = selection.item;
	for (const auto& access : snapshot.inventory.containerAccess) {
		if (!session.containerAccessAttempted(access.container)) {
			if (!snapshot.canDoAction) {
				decision.command.type = PlayerBotLootCommandType::Wait;
				return decision;
			}
			session.markContainerAccessAttempted(access.container);
			decision.command.type = PlayerBotLootCommandType::OpenCargo;
			decision.command.item = item;
			decision.command.containerAccess = access;
			return decision;
		}
	}
	const PlayerBotLootPlacement placement = policy.placementFor(item, snapshot.inventory);
	if (placement.result == PlayerBotLootPlacementResult::Placed) {
		if (!snapshot.canDoAction) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			return decision;
		}
		issueMove(item, placement.destination, placement.count);
		return decision;
	}
	const PlayerBotLootReplacement replacement = policy.replacementFor(item, snapshot.inventory);
	if (replacement.viable) {
		const bool groundAutoStackWouldSplit = replacement.cargo.stackable &&
			std::any_of(snapshot.groundItems.begin(), snapshot.groundItems.end(), [&](const auto& groundItem) {
				return groundItem.position == snapshot.currentPosition && groundItem.itemId == replacement.cargo.itemId &&
				       groundItem.clientId == replacement.cargo.clientId && groundItem.count < 100 &&
				       replacement.count > 100 - groundItem.count;
			});
		if (groundAutoStackWouldSplit) {
			session.suppressLootItem(item.itemId);
			decision.command.type = PlayerBotLootCommandType::Wait;
			decision.command.outcome = placement.result == PlayerBotLootPlacementResult::NoSlot ?
				PlayerBotLootOutcome::NoSlot : PlayerBotLootOutcome::NoCapacity;
			decision.command.item = item;
			return decision;
		}
		if (!snapshot.canDoAction) {
			decision.command.type = PlayerBotLootCommandType::Wait;
			return decision;
		}
		std::vector<PlayerBotLootGroundItemState> destinationItems;
		for (const auto& groundItem : snapshot.groundItems) {
			if (groundItem.position == snapshot.currentPosition && groundItem.itemId == replacement.cargo.itemId) {
				destinationItems.push_back(groundItem);
			}
		}
		session.beginDiscardMove({replacement.cargo.itemId, replacement.count,
		                          inventoryCount(replacement.cargo.itemId),
		                          static_cast<uint32_t>(replacement.discardedValue), item.itemId,
		                          replacement.cargo.source, replacement.cargo.item,
		                          replacement.cargo.sourceCount, snapshot.currentPosition,
		                          groundItemCount(replacement.cargo.itemId), std::move(destinationItems)});
		replacementPlan = replacement;
		decision.command.type = PlayerBotLootCommandType::DiscardCargo;
		decision.command.item = item;
		decision.command.cargo = replacement.cargo;
		decision.command.itemDestination = replacement.destination;
		decision.command.count = replacement.count;
		return decision;
	}
	session.suppressLootItem(item.itemId);
	decision.command.type = PlayerBotLootCommandType::Wait;
	decision.command.outcome = placement.result == PlayerBotLootPlacementResult::NoSlot ?
		PlayerBotLootOutcome::NoSlot : PlayerBotLootOutcome::NoCapacity;
	decision.command.item = item;
	return decision;
}
