#ifndef FS_PLAYERBOTHUNTTIMING_H
#define FS_PLAYERBOTHUNTTIMING_H

#include <algorithm>
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

// add() must cover every counter.
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
	uint64_t topologyQueries = 0, topologyCacheHits = 0, topologyExpandedNodes = 0, coarseRejects = 0;
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

	// Sums work counters; request identity and funds describe the latest call.
	void add(const PlayerBotHuntRouteTiming& other)
	{
		attempts += other.attempts;
		walkingReached += other.walkingReached;
		npcReturned += other.npcReturned;
		npcReached += other.npcReached;
		walkingExpandedNodes += other.walkingExpandedNodes;
		npcReturnedExpandedNodes += other.npcReturnedExpandedNodes;
		requestSequence = other.requestSequence;
		transportSpendableFunds = other.transportSpendableFunds;
		localSearches += other.localSearches;
		localExpandedNodes += other.localExpandedNodes;
		nodeLimits += other.nodeLimits;
		localBoundStops += other.localBoundStops;
		topologyQueries += other.topologyQueries;
		topologyCacheHits += other.topologyCacheHits;
		topologyExpandedNodes += other.topologyExpandedNodes;
		coarseRejects += other.coarseRejects;
		coarseConnections += other.coarseConnections;
		localConnections += other.localConnections;
		connectionCacheHits += other.connectionCacheHits;
		unknownConnections += other.unknownConnections;
		boundRejects += other.boundRejects;
		riskRejects += other.riskRejects;
		graphLabels += other.graphLabels;
		yields += other.yields;
		sharedCacheHits += other.sharedCacheHits;
		sharedCacheMisses += other.sharedCacheMisses;
		incompleteCacheHits += other.incompleteCacheHits;
		hierarchyFallbacks += other.hierarchyFallbacks;
		ordinarySearches += other.ordinarySearches;
		sourceTreeHits += other.sourceTreeHits;
		alternateItineraries += other.alternateItineraries;
		corridorSearches += other.corridorSearches;
		corridorWidenings += other.corridorWidenings;
		guidedSearches += other.guidedSearches;
		heuristicBuilds += other.heuristicBuilds;
		heuristicRestarts += other.heuristicRestarts;
		transportRestarts += other.transportRestarts;
		transportRestartLimits += other.transportRestartLimits;
		requestRestartLimits += other.requestRestartLimits;
		if (other.invalidations) {
			invalidationReason = other.invalidationReason;
			invalidationCause = other.invalidationCause;
			invalidationChangedTile = other.invalidationChangedTile;
			invalidationJournalDelta = other.invalidationJournalDelta;
			invalidationWatchedTiles = other.invalidationWatchedTiles;
		}
		invalidations += other.invalidations;
		walkingTime += other.walkingTime;
		npcTime += other.npcTime;
	}
};

// Work measured by admitted selectHuntRegion calls. Quiet route-search calls
// share one hunt_planning_slice record, so every field except the maxima sums
// over `slices` calls.
struct PlayerBotHuntSliceCounters {
	uint32_t slices = 1;
	int64_t elapsedUs = 0, maxElapsedUs = 0, maxScheduleLateUs = 0;
	std::array<int64_t, static_cast<size_t>(PlayerBotHuntSlicePart::Count)> partUs{};
	uint32_t transportWork = 0, scoreWork = 0, candidateEmissions = 0;
	uint32_t outboundChecks = 0, depotChecks = 0, depotUnavailableChecks = 0, supplierChecks = 0;
	uint32_t discoveryChecks = 0, depotDiscoveryCalls = 0, supplierDiscoveryCalls = 0;
	std::chrono::steady_clock::duration outboundRoute{}, depotRoute{}, supplierRoute{};
	std::chrono::steady_clock::duration depotDiscovery{}, supplierDiscovery{};
	PlayerBotHuntRouteTiming routes;

	void add(const PlayerBotHuntSliceCounters& other)
	{
		slices += other.slices;
		elapsedUs += other.elapsedUs;
		maxElapsedUs = std::max(maxElapsedUs, other.maxElapsedUs);
		maxScheduleLateUs = std::max(maxScheduleLateUs, other.maxScheduleLateUs);
		for (size_t part = 0; part < partUs.size(); ++part) partUs[part] += other.partUs[part];
		transportWork += other.transportWork;
		scoreWork += other.scoreWork;
		candidateEmissions += other.candidateEmissions;
		outboundChecks += other.outboundChecks;
		depotChecks += other.depotChecks;
		depotUnavailableChecks += other.depotUnavailableChecks;
		supplierChecks += other.supplierChecks;
		discoveryChecks += other.discoveryChecks;
		depotDiscoveryCalls += other.depotDiscoveryCalls;
		supplierDiscoveryCalls += other.supplierDiscoveryCalls;
		outboundRoute += other.outboundRoute;
		depotRoute += other.depotRoute;
		supplierRoute += other.supplierRoute;
		depotDiscovery += other.depotDiscovery;
		supplierDiscovery += other.supplierDiscovery;
		routes.add(other.routes);
	}
	int64_t us(PlayerBotHuntSlicePart part) const { return partUs[static_cast<size_t>(part)]; }
};

#endif
