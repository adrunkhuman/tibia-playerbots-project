/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "playerbotlootpolicy.h"

#include <algorithm>

PlayerBotLootSelection PlayerBotLootPolicy::select(const std::vector<PlayerBotLootItemSnapshot>& items,
	const PlayerBotLootInventorySnapshot& inventory, const std::set<uint16_t>& unavailableItems) const
{
	PlayerBotLootSelection selection;
	bool skippedSurplusFood = false;
	for (const PlayerBotLootItemSnapshot& candidate : items) {
		const uint32_t candidateValue = std::max<uint32_t>(candidate.unitValue, candidate.food ? 1 : 0);
		if (candidateValue == 0 || unavailableItems.find(candidate.itemId) != unavailableItems.end()) {
			continue;
		}
		if (candidate.food && inventory.heldFood >= preferredFoodCount) {
			skippedSurplusFood = true;
			selection.item = candidate;
			continue;
		}
		if (selection.result != PlayerBotLootSelectionResult::Selected) {
			selection.result = PlayerBotLootSelectionResult::Selected;
			selection.item = candidate;
			continue;
		}
		const uint64_t candidateDensity = static_cast<uint64_t>(candidateValue) * selection.item.unitWeight;
		const uint64_t selectedDensity = static_cast<uint64_t>(selection.item.unitValue) * candidate.unitWeight;
		if (candidateDensity > selectedDensity ||
		    (candidateDensity == selectedDensity && candidateValue > selection.item.unitValue)) {
			selection.item = candidate;
		}
	}
	if (selection.result != PlayerBotLootSelectionResult::Selected) {
		selection.result = skippedSurplusFood ? PlayerBotLootSelectionResult::FoodPreferenceSatisfied :
		                                        PlayerBotLootSelectionResult::NoEligibleLoot;
	}
	if (selection.result == PlayerBotLootSelectionResult::Selected && selection.item.food) {
		selection.item.count = static_cast<uint8_t>(std::min<uint32_t>(selection.item.count,
			preferredFoodCount - inventory.heldFood));
	}
	return selection;
}

namespace {
	std::vector<PlayerBotLootDestinationSnapshot> destinationsFor(const PlayerBotLootItemSnapshot& incoming,
		const PlayerBotLootInventorySnapshot& inventory)
	{
		std::vector<PlayerBotLootDestinationSnapshot> destinations;
		if (incoming.stackable) {
			for (const PlayerBotLootCargoSnapshot& cargo : inventory.cargo) {
				if (!cargo.stackable || cargo.itemId != incoming.itemId || cargo.sourceCount >= 100 || cargo.containerId < 0) continue;
				auto container = std::find_if(inventory.containers.begin(), inventory.containers.end(), [&cargo](const auto& candidate) {
					return candidate.container == cargo.source && candidate.containerId == cargo.containerId;
				});
				if (container == inventory.containers.end() || !container->contentsComplete) continue;
				destinations.push_back({cargo.source, cargo.item, container->itemId, container->clientId,
				                        cargo.itemId, cargo.clientId, cargo.index, cargo.sourceCount,
				                        static_cast<uint8_t>(100 - cargo.sourceCount), cargo.containerId, true});
			}
		}
		for (const PlayerBotLootContainerSnapshot& container : inventory.containers) {
			if (!container.contentsComplete || container.containerId < 0 || container.size >= container.capacity) continue;
			destinations.push_back({container.container, nullptr, container.itemId, container.clientId, 0, 0,
			                        container.size, 0, static_cast<uint8_t>(incoming.stackable ? 100 : 1),
			                        container.containerId, false});
		}
		return destinations;
	}

	uint8_t capacityCount(const PlayerBotLootItemSnapshot& incoming, uint64_t capacity)
	{
		if (incoming.unitWeight == 0) return incoming.count;
		return static_cast<uint8_t>(std::min<uint64_t>(incoming.count, capacity / incoming.unitWeight));
	}

	bool lowerDensity(const PlayerBotLootItemSnapshot& incoming, const PlayerBotLootCargoSnapshot& cargo)
	{
		return cargo.unitWeight != 0 &&
		       static_cast<uint64_t>(incoming.unitValue) * cargo.unitWeight >
		       static_cast<uint64_t>(cargo.unitValue) * incoming.unitWeight;
	}
}

