#include "otpch.h"

#include "playerbotserviceroute.h"
#include "playerbotpathsearch.h"
#include "playerbotplanningbudget.h"
#include "playerbotroutechanges.h"

#include <cassert>
#include <iostream>

namespace {
using namespace std::chrono_literals;
const Position source(100, 100, 7), direct(101, 100, 7), seller(102, 100, 7);
const PlayerBotEconomyCatalog catalog;
const PlayerBotDispositionPolicy disposition;

PlayerBotNavigationStep move(Position target, Direction direction)
{
	PlayerBotNavigationStep step;
	step.action = PlayerBotNavigationAction::Move;
	step.target = step.expectedPosition = target;
	step.direction = direction;
	return step;
}

PlayerBotServiceLiquidationPlan quote()
{
	PlayerBotServiceLiquidationPlan plan{7, {{5880, 1, 500, 0}}};
	plan.maximumRouteSteps = 8;
	return plan;
}

PlayerBotServiceObservation observation()
{
	PlayerBotServiceObservation value;
	value.currentPosition = source;
	value.shops.push_back({7, Position(105, 100, 7), {{5880, 0, 500, 0}}});
	PlayerBotServiceProviderObservation provider{true, false, false, true};
	provider.approaches.push_back({seller, 2});
	value.providers.emplace(7, provider);
	value.maximumAttempts = 3;
	return value;
}

void requestAndQuoteRestrictions()
{
	auto plan = quote();
	const auto request = playerBotServiceRouteRequest(source, seller, 42, false, &plan, {direct});
	assert(request.walkingOnly && request.preferSafeWalking);
	assert(request.planningPass == 42 && request.from == source && request.to == seller);
	assert(request.blockedPositions == std::set<Position>{direct});
	const auto repeated = playerBotServiceRouteRequest(source, seller, 42, false, &plan, {direct});
	assert(repeated.planningPass == request.planningPass && repeated.blockedPositions == request.blockedPositions);
	assert(playerBotServiceRouteRequest(source, Position(1000, 1000, 7), 42, false, &plan, {}).walkingOnly);
	assert(!playerBotServiceRouteRequest(source, Position(1000, 1000, 7), 42, false, &plan, {}).preferSafeWalking);
	plan.allowNpcTravel = true;
	plan.maximumFare = 40;
	assert(!playerBotServiceRouteRequest(source, seller, 42, false, &plan, {}).walkingOnly);
	assert(playerBotServiceRouteRequest(source, seller, 42, true, &plan, {}).walkingOnly);
	assert(playerBotServiceRouteRequest(source, seller, 42, true, nullptr, {}).walkingOnly);
	assert(!playerBotServiceRouteRequest(source, seller, 42, false, nullptr, {}).walkingOnly);

	PlayerBotNavigationPlanMetrics metrics;
	metrics.steps = 8;
	metrics.fare = 40;
	assert(playerBotServiceRouteQuoteAllows(&plan, metrics, true));
	metrics.fare = 41;
	assert(!playerBotServiceRouteQuoteAllows(&plan, metrics, true));
	metrics.fare = 40;
	metrics.steps = 9;
	assert(!playerBotServiceRouteQuoteAllows(&plan, metrics, true));
	metrics.steps = 8;
	plan.allowNpcTravel = false;
	assert(!playerBotServiceRouteQuoteAllows(&plan, metrics, true));
	metrics.fare = 0; // Free NPC travel is also outside a walking-only quote.
	assert(!playerBotServiceRouteQuoteAllows(&plan, metrics, true));
	assert(playerBotServiceRouteQuoteAllows(&plan, metrics, false));

	// The real workflow independently refuses an unquoted paid or free trip.
	for (const uint64_t fare : {0u, 40u}) {
		PlayerBotServiceWorkflow workflow;
		workflow.reset(PlayerBotServiceIntent::ResupplyWithLocalSale);
		workflow.setLiquidationPlan(plan);
		auto observed = observation();
		assert(workflow.advance(observed, catalog, disposition).type == PlayerBotServiceCommandType::ValidateProviderRoute);
		observed.approachRoute = {7, seller, PlayerBotServiceRouteResult::Reached, 8, 0, 0, fare, 0, true};
		const auto rejected = workflow.advance(observed, catalog, disposition);
		assert(rejected.type != PlayerBotServiceCommandType::NavigateProvider);
		assert(!workflow.approach().selected());
	}
}

// Exercise the production request builder, workflow, sliced tile-search kernel,
// and movement runtime together. Only the world graph and movement dispatch are
// synthetic; this does not load the hierarchical router's world/NPC adapters.
void slicedSaleAndInterruption()
{
	const auto plan = quote();
	PlayerBotServiceWorkflow workflow;
	workflow.reset(PlayerBotServiceIntent::ResupplyWithLocalSale);
	workflow.setLiquidationPlan(plan);
	auto observed = observation();
	const auto command = workflow.advance(observed, catalog, disposition);
	assert(command.type == PlayerBotServiceCommandType::ValidateProviderRoute);
	PlayerBotNavigationRuntime navigation;
	const auto goal = PlayerBotNavigationGoal::exact(seller);
	const auto now = std::chrono::steady_clock::time_point{};
	PlayerBotNavigationRoutePlan original;
	original.metrics.result = PlayerBotNavigationResult::Reached;
	original.steps = {move(direct, DIRECTION_EAST), move(seller, DIRECTION_EAST)};
	navigation.observePlan({goal, original, true, true, now});
	navigation.observeStep({original.steps.front(), PlayerBotNavigationStepResult::Dispatched, now, 30s});
	const auto failed = navigation.process({source, goal, false, true, {now + 1s, 1s, 30s, 30s}});
	assert(failed.routeRequest && failed.routeRequest->blockedPositions.count(direct));
	navigation.reset(); // Interruption retains short-lived failure evidence.
	const auto request = playerBotServiceRouteRequest(source, command.destination, 42, false,
	    &*workflow.liquidation(), navigation.activeBlockedPositions(now + 1s));
	assert(request.walkingOnly && request.blockedPositions.count(direct));

	auto expand = [&](Position from) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		for (const auto direction : {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST}) {
			Position to = from;
			if (direction == DIRECTION_NORTH) --to.y;
			else if (direction == DIRECTION_SOUTH) ++to.y;
			else if (direction == DIRECTION_EAST) ++to.x;
			else --to.x;
			if (to.x < source.x || to.x > seller.x || to.y < source.y || to.y > source.y + 1 ||
			    request.blockedPositions.count(to)) continue;
			arcs.push_back({move(to, direction), 10, 1, 1});
		}
		return arcs;
	};
	PlayerBotPathSearch search(request.from, goal, 32, true);
	PlayerBotPlanningBudget budget(now);
	unsigned pending = 0;
	for (;;) {
		const auto sliceTime = now + pending * 1ms;
		assert(budget.request(7, sliceTime).admitted);
		const auto result = search.advance(1, expand, [&](Position p) { return goal.reached(p); }, [](Position) { return 0u; });
		assert(budget.finish(7, sliceTime));
		if (result) break;
		assert(++pending < 32);
		// A yielded slice supplies no negative observation or retry to workflow.
		const auto stillPending = workflow.advance(observed, catalog, disposition);
		assert(stillPending.type == command.type && stillPending.destination == command.destination);
	}
	assert(pending > 1 && search.result == PlayerBotNavigationResult::Reached && search.steps.size() == 4);
	PlayerBotNavigationRoutePlan detour;
	detour.metrics.result = *search.result;
	detour.metrics.steps = search.steps.size();
	detour.metrics.dangerCost = search.summary.dangerCost;
	detour.metrics.maximumHealthLossPerSecond = search.summary.maximumHealthLossPerSecond;
	detour.steps = search.steps;
	PlayerBotNavigationRiskProfile risk;
	risk.healthLossCost = 1;
	risk.maximumRouteHealthLoss = 4;
	risk.maximumHealthLossPerSecond = 1;
	assert(playerBotNavigationRiskVerdict(risk, detour.metrics) == PlayerBotNavigationRiskVerdict::Accepted);
	assert(playerBotServiceRouteQuoteAllows(&plan, detour.metrics, false));
	observed.approachRoute = {7, seller, PlayerBotServiceRouteResult::Reached, 4};
	assert(workflow.advance(observed, catalog, disposition).outcome == PlayerBotServiceOutcome::Success);
	assert(workflow.advance(observed, catalog, disposition).type == PlayerBotServiceCommandType::NavigateProvider);

