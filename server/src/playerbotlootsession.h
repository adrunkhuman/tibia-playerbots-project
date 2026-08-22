/**
 * Per-bot corpse access and loot transaction state.
 * Workflow-owned transaction bookkeeping; it has no world or inventory dependencies.
 */
#ifndef FS_PLAYERBOTLOOTSESSION_H
#define FS_PLAYERBOTLOOTSESSION_H

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>

#include "playerbottargetingsession.h"

class PlayerBotLootWorkflow;

enum class PlayerBotLootNavigationTransition : uint8_t {
	None,
	Resumed,
	Suspended,
	Failed,
};

struct PlayerBotLootMove {
	uint16_t itemId = 0;
	uint8_t requestedCount = 0;
	uint32_t inventoryCount = 0;
	uint8_t sourceIndex = 0;
};

struct PlayerBotLootMoveVerification {
	PlayerBotLootMove move;
	uint32_t inventoryCount = 0;
	uint32_t movedCount = 0;
	bool moved = false;
};

struct PlayerBotLootDiscardMove {
	uint16_t itemId = 0;
	uint8_t requestedCount = 0;
	uint32_t inventoryCount = 0;
	uint32_t value = 0;
	uint16_t incomingItemId = 0;
};

struct PlayerBotLootDiscardVerification {
	PlayerBotLootDiscardMove move;
	uint32_t inventoryCount = 0;
	bool discarded = false;
};

class PlayerBotLootSession
{
	public:
		uint32_t targetId() const { return target; }
		const PlayerBotExpectedCorpse& expectedCorpse() const { return expectation; }
		const Position& deathPosition() const { return lastKnownDeathPosition; }
		const Position& corpsePosition() const { return observedPosition; }
		uint16_t observedCorpseItemId() const { return observedItemId; }
		uint32_t observedCorpseOwnerId() const { return observedOwnerId; }
		bool corpseObserved() const { return observed; }

		bool corpseContainerOpen() const { return corpseOpen; }
		bool backpackContainerOpen() const { return backpackOpen; }

		uint32_t searchAttempts() const { return searchAttemptCount; }
		uint32_t openAttempts() const { return openAttemptCount; }
		bool lootedCurrentCorpse() const { return looted; }

		bool timedOut(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration timeout) const;
		int64_t elapsedMilliseconds(std::chrono::steady_clock::time_point now) const;
		bool navigationSuspended() const { return navigationPaused; }
		uint32_t navigationFailures() const { return navigationFailureCount; }
		uint32_t navigationSuspensions() const { return navigationSuspensionCount; }
		std::chrono::steady_clock::time_point navigationRetryAt() const { return retryAt; }

		bool hasPendingLootMove() const { return pendingLoot.has_value(); }
		const std::optional<PlayerBotLootMove>& pendingLootMove() const { return pendingLoot; }

		bool hasPendingDiscardMove() const { return pendingDiscard.has_value(); }
		const std::optional<PlayerBotLootDiscardMove>& pendingDiscardMove() const { return pendingDiscard; }

		bool lootItemUnavailable(uint16_t itemId) const;
		const std::set<uint16_t>& unavailableLootItems() const { return unavailableItems; }

	private:
		friend class PlayerBotLootWorkflow;

		void begin(uint32_t targetId, const Position& deathPosition, PlayerBotExpectedCorpse expectedCorpse,
		           const Position& currentPosition, std::chrono::steady_clock::time_point now);
		void reset();
		void observeCorpse(uint16_t itemId, uint32_t ownerId, const Position& position);
		uint32_t incrementSearchAttempts() { return ++searchAttemptCount; }
		uint32_t incrementOpenAttempts() { return ++openAttemptCount; }
		void markLooted() { looted = true; }
		PlayerBotLootNavigationTransition resumeNavigation(const Position& currentPosition,
		                                                  std::chrono::steady_clock::time_point now);
		PlayerBotLootNavigationTransition observeNavigationFailure(const Position& currentPosition,
		                                                          std::chrono::steady_clock::time_point now,
		                                                          uint32_t maximumFailures,
		                                                          uint32_t suspendThreshold,
		                                                          std::chrono::milliseconds retryInterval);
		void beginLootMove(PlayerBotLootMove move);
		std::optional<PlayerBotLootMoveVerification> verifyLootMove(uint32_t inventoryCount);
		void beginDiscardMove(PlayerBotLootDiscardMove move);
		std::optional<PlayerBotLootDiscardVerification> verifyDiscardMove(uint32_t inventoryCount);
		void suppressLootItem(uint16_t itemId);

		uint32_t target = 0;
		PlayerBotExpectedCorpse expectation;
		Position lastKnownDeathPosition;
		Position observedPosition;
		Position navigationFailurePosition;
		uint16_t observedItemId = 0;
		uint32_t observedOwnerId = 0;
		uint32_t searchAttemptCount = 0;
		uint32_t openAttemptCount = 0;
		uint32_t navigationFailureCount = 0;
		uint32_t consecutiveNavigationFailures = 0;
		uint32_t navigationSuspensionCount = 0;
		std::chrono::steady_clock::time_point started;
		std::chrono::steady_clock::time_point retryAt;
		std::optional<PlayerBotLootMove> pendingLoot;
		std::optional<PlayerBotLootDiscardMove> pendingDiscard;
		std::set<uint16_t> unavailableItems;
		bool observed = false;
		bool corpseOpen = false;
		bool backpackOpen = false;
		bool looted = false;
		bool navigationPaused = false;
};

#endif
