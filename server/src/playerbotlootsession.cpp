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

#include "playerbotlootsession.h"

#include <algorithm>

void PlayerBotLootSession::begin(uint32_t targetId, const Position& deathPosition, PlayerBotExpectedCorpse expectedCorpse,
                                 const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	reset();
	target = targetId;
	expectation = expectedCorpse;
	lastKnownDeathPosition = deathPosition;
	observedPosition = deathPosition;
	navigationFailurePosition = currentPosition;
	started = now;
}

void PlayerBotLootSession::reset()
{
	target = 0;
	expectation = {};
	lastKnownDeathPosition = Position();
	observedPosition = Position();
	navigationFailurePosition = Position();
	observedItemId = 0;
	observedOwnerId = 0;
	searchAttemptCount = 0;
	openAttemptCount = 0;
	navigationFailureCount = 0;
	consecutiveNavigationFailures = 0;
	navigationSuspensionCount = 0;
	started = {};
	retryAt = {};
	pendingLoot.reset();
	pendingDiscard.reset();
	pendingRecovery.reset();
	unavailableItems.clear();
	attemptedContainerAccess.clear();
	observed = false;
	corpseOpen = false;
	backpackOpen = false;
	looted = false;
	navigationPaused = false;
}

void PlayerBotLootSession::observeCorpse(uint16_t itemId, uint32_t ownerId, const Position& position)
{
	observedItemId = itemId;
	observedOwnerId = ownerId;
	observedPosition = position;
	observed = true;
}

bool PlayerBotLootSession::timedOut(std::chrono::steady_clock::time_point now,
                                    std::chrono::steady_clock::duration timeout) const
{
	return started.time_since_epoch().count() != 0 && now - started >= timeout;
}

int64_t PlayerBotLootSession::elapsedMilliseconds(std::chrono::steady_clock::time_point now) const
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
}

PlayerBotLootNavigationTransition PlayerBotLootSession::resumeNavigation(
	const Position& currentPosition, std::chrono::steady_clock::time_point now)
{
	if (!navigationPaused || (currentPosition == navigationFailurePosition && now < retryAt)) {
		return PlayerBotLootNavigationTransition::None;
	}
	navigationPaused = false;
	consecutiveNavigationFailures = 0;
	return PlayerBotLootNavigationTransition::Resumed;
}

PlayerBotLootNavigationTransition PlayerBotLootSession::observeNavigationFailure(
	const Position& currentPosition, std::chrono::steady_clock::time_point now, uint32_t maximumFailures,
	uint32_t suspendThreshold, std::chrono::milliseconds retryInterval)
{
	if (currentPosition != navigationFailurePosition) {
		navigationFailurePosition = currentPosition;
		consecutiveNavigationFailures = 0;
	}
	++navigationFailureCount;
	++consecutiveNavigationFailures;
	navigationFailurePosition = currentPosition;
	if (navigationFailureCount >= maximumFailures) {
		return PlayerBotLootNavigationTransition::Failed;
	}
	if (consecutiveNavigationFailures >= suspendThreshold) {
		navigationPaused = true;
		retryAt = now + retryInterval;
		++navigationSuspensionCount;
		return PlayerBotLootNavigationTransition::Suspended;
	}
	return PlayerBotLootNavigationTransition::None;
}

void PlayerBotLootSession::beginLootMove(PlayerBotLootMove move)
{
	pendingLoot = move;
}

std::optional<PlayerBotLootMoveVerification> PlayerBotLootSession::verifyLootMove(uint8_t sourceCount,
	uint32_t destinationCount, uint32_t inventoryCount)
{
	if (!pendingLoot) return std::nullopt;
	PlayerBotLootMoveVerification verification;
	verification.move = *pendingLoot;
	verification.inventoryCount = inventoryCount;
	const uint32_t sourceDelta = pendingLoot->sourceCount >= sourceCount ? pendingLoot->sourceCount - sourceCount : 0;
	const uint32_t destinationDelta = destinationCount >= pendingLoot->destinationCount ?
		destinationCount - pendingLoot->destinationCount : 0;
	verification.movedCount = std::min(sourceDelta, destinationDelta);
	verification.moved = sourceDelta == pendingLoot->requestedCount &&
	                     destinationDelta == pendingLoot->requestedCount;
	if (!verification.moved) suppressLootItem(pendingLoot->itemId);
	pendingLoot.reset();
	return verification;
}

