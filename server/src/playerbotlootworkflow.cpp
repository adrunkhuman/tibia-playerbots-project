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

#include "playerbotlootworkflow.h"

PlayerBotLootWorkflow::PlayerBotLootWorkflow(PlayerBotLootWorkflowConfig config) : config(config), policy(config.preferredFoodCount) {}

PlayerBotLootCommand PlayerBotLootWorkflow::begin(uint32_t targetId, const Position& deathPosition, PlayerBotExpectedCorpse expectedCorpse,
	const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	session.begin(targetId, deathPosition, expectedCorpse, currentPosition, now);
	return {expectedCorpse.lootable ? PlayerBotLootCommandType::None : PlayerBotLootCommandType::Finish,
	        expectedCorpse.lootable ? PlayerBotLootOutcome::None : PlayerBotLootOutcome::CorpseNotLootable};
}

void PlayerBotLootWorkflow::reset() { session.reset(); }

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
	if (session.hasPendingLootMove()) {
		const uint16_t itemId = session.pendingLootMove()->itemId;
		const auto count = snapshot.inventory.itemCounts.find(itemId);
		const uint32_t inventoryCount = count == snapshot.inventory.itemCounts.end() ? 0 : count->second;
		decision.lootVerification = session.verifyLootMove(inventoryCount);
		if (decision.lootVerification->moved) session.markLooted();
	}
	if (session.hasPendingDiscardMove()) {
		const uint16_t itemId = session.pendingDiscardMove()->itemId;
		const auto count = snapshot.inventory.itemCounts.find(itemId);
		const uint32_t inventoryCount = count == snapshot.inventory.itemCounts.end() ? 0 : count->second;
		decision.discardVerification = session.verifyDiscardMove(inventoryCount);
	}
	if (snapshot.discoveredCorpse) {
		const auto& corpse = *snapshot.discoveredCorpse;
		session.observeCorpse(corpse.itemId, corpse.ownerId, corpse.position);
	}
	if (!Position::areInRange<1, 1, 0>(snapshot.currentPosition, session.corpsePosition())) {
		if (session.timedOut(snapshot.now, config.timeout)) {
			decision.command = {PlayerBotLootCommandType::Fail, PlayerBotLootOutcome::CorpseInaccessible};
			return decision;
		}
		if (session.navigationSuspended()) {
			if (session.resumeNavigation(snapshot.currentPosition, snapshot.now) != PlayerBotLootNavigationTransition::Resumed) {
				decision.command = {PlayerBotLootCommandType::Wait};
				return decision;
			}
		}
		decision.command = {PlayerBotLootCommandType::Navigate, PlayerBotLootOutcome::None, session.corpsePosition()};
		return decision;
	}
	if (!snapshot.discoveredCorpse) {
		if (session.corpseObserved()) {
			decision.command = {PlayerBotLootCommandType::Fail, PlayerBotLootOutcome::CorpseExpired};
		} else if (session.incrementSearchAttempts() >= config.maximumSearchAttempts) {
			decision.command = {PlayerBotLootCommandType::Finish, PlayerBotLootOutcome::OwnedCorpseUnavailable};
		} else {
			decision.command = {PlayerBotLootCommandType::Wait};
		}
		return decision;
	}
	if (!snapshot.corpseContainerOpen) {
		if (!snapshot.canDoAction) {
			decision.command = {PlayerBotLootCommandType::Wait};
		} else if (session.incrementOpenAttempts() > 2) {
			decision.command = {PlayerBotLootCommandType::Finish, PlayerBotLootOutcome::CorpseOpenFailed};
		} else {
			decision.command = {PlayerBotLootCommandType::OpenCorpse};
		}
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
		decision.command = {snapshot.canDoAction ? PlayerBotLootCommandType::OpenBackpack : PlayerBotLootCommandType::Wait};
		return decision;
	}
	const PlayerBotLootSelection availableSelection = policy.select(snapshot.corpseItems, snapshot.inventory,
	                                                               session.unavailableLootItems());
	if (availableSelection.result != PlayerBotLootSelectionResult::Selected) {
		decision.command = {PlayerBotLootCommandType::Finish,
		                    availableSelection.result == PlayerBotLootSelectionResult::FoodPreferenceSatisfied ?
		                        PlayerBotLootOutcome::FoodPreferenceSatisfied : PlayerBotLootOutcome::NoEligibleLoot,
		                    {}, availableSelection.item};
		return decision;
	}
	const PlayerBotLootItemSnapshot& item = availableSelection.item;
	if (item.unitWeight * item.count > snapshot.inventory.freeCapacity) {
		const PlayerBotLootReplacement replacement = policy.replacementFor(item, snapshot.inventory);
		if (!replacement.viable) {
			session.suppressLootItem(item.itemId);
			decision.command = {PlayerBotLootCommandType::Wait, PlayerBotLootOutcome::NoCapacity, {}, item};
			return decision;
		}
		if (replacement.cargo.containerId < 0) {
			decision.command = {PlayerBotLootCommandType::OpenCargo, PlayerBotLootOutcome::None, {}, item, replacement.cargo};
			return decision;
		}
		if (!snapshot.canDoAction) {
			decision.command = {PlayerBotLootCommandType::Wait};
			return decision;
		}
		const auto count = snapshot.inventory.itemCounts.find(replacement.cargo.itemId);
		session.beginDiscardMove({replacement.cargo.itemId, replacement.count,
		                          count == snapshot.inventory.itemCounts.end() ? 0 : count->second,
		                          static_cast<uint32_t>(replacement.count) * replacement.cargo.unitValue, item.itemId});
		decision.command = {PlayerBotLootCommandType::DiscardCargo, PlayerBotLootOutcome::None, {}, item, replacement.cargo,
		                    replacement.count};
		return decision;
	}
	if (!snapshot.canDoAction) {
		decision.command = {PlayerBotLootCommandType::Wait};
		return decision;
	}
	session.beginLootMove({item.itemId, item.count, item.inventoryCount, item.index});
	decision.command = {PlayerBotLootCommandType::MoveItem, PlayerBotLootOutcome::None, {}, item, {}, item.count};
	return decision;
}
