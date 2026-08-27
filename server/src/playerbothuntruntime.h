/**
 * Per-bot hunt state machine. The controller supplies authoritative player
 * state and executes requested navigation, combat, scheduling, and telemetry.
 */
#ifndef FS_PLAYERBOTHUNTRUNTIME_H
#define FS_PLAYERBOTHUNTRUNTIME_H

#include "playerbothuntplanningsession.h"
#include "playerbothuntpolicy.h"
#include "playerbotnavigationruntime.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct PlayerBotEquipmentHuntSummary;

enum class PlayerBotHuntRuntimeCommand : uint8_t {
	None,
	PlanningStarted,
	PlanningYield,
	PlanningScored,
	PlanningCancelled,
	RegionSelected,
	ScopeExhausted,
	ScopeReevaluationPending,
};

struct PlayerBotHuntRuntimePlayerObservation {
	Position position;
	uint32_t level = 0;
	int32_t health = 0;
	int32_t maximumHealth = 0;
	uint16_t staminaMinutes = 0;
	uint64_t experience = 0;
	uint64_t topologyGeneration = 0;
	std::set<uint64_t> excludedVariants;
	bool canUseRope = false;
	bool canUseShovel = false;
};

struct PlayerBotHuntRuntimeCooldownCommand {
	uint64_t variantId = 0;
	std::chrono::steady_clock::duration duration{};
};

struct PlayerBotHuntRuntimePlanningStartObservation {
	PlayerBotHuntRegionScan scan;
	PlayerBotHuntPlanningProfile profile;
	std::shared_ptr<const PlayerBotTopologyDistances> topologyDistances;
	uint64_t topologyDistanceTimeUs = 0;
};

struct PlayerBotHuntRuntimePlanningInput {
	PlayerBotHuntRuntimePlayerObservation player;
	uint64_t cacheRevision = 0;
	uint32_t huntDurationSeconds = 0;
	std::string reason;
	std::optional<PlayerBotHuntRuntimePlanningStartObservation> start;
};

struct PlayerBotHuntRuntimeScoreWork {
	size_t candidateIndex = 0;
	PlayerBotHuntPlanningProfile profile;
	uint64_t cacheRevision = 0;
	std::set<uint64_t> excludedVariants;
	std::map<uint64_t, PlayerBotHuntRegionPerformance> performance;
	std::shared_ptr<const PlayerBotTopologyDistances> topologyDistances;
	uint32_t huntDurationSeconds = 0;
};

struct PlayerBotHuntRuntimeScoreObservation {
	size_t candidateIndex = 0;
	bool valid = false;
	bool candidateFactsAvailable = false;
	bool withinPlanningScope = false;
	PlayerBotHuntRegion region;
};

struct PlayerBotHuntRuntimeOutcome {
	PlayerBotHuntRuntimeCommand command = PlayerBotHuntRuntimeCommand::None;
	std::vector<PlayerBotHuntRuntimeScoreWork> scoreWork;
	std::optional<PlayerBotHuntRegion> selectedRegion;
	std::vector<PlayerBotHuntRegion> candidates;
	bool staleRevision = false;
	bool invalidateCache = false;
	bool stopForScopeExhaustion = false;
	std::chrono::steady_clock::duration retryAfter{};
	uint32_t scopeExhaustionAttempt = 0;
};

// Callers supply fixture inputs as immutable planning observations. Runtime owns
// the resulting session and cache state transitions.
struct PlayerBotHuntPlanningObservation {
	bool candidatesAvailable = true;
	bool cancelAtScoreBarrier = false;
	bool invalidateCacheRevision = false;
};

struct PlayerBotHuntRuntimeCompletion {
	PlayerBotHuntRegion region;
	PlayerBotHuntCombatSummary combat;
	PlayerBotHuntPerformanceUpdate performance;
	PlayerBotHuntChallengeUpdate challenge;
	uint64_t durationSeconds = 0;
	uint64_t experienceGained = 0;
	uint32_t levelBefore = 0;
};

enum class PlayerBotHuntPatrolCommand : uint8_t {
	Continue,
	WaypointReached,
	WaitAtWaypoint,
	SkipWaypoint,
	RegionExhausted,
};

struct PlayerBotHuntPatrolOutcome {
	PlayerBotHuntPatrolCommand command = PlayerBotHuntPatrolCommand::Continue;
	Position destination;
	uint32_t waypoint = 0;
	std::optional<uint32_t> regionId;
	const char* reason = nullptr;
	uint32_t stepFailures = 0;
	uint32_t routeFailures = 0;
	uint64_t elapsedMs = 0;
	uint64_t expandedNodes = 0;
	std::optional<PlayerBotHuntRuntimeCooldownCommand> cooldown;
};

class PlayerBotHuntRuntime
{
	public:
		explicit PlayerBotHuntRuntime(std::vector<Position> fallbackPatrol);

