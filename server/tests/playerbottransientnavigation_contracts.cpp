#include "playerbotnavigationruntime.h"
#include "playerbotdepotworkflow.h"
#include "playerbotpathsearch.h"
#include "playerbottravelsafety.h"

#include <array>
#include <cassert>
#include <iostream>

namespace {
using namespace std::chrono_literals;
using Result = PlayerBotNavigationResult;
const Position source(100, 100, 7), prefix(101, 100, 7), crossing(102, 100, 7), depot(103, 100, 7);
const auto epoch = std::chrono::steady_clock::time_point{} + 100s;

PlayerBotNavigationStep move(Position target)
{
	PlayerBotNavigationStep step;
	step.action = PlayerBotNavigationAction::Move;
	step.direction = DIRECTION_EAST;
	step.target = step.expectedPosition = target;
	return step;
}

uint32_t exposure(const PlayerBotNavigationStep&) { return 1000; }

PlayerBotNavigationCostPolicy policy(double threat, double baseline = 0)
{
	PlayerBotNavigationCostPolicy value;
	value.transientHealthLossPerSecond = [threat](Position p) { return p == crossing ? threat : 0; };
	value.expectedHealthLossPerSecond = [threat, baseline](Position p) {
		return std::max(baseline, p == crossing ? threat : 0);
	};
	return value;
}

// The production search kernel and rejection selectors over a tiny world graph.
// Only tile inspection is synthetic; the risk policy and frontier are real.
PlayerBotNavigationRoutePlan search(const PlayerBotNavigationCostPolicy& cost, bool detour = false,
                                    uint64_t allowance = 20)
{
	const auto goal = PlayerBotNavigationGoal::exact(depot);
	PlayerBotPathSearch frontier(source, goal, allowance);
	frontier.constrain(cost.risk);
	if (!frontier.rejectUnsafeTransientEndpoints(cost)) {
		frontier.rejectUnsafeEndpoints([&](Position p) { return cost.dangerAt(p); });
	}
	auto expand = [&](Position from) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		auto add = [&](Position to) {
			if (frontier.rejectTransientArc(cost.transientHealthLossPerSecond(to),
			                                cost.risk.maximumHealthLossPerSecond)) return;
			arcs.push_back({move(to), 10, cost.dangerCost(to, 1000), cost.dangerAt(to)});
		};
		if (from == source) add(prefix);
		if (from == prefix) {
			add(crossing);
			if (detour) add(depot);
		}
		if (from == crossing) add(depot);
		return arcs;
	};
	while (!frontier.advance(1, expand, [&](Position p) { return p == depot; },
	    [&](Position p) { return playerBotNavigationDistance(p, depot); })) {}
	PlayerBotNavigationRoutePlan result;
	result.metrics.attempted = true;
	result.metrics.result = *frontier.result;
	result.metrics.dangerCost = frontier.summary.dangerCost;
	result.metrics.maximumHealthLossPerSecond = frontier.summary.maximumHealthLossPerSecond;
	result.metrics.transientRiskRejected = frontier.summary.transientRiskRejected;
	result.steps = std::move(frontier.steps);
	return result;
}