PlayerBotLootPlacement PlayerBotLootPolicy::placementFor(const PlayerBotLootItemSnapshot& incoming,
	const PlayerBotLootInventorySnapshot& inventory) const
{
	PlayerBotLootPlacement placement;
	const auto destinations = destinationsFor(incoming, inventory);
	if (destinations.empty()) return placement;
	placement.destination = destinations.front();
	placement.count = std::min<uint8_t>({incoming.count, placement.destination.availableCount,
	                                    capacityCount(incoming, inventory.freeCapacity)});
	placement.result = placement.count == 0 ? PlayerBotLootPlacementResult::NoCapacity :
	                                        PlayerBotLootPlacementResult::Placed;
	return placement;
}

PlayerBotLootReplacement PlayerBotLootPolicy::replacementFor(const PlayerBotLootItemSnapshot& incoming,
	const PlayerBotLootInventorySnapshot& inventory) const
{
	PlayerBotLootReplacement replacement;
	if (incoming.count == 0 || incoming.unitWeight == 0) return replacement;
	const auto destinations = destinationsFor(incoming, inventory);
	const bool needsSlot = destinations.empty();
	std::vector<PlayerBotLootCargoSnapshot> candidates;
	for (const PlayerBotLootCargoSnapshot& cargo : inventory.cargo) {
		if (cargo.replaceable && cargo.count != 0 && cargo.itemId != incoming.itemId && cargo.containerId >= 0) {
			candidates.push_back(cargo);
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
		const uint64_t leftDensity = static_cast<uint64_t>(left.unitValue) * right.unitWeight;
		const uint64_t rightDensity = static_cast<uint64_t>(right.unitValue) * left.unitWeight;
		return leftDensity == rightDensity ? left.itemId < right.itemId : leftDensity < rightDensity;
	});
	for (const PlayerBotLootCargoSnapshot& candidate : candidates) {
		if (!lowerDensity(incoming, candidate)) continue;
		PlayerBotLootDestinationSnapshot destination;
		uint32_t discardedCount = 0;
		uint8_t incomingCount = 0;
		if (needsSlot) {
			if (!candidate.wholeStackReplaceable) continue;
			const auto container = std::find_if(inventory.containers.begin(), inventory.containers.end(), [&candidate](const auto& observed) {
				return observed.container == candidate.source && observed.containerId == candidate.containerId;
			});
			if (container == inventory.containers.end() || !container->contentsComplete) continue;
			discardedCount = candidate.count;
			incomingCount = std::min<uint8_t>(incoming.count, capacityCount(incoming,
				static_cast<uint64_t>(inventory.freeCapacity) + static_cast<uint64_t>(discardedCount) * candidate.unitWeight));
			if (incomingCount == 0) continue;
			destination = {container->container, nullptr, container->itemId, container->clientId, 0, 0,
			               static_cast<uint8_t>(container->size - 1), 0,
			               static_cast<uint8_t>(incoming.stackable ? 100 : 1), container->containerId, false};
		} else {
			destination = destinations.front();
			const uint8_t destinationLimit = std::min<uint8_t>(incoming.count, destination.availableCount);
			incomingCount = std::min<uint8_t>(destinationLimit, capacityCount(incoming,
				static_cast<uint64_t>(inventory.freeCapacity) + static_cast<uint64_t>(candidate.count) * candidate.unitWeight));
			if (incomingCount == 0) continue;
			const uint64_t requiredWeight = static_cast<uint64_t>(incomingCount) * incoming.unitWeight > inventory.freeCapacity ?
				static_cast<uint64_t>(incomingCount) * incoming.unitWeight - inventory.freeCapacity : 0;
			discardedCount = static_cast<uint32_t>((requiredWeight + candidate.unitWeight - 1) / candidate.unitWeight);
			if (discardedCount == 0 || discardedCount > candidate.count) continue;
		}
		const uint64_t discardedValue = static_cast<uint64_t>(discardedCount) * candidate.unitValue;
		if (!incoming.currency && static_cast<uint64_t>(incoming.unitValue) * incomingCount <= discardedValue) continue;
		replacement.incoming = incoming;
		replacement.cargo = candidate;
		replacement.destination = destination;
		replacement.count = static_cast<uint8_t>(discardedCount);
		replacement.incomingCount = incomingCount;
		replacement.discardedValue = discardedValue;
		replacement.slotReplacement = needsSlot;
		replacement.viable = true;
		return replacement;
	}
	return replacement;
}
