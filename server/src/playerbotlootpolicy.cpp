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

#include "playerbotlootpolicy.h"

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

PlayerBotLootReplacement PlayerBotLootPolicy::replacementFor(const PlayerBotLootItemSnapshot& incoming,
	const PlayerBotLootInventorySnapshot& inventory) const
{
	PlayerBotLootReplacement replacement;
	const uint32_t incomingWeight = incoming.unitWeight * incoming.count;
	uint32_t requiredWeight = incomingWeight > inventory.freeCapacity ? incomingWeight - inventory.freeCapacity : 0;
	std::vector<PlayerBotLootCargoSnapshot> candidates;
	for (const PlayerBotLootCargoSnapshot& cargo : inventory.cargo) {
		if (cargo.replaceable) candidates.push_back(cargo);
	}
	std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
		const uint64_t leftDensity = static_cast<uint64_t>(left.unitValue) * right.unitWeight;
		const uint64_t rightDensity = static_cast<uint64_t>(right.unitValue) * left.unitWeight;
		return leftDensity == rightDensity ? left.itemId < right.itemId : leftDensity < rightDensity;
	});
	uint64_t totalDiscardedValue = 0;
	bool lowerDensity = true;
	for (const PlayerBotLootCargoSnapshot& candidate : candidates) {
		if (candidate.unitWeight == 0) continue;
		const uint32_t count = std::min<uint32_t>(candidate.count,
			(requiredWeight + candidate.unitWeight - 1) / candidate.unitWeight);
		if (count == 0) continue;
		if (replacement.count == 0) {
			replacement.cargo = candidate;
			replacement.count = static_cast<uint8_t>(count);
		}
		totalDiscardedValue += static_cast<uint64_t>(count) * candidate.unitValue;
		lowerDensity = lowerDensity &&
			static_cast<uint64_t>(incoming.unitValue) * candidate.unitWeight >
			static_cast<uint64_t>(candidate.unitValue) * incoming.unitWeight;
		const uint32_t releasedWeight = count * candidate.unitWeight;
		if (releasedWeight >= requiredWeight) {
			requiredWeight = 0;
			break;
		}
		requiredWeight -= releasedWeight;
	}
	replacement.discardedValue = totalDiscardedValue;
	replacement.viable = incomingWeight != 0 && incomingWeight > inventory.freeCapacity && replacement.count != 0 &&
	                    requiredWeight == 0 && lowerDensity &&
	                    (incoming.currency || static_cast<uint64_t>(incoming.unitValue) * incoming.count > totalDiscardedValue);
	return replacement;
}
