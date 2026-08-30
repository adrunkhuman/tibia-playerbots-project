/**
 * Per-bot depot selection, container opening, and deposit verification state.
 * World discovery, movement dispatch, and telemetry remain controller work.
 */
#ifndef FS_PLAYERBOTDEPOTSESSION_H
#define FS_PLAYERBOTDEPOTSESSION_H

#include <cstdint>
#include "position.h"

#include <chrono>
#include <map>
#include <vector>

enum slots_t : uint8_t;

enum class PlayerBotDepotStage : uint8_t {
	Discover,
	Approach,
	OpenLocker,
	OpenChest,
	Deposit,
	VerifyMove,
	Depart,
};

struct PlayerBotDepotCandidate {
	uint16_t depotId = 0;
	uint16_t lockerItemId = 0;
	Position lockerPosition;
	Position approachPosition;
	uint32_t distance = 0;
};

struct PlayerBotDepotMove {
	uint16_t itemId = 0;
	uint32_t destinationCount = 0;
	uint32_t inventoryCount = 0;
	uint8_t requestedCount = 0;
	slots_t sourceSlot;
};

enum class PlayerBotDepotMoveResult : uint8_t {
	Moved,
	Mismatch,
	Retry,
	Rejected,
};

struct PlayerBotDepotMoveVerification {
	PlayerBotDepotMove before;
	uint32_t inventoryCount = 0;
	uint32_t destinationCount = 0;
	uint32_t movedCount = 0;
	uint32_t attempts = 0;
	PlayerBotDepotMoveResult result;
};

class PlayerBotDepotSession
{
	public:
	void reset();
	void clearDiscovery();

	PlayerBotDepotStage stage() const { return depotStage; }

	private:
	friend class PlayerBotDepotWorkflow;
	void discover() { depotStage = PlayerBotDepotStage::Discover; }
	void openLocker() { depotStage = PlayerBotDepotStage::OpenLocker; }
	void openChest() { depotStage = PlayerBotDepotStage::OpenChest; }
	void deposit() { depotStage = PlayerBotDepotStage::Deposit; }
	void depart() { depotStage = PlayerBotDepotStage::Depart; }
	uint32_t attempts() const { return depotAttempts; }
	uint32_t incrementAttempts() { return ++depotAttempts; }
	void resetAttempts() { depotAttempts = 0; }

	uint16_t depotId() const { return selectedDepotId; }
	uint16_t lockerItemId() const { return selectedLockerItemId; }
	const Position& lockerPosition() const { return selectedLockerPosition; }
	const Position& approachPosition() const { return selectedApproachPosition; }
	bool hasSelectedDepot() const { return depotSelected; }
	void select(PlayerBotDepotCandidate candidate);

	bool candidatesPrepared() const { return candidatesReady; }
	void prepareCandidates(const Position& anchor);
	const Position& discoveryAnchor() const { return anchorPosition; }
	const std::vector<PlayerBotDepotCandidate>& candidates() const { return depotCandidates; }
	size_t nextCandidate() const { return nextCandidateIndex; }
	void advanceCandidate() { ++nextCandidateIndex; }
	void resetCandidates();

	uint32_t indexedCandidateCount() const { return indexedCandidates; }
	uint32_t inScopeCandidateCount() const { return inScopeCandidates; }
	uint32_t standableCandidateCount() const { return standableCandidates; }
	uint32_t suppressedApproachCount() const { return suppressedApproaches; }

	void expireRejectedApproaches(std::chrono::steady_clock::time_point now);
	bool isApproachRejected(const Position& position) const;
	void rejectApproach(const Position& position, std::chrono::steady_clock::time_point expires);
	void clearRejectedApproaches();
	const std::map<Position, std::chrono::steady_clock::time_point>& rejectedApproaches() const { return rejectedApproachTimes; }

	bool hasPendingMove() const { return pendingMove.itemId != 0; }
	const PlayerBotDepotMove& move() const { return pendingMove; }
	void beginMove(PlayerBotDepotMove move);
	void clearMove();
	PlayerBotDepotMoveVerification verifyMove(uint32_t inventoryCount, uint32_t destinationCount,
	                                          uint32_t maximumAttempts);

	PlayerBotDepotStage depotStage = PlayerBotDepotStage::Discover;
	uint32_t depotAttempts = 0;
	uint16_t selectedDepotId = 0;
	bool depotSelected = false;
	uint16_t selectedLockerItemId = 0;
	Position selectedLockerPosition;
	Position selectedApproachPosition;
	std::vector<PlayerBotDepotCandidate> depotCandidates;
	std::map<Position, std::chrono::steady_clock::time_point> rejectedApproachTimes;
	size_t nextCandidateIndex = 0;
	uint32_t indexedCandidates = 0;
	uint32_t inScopeCandidates = 0;
	uint32_t standableCandidates = 0;
	uint32_t suppressedApproaches = 0;
	Position anchorPosition;
	bool candidatesReady = false;
	PlayerBotDepotMove pendingMove{};
};

#endif
