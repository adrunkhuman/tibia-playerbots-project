/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "creature.h"
#include "playerbotdepotsession.h"

void PlayerBotDepotSession::reset()
{
	depotAttempts = 0;
	clearDiscovery();
	clearRejectedApproaches();
	depotStage = PlayerBotDepotStage::Discover;
	clearMove();
}

void PlayerBotDepotSession::clearDiscovery()
{
	selectedDepotId = 0;
	depotSelected = false;
	selectedLockerItemId = 0;
	selectedLockerPosition = Position();
	selectedApproachPosition = Position();
	resetCandidates();
}

void PlayerBotDepotSession::select(PlayerBotDepotCandidate candidate)
{
	selectedDepotId = candidate.depotId;
	depotSelected = true;
	selectedLockerItemId = candidate.lockerItemId;
	selectedLockerPosition = candidate.lockerPosition;
	selectedApproachPosition = candidate.approachPosition;
	depotStage = PlayerBotDepotStage::Approach;
	depotAttempts = 0;
	depotCandidates.clear();
	nextCandidateIndex = 0;
	anchorPosition = Position();
	candidatesReady = false;
}

void PlayerBotDepotSession::prepareCandidates(const Position& anchor)
{
	resetCandidates();
	anchorPosition = anchor;
	candidatesReady = true;
}

void PlayerBotDepotSession::resetCandidates()
{
	depotCandidates.clear();
	nextCandidateIndex = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
	anchorPosition = Position();
	candidatesReady = false;
}

void PlayerBotDepotSession::expireRejectedApproaches(std::chrono::steady_clock::time_point now)
{
	for (auto it = rejectedApproachTimes.begin(); it != rejectedApproachTimes.end();) {
		if (it->second <= now) {
			it = rejectedApproachTimes.erase(it);
		} else {
			++it;
		}
	}
}

bool PlayerBotDepotSession::isApproachRejected(const Position& position) const
{
	return rejectedApproachTimes.find(position) != rejectedApproachTimes.end();
}

void PlayerBotDepotSession::rejectApproach(const Position& position, std::chrono::steady_clock::time_point expires)
{
	rejectedApproachTimes[position] = expires;
}

void PlayerBotDepotSession::clearRejectedApproaches()
{
	rejectedApproachTimes.clear();
}

void PlayerBotDepotSession::beginMove(PlayerBotDepotMove move)
{
	pendingMove = move;
	depotStage = PlayerBotDepotStage::VerifyMove;
}

void PlayerBotDepotSession::clearMove()
{
	pendingMove = {};
	pendingMove.sourceSlot = CONST_SLOT_WHEREEVER;
}

PlayerBotDepotMoveVerification PlayerBotDepotSession::verifyMove(uint32_t inventoryCount, uint32_t destinationCount,
	                                                                uint32_t maximumAttempts)
{
	const PlayerBotDepotMove before = pendingMove;
	const uint32_t movedFromInventory = before.inventoryCount - std::min(before.inventoryCount, inventoryCount);
	const uint32_t movedToDepot = destinationCount - std::min(before.destinationCount, destinationCount);
	PlayerBotDepotMoveVerification verification{before, inventoryCount, destinationCount, movedFromInventory, depotAttempts,
	                                            PlayerBotDepotMoveResult::Retry};
	if (movedFromInventory != 0 && movedFromInventory == movedToDepot) {
		clearMove();
		resetAttempts();
		depotStage = PlayerBotDepotStage::Deposit;
		verification.result = PlayerBotDepotMoveResult::Moved;
		return verification;
	}
	if (movedFromInventory != 0 || movedToDepot != 0) {
		verification.result = PlayerBotDepotMoveResult::Mismatch;
		return verification;
	}
	verification.attempts = ++depotAttempts;
	if (depotAttempts < maximumAttempts) {
		clearMove();
		depotStage = PlayerBotDepotStage::Deposit;
		verification.result = PlayerBotDepotMoveResult::Retry;
		return verification;
	}
	if (before.sourceSlot != CONST_SLOT_WHEREEVER) {
		clearMove();
		resetAttempts();
		depotStage = PlayerBotDepotStage::Deposit;
		verification.result = PlayerBotDepotMoveResult::Deferred;
		return verification;
	}
	verification.result = PlayerBotDepotMoveResult::Rejected;
	return verification;
}
