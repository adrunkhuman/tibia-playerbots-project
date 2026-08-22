#include "otpch.h"

#include "playerbothuntruntime.h"

#include "configmanager.h"
#include "player.h"

extern ConfigManager g_config;

namespace {
	double projectedHuntStaminaMultiplier(const Player& player, double availableHuntSeconds)
	{
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		if (staminaMinutes == 0) return 0;
		if (!g_config.getBoolean(ConfigManager::STAMINA_SYSTEM)) return 1;
		if (staminaMinutes > 2400 && player.isPremium() && availableHuntSeconds > 0) {
			const double bonusSeconds = std::min(availableHuntSeconds, std::max<int32_t>(0, staminaMinutes - 2402) * 60.0);
			return 1 + 0.5 * bonusSeconds / availableHuntSeconds;
		}
		return staminaMinutes <= 840 ? 0.5 : 1;
	}
}

PlayerBotHuntRuntime::PlayerBotHuntRuntime(std::map<Position, std::chrono::steady_clock::time_point>& sharedCooldowns,
	                                         std::vector<Position> fallbackPatrol) :
	cooldowns(sharedCooldowns), fallbackPatrol(std::move(fallbackPatrol))
{}

PlayerBotHuntPlanningSnapshot PlayerBotHuntRuntime::snapshot(const Player& player, uint64_t revision,
	                                                          std::chrono::steady_clock::time_point now)
{
	std::set<Position> excluded;
	for (auto it = cooldowns.begin(); it != cooldowns.end();) {
		if (now >= it->second) it = cooldowns.erase(it);
		else { excluded.insert(it->first); ++it; }
	}
	return {player.getPosition(), player.getLevel(), player.getHealth(), player.getStaminaMinutes(), revision, std::move(excluded)};
}