void PlayerBotLootSession::cancelLootMove()
{
	if (pendingLoot) suppressLootItem(pendingLoot->itemId);
	pendingLoot.reset();
}

void PlayerBotLootSession::beginDiscardMove(PlayerBotLootDiscardMove move)
{
	pendingDiscard = move;
}

std::optional<PlayerBotLootDiscardVerification> PlayerBotLootSession::verifyDiscardMove(uint8_t sourceCount,
	const std::vector<PlayerBotLootGroundItemState>& destinationItems, uint32_t inventoryCount)
{
	if (!pendingDiscard) return std::nullopt;
	PlayerBotLootDiscardVerification verification;
	verification.move = *pendingDiscard;
	verification.inventoryCount = inventoryCount;
	const bool sourceDeltaExact = pendingDiscard->sourceCount >= sourceCount &&
	                              pendingDiscard->sourceCount - sourceCount == pendingDiscard->requestedCount;
	const PlayerBotLootGroundItemState* changedGroundItem = nullptr;
	for (const auto& observed : destinationItems) {
		if (observed.position != pendingDiscard->destination || observed.itemId != pendingDiscard->itemId) continue;
		auto previous = std::find_if(pendingDiscard->destinationItems.begin(), pendingDiscard->destinationItems.end(),
			[&observed](const auto& item) { return item.item == observed.item; });
		const uint8_t previousCount = previous == pendingDiscard->destinationItems.end() ? 0 : previous->count;
		if (observed.count < previousCount || observed.count - previousCount != pendingDiscard->requestedCount) continue;
		if (observed.item == pendingDiscard->sourceItem) {
			changedGroundItem = &observed;
			break;
		}
		if (!changedGroundItem) changedGroundItem = &observed;
	}
	if (sourceDeltaExact && changedGroundItem) {
		verification.groundItem = *changedGroundItem;
		verification.discarded = true;
	}
	if (!verification.discarded) suppressLootItem(pendingDiscard->incomingItemId);
	pendingDiscard.reset();
	return verification;
}

void PlayerBotLootSession::cancelDiscardMove()
{
	if (pendingDiscard) suppressLootItem(pendingDiscard->incomingItemId);
	pendingDiscard.reset();
}

void PlayerBotLootSession::beginRecoveryMove(PlayerBotLootRecoveryMove move)
{
	pendingRecovery = move;
}

std::optional<PlayerBotLootRecoveryVerification> PlayerBotLootSession::verifyRecoveryMove(uint8_t sourceCount,
	uint32_t destinationCount)
{
	if (!pendingRecovery) return std::nullopt;
	PlayerBotLootRecoveryVerification verification;
	verification.move = *pendingRecovery;
	const uint32_t sourceDelta = pendingRecovery->sourceCount >= sourceCount ?
		pendingRecovery->sourceCount - sourceCount : 0;
	const uint32_t destinationDelta = destinationCount >= pendingRecovery->destinationCount ?
		destinationCount - pendingRecovery->destinationCount : 0;
	verification.recoveredCount = std::min(sourceDelta, destinationDelta);
	verification.recovered = sourceDelta == pendingRecovery->requestedCount &&
	                         destinationDelta == pendingRecovery->requestedCount;
	pendingRecovery.reset();
	return verification;
}

void PlayerBotLootSession::cancelRecoveryMove()
{
	pendingRecovery.reset();
}

bool PlayerBotLootSession::lootItemUnavailable(uint16_t itemId) const
{
	return unavailableItems.find(itemId) != unavailableItems.end();
}

bool PlayerBotLootSession::containerAccessAttempted(const void* container) const
{
	return attemptedContainerAccess.find(container) != attemptedContainerAccess.end();
}

void PlayerBotLootSession::markContainerAccessAttempted(const void* container)
{
	if (container) attemptedContainerAccess.insert(container);
}

void PlayerBotLootSession::suppressLootItem(uint16_t itemId)
{
	unavailableItems.insert(itemId);
}
