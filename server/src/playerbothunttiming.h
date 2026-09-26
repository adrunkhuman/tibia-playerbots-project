#ifndef FS_PLAYERBOTHUNTTIMING_H
#define FS_PLAYERBOTHUNTTIMING_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Per-call only: no session-sized snapshots or state retained between slices.
enum class PlayerBotHuntSlicePart : size_t {
	Setup, Transport, Score, ScoreCompletion, Evidence, RouteBookkeeping, RouteChecks, Other, Count
};

struct PlayerBotHuntSliceTiming {
	using Clock = std::chrono::steady_clock;
	using Time = Clock::time_point;
	static constexpr int64_t overrunThresholdUs = 10000; // Observation, never a budget.

	Time started;
	Time boundary;
	PlayerBotHuntSlicePart part = PlayerBotHuntSlicePart::Setup;
	std::array<Clock::duration, static_cast<size_t>(PlayerBotHuntSlicePart::Count)> durations{};

	explicit PlayerBotHuntSliceTiming(Time now = Clock::now()) : started(now), boundary(now) {}
	void mark(PlayerBotHuntSlicePart next, Time now = Clock::now())
	{
		durations[static_cast<size_t>(part)] += now - boundary;
		boundary = now;
		part = next;
	}
	int64_t us(PlayerBotHuntSlicePart section) const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(durations[static_cast<size_t>(section)]).count();
	}
	int64_t elapsedUs() const
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(boundary - started).count();
	}
	static int64_t latenessUs(Time due, Time executed)
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(executed > due ? executed - due : Clock::duration::zero()).count();
	}
};

struct PlayerBotHuntSliceAttribution {
	uint64_t planningPass = 0;
	uint64_t scoringRevision = 0;
	uint64_t invalidatedPass = 0;
	uint64_t invalidatedRevision = 0;

	void update(uint64_t pass, uint64_t revision, uint64_t oldPass, uint64_t oldRevision)
	{
		if (oldPass) {
			invalidatedPass = oldPass;
			invalidatedRevision = oldRevision;
		}
		if (pass) {
			planningPass = pass;
			scoringRevision = revision;
		} else if (!planningPass && invalidatedPass) {
			planningPass = invalidatedPass;
			scoringRevision = invalidatedRevision;
		}
	}
};

struct PlayerBotHuntRouteTiming {
	uint32_t attempts = 0;
	uint32_t walkingReached = 0;
	uint32_t npcReturned = 0;
	uint32_t npcReached = 0;
	uint64_t walkingExpandedNodes = 0;
	uint64_t npcReturnedExpandedNodes = 0;
	uint64_t requestSequence = 0;
	uint64_t transportSpendableFunds = 0;
	uint64_t localSearches = 0, localExpandedNodes = 0, nodeLimits = 0, localBoundStops = 0;
	uint64_t topologyQueries = 0, topologyCacheHits = 0, topologyExpandedNodes = 0;
	uint64_t coarseConnections = 0, localConnections = 0, connectionCacheHits = 0;
	uint64_t unknownConnections = 0, boundRejects = 0, riskRejects = 0, graphLabels = 0;
	uint64_t yields = 0, invalidations = 0;
	uint64_t sharedCacheHits = 0, sharedCacheMisses = 0, incompleteCacheHits = 0;
	uint64_t hierarchyFallbacks = 0, ordinarySearches = 0, sourceTreeHits = 0;
	uint64_t alternateItineraries = 0, corridorSearches = 0, corridorWidenings = 0;
	uint64_t guidedSearches = 0, heuristicBuilds = 0, heuristicRestarts = 0;
	uint64_t transportRestarts = 0, transportRestartLimits = 0;
	uint64_t requestRestartLimits = 0;
	const char* invalidationReason = "none";
	const char* invalidationCause = "none";
	uint64_t invalidationChangedTile = 0, invalidationJournalDelta = 0;
	uint64_t invalidationWatchedTiles = 0;
	std::chrono::steady_clock::duration walkingTime{};
	std::chrono::steady_clock::duration npcTime{};
};

#endif