		bool planningStartRequired(std::chrono::steady_clock::time_point now) const;
		PlayerBotHuntRuntimeOutcome advancePlanning(const PlayerBotHuntRuntimePlanningInput& input,
		                                            std::chrono::steady_clock::time_point now,
		                                            const PlayerBotHuntPlanningObservation& observation = {});
		PlayerBotHuntRuntimeOutcome completeScoreWork(const std::vector<PlayerBotHuntRuntimeScoreObservation>& observations,
		                                              uint64_t elapsedUs);
		// Keep the completed session through final telemetry, then release it.
		void completePlanningSelection() { planning.reset(); pendingScoreCandidates.clear(); }
		void selectPlanningRegion(PlayerBotHuntRegion region, const PlayerBotHuntRuntimePlayerObservation& player,
		                         std::chrono::steady_clock::time_point now) { activate(std::move(region), player, now); }
		void cancelPlanning() { planning.reset(); pendingScoreCandidates.clear(); }
		bool planningActive() const { return planning.has_value(); }
		std::optional<PlayerBotHuntPlanningSession> planningSession() const
		{
			if (planning) return *planning;
			return std::nullopt;
		}

		void beginCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds);
		bool deadlineReached(std::chrono::steady_clock::time_point now) const { return huntDeadline != std::chrono::steady_clock::time_point{} && now >= huntDeadline; }
		uint32_t completedCycles() const { return cycles; }
		bool active() const { return activeRegion.has_value(); }
		std::optional<PlayerBotHuntRegion> region() const { return activeRegion; }
		bool matchesMonster(const std::string& name) const;

		void sampleCombat(const PlayerBotHuntCombatSnapshot& snapshot) { policy.sampleCombat(snapshot); }
		void observeDamage(uint32_t damage) { policy.observeDamage(damage); }
		void observeRecovery(bool potion) { policy.observeRecovery(potion); }
		void observeKill() { policy.observeKill(); }
		bool observeDanger(int32_t maximumHealth, std::chrono::steady_clock::duration age) { return policy.observeDanger(maximumHealth, age); }
		std::optional<PlayerBotHuntRuntimeCooldownCommand> dangerObserved(int32_t maximumHealth,
		                                                                  std::chrono::steady_clock::time_point now,
		                                                                  std::chrono::steady_clock::duration cooldown);
		std::optional<PlayerBotHuntRuntimeCompletion> complete(const PlayerBotHuntRuntimePlayerObservation& player,
		                                                       std::chrono::steady_clock::time_point now,
		                                                       uint32_t configuredDurationSeconds);
		std::optional<PlayerBotHuntRuntimeCooldownCommand> observeDeath(bool activeCombat,
		                                                                std::chrono::steady_clock::duration cooldown);
		PlayerBotHuntPlanningProfile planningProfile(PlayerBotHuntPlanningProfile profile) const;
		std::map<uint64_t, PlayerBotHuntRegionPerformance> regionPerformance() const { return policy.regionPerformance(); }
		PlayerBotEquipmentHuntSummary summarizeEquipmentHunts(const std::vector<PlayerBotHuntRegion>& regions, bool truncated) const;

		PlayerBotHuntPatrolOutcome patrolTarget() const;
		PlayerBotHuntPatrolOutcome observePatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
		                                                   std::chrono::steady_clock::time_point now,
		                                                   uint32_t repeatedStepLimit, uint32_t routeFailureLimit);
		void resetPatrolFailures();

	private:
		PlayerBotHuntPlanningSnapshot snapshot(const PlayerBotHuntRuntimePlayerObservation& player, uint64_t revision);
		PlayerBotHuntRuntimeOutcome exhaustScope(std::chrono::steady_clock::time_point now,
		                                        std::chrono::steady_clock::duration retryAfter);
		void applyCandidateSuitability(PlayerBotHuntRegion& region, const PlayerBotHuntRuntimeScoreObservation& observation) const;
		void activate(PlayerBotHuntRegion region, const PlayerBotHuntRuntimePlayerObservation& player,
		              std::chrono::steady_clock::time_point now);

		std::optional<PlayerBotHuntPlanningSession> planning;
		PlayerBotHuntPolicy policy;
		std::optional<PlayerBotHuntRegion> activeRegion;
		std::vector<Position> fallbackPatrol;
		std::chrono::steady_clock::time_point scopeReevaluationAfter;
		std::chrono::steady_clock::time_point huntStarted;
		std::chrono::steady_clock::time_point huntDeadline;
		std::chrono::steady_clock::time_point patrolFailureStarted;
		Position patrolFailureTarget;
		size_t patrolIndex = 0;
		bool singleWaypointReached = false;
		uint32_t patrolRouteFailures = 0;
		uint64_t patrolFailureExpandedNodes = 0;
		uint32_t scopeExhaustions = 0;
		uint32_t cycles = 0;
		uint64_t huntStartExperience = 0;
		uint32_t huntStartLevel = 0;
		uint32_t plannedHuntDurationSeconds = 0;
		std::vector<size_t> pendingScoreCandidates;
};

#endif
