/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotrecoverysession.h"

bool PlayerBotRecoverySession::hasPendingPotion() const
{
	return potionPending;
}

void PlayerBotRecoverySession::beginPotion(PlayerBotPotionAttempt attempt)
{
	potionAttempt = attempt;
	potionPending = true;
}

std::optional<PlayerBotPotionVerification> PlayerBotRecoverySession::verifyPotion(
	PlayerBotPotionAttempt current, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration retryDelay)
{
	if (!potionPending) {
		return std::nullopt;
	}
	PlayerBotPotionVerification verification;
	verification.before = potionAttempt;
	verification.after = current;
	verification.result = current.potionCount < potionAttempt.potionCount && current.health > potionAttempt.health ?
		PlayerBotPotionVerificationResult::Success : current.potionCount < potionAttempt.potionCount ?
		PlayerBotPotionVerificationResult::IneffectiveRecovery : PlayerBotPotionVerificationResult::UseNotVerified;
	if (verification.result != PlayerBotPotionVerificationResult::Success) {
		potionRetryAfter = now + retryDelay;
	}
	potionPending = false;
	return verification;
}

bool PlayerBotRecoverySession::canRetryPotion(std::chrono::steady_clock::time_point now) const
{
	return now >= potionRetryAfter;
}

bool PlayerBotRecoverySession::hasPendingFood() const
{
	return foodPending;
}

const PlayerBotFoodAttempt* PlayerBotRecoverySession::pendingFood() const
{
	return foodPending ? &foodAttempt : nullptr;
}

void PlayerBotRecoverySession::beginFood(PlayerBotFoodAttempt attempt)
{
	foodAttempt = attempt;
	foodPending = true;
}

std::optional<PlayerBotFoodVerification> PlayerBotRecoverySession::verifyFood(
	uint32_t inventoryCount, int32_t foodTicks, bool canEat, std::chrono::steady_clock::time_point now,
	uint32_t maximumFailures, std::chrono::steady_clock::duration retryDelay,
	std::chrono::steady_clock::duration failureCooldown)
{
	if (!foodPending) {
		return std::nullopt;
	}
	PlayerBotFoodVerification verification;
	verification.before = foodAttempt;
	verification.inventoryCount = inventoryCount;
	verification.foodTicks = foodTicks;
	if (inventoryCount + 1 == foodAttempt.inventoryCount && foodTicks > foodAttempt.foodTicks) {
		foodFailures = 0;
		verification.result = PlayerBotFoodVerificationResult::Success;
	} else if (inventoryCount == foodAttempt.inventoryCount && !canEat) {
		foodFailures = 0;
		verification.result = PlayerBotFoodVerificationResult::Full;
	} else if (++foodFailures >= maximumFailures) {
		foodFailures = 0;
		foodRetryAfter = now + failureCooldown;
		verification.result = PlayerBotFoodVerificationResult::Cooldown;
	} else {
		foodRetryAfter = now + retryDelay;
		verification.result = PlayerBotFoodVerificationResult::Failed;
	}
	verification.failures = foodFailures;
	verification.retryAfter = foodRetryAfter;
	foodPending = false;
	foodAttempt.itemId = 0;
	return verification;
}

bool PlayerBotRecoverySession::canRetryFood(std::chrono::steady_clock::time_point now) const
{
	return now >= foodRetryAfter;
}
