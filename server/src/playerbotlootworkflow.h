/** Corpse-loot state machine. The controller supplies world snapshots and executes its commands. */
#ifndef FS_PLAYERBOTLOOTWORKFLOW_H
#define FS_PLAYERBOTLOOTWORKFLOW_H

#include "playerbotlootpolicy.h"
#include "playerbotlootsession.h"

enum class PlayerBotLootCommandType : uint8_t {
	None,
	Navigate,
	OpenCorpse,
	OpenBackpack,
	OpenCargo,
	MoveItem,
	DiscardCargo,
	Finish,
	Fail,
	Wait,
};

enum class PlayerBotLootOutcome : uint8_t {
	None,
	CorpseNotLootable,
	CorpseExpired,
	CorpseInaccessible,
	OwnedCorpseUnavailable,
	CorpseOpenFailed,
	CorpseEmpty,
	BackpackUnavailable,
	FoodPreferenceSatisfied,
	NoEligibleLoot,
	NoCapacity,
};

struct PlayerBotLootCorpseSnapshot {
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint32_t ownerId = 0;
	Position position;
};

struct PlayerBotLootWorkflowSnapshot {
	Position currentPosition;
	std::chrono::steady_clock::time_point now;
	std::optional<PlayerBotLootCorpseSnapshot> discoveredCorpse;
	bool corpseContainerOpen = false;
	bool backpackAvailable = false;
	bool backpackContainerOpen = false;
	bool canDoAction = false;
	PlayerBotLootInventorySnapshot inventory;
	// Populated only when corpseContainerOpen is true for the observed corpse.
	std::vector<PlayerBotLootItemSnapshot> corpseItems;
};

struct PlayerBotLootCommand {
	PlayerBotLootCommandType type = PlayerBotLootCommandType::None;
	PlayerBotLootOutcome outcome = PlayerBotLootOutcome::None;
	Position destination;
	PlayerBotLootItemSnapshot item;
	PlayerBotLootCargoSnapshot cargo;
	uint8_t count = 0;
};

struct PlayerBotLootDecision {
	PlayerBotLootCommand command;
	std::optional<PlayerBotLootMoveVerification> lootVerification;
	std::optional<PlayerBotLootDiscardVerification> discardVerification;
};

struct PlayerBotLootWorkflowConfig {
	uint32_t maximumSearchAttempts = 0;
	uint32_t maximumNavigationFailures = 0;
	uint32_t navigationSuspendThreshold = 0;
	std::chrono::milliseconds navigationRetryInterval{};
	std::chrono::steady_clock::duration timeout{};
	uint32_t preferredFoodCount = 0;
};

class PlayerBotLootWorkflow
{
	public:
		explicit PlayerBotLootWorkflow(PlayerBotLootWorkflowConfig config);

		PlayerBotLootCommand begin(uint32_t targetId, const Position& deathPosition, PlayerBotExpectedCorpse expectedCorpse,
		                           const Position& currentPosition, std::chrono::steady_clock::time_point now);
		void reset();
		PlayerBotLootDecision advance(const PlayerBotLootWorkflowSnapshot& snapshot);
		PlayerBotLootNavigationTransition observeNavigationFailure(const Position& currentPosition,
		                                                          std::chrono::steady_clock::time_point now);
		PlayerBotLootNavigationTransition resumeNavigation(const Position& currentPosition,
		                                                   std::chrono::steady_clock::time_point now);

		bool hasPendingLootMove() const { return session.hasPendingLootMove(); }
		bool navigationSuspended() const { return session.navigationSuspended(); }
		bool timedOut(std::chrono::steady_clock::time_point now) const { return session.timedOut(now, config.timeout); }
		uint32_t targetId() const { return session.targetId(); }
		const PlayerBotExpectedCorpse& expectedCorpse() const { return session.expectedCorpse(); }
		const Position& deathPosition() const { return session.deathPosition(); }
		const Position& corpsePosition() const { return session.corpsePosition(); }
		bool corpseObserved() const { return session.corpseObserved(); }
		bool lootedCurrentCorpse() const { return session.lootedCurrentCorpse(); }
		uint32_t searchAttempts() const { return session.searchAttempts(); }
		uint32_t navigationFailures() const { return session.navigationFailures(); }
		uint32_t navigationSuspensions() const { return session.navigationSuspensions(); }
		int64_t elapsedMilliseconds(std::chrono::steady_clock::time_point now) const { return session.elapsedMilliseconds(now); }
		std::chrono::steady_clock::time_point navigationRetryAt() const { return session.navigationRetryAt(); }

	private:
		PlayerBotLootWorkflowConfig config;
		PlayerBotLootPolicy policy;
		PlayerBotLootSession session;
};

#endif