	// An interrupted selected approach still yields NavigateProvider. The
	// controller revalidates it and installs fresh steps; runtime must retain
	// those steps rather than requesting legacy planning on the same turn.
	navigation.reset();
	assert(workflow.advance(observed, catalog, disposition).type == PlayerBotServiceCommandType::NavigateProvider);
	navigation.observePlan({goal, detour, true, true, now + 1s});
	Position cursor = source;
	for (const auto& step : detour.steps) {
		const auto next = navigation.process({cursor, goal, false, true, {now + 2s, 1s, 30s, 30s}});
		assert(!next.routeRequest && next.nextStep && next.nextStep->target == step.target);
		assert(step.action == PlayerBotNavigationAction::Move && step.target != direct);
		navigation.observeStep({step, PlayerBotNavigationStepResult::Dispatched, now + 2s, 30s});
		cursor = step.expectedPosition;
	}
	assert(navigation.process({cursor, goal, false, true, {now + 3s, 1s, 30s, 30s}}).destinationReached);

	// A real dependency change invalidates the watch. A fresh risk verdict
	// cannot accept yesterday's safe proof, and a restricted search stays bounded.
	PlayerBotRouteChanges::Watch watch;
	{ PlayerBotRouteChanges::Scope reads(watch); PlayerBotRouteChanges::read(direct); }
	assert(watch.valid());
	PlayerBotRouteChanges::changed(direct, PlayerBotRouteChanges::Cause::TileFlag);
	assert(!watch.valid());
	detour.metrics.dangerCost = 5;
	assert(playerBotNavigationRiskVerdict(risk, detour.metrics) == PlayerBotNavigationRiskVerdict::Rejected);
	PlayerBotPathSearch bounded(request.from, goal, 2, true);
	unsigned slices = 0;
	while (!bounded.advance(1, expand, [&](Position p) { return goal.reached(p); }, [](Position) { return 0u; }))
		assert(++slices <= 2);
	assert(bounded.result == PlayerBotNavigationResult::NodeLimit && bounded.expanded == 2);
	const auto changed = playerBotServiceRouteRequest(source, seller, 42, false, &plan, {direct, seller});
	assert(changed.planningPass == request.planningPass && changed.blockedPositions != request.blockedPositions);
	PlayerBotPathSearch blocked(source, goal, 32, true);
	auto blockedExpand = [&](Position from) {
		auto arcs = expand(from);
		arcs.erase(std::remove_if(arcs.begin(), arcs.end(), [&](const auto& arc) {
			return changed.blockedPositions.count(arc.step.target);
		}), arcs.end());
		return arcs;
	};
	slices = 0;
	while (!blocked.advance(1, blockedExpand, [&](Position p) { return goal.reached(p); }, [](Position) { return 0u; }))
		assert(++slices < 32);
	assert(blocked.result == PlayerBotNavigationResult::Unreachable && blocked.expanded < 32);
}
}

int main()
{
	requestAndQuoteRestrictions();
	slicedSaleAndInterruption();
	std::cout << "playerbotserviceroute contracts passed\n";
}