void explicitSearchEvidence()
{
	const auto hazard = policy(0.4);
	const auto rejected = search(hazard);
	assert(rejected.metrics.result == Result::RiskRejected);
	assert(rejected.metrics.transientRiskRejected && rejected.steps.empty());
	assert(rejected.metrics.maximumHealthLossPerSecond == 0.4);
	assert(playerBotNavigationTransientRejection(rejected, hazard, exposure) == 0.4);

	// Seeing rejected arcs does not taint a safe route that was actually found.
	const auto detour = search(hazard, true);
	assert(detour.metrics.result == Result::Reached && !detour.metrics.transientRiskRejected);
	assert(!playerBotNavigationTransientRejection(detour, hazard, exposure));

	// A partial search is still incomplete, not a proven transient rejection.
	const auto limited = search(hazard, true, 2);
	assert(limited.metrics.result == Result::NodeLimit && !limited.metrics.transientRiskRejected);
	assert(!playerBotNavigationTransientRejection(limited, hazard, exposure));

	// Static zero-risk rejection must not defer just because another tile has
	// a raid warning or this bot remembers a harmless rat.
	auto staticRisk = policy(0.001, 0.4);
	staticRisk.risk.maximumRouteHealthLoss = 0;
	staticRisk.risk.maximumHealthLossPerSecond = 0;
	const auto staticRejected = search(staticRisk);
	assert(staticRejected.metrics.result == Result::RiskRejected);
	assert(!staticRejected.metrics.transientRiskRejected);
	assert(!playerBotNavigationTransientRejection(staticRejected, staticRisk, exposure));

	const auto weightedStatic = search(policy(0.001, 0.4));
	assert(weightedStatic.metrics.result == Result::Reached);
	assert(playerBotNavigationRiskVerdict(policy(0).risk, weightedStatic.metrics) ==
	       PlayerBotNavigationRiskVerdict::Rejected);
	assert(!playerBotNavigationTransientRejection(weightedStatic, policy(0.001, 0.4), exposure));

	PlayerBotPathSearch endpoint(source, PlayerBotNavigationGoal::exact(crossing), 20);
	assert(endpoint.rejectUnsafeTransientEndpoints(hazard));
	assert(endpoint.result == Result::RiskRejected && endpoint.summary.transientRiskRejected);
	PlayerBotPathSearch alternatives(source, PlayerBotNavigationGoal::anyOf({crossing, depot}), 20);
	assert(!alternatives.rejectUnsafeTransientEndpoints(hazard));
	PlayerBotPathSearch alreadyThere(crossing, PlayerBotNavigationGoal::exact(crossing), 20);
	assert(!alreadyThere.rejectUnsafeTransientEndpoints(hazard));
}

void acceptedPrefixAndResolvedPortal()
{
	PlayerBotNavigationRoutePlan partial;
	partial.metrics.result = Result::Reached;
	partial.metrics.dangerEvidence = PlayerBotNavigationDangerEvidence::Coarse;
	partial.steps.push_back(move(prefix));
	auto portal = move(crossing);
	portal.action = PlayerBotNavigationAction::UseRope;
	portal.topologyPortal = true;
	partial.steps.push_back(portal);
	// The detailed local prefix is safe, but the appended crossing is not.
	assert(playerBotNavigationLocalRiskVerdict(policy(0).risk, partial.metrics) ==
	       PlayerBotNavigationRiskVerdict::Accepted);
	assert(playerBotNavigationTransientRejection(partial, policy(0.4), exposure) == 0.4);
	assert(!playerBotNavigationTransientRejection(partial, policy(0.001), exposure));

	// Re-resolving an initially safe portal can reveal a different unsafe
	// landing. The dispatch selector runs before static-risk failure accounting.
	portal.expectedPosition = depot;
	assert(!playerBotNavigationTransientRouteRisk(policy(0.4), std::array{portal}, exposure).transientRiskRejected);
	portal.expectedPosition = crossing;
	assert(playerBotNavigationTransientRouteRisk(policy(0.4), std::array{portal}, exposure).transientRiskRejected);

	// Below-peak transient exposure can still exhaust the route-loss budget.
	const auto cumulative = playerBotNavigationTransientRouteRisk(policy(0.06), std::array{portal},
	    [](const PlayerBotNavigationStep&) { return 10000; });
	assert(cumulative.transientRiskRejected && cumulative.maximumHealthLossPerSecond == 0.06);
	assert(cumulative.dangerCost == 600);
}

