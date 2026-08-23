/** Depot state machine over a DepotSession. World inspection and dispatch remain external. */
#ifndef FS_PLAYERBOTDEPOTWORKFLOW_H
#define FS_PLAYERBOTDEPOTWORKFLOW_H

#include "playerbotdepotsession.h"

#include <optional>

enum class PlayerBotDepotCommandType : uint8_t { None, Approach, OpenLocker, OpenChest, Deposit, Depart };
enum class PlayerBotDepotOutcome : uint8_t { Pending, Ready, Retry, Moved, Partial, Deferred, Rejected, Unavailable };

class PlayerBotDepotWorkflow
{
	public:
		void reset();
		void clearDiscovery();
		PlayerBotDepotStage stage() const { return session.stage(); }
		void setStage(PlayerBotDepotStage stage) { session.setStage(stage); }
		uint32_t attempts() const { return session.attempts(); }
		uint32_t incrementAttempts() { return session.incrementAttempts(); }
		void resetAttempts() { session.resetAttempts(); }
		uint16_t depotId() const { return session.depotId(); }
		uint16_t lockerItemId() const { return session.lockerItemId(); }
		const Position& lockerPosition() const { return session.lockerPosition(); }
		const Position& approachPosition() const { return session.approachPosition(); }
		bool hasSelectedDepot() const { return session.hasSelectedDepot(); }
		void select(PlayerBotDepotCandidate candidate) { session.select(candidate); }

		bool candidatesPrepared() const { return session.candidatesPrepared(); }
		void beginDiscovery(const Position& anchor) { session.prepareCandidates(anchor); }
		const Position& discoveryAnchor() const { return session.discoveryAnchor(); }
		void recordIndexedCandidate() { ++indexedCandidates; }
		void recordInScopeCandidate() { ++inScopeCandidates; }
		void recordStandableCandidate() { ++standableCandidates; }
		void recordSuppressedApproach() { ++suppressedApproaches; }
		void recordCandidate(PlayerBotDepotCandidate candidate);
		void sortCandidates();
		bool hasCandidates() const { return !discoveryCandidates.empty(); }
		bool hasNextCandidate() const;
		std::optional<PlayerBotDepotCandidate> takeNextCandidate();
		uint32_t indexedCandidateCount() const { return indexedCandidates; }
		uint32_t inScopeCandidateCount() const { return inScopeCandidates; }
		uint32_t standableCandidateCount() const { return standableCandidates; }
		uint32_t suppressedApproachCount() const { return suppressedApproaches; }

		void expireRejectedApproaches(std::chrono::steady_clock::time_point now) { session.expireRejectedApproaches(now); }
		bool isApproachRejected(const Position& position) const { return session.isApproachRejected(position); }
		void rejectApproach(const Position& position, std::chrono::steady_clock::time_point expires) { session.rejectApproach(position, expires); }
		std::optional<std::chrono::steady_clock::time_point> earliestRejectedApproachExpiry() const;

		bool hasPendingMove() const { return session.hasPendingMove(); }
		const PlayerBotDepotMove& move() const { return session.move(); }
		void beginMove(PlayerBotDepotMove move) { session.beginMove(move); }
		void clearMove() { session.clearMove(); }
		PlayerBotDepotMoveVerification verifyMove(uint32_t inventory, uint32_t destination, uint32_t maximumAttempts)
		{
			return session.verifyMove(inventory, destination, maximumAttempts);
		}

	private:
		PlayerBotDepotSession session;
		std::vector<PlayerBotDepotCandidate> discoveryCandidates;
		size_t nextCandidateOffset = 0;
		uint32_t indexedCandidates = 0;
		uint32_t inScopeCandidates = 0;
		uint32_t standableCandidates = 0;
		uint32_t suppressedApproaches = 0;
};

#endif