PlayerBotHuntRuntimeOutcome PlayerBotHuntRuntime::advancePlanning(Player& player, const char* reason,
	                                                                std::chrono::steady_clock::time_point now,
	                                                                uint32_t huntDurationSeconds)
{
	PlayerBotHuntRuntimeOutcome outcome;
	const uint64_t revision = PlayerBotHuntRegionPlanner::getCacheRevision();
	const PlayerBotHuntPlanningSnapshot current = snapshot(player, revision, now);
	if (!planning && now < scopeReevaluationAfter) {
		outcome.command = PlayerBotHuntRuntimeCommand::ScopeReevaluationPending;
		outcome.retryAfter = scopeReevaluationAfter - now;
		return outcome;
	}
	if (planning && planning->invalidated(current)) {
		outcome.staleRevision = planning->snapshot().cacheRevision != revision;
		planning.reset();
	}
	if (!planning) {
		const PlayerBotHuntRegionScan scan = planner.beginScan(player);
		const Item* weapon = player.getWeapon(true);
		const PlayerBotCombatProfile combat{player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(),
			weapon ? weapon->getAttack() : 7, weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor()};
		planning.emplace(PlayerBotHuntPlanningStart{scan, playerBotHuntPlanningProfile(player, combat, policy.challengeFrontier()),
			snapshot(player, scan.revision, now), reason, now});
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningStarted;
		return outcome;
	}

	planning->beginTurn();
	if (planning->scoring()) {
		const auto started = std::chrono::steady_clock::now();
		while (const auto work = planning->nextScoringWork(32)) {
			PlayerBotHuntRegion region;
			if (!planner.score(player, planning->profile(), planning->snapshot().cacheRevision, work->candidateIndex,
			                  planning->snapshot().excludedRegions, policy.regionPerformance(), huntDurationSeconds, region)) {
				outcome.staleRevision = true;
				planning.reset();
				return outcome;
			}
			planning->scoreCompleted(std::move(region));
		}
		planning->addScoringTime(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
		if (planning->completeScoring() == PlayerBotHuntPlanningProgress::ScoringYield) {
			outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
			return outcome;
		}
		if (forceScopeExhaustion) {
			planning->rejectSuitableCandidates("fixture_scope_exhausted");
			forceScopeExhaustion = false;
		}
		outcome.command = PlayerBotHuntRuntimeCommand::PlanningYield;
		return outcome;
	}
	if (const auto work = planning->nextRouteValidationWork(1)) {
		outcome.command = PlayerBotHuntRuntimeCommand::RouteValidationRequested;
		outcome.routeWork = {{work->regionIndex, planning->region(work->regionIndex).destination}};
		return outcome;
	}

	const auto& regions = planning->regions();
	outcome.candidates = regions;
	if (playerBotHuntScopeExhausted(regions)) {
		++scopeExhaustions;
		const bool validationBudget = std::any_of(regions.begin(), regions.end(), [](const auto& region) { return region.rejectionReason == "navigation_node_budget"; });
		outcome.command = PlayerBotHuntRuntimeCommand::ScopeExhausted;
		outcome.scopeExhaustionAttempt = scopeExhaustions;
		outcome.stopForScopeExhaustion = scopeExhaustions >= 3;
		outcome.retryAfter = forceScopeExhaustion ? std::chrono::seconds(1) : std::chrono::seconds(30);
		(void) validationBudget;
		scopeReevaluationAfter = now + outcome.retryAfter;
		planning.reset();
		return outcome;
	}
	auto selected = std::max_element(regions.begin(), regions.end(), [](const auto& left, const auto& right) {
		return playerBotPreferHuntRegion(right, left);
	});
	activate(*selected, player, now);
	outcome.command = PlayerBotHuntRuntimeCommand::RegionSelected;
	outcome.selectedRegion = activeRegion;
	planning.reset();
	scopeExhaustions = 0;
	scopeReevaluationAfter = {};
	return outcome;
}

void PlayerBotHuntRuntime::completeRouteWork(Player& player, const PlayerBotHuntRuntimeRouteWork& work,
	                                          const PlayerBotNavigationRoutePlan& routePlan, uint32_t huntDurationSeconds)
{
	if (!planning) return;
	double travelSeconds = 0;
	for (const PlayerBotNavigationStep& step : routePlan.steps) travelSeconds += step.action == PlayerBotNavigationAction::Move ? player.getStepDuration(step.direction) / 1000.0 : 1.0;
	const double available = std::max(0.0, huntDurationSeconds - travelSeconds);
	planning->routeValidationCompleted(work.regionIndex, routePlan.metrics.attempted,
		routePlan.metrics.result == PlayerBotNavigationResult::Reached, routePlan.metrics.result == PlayerBotNavigationResult::NodeLimit,
		routePlan.metrics.expandedNodes, static_cast<uint32_t>(routePlan.steps.size()), travelSeconds,
		projectedHuntStaminaMultiplier(player, available), huntDurationSeconds);
	planning->completeRouteValidation();
}

void PlayerBotHuntRuntime::activate(PlayerBotHuntRegion region, const Player& player, std::chrono::steady_clock::time_point now)
{
	auto first = std::find(region.patrolPoints.begin(), region.patrolPoints.end(), region.destination);
	if (first != region.patrolPoints.end()) std::rotate(region.patrolPoints.begin(), first, region.patrolPoints.end());
	activeRegion = std::move(region);
	patrolIndex = 0;
	huntStarted = now;
	huntStartExperience = player.getExperience();
	huntStartLevel = player.getLevel();
	policy.resetCombatEvidence();
	resetPatrolFailures();
}

void PlayerBotHuntRuntime::beginCycle(std::chrono::steady_clock::time_point now, uint32_t durationSeconds)
{
	huntDeadline = now + std::chrono::seconds(durationSeconds);
	++cycles;
}

bool PlayerBotHuntRuntime::matchesMonster(const std::string& name) const
{
	return !activeRegion || std::any_of(activeRegion->monsters.begin(), activeRegion->monsters.end(), [&name](const auto& monster) {
		return strcasecmp(monster.name.c_str(), name.c_str()) == 0;
	});
}

PlayerBotEquipmentHuntSummary PlayerBotHuntRuntime::scoreEquipmentHunts(Player& player,
	const PlayerBotCombatProfile& profile, size_t maximumRegions) const
{
	PlayerBotEquipmentHuntSummary summary;
	summary.lowestThreatRatio = std::numeric_limits<double>::max();
	const PlayerBotHuntRegionScan scan = planner.beginScan(player);
	const PlayerBotHuntPlanningProfile planningProfile = playerBotHuntPlanningProfile(player, profile, policy.challengeFrontier());
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	for (size_t candidateIndex : scan.candidateIndices) {
		if (summary.evaluatedRegions >= maximumRegions) { summary.truncated = true; break; }
		PlayerBotHuntRegion region;
		if (!planner.score(player, planningProfile, scan.revision, candidateIndex, {}, policy.regionPerformance(), duration, region)) continue;
		++summary.evaluatedRegions;
		summary.lowestThreatRatio = std::min(summary.lowestThreatRatio, region.threatRatio);
		if (region.suitable) {
			++summary.suitableRegions;
			summary.bestProjectedExperience = std::max(summary.bestProjectedExperience, region.projectedExperience);
		}
	}
	if (summary.lowestThreatRatio == std::numeric_limits<double>::max()) summary.lowestThreatRatio = 0;
	return summary;
}

bool PlayerBotHuntRuntime::fixtureScoreRegion(Player& player, const PlayerBotCombatProfile& profile,
	PlayerBotHuntRegion& current, PlayerBotHuntRegion& improved) const
{
	const PlayerBotHuntRegionScan scan = planner.beginScan(player);
	if (scan.candidateIndices.empty()) return false;
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	const PlayerBotHuntPlanningProfile before = playerBotHuntPlanningProfile(player, profile, policy.challengeFrontier());
	PlayerBotCombatProfile upgraded = profile;
	upgraded.armor += 20;
	const PlayerBotHuntPlanningProfile after = playerBotHuntPlanningProfile(player, upgraded, policy.challengeFrontier());
	return planner.score(player, before, scan.revision, scan.candidateIndices.front(), {}, policy.regionPerformance(), duration, current) &&
	       planner.score(player, after, scan.revision, scan.candidateIndices.front(), {}, policy.regionPerformance(), duration, improved);
}

bool PlayerBotHuntRuntime::dangerObserved(Player& player, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown)
{
	if (!activeRegion || !policy.observeDanger(player.getMaxHealth(), now - huntStarted)) return false;
	cooldowns[activeRegion->center] = now + cooldown;
	return true;
}

std::optional<PlayerBotHuntRuntimeCompletion> PlayerBotHuntRuntime::complete(Player& player, const char* reason,
	                                                                           std::chrono::steady_clock::time_point now,
	                                                                           uint32_t configuredDurationSeconds)
{
	if (!activeRegion) return std::nullopt;
	PlayerBotHuntRuntimeCompletion result;
	result.region = *activeRegion;
	result.durationSeconds = static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(now - huntStarted).count()));
	result.experienceGained = player.getExperience() >= huntStartExperience ? player.getExperience() - huntStartExperience : 0;
	result.levelBefore = huntStartLevel;
	result.combat = policy.combatSummary();
	result.performance = policy.observePerformance(activeRegion->center, {result.durationSeconds, result.combat.kills, result.experienceGained,
		activeRegion->projectedExperience, activeRegion->observedCorrection, configuredDurationSeconds});
	result.challenge = policy.updateChallengeFrontier({result.durationSeconds, player.getMaxHealth()});
	activeRegion.reset();
	policy.resetCombatEvidence();
	return result;
}