void installedRetainedExposure()
{
	PlayerBotNavigationRuntime runtime;
	const auto goal = PlayerBotNavigationGoal::exact(depot);
	PlayerBotTravelRouteExposure retained;
	const auto baseline = policy(0);
	// Replace an unrelated prior proof, retaining the validated whole-trip
	// budget even when only its current executable leg is installed.
	retained.retain(std::array{move({500, 500, 7})}, baseline, exposure, 0, 0);
	auto route = search(baseline);
	route.metrics.dangerCost = 400;
	retained.retain(route.steps, baseline, exposure, route.metrics.dangerCost, route.metrics.maximumHealthLossPerSecond);
	const auto installed = runtime.observePlan({goal, route, true, true, epoch});
	assert(installed.plan.dangerCost == 400 && runtime.routeSize() == route.steps.size());
	assert(retained.samples.size() == route.steps.size() && retained.samples.front().position == prefix);
	PlayerBotTravelDangerField unrelated({}, {{{500, 500, 7}, 4}}, {});
	assert(!retained.refreshed(unrelated).transientRiskRejected);
	assert(runtime.routeSize() == route.steps.size());
	PlayerBotTravelDangerField changed({{prefix, depot, 0.06}}, {}, {});
	const auto fresh = retained.refreshed(changed);
	assert(fresh.maximumHealthLossPerSecond == 0.06 && fresh.dangerCost == 580);
	assert(fresh.transientRiskRejected);
	runtime.deferPlan();
	assert(runtime.routeSize() == 0 && runtime.fixedTargetRouteFailureCount() == 0);
}

PlayerBotDepotCommand scan(PlayerBotDepotWorkflow& workflow)
{
	PlayerBotDepotObservation observed;
	observed.currentPosition = source;
	observed.now = epoch;
	assert(workflow.advance(observed, 2, 3, 30s).type == PlayerBotDepotCommandType::Scan);
	observed.scan.observed = true;
	observed.scan.indexedCandidates = observed.scan.inScopeCandidates = observed.scan.standableCandidates = 1;
	observed.scan.candidates.push_back({1, 2589, Position(104, 100, 7), depot, 3});
	return workflow.advance(observed, 2, 3, 30s);
}

void depotAndFailureBounds()
{
	PlayerBotDepotWorkflow workflow;
	const auto initial = scan(workflow);
	assert(initial.type == PlayerBotDepotCommandType::ValidateRoute);
	PlayerBotNavigationRuntime runtime;
	const auto goal = PlayerBotNavigationGoal::exact(depot);
	runtime.process({source, goal, false, true, {epoch, 1s, 30s, 30s}});
	PlayerBotNavigationRoutePlan staticFailure;
	staticFailure.metrics.result = Result::RiskRejected;
	for (int i = 0; i < 3; ++i) runtime.observePlan({goal, staticFailure, true, false, epoch});
	assert(runtime.fixedTargetRouteFailureCount() == 3);
	const auto installed = search(policy(0));
	runtime.observePlan({goal, installed, true, false, epoch});
	assert(runtime.routeSize() != 0 && runtime.fixedTargetRouteFailureCount() == 3);

	const auto hazard = policy(0.4);
	const auto rejected = search(hazard);
	for (int turn = 0; turn < 60; ++turn) {
		assert(playerBotNavigationTransientRejection(rejected, hazard, exposure));
		// The production callers leave the route observation absent on defer.
		// Neither workflow unavailability nor fixed-target exhaustion advances.
		runtime.deferPlan();
		assert(runtime.routeSize() == 0);
		const auto retry = runtime.process({source, goal, false, true,
		    {epoch + std::chrono::seconds(turn), 1s, 30s, 30s}});
		assert(retry.routeRequest && retry.fixedTargetRouteFailures == 3 && !retry.fixedTargetRouteExhausted);
		PlayerBotDepotObservation observed;
		observed.currentPosition = source;
		observed.now = epoch + std::chrono::seconds(turn);
		const auto pending = workflow.advance(observed, 2, 3, 30s);
		assert(pending.type == PlayerBotDepotCommandType::ValidateRoute);
		assert(pending.snapshot.routeCandidate.approachPosition == depot);
		assert(pending.snapshot.attempts == initial.snapshot.attempts);
		assert(pending.snapshot.candidateOffset == initial.snapshot.candidateOffset);
		assert(pending.snapshot.suppressedApproaches == 0 && pending.snapshot.unsafeRouteCandidates == 0);
		assert(runtime.fixedTargetRouteFailureCount() == 3);
	}
	const auto safe = search(policy(0));
	assert(safe.metrics.result == Result::Reached);
	assert(!playerBotNavigationTransientRejection(safe, policy(0), exposure));
	PlayerBotDepotObservation observed;
	observed.currentPosition = source;
	observed.now = epoch + 60s;
	observed.routeResult = PlayerBotDepotRouteResult::Reached;
	assert(workflow.advance(observed, 2, 3, 30s).type == PlayerBotDepotCommandType::Navigate);

	// Static failures still exhaust the original fixed-target bound.
	for (int i = 3; i < 20; ++i) {
		assert(!playerBotNavigationTransientRejection(staticFailure, hazard, exposure));
		const auto failed = runtime.observePlan({goal, staticFailure, true, false, epoch});
		assert(failed.fixedTargetRouteExhausted == (i == 19));
	}

	// Post-plan/step rejection still has the same terminal bound when its
	// cause is static. Transient step gates must run before this call.
	PlayerBotNavigationRuntime stepRuntime;
	stepRuntime.process({source, goal, false, true, {epoch, 1s, 30s, 30s}});
	for (int i = 0; i < 20; ++i) {
		stepRuntime.observePlan({goal, installed, true, false, epoch});
		const auto failed = stepRuntime.rejectAcceptedPlan();
		assert(failed.fixedTargetRouteExhausted == (i == 19));
	}

	// Preserve the existing forced-return fallback for static danger. A raid
	// elsewhere must not turn this fallback into a wait.
	PlayerBotDepotWorkflow forced;
	assert(scan(forced).type == PlayerBotDepotCommandType::ValidateRoute);
	observed.routeResult = PlayerBotDepotRouteResult::Unsafe;
	observed.dangerCost = 800;
	observed.maximumHealthLossPerSecond = 0.4;
	const auto fallback = forced.advance(observed, 2, 3, 30s);
	assert(fallback.type == PlayerBotDepotCommandType::ValidateRoute && fallback.snapshot.validatingRiskFallback);
	assert(playerBotDepotRouteSafetyAccepted(false, false, PlayerBotRouteIntent::ForcedReturn));
	assert(!playerBotDepotRouteSafetyAccepted(false, true, PlayerBotRouteIntent::ForcedReturn));
	assert(!playerBotDepotRouteSafetyAccepted(false, false, PlayerBotRouteIntent::Optional));
	observed.routeResult = PlayerBotDepotRouteResult::Reached;
	const auto selected = forced.advance(observed, 2, 3, 30s);
	assert(selected.type == PlayerBotDepotCommandType::Navigate && selected.telemetry.riskFallback);
}
}

