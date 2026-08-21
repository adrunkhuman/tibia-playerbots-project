/**
 * Per-bot potion and food transaction state. Dispatch and telemetry remain
 * controller responsibilities; this session owns only verification and retry transitions.
 */
#ifndef FS_PLAYERBOTRECOVERYSESSION_H
#define FS_PLAYERBOTRECOVERYSESSION_H

#include <chrono>
#include <cstdint>
#include <optional>

struct PlayerBotPotionAttempt {
	int32_t health = 0;
	int32_t healthMaximum = 0;
	uint32_t potionCount = 0;
};

enum class PlayerBotPotionVerificationResult : uint8_t {
	Success,
	IneffectiveRecovery,
	UseNotVerified,
};

struct PlayerBotPotionVerification {
	PlayerBotPotionAttempt before;
	PlayerBotPotionAttempt after;
	PlayerBotPotionVerificationResult result;
};

struct PlayerBotFoodAttempt {
	uint16_t itemId = 0;
	uint32_t inventoryCount = 0;
	int32_t foodTicks = 0;
};

enum class PlayerBotFoodVerificationResult : uint8_t {
	Success,
	Full,
	Failed,
	Cooldown,
};

struct PlayerBotFoodVerification {
	PlayerBotFoodAttempt before;
	uint32_t inventoryCount = 0;
	int32_t foodTicks = 0;
	PlayerBotFoodVerificationResult result;
	uint32_t failures = 0;
	std::chrono::steady_clock::time_point retryAfter;
};

class PlayerBotRecoverySession
{
	public:
		bool hasPendingPotion() const;
		void beginPotion(PlayerBotPotionAttempt attempt);
		std::optional<PlayerBotPotionVerification> verifyPotion(PlayerBotPotionAttempt current,
		                                                        std::chrono::steady_clock::time_point now,
		                                                        std::chrono::steady_clock::duration retryDelay);
		bool canRetryPotion(std::chrono::steady_clock::time_point now) const;

		bool hasPendingFood() const;
		const PlayerBotFoodAttempt* pendingFood() const;
		void beginFood(PlayerBotFoodAttempt attempt);
		std::optional<PlayerBotFoodVerification> verifyFood(uint32_t inventoryCount, int32_t foodTicks, bool canEat,
		                                                    std::chrono::steady_clock::time_point now, uint32_t maximumFailures,
		                                                    std::chrono::steady_clock::duration retryDelay,
		                                                    std::chrono::steady_clock::duration failureCooldown);
		bool canRetryFood(std::chrono::steady_clock::time_point now) const;

	private:
		PlayerBotPotionAttempt potionAttempt;
		bool potionPending = false;
		std::chrono::steady_clock::time_point potionRetryAfter;
		PlayerBotFoodAttempt foodAttempt;
		bool foodPending = false;
		uint32_t foodFailures = 0;
		std::chrono::steady_clock::time_point foodRetryAfter;
};

#endif
