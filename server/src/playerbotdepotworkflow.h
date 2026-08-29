/** Depot state machine over a DepotSession. World inspection and dispatch remain external. */
#ifndef FS_PLAYERBOTDEPOTWORKFLOW_H
#define FS_PLAYERBOTDEPOTWORKFLOW_H

#include "playerbotdepotsession.h"

#include <optional>

enum class PlayerBotDepotCommandType : uint8_t {
	None,
	Scan,
	ValidateRoute,
	Navigate,
	OpenLocker,
	OpenChest,
	SelectDeposit,
	MoveDeposit,
	Depart,
	Fail,
	Wait,
};
enum class PlayerBotDepotOutcome : uint8_t { Pending, Ready, Success, Retry, Moved, Partial, Deferred, Rejected, Unavailable };
enum class PlayerBotDepotRouteResult : uint8_t { NotObserved, Reached, Unsafe, Unreachable };
enum class PlayerBotDepotActionResult : uint8_t {
	None,
	SelectedLockerUnavailable,
	MoveDestinationUnavailable,
	RejectedMoveDiscarded,
};

struct PlayerBotDepotScan {
	bool observed = false;
	uint32_t indexedCandidates = 0;
	uint32_t inScopeCandidates = 0;
	uint32_t standableCandidates = 0;
	std::vector<PlayerBotDepotCandidate> candidates;
};

struct PlayerBotDepotDepositSnapshot {
	bool observed = false;
	bool hasDepositableItem = false;
	PlayerBotDepotMove move;
};

struct PlayerBotDepotMoveObservation {
	bool observed = false;
	uint32_t inventoryCount = 0;
	uint32_t destinationCount = 0;
};

struct PlayerBotDepotObservation {
	Position currentPosition;
	std::chrono::steady_clock::time_point now;
	PlayerBotDepotScan scan;
	PlayerBotDepotRouteResult routeResult = PlayerBotDepotRouteResult::NotObserved;
	uint32_t routeSteps = 0;
	uint64_t expandedNodes = 0;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
	bool atApproach = false;
	bool lockerOpen = false;
	bool chestOpen = false;
	bool canDoAction = false;
	bool fixtureSynthetic = false;
	PlayerBotDepotActionResult actionResult = PlayerBotDepotActionResult::None;
	PlayerBotDepotDepositSnapshot deposit;
	PlayerBotDepotMoveObservation move;
};

struct PlayerBotDepotSnapshot {
	PlayerBotDepotStage stage = PlayerBotDepotStage::Discover;
	bool hasSelectedDepot = false;
	PlayerBotDepotCandidate selected;
	bool hasRouteCandidate = false;
	PlayerBotDepotCandidate routeCandidate;
	bool validatingRiskFallback = false;
	bool hasPendingMove = false;
	PlayerBotDepotMove pendingMove;
	uint32_t attempts = 0;
	uint32_t indexedCandidates = 0;
	uint32_t inScopeCandidates = 0;
	uint32_t standableCandidates = 0;
	uint32_t suppressedApproaches = 0;
	uint32_t unsafeRouteCandidates = 0;
	std::optional<std::chrono::steady_clock::time_point> retryAt;
	std::optional<std::chrono::steady_clock::time_point> deferredDepositRetryAt;
};

struct PlayerBotDepotTelemetry {
	uint32_t routeSteps = 0;
	uint64_t expandedNodes = 0;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
	bool riskFallback = false;
	std::optional<PlayerBotDepotMoveVerification> moveVerification;
};

struct PlayerBotDepotCommand {
	PlayerBotDepotCommandType type = PlayerBotDepotCommandType::None;
	PlayerBotDepotOutcome outcome = PlayerBotDepotOutcome::Pending;
	PlayerBotDepotSnapshot snapshot;
	PlayerBotDepotTelemetry telemetry;
};

class PlayerBotDepotWorkflow
{
	public:
		void reset();
		PlayerBotDepotSnapshot snapshot() const;
		PlayerBotDepotCommand advance(const PlayerBotDepotObservation& observation, uint32_t routeBudget,
		                              uint32_t maximumDiscoveryAttempts, std::chrono::steady_clock::duration suppression);

	private:
		void clearDiscovery();
		void recordCandidate(PlayerBotDepotCandidate candidate);
		void recordUnsafeCandidate(const PlayerBotDepotObservation& observation);
		void sortCandidates();
		bool hasCandidates() const { return !discoveryCandidates.empty(); }
		bool hasNextCandidate() const;
		std::optional<PlayerBotDepotCandidate> takeNextCandidate();
		std::optional<std::chrono::steady_clock::time_point> earliestRejectedApproachExpiry() const;
		std::optional<std::chrono::steady_clock::time_point> earliestDeferredDepositExpiry() const;
		PlayerBotDepotCommand command(PlayerBotDepotCommandType type, PlayerBotDepotOutcome outcome,
		                              PlayerBotDepotTelemetry telemetry = {}) const;

		PlayerBotDepotSession session;
		std::vector<PlayerBotDepotCandidate> discoveryCandidates;
		std::optional<PlayerBotDepotCandidate> routeCandidate;
		struct UnsafeCandidate {
			PlayerBotDepotCandidate candidate;
			uint32_t dangerCost = 0;
			double maximumHealthLossPerSecond = 0;
			uint32_t routeSteps = 0;
		};
		std::optional<UnsafeCandidate> riskFallback;
		bool validatingRiskFallback = false;
		size_t nextCandidateOffset = 0;
		uint32_t indexedCandidates = 0;
		uint32_t inScopeCandidates = 0;
		uint32_t standableCandidates = 0;
		uint32_t suppressedApproaches = 0;
		uint32_t unsafeRouteCandidates = 0;
		std::map<std::pair<uint16_t, slots_t>, std::chrono::steady_clock::time_point> deferredSlottedDeposits;
};

#endif