int main()
{
	// Individually acceptable, disjoint exposure still shares one route budget.
	auto mixed = policy(0.02);
	mixed.staticHealthLossPerSecond = [](Position p) { return p == prefix ? 0.04 : 0.; };
	mixed.expectedHealthLossPerSecond = [](Position p) { return p == prefix ? 0.04 : p == crossing ? 0.02 : 0.; };
	PlayerBotNavigationRoutePlan frozen;
	frozen.metrics.result = Result::Reached;
	frozen.metrics.dangerCost = 600;
	frozen.steps = {move(prefix), move(crossing)};
	const auto longStep = [](const PlayerBotNavigationStep&) { return 10000u; };
	const auto combined = playerBotNavigationTransientRouteRisk(mixed, frozen.steps, longStep);
	assert(combined.transientRiskRejected && combined.dangerCost == 600);
	assert(combined.maximumHealthLossPerSecond == 0.04);
	assert(playerBotNavigationTransientRejection(frozen, mixed, longStep) == 0.04);
	frozen.metrics.result = Result::RiskRejected;
	assert(playerBotNavigationTransientRejection(frozen, mixed, longStep) == 0.04);
	mixed.transientHealthLossPerSecond = [](Position) { return 0.; };
	assert(!playerBotNavigationTransientRejection(frozen, mixed, longStep));
	// A route already unsafe from static exposure keeps the normal failure path.
	mixed.staticHealthLossPerSecond = [](Position p) { return p == prefix ? 0.06 : 0.; };
	mixed.transientHealthLossPerSecond = [](Position p) { return p == crossing ? 0.02 : 0.; };
	assert(!playerBotNavigationTransientRejection(frozen, mixed, longStep));
	explicitSearchEvidence();
	acceptedPrefixAndResolvedPortal();
	installedRetainedExposure();
	depotAndFailureBounds();
	std::cout << "playerbot transient navigation contracts passed\n";
}
