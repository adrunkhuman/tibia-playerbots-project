/**
 * Per-bot hunt state machine. The controller supplies authoritative player
 * state and executes requested navigation, combat, scheduling, and telemetry.
 */
#ifndef FS_PLAYERBOTHUNTRUNTIME_H
#define FS_PLAYERBOTHUNTRUNTIME_H

#include "playerbothuntplanningsession.h"
#include "playerbothuntpolicy.h"
#include "playerbotnavigationruntime.h"

#include <map>
#include <optional>

class Player;

enum class PlayerBotHuntRuntimeCommand : uint8_t {
	None,
	PlanningStarted,
	PlanningYield,
	RouteValidationRequested,
	RegionSelected,
	ScopeExhausted,
	ScopeReevaluationPending,
};

struct PlayerBotHuntRuntimeRouteWork {
	size_t regionIndex = 0;
	Position destination;
};

struct PlayerBotHuntRuntimeOutcome {
	PlayerBotHuntRuntimeCommand command = PlayerBotHuntRuntimeCommand::None;
	std::optional<PlayerBotHuntRuntimeRouteWork> routeWork;
	std::optional<PlayerBotHuntRegion> selectedRegion;
	std::vector<PlayerBotHuntRegion> candidates;
	bool staleRevision = false;
	bool stopForScopeExhaustion = false;
	std::chrono::steady_clock::duration retryAfter{};
	uint32_t scopeExhaustionAttempt = 0;
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
};

class PlayerBotHuntRuntime
{
	public:
		PlayerBotHuntRuntime(std::map<Position, std::chrono::steady_clock::time_point>& sharedCooldowns,
		                     std::vector<Position> fallbackPatrol);

		PlayerBotHuntRuntimeOutcome advancePlanning(Player& player, const char* reason,
		                                            std::chrono::steady_clock::time_point now,
		                                            uint32_t huntDurationSeconds);
		void completeRouteWork(Player& player, const PlayerBotHuntRuntimeRouteWork& work,
		                       const PlayerBotNavigationRoutePlan& routePlan, uint32_t huntDurationSeconds);
		void cancelPlanning() { planning.reset(); }
		bool planningActive() const { return planning.has_value(); }
		const PlayerBotHuntPlanningSession* planningSession() const { return planning ? &*planning : nullptr; }

		void beginCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds);
		bool deadlineReached(std::chrono::steady_clock::time_point now) const { return huntDeadline != std::chrono::steady_clock::time_point{} && now >= huntDeadline; }
		uint32_t completedCycles() const { return cycles; }
		bool active() const { return activeRegion.has_value(); }
		const PlayerBotHuntRegion* region() const { return activeRegion ? &*activeRegion : nullptr; }
		bool matchesMonster(const std::string& name) const;

		void sampleCombat(const PlayerBotHuntCombatSnapshot& snapshot) { policy.sampleCombat(snapshot); }
		void observeDamage(uint32_t damage) { policy.observeDamage(damage); }
		void observeRecovery(bool potion) { policy.observeRecovery(potion); }
		void observeKill() { policy.observeKill(); }
		bool observeDanger(int32_t maximumHealth, std::chrono::steady_clock::duration age) { return policy.observeDanger(maximumHealth, age); }
		bool dangerObserved(Player& player, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown);
		std::optional<PlayerBotHuntRuntimeCompletion> complete(Player& player, const char* reason,
		                                                       std::chrono::steady_clock::time_point now,
		                                                       uint32_t configuredDurationSeconds);
		void observeDeath(bool activeCombat, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown);
		const PlayerBotHuntPolicy& huntPolicy() const { return policy; }
		PlayerBotHuntPolicy& testPolicy() { return policy; }
		PlayerBotHuntRegionPlanner& testPlanner() { return planner; }
		const PlayerBotHuntRegionPlanner& testPlanner() const { return planner; }

		PlayerBotHuntPatrolOutcome patrolTarget() const;
		PlayerBotHuntPatrolOutcome observePatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
		                                                   std::chrono::steady_clock::time_point now,
		                                                   uint32_t repeatedStepLimit, uint32_t routeFailureLimit);
		void resetPatrolFailures();

		// Fixture-only hooks keep test mutation at the runtime boundary.
		void forceScopeExhaustionForTest() { forceScopeExhaustion = true; }

	private:
		PlayerBotHuntPlanningSnapshot snapshot(const Player& player, uint64_t revision,
		                                      std::chrono::steady_clock::time_point now);
		void activate(PlayerBotHuntRegion region, const Player& player, std::chrono::steady_clock::time_point now);

		PlayerBotHuntRegionPlanner planner;
		std::optional<PlayerBotHuntPlanningSession> planning;
		PlayerBotHuntPolicy policy;
		std::optional<PlayerBotHuntRegion> activeRegion;
		std::map<Position, std::chrono::steady_clock::time_point>& cooldowns;
		std::vector<Position> fallbackPatrol;
		std::chrono::steady_clock::time_point scopeReevaluationAfter;
		std::chrono::steady_clock::time_point huntStarted;
		std::chrono::steady_clock::time_point huntDeadline;
		std::chrono::steady_clock::time_point patrolFailureStarted;
		Position patrolFailureTarget;
		size_t patrolIndex = 0;
		uint32_t patrolRouteFailures = 0;
		uint64_t patrolFailureExpandedNodes = 0;
		uint32_t scopeExhaustions = 0;
		uint32_t cycles = 0;
		uint64_t huntStartExperience = 0;
		uint32_t huntStartLevel = 0;
		bool forceScopeExhaustion = false;
};

#endif
