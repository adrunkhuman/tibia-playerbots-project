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

#include "playerbotlootsession.h"

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
	selectedLoot.reset();
	pendingLoot.reset();
	pendingDiscard.reset();
	unavailableItems.clear();
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
	selectedLoot = move;
	pendingLoot = move;
}

std::optional<PlayerBotLootMoveVerification> PlayerBotLootSession::verifyLootMove(uint32_t inventoryCount)
{
	if (!pendingLoot) {
		return std::nullopt;
	}
	PlayerBotLootMoveVerification verification;
	verification.move = *pendingLoot;
	verification.inventoryCount = inventoryCount;
	verification.moved = inventoryCount > pendingLoot->inventoryCount;
	verification.movedCount = verification.moved ? inventoryCount - pendingLoot->inventoryCount : 0;
	if (!verification.moved) {
		suppressLootItem(pendingLoot->itemId);
	}
	pendingLoot.reset();
	return verification;
}

void PlayerBotLootSession::beginDiscardMove(PlayerBotLootDiscardMove move)
{
	pendingDiscard = move;
}

std::optional<PlayerBotLootDiscardVerification> PlayerBotLootSession::verifyDiscardMove(uint32_t inventoryCount)
{
	if (!pendingDiscard) {
		return std::nullopt;
	}
	PlayerBotLootDiscardVerification verification;
	verification.move = *pendingDiscard;
	verification.inventoryCount = inventoryCount;
	verification.discarded = inventoryCount + pendingDiscard->requestedCount <= pendingDiscard->inventoryCount;
	if (!verification.discarded) {
		suppressLootItem(pendingDiscard->incomingItemId);
	}
	pendingDiscard.reset();
	return verification;
}

bool PlayerBotLootSession::lootItemUnavailable(uint16_t itemId) const
{
	return unavailableItems.find(itemId) != unavailableItems.end();
}

void PlayerBotLootSession::suppressLootItem(uint16_t itemId)
{
	unavailableItems.insert(itemId);
}