void PlayerBotHuntRuntime::observeDeath(bool activeCombat, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown)
{
	if (activeCombat) policy.observeDeath();
	if (activeRegion) cooldowns[activeRegion->center] = now + cooldown;
}

PlayerBotHuntPatrolOutcome PlayerBotHuntRuntime::patrolTarget() const
{
	PlayerBotHuntPatrolOutcome outcome;
	const auto& points = activeRegion && !activeRegion->patrolPoints.empty() ? activeRegion->patrolPoints : fallbackPatrol;
	if (points.empty()) return outcome;
	outcome.destination = points[patrolIndex % points.size()];
	outcome.waypoint = static_cast<uint32_t>(patrolIndex);
	if (activeRegion) outcome.regionId = activeRegion->id;
	return outcome;
}

PlayerBotHuntPatrolOutcome PlayerBotHuntRuntime::observePatrolNavigation(const PlayerBotNavigationRuntimeOutcome& navigation,
	                                                                      std::chrono::steady_clock::time_point now,
	                                                                      uint32_t repeatedStepLimit, uint32_t routeFailureLimit)
{
	PlayerBotHuntPatrolOutcome outcome = patrolTarget();
	if (navigation.destinationReached) {
		resetPatrolFailures();
		const auto& points = activeRegion && !activeRegion->patrolPoints.empty() ? activeRegion->patrolPoints : fallbackPatrol;
		if (!points.empty()) patrolIndex = (patrolIndex + 1) % points.size();
		outcome.command = PlayerBotHuntPatrolCommand::WaypointReached;
		outcome.waypoint = static_cast<uint32_t>(patrolIndex);
		return outcome;
	}
	if (navigation.plan.attempted && navigation.routeUnavailable) {
		if (patrolFailureTarget != outcome.destination) resetPatrolFailures();
		patrolFailureTarget = outcome.destination;
		if (patrolRouteFailures++ == 0) patrolFailureStarted = now;
		patrolFailureExpandedNodes += navigation.plan.expandedNodes;
	}
	const bool oscillating = navigation.oscillation.has_value();
	const bool repeatedSteps = navigation.stepFailureCount >= repeatedStepLimit;
	const bool repeatedRoutes = patrolRouteFailures >= routeFailureLimit;
	if (!oscillating && !repeatedSteps && !repeatedRoutes) return outcome;
	outcome.command = activeRegion ? PlayerBotHuntPatrolCommand::SkipWaypoint : PlayerBotHuntPatrolCommand::SkipWaypoint;
	outcome.reason = oscillating ? "position_oscillation" : repeatedSteps ? "repeated_step_failure" : "route_unavailable";
	outcome.stepFailures = navigation.stepFailureCount;
	outcome.routeFailures = patrolRouteFailures;
	outcome.expandedNodes = patrolFailureExpandedNodes;
	outcome.elapsedMs = patrolFailureStarted == std::chrono::steady_clock::time_point{} ? 0 : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - patrolFailureStarted).count());
	if (activeRegion) {
		activeRegion->patrolPoints.erase(activeRegion->patrolPoints.begin() + patrolIndex);
		if (activeRegion->patrolPoints.empty()) {
			outcome.command = PlayerBotHuntPatrolCommand::RegionExhausted;
			cooldowns[activeRegion->center] = now + std::chrono::minutes(10);
		} else patrolIndex %= activeRegion->patrolPoints.size();
	} else if (!fallbackPatrol.empty()) patrolIndex = (patrolIndex + 1) % fallbackPatrol.size();
	resetPatrolFailures();
	return outcome;
}

void PlayerBotHuntRuntime::resetPatrolFailures()
{
	patrolRouteFailures = 0;
	patrolFailureExpandedNodes = 0;
	patrolFailureTarget = Position();
	patrolFailureStarted = {};
}
