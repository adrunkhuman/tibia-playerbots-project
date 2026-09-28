#include "playerbotpathsearch.h"
#include "playerbothuntcoarsepolicy.h"
#include "playerbothunttravelpolicy.h"
#include "playerbothuntregions.h"
#include "playerbotroutechanges.h"
#include "playerbotrouteitemchange.h"
#include "playerbottransportsearch.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <memory>

namespace {
using Segment = PlayerBotTransportSegment;
using Result = PlayerBotNavigationResult;
using Transport = PlayerBotTransportSearch;

PlayerBotPathSearch path(uint64_t slice, uint64_t allowance = 100000)
{
	PlayerBotNavigationGoal goal;
	goal.position = Position(60000, 60000, 7);
	PlayerBotPathSearch search(Position(0, 0, 7), goal, allowance);
	auto expand = [](Position from) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		if (from.x < 40) {
			PlayerBotNavigationStep step;
			step.expectedPosition = Position(from.x + 1, 0, 7);
			arcs.push_back({step, 10, 0, 0});
			step.expectedPosition = Position(60000, 60000, 7);
			// The cheapest solution is the remote shortcut at x=20, not a
			// geometrically close frontier. Also produce stale queue entries.
			arcs.push_back({step, from.x == 20 ? 1u : 1000u - from.x * 20u, 0, 0});
		}
		return arcs;
	};
	while (!search.advance(slice, expand, [&](Position p) { return p == goal.position; },
	    [](Position p) { return 60000u - p.x; })) {}
	return search;
}

PlayerBotPathSearch gridPath(const PlayerBotNavigationGoal& goal, bool guided, uint64_t slice = 100000)
{
	const Position start(100, 120, 7);
	PlayerBotPathSearch search(start, goal, 100000, true);
	const Direction directions[] = {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
	    DIRECTION_NORTHEAST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST, DIRECTION_NORTHWEST};
	auto expand = [&](const Position& from) {
		std::vector<PlayerBotPathSearch::Arc> arcs;
		for (const Direction direction : directions) {
			int dx = 0, dy = 0;
			switch (direction) {
				case DIRECTION_NORTH: case DIRECTION_NORTHEAST: case DIRECTION_NORTHWEST: dy = -1; break;
				case DIRECTION_SOUTH: case DIRECTION_SOUTHEAST: case DIRECTION_SOUTHWEST: dy = 1; break;
				default: break;
			}
			switch (direction) {
				case DIRECTION_EAST: case DIRECTION_NORTHEAST: case DIRECTION_SOUTHEAST: dx = 1; break;
				case DIRECTION_WEST: case DIRECTION_NORTHWEST: case DIRECTION_SOUTHWEST: dx = -1; break;
				default: break;
			}
			const Position to(from.x + dx, from.y + dy, from.z);
			const int x = int(to.x) - 100, y = int(to.y) - 100;
			if (x < 0 || x > 60 || y < 0 || y > 45 || (x == 25 && y <= 30)) continue;
			PlayerBotNavigationStep step;
			step.action = PlayerBotNavigationAction::Move;
			step.direction = direction;
			step.target = step.expectedPosition = to;
			const bool hazardous = x >= 8 && x <= 12 && y == 20;
			arcs.push_back({step, (direction & DIRECTION_DIAGONAL_MASK) ? 30u : 10u,
			                hazardous ? 300u : 0u, hazardous ? 0.5 : 0.0});
		}
		return arcs;
	};
	while (!search.advance(slice, expand, [&](const Position& p) {
	    return goal.type == PlayerBotNavigationGoalType::AnyOf ?
	        std::find(goal.positions.begin(), goal.positions.end(), p) != goal.positions.end() : p == goal.position;
	}, [&](const Position& p) { return playerBotOrdinaryRemainingCost(p, goal); }, 0,
	    [&](const Position& p) { return guided ? playerBotOrdinaryRemainingCost(p, goal) : 0; })) {}
	return search;
}

void huntCoarseVerdictContracts()
{
	using Verdict = PlayerBotHuntCoarseVerdict;
	// Directed graph: 0 -> 1 is free; 1 -> 2/3/4 requires rope/shovel/level 20.
	// Node 5 is represented but isolated; 6 is outside the graph.
	const auto known = [](int node) { return node >= 0 && node < 6; };
	const auto check = [&](int source, std::vector<int> goals, bool rope, bool shovel, unsigned level,
	                       bool unchanged = true) {
		auto reachable = [&](int goal) {
			assert(known(goal));
			bool visited[6] = {};
			std::vector<int> pending{source};
			while (!pending.empty()) {
				const int node = pending.back();
				pending.pop_back();
				if (visited[node]) continue;
				visited[node] = true;
				if (node == goal) return true;
				if (node == 0) pending.push_back(1);
				if (node == 1) {
					if (rope) pending.push_back(2);
					if (shovel) pending.push_back(3);
					if (level >= 20) pending.push_back(4);
				}
			}
			return false;
		};
		return playerBotHuntCoarseVerdict(unchanged, source, goals, known, reachable);
	};
	assert(check(0, {2, 3, 4, 5}, false, false, 8) == Verdict::Disconnected);
	assert(check(0, {5}, false, false, 8, false) == Verdict::Unknown); // Map changed after build.
	assert(check(0, {1}, false, false, 8, false) == Verdict::Reachable); // Hints remain usable.
	assert(check(2, {0}, true, true, 20) == Verdict::Disconnected); // Edges are not reversible.
	assert(check(6, {1}, false, false, 8) == Verdict::Unknown);
	assert(check(0, {5, 6}, false, false, 8) == Verdict::Unknown);
	assert(check(0, {}, false, false, 8) == Verdict::Unknown);
	assert(check(0, {5, 1}, false, false, 8) == Verdict::Reachable);
	assert(check(0, {6, 1}, false, false, 8) == Verdict::Reachable);
	assert(check(0, {1, 6}, false, false, 8) == Verdict::Reachable);
	assert(check(0, {2}, false, false, 20) == Verdict::Disconnected);
	assert(check(0, {2}, true, false, 8) == Verdict::Reachable);
	assert(check(0, {3}, false, true, 8) == Verdict::Reachable);
	assert(check(0, {3}, true, false, 20) == Verdict::Disconnected);
	assert(check(0, {4}, true, true, 19) == Verdict::Disconnected);
	assert(check(0, {4}, false, false, 20) == Verdict::Reachable);
	assert(check(0, {2, 3, 4}, false, true, 8) == Verdict::Reachable);
	const std::optional<std::pair<uint64_t, uint64_t>> rejectedAt{{7, 3}};
	assert(!playerBotHuntCoarseEvidenceStale({}, 8, 3));
	assert(!playerBotHuntCoarseEvidenceStale(rejectedAt, 7, 3));
	assert(playerBotHuntCoarseEvidenceStale(rejectedAt, 8, 3)); // A tile changed while the route yielded.
	assert(playerBotHuntCoarseEvidenceStale(rejectedAt, 7, 4)); // Global invalidation.
}

void ordinaryWalkingContracts()
{
	PlayerBotNavigationGoal exact;
	exact.position = Position(150, 120, 7);
	const auto dijkstra = gridPath(exact, false);
	const auto guided = gridPath(exact, true, 3);
	assert(guided.result == Result::Reached && dijkstra.result == guided.result);
	assert(guided.summary.movementCost == dijkstra.summary.movementCost);
	assert(guided.summary.dangerCost == dijkstra.summary.dangerCost);
	assert(guided.summary.maximumHealthLossPerSecond == 0); // A safe detour avoids the hazard.
	assert(guided.expanded < dijkstra.expanded);
	assert(guided.steps.size() > 50); // Wall forces a detour, not a direct-line search.
	for (const auto& step : guided.steps) assert(step.action == PlayerBotNavigationAction::Move);

	PlayerBotNavigationGoal approaches;
	approaches.type = PlayerBotNavigationGoalType::AnyOf;
	approaches.positions = {Position(150, 120, 7), Position(145, 122, 7)};
	const auto anyDijkstra = gridPath(approaches, false);
	const auto anyGuided = gridPath(approaches, true, 7);
	assert(anyGuided.result == Result::Reached && anyGuided.summary.movementCost == anyDijkstra.summary.movementCost);
	assert(anyGuided.summary.dangerCost == anyDijkstra.summary.dangerCost);
	assert(std::find(approaches.positions.begin(), approaches.positions.end(),
	    anyGuided.steps.back().expectedPosition) != approaches.positions.end());
	assert(anyGuided.expanded < anyDijkstra.expanded);
	assert(playerBotOrdinaryRemainingCost(Position(100, 120, 7), approaches) == 470);
	assert(playerBotOrdinaryRemainingCost(Position(100, 120, 8), approaches) == 0);
	PlayerBotNavigationGoal range;
	range.type = PlayerBotNavigationGoalType::WithinRange;
	range.position = Position(110, 110, 7);
	range.rangeX = 2;
	range.rangeY = 3;
	assert(playerBotOrdinaryRemainingCost(Position(100, 100, 7), range) == 150);

	const Position start(100, 100, 7), east(101, 100, 7), goalPos(102, 100, 7);
	auto make = [&](PlayerBotNavigationAction action, Position target, Position expected, Direction direction = DIRECTION_EAST) {
		PlayerBotNavigationStep step;
		step.action = action;
		step.target = target;
		step.expectedPosition = expected;
		step.direction = direction;
		return step;
	};
	const auto door = make(PlayerBotNavigationAction::UseDoor, east, east);
	assert(PlayerBotPathSearch::ordinaryArc(start, door));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::UseDoor, east, goalPos)));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::UseRope, east, east)));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::UseShovel, east, east)));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::Use, east, east)));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::Move, east, Position(101, 100, 8))));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::Move, east, goalPos)));
	assert(!PlayerBotPathSearch::ordinaryArc(start, make(PlayerBotNavigationAction::Move, east, east, DIRECTION_NORTH)));
	PlayerBotNavigationGoal doorGoal;
	doorGoal.position = goalPos;
	PlayerBotPathSearch restricted(start, doorGoal, 100, true);
	auto arcs = [&](Position from) {
		if (from == start) return std::vector<PlayerBotPathSearch::Arc>{
		    {make(PlayerBotNavigationAction::Move, east, goalPos), 1, 0, 0},
		    {door, 20, 0, 0},
		    {make(PlayerBotNavigationAction::UseRope, goalPos, goalPos), 1, 0, 0}};
		if (from == east) return std::vector<PlayerBotPathSearch::Arc>{
		    {make(PlayerBotNavigationAction::Move, goalPos, goalPos), 10, 0, 0}};
		return std::vector<PlayerBotPathSearch::Arc>{};
	};
	while (!restricted.advance(1, arcs, [&](Position p) { return p == goalPos; },
	    [&](Position p) { return Position::getDistanceX(p, goalPos); }, 0,
	    [&](Position p) { return playerBotOrdinaryRemainingCost(p, restricted.goal); })) {}
	assert(restricted.result == Result::Reached && restricted.summary.movementCost == 30);
	assert(restricted.steps.front().action == PlayerBotNavigationAction::UseDoor);
	// Unrestricted paths may jump across the map; Manhattan must not rank
	// their frontier. The legacy five-argument advance keeps the edge bound.
	const Position remote(0, 100, 7), finish(120, 100, 7);
	PlayerBotNavigationGoal unrestrictedGoal;
	unrestrictedGoal.position = finish;
	PlayerBotPathSearch unrestricted(start, unrestrictedGoal, 100);
	auto jumps = [&](Position from) {
		if (from == start) return std::vector<PlayerBotPathSearch::Arc>{
		    {make(PlayerBotNavigationAction::Move, finish, finish), 30, 0, 0},
		    {make(PlayerBotNavigationAction::Move, remote, remote), 10, 0, 0}};
		if (from == remote) return std::vector<PlayerBotPathSearch::Arc>{
		    {make(PlayerBotNavigationAction::Move, finish, finish), 10, 0, 0}};
		return std::vector<PlayerBotPathSearch::Arc>{};
	};
	while (!unrestricted.advance(1, jumps, [&](Position p) { return p == finish; },
	    [&](Position p) { return Position::getDistanceX(p, finish); }, 10)) {}
	assert(unrestricted.result == Result::Reached && unrestricted.summary.movementCost == 20);

	std::cout << "ordinary walking expansions: exact " << guided.expanded << '/' << dijkstra.expanded
	          << ", any-of " << anyGuided.expanded << '/' << anyDijkstra.expanded << " (guided/Dijkstra)\n";
}

void peakDangerPathContracts()
{
	const Position start(100, 100, 7), east(101, 100, 7);
	const Position south(100, 101, 7), southeast(101, 101, 7);
	PlayerBotNavigationGoal goal;
	goal.position = east;
	const auto move = [](Direction direction, Position to, uint32_t danger, double peak) {
		PlayerBotNavigationStep step;
		step.direction = direction;
		step.target = step.expectedPosition = to;
		return PlayerBotPathSearch::Arc{step, 10, danger, peak};
	};
	auto run = [&](bool guided, double limit, bool detour) {
		PlayerBotPathSearch search(start, goal, 32, true);
		search.maximumPeakDanger = limit;
		auto expand = [&](Position from) {
			if (from == start) {
				std::vector<PlayerBotPathSearch::Arc> arcs{move(DIRECTION_EAST, east, 1, 0.2)};
				if (detour) arcs.push_back(move(DIRECTION_SOUTH, south, 0, 0));
				return arcs;
			}
			if (detour && from == south) return std::vector<PlayerBotPathSearch::Arc>{
			    move(DIRECTION_EAST, southeast, 0, 0.04)};
			if (detour && from == southeast) return std::vector<PlayerBotPathSearch::Arc>{
			    move(DIRECTION_NORTH, east, 0, 0)};
			return std::vector<PlayerBotPathSearch::Arc>{};
		};
		while (!search.advance(1, expand, [&](Position p) { return p == east; },
		    [&](Position p) { return playerBotOrdinaryRemainingCost(p, goal); }, 0,
		    [&](Position p) { return guided ? playerBotOrdinaryRemainingCost(p, goal) : 0; })) {}
		return search;
	};
	const double unlimited = std::numeric_limits<double>::infinity();
	PlayerBotPathSearch generic(start, goal, 32); // Default remains unrestricted, including redirects and danger.
	assert(generic.maximumPeakDanger == unlimited && !generic.ordinaryOnly);
	auto shortcut = [&](Position from) {
		if (from != start) return std::vector<PlayerBotPathSearch::Arc>{};
		auto arc = move(DIRECTION_EAST, east, 1, 0.2);
		arc.step.target = south; // A redirected move is not an ordinary walk.
		return std::vector<PlayerBotPathSearch::Arc>{arc};
	};
	while (!generic.advance(1, shortcut, [&](Position p) { return p == east; },
	    [&](Position p) { return Position::getDistanceX(p, east); }, 10)) {}
	assert(generic.result == Result::Reached && generic.steps.size() == 1);
	assert(generic.summary.movementCost == 10 && generic.summary.dangerCost == 1);
	assert(generic.summary.maximumHealthLossPerSecond == 0.2);
	const auto ordinaryUnlimited = run(true, unlimited, true);
	assert(ordinaryUnlimited.result == Result::Reached && ordinaryUnlimited.steps.size() == 1);

	const auto dijkstra = run(false, 0.08, true);
	const auto guided = run(true, 0.08, true);
	assert(dijkstra.result == Result::Reached && guided.result == Result::Reached);
	assert(dijkstra.steps.size() == 3 && guided.steps.size() == 3);
	assert(dijkstra.steps.front().expectedPosition == south && guided.steps.front().expectedPosition == south);
	assert(dijkstra.steps.back().expectedPosition == east && guided.steps.back().expectedPosition == east);
	assert(dijkstra.summary.movementCost == 30 && guided.summary.movementCost == dijkstra.summary.movementCost);
	assert(dijkstra.summary.dangerCost == 0 && guided.summary.dangerCost == dijkstra.summary.dangerCost);
	assert(dijkstra.summary.maximumHealthLossPerSecond == 0.04 &&
	       guided.summary.maximumHealthLossPerSecond == dijkstra.summary.maximumHealthLossPerSecond);

	const auto unsafeOnly = run(true, 0.08, false);
	assert(unsafeOnly.result == Result::Unreachable && unsafeOnly.steps.empty());
	assert(unsafeOnly.summary.movementCost == 0 && unsafeOnly.summary.dangerCost == 0);
	assert(unsafeOnly.closest == start);
}

void reusableSourceTreeContracts()
{
	const Position start(100, 100, 7), a(101, 100, 7), b(102, 100, 7);
	const Position goalB(103, 100, 7), goalC(104, 100, 7);
	const Position remote(500, 500, 7), goalA(500, 501, 7);
	const auto arc = [](Position target, Position destination, uint32_t movement,
	                    uint32_t danger = 0, double peak = 0) {
		PlayerBotNavigationStep step;
		step.target = target;
		step.expectedPosition = destination;
		return PlayerBotPathSearch::Arc{step, movement, danger, peak};
	};
	auto expand = [&](Position from) {
		if (from == start) return std::vector<PlayerBotPathSearch::Arc>{
		    arc(a, a, 10), arc(a, remote, 10), arc(b, goalB, 1, 0, 0.2)};
		if (from == a) return std::vector<PlayerBotPathSearch::Arc>{
		    arc(b, b, 10), arc(b, goalA, 90)};
		if (from == remote) return std::vector<PlayerBotPathSearch::Arc>{arc(goalA, goalA, 10, 2, 0.03)};
		if (from == b) return std::vector<PlayerBotPathSearch::Arc>{arc(goalB, goalB, 10)};
		if (from == goalB) return std::vector<PlayerBotPathSearch::Arc>{arc(goalC, goalC, 10)};
		return std::vector<PlayerBotPathSearch::Arc>{};
	};
	PlayerBotNavigationGoal noEndpoint;
	noEndpoint.type = PlayerBotNavigationGoalType::AnyOf;
	PlayerBotPathSearch tree(start, noEndpoint, 5, false, true);
	tree.maximumPeakDanger = 0.08;
	std::deque<PlayerBotNavigationStep> steps;
	PlayerBotNavigationCostSummary summary;
	Position selected;
	assert(tree.extract({start}, steps, summary, &selected));
	assert(selected == start && steps.empty() && summary.movementCost == 0);
	assert(!tree.extract({goalA}, steps, summary)); // Discovered is not settled.
	const size_t initialBytes = tree.memoryBytes();
	const auto unused = [](Position) { assert(false && "source tree must not consult endpoint callbacks"); return 0u; };
	while (!tree.advance(2, expand, unused, unused, 0, unused)) {}
	assert(tree.result == Result::NodeLimit && tree.expanded == 5);
	assert(tree.memoryBytes() > initialBytes);
	assert(!tree.extract({goalB}, steps, summary)); // Discovered, but not settled yet.
	assert(tree.extract({goalA}, steps, summary, &selected) && selected == goalA);
	assert(steps.size() == 2 && summary.movementCost == 20 && summary.dangerCost == 2);
	assert(!tree.extract({goalC}, steps, summary) && steps.empty());
	// The total allowance and the result can be reset without discarding labels.
	tree.allowance = 20;
	tree.result.reset();
	while (!tree.advance(2, expand, unused, unused, 0, unused)) {}
	assert(tree.result == Result::Unreachable && tree.expanded > 5); // Entire reachable graph settled.
	assert(tree.extract({goalC}, steps, summary, &selected) && selected == goalC);
	assert(steps.size() == 4 && summary.movementCost == 40);

	auto independent = [&](Position destination) {
		PlayerBotNavigationGoal exact;
		exact.position = destination;
		PlayerBotPathSearch search(start, exact, 20);
		search.maximumPeakDanger = 0.08;
		while (!search.advance(2, expand, [&](Position p) { return p == destination; },
		    [&](Position p) { return Position::getDistanceX(p, destination); }, 10)) {}
		assert(search.result == Result::Reached);
		return search;
	};
	for (const Position destination : {goalA, goalB, goalC}) {
		const auto reference = independent(destination);
		assert(tree.extract({destination}, steps, summary, &selected) && selected == destination);
		assert(steps.size() == reference.steps.size());
		assert(summary.movementCost == reference.summary.movementCost);
		assert(summary.dangerCost == reference.summary.dangerCost);
		assert(summary.maximumHealthLossPerSecond == reference.summary.maximumHealthLossPerSecond);
		for (size_t i = 0; i < steps.size(); ++i)
			assert(steps[i].expectedPosition == reference.steps[i].expectedPosition);
	}
	assert(tree.extract({goalC, goalA}, steps, summary, &selected) && selected == goalA);
	assert(summary.movementCost == 20 && summary.dangerCost == 2 && summary.maximumHealthLossPerSecond == 0.03);
	assert(!tree.extract({Position(600, 600, 7)}, steps, summary) && steps.empty());
}

void resumedPaths()
{
	const auto reference = path(100000);
	assert(reference.result == Result::Reached);
	assert(reference.summary.movementCost == 201);
	for (const auto slice : {1u, 2u, 7u, 2048u}) {
		const auto resumed = path(slice);
		assert(resumed.result == reference.result);
		assert(resumed.expanded == reference.expanded);
		assert(resumed.summary.movementCost == reference.summary.movementCost);
		assert(resumed.steps.size() == reference.steps.size());
		for (size_t i = 0; i < resumed.steps.size(); ++i) {
			assert(resumed.steps[i].expectedPosition == reference.steps[i].expectedPosition);
		}
	}
	for (const auto slice : {1u, 3u, 100000u}) {
		const auto limited = path(slice, 20);
		assert(limited.result == Result::NodeLimit && limited.expanded == 20);
	}
	PlayerBotNavigationGoal goal;
	PlayerBotPathSearch large(Position(0, 0, 0), goal, 100000);
	uint64_t turns = 0;
	while (!large.advance(127, [](Position p) {
		PlayerBotNavigationStep step;
		const uint32_t index = uint32_t(p.y) * 60000 + p.x + 1;
		step.expectedPosition = Position(index % 60000, index / 60000, 0);
		return std::vector<PlayerBotPathSearch::Arc>{{step, 1, 0, 0}};
	}, [](Position) { return false; }, [](Position) { return 1; })) ++turns;
	assert(turns > 700 && large.expanded == 100000 && large.result == Result::NodeLimit);
}

template<class Query>
void finish(Transport& graph, Query query)
{
	auto detailedOnly = [&](size_t state, size_t connection, bool detailed) -> std::optional<Segment> {
		assert(detailed); // No discarded coarse graph callback remains.
		return query(state, connection, detailed);
	};
	for (unsigned operation = 0; operation < 10000; ++operation) {
		if (graph.advance(detailedOnly)) {
			assert(graph.counters.coarseQueries == 0);
			return;
		}
	}
	assert(false && "transport graph must finish");
}

void remoteAlternativesAndFareLabels()
{
	// Fast/high-fare and slow/low-fare labels reach the same remote state.
	// Only the latter can pay for the second boat. No geometric radius here.
	Transport graph(3, {{0, 1, 90}, {1, 1, 10}, {2, 2, 50}}, 100);
	unsigned pending = 0;
	finish(graph, [&](size_t state, size_t connection, bool detailed) -> std::optional<Segment> {
		if (state == 0 && connection == 0) return Segment{Result::Reached, 1, 1, 0, 0};
		if (state == 0 && connection == 1) {
			assert(detailed);
			if (++pending < 4) return std::nullopt;
			return Segment{Result::Reached, 30, 30, 0, 0};
		}
		if (state == 1 && connection == 2) return Segment{Result::Reached, 1, 1, 0, 0};
		if (state == 2 && connection == SIZE_MAX) return Segment{Result::Reached, 1, 1, 0, 0};
		return Segment{Result::Unreachable};
	});
	assert(pending == 4);
	assert(graph.bestPaid && graph.bestPaid->fare == 60 && graph.bestPaid->firstOffer == 1);
	assert(graph.bestPaid->seconds == 34);
}

void goalDirectedTransportScheduling()
{
	// Three superficially plausible arrivals each waste 30k detailed nodes.
	// An intermediate boat stop is farther from the goal than all three,
	// but its connecting offer reaches the goal before the 100k allowance.
	std::vector<Transport::Offer> offers{{0, 1, 0, 600}, {0, 2, 0, 650},
	    {0, 3, 0, 700}, {0, 4, 0, 850}, {1, 5, 90, 10}, {1, 5, 10, 10}};
	Transport graph(6, offers, 100, std::nullopt, {1000, 600, 650, 700, 850, 10},
	    {Position(0, 0, 7), Position(1000, 0, 7), Position(1001, 0, 7),
	     Position(1002, 0, 7), Position(1003, 0, 7), Position(2000, 0, 7)},
	    {Position(0, 0, 7), Position(1003, 0, 7)});
	unsigned nodes = 0, wastedFinals = 0, goalNodes = 0, goalWastedFinals = 0;
	finish(graph, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (nodes >= 100000) return Segment{Result::NodeLimit};
		if (connection == SIZE_MAX && state != 5) {
			nodes += std::min(30000u, 100000u - nodes);
			++wastedFinals;
			return Segment{Result::NodeLimit};
		}
		if (connection == 0 && state == 0) { nodes += 10; return Segment{Result::Reached, 1, 1, 0, 0}; }
		if (connection == 1 && state == 4) { nodes += 10; return Segment{Result::Reached, 1, 1, 0, 0}; }
		if (connection == SIZE_MAX && state == 5) {
			nodes += 10;
			goalNodes = nodes;
			goalWastedFinals = wastedFinals;
			return Segment{Result::Reached, 1, 1, 0, 0};
		}
		return Segment{Result::Unreachable};
	});
	assert(graph.bestPaid && graph.bestPaid->fare == 10 && graph.bestPaid->firstOffer == 3);
	assert(goalNodes && goalNodes < 100000 && goalWastedFinals < 3);

	// A far-looking intermediate can lead through a zero-cost teleport.
	// Rank never changes the admissible bound or discards that connection.
	Transport remote(3, {{0, 1, 0, 100000}, {1, 2, 1, 0}}, 1,
	    std::nullopt, {0, 100000, 0});
	finish(remote, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if ((state == 0 && connection == 0) || (state == 1 && connection == 1) ||
		    (state == 2 && connection == SIZE_MAX)) return Segment{Result::Reached, 1, 1, 0, 0};
		return Segment{Result::Unreachable};
	});
	assert(remote.bestPaid && remote.bestPaid->fare == 1);

	// High-fare fast and low-fare slow arrivals must both survive ranking;
	// a safe paid incumbent must not suppress a cheaper-fare extension.
	Transport fares(2, {{0, 1, 90, 0}, {1, 1, 10, 10000}}, 100,
	    std::nullopt, {0, 5});
	unsigned cheapApproaches = 0;
	finish(fares, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (state == 0 && connection == 0) return Segment{Result::Reached, 1, 1, 0, 0};
		if (state == 0 && connection == 1) {
			++cheapApproaches;
			return Segment{Result::Reached, 20, 20, 0, 0};
		}
		if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached, 1, 1, 0, 0};
		return Segment{Result::Unreachable};
	});
	assert(fares.bestPaid && fares.bestPaid->fare == 90);
	assert(cheapApproaches == 1);
	assert(fares.counters.labels >= 3); // Both fare labels at the arrival survive.
}

void boundsAndRisk()
{
	for (uint64_t funds : {0u, 99u, 100u, 101u, 500u}) {
		const uint64_t spendable = playerBotHuntTransportFunds(funds, 100);
		for (uint64_t fare : {0u, 1u, 20u, 100u, 401u}) {
			assert((fare <= spendable) == playerBotHuntTravelAffordable(funds, 100, fare, 0, 0));
		}
	}
	// A sale sourced at a depot must plan against the same supply reserve
	// enforced when its seller route is executed, including both fare legs.
	const uint64_t saleFunds = 263, saleReserve = 220;
	const uint64_t saleFare = playerBotHuntTransportFunds(saleFunds, saleReserve);
	assert(saleFare == 43);
	assert(!playerBotHuntTravelPaymentAffordable(saleFunds, saleReserve, 60, 0,
	    PlayerBotHuntTravelBudgetPhase::Supply));
	assert(playerBotHuntTravelPaymentAffordable(saleFunds, saleReserve, 0, 0,
	    PlayerBotHuntTravelBudgetPhase::Supply));
	assert(playerBotHuntTravelPaymentAffordable(saleFunds, saleReserve, 20 + 23, 0,
	    PlayerBotHuntTravelBudgetPhase::Supply));
	assert(!playerBotHuntTravelPaymentAffordable(saleFunds, saleReserve, 20 + 24, 0,
	    PlayerBotHuntTravelBudgetPhase::Supply));
	// Even a broke bot can use free transport; paid alternatives cannot spend
	// gold that final hunt acceptance already reserves for recovery.
	Transport freeTrip(2, {{0, 1, 0}, {1, 1, 1}}, playerBotHuntTransportFunds(100, 100));
	finish(freeTrip, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
		assert(connection != 1);
		return Segment{Result::Reached, state == 0 ? 1.0 : 2.0, 1, 0, 0};
	});
	assert(freeTrip.bestPaid && freeTrip.bestPaid->fare == 0);

	assert(playerBotTransportBoundRejects(11, 100, 10, 0, true));
	assert(!playerBotTransportBoundRejects(11, 10, 10, 100, true)); // useful cheap fare
	assert(!playerBotTransportBoundRejects(11, 100, 10, 0, false)); // unsafe incumbent
	assert(!playerBotTransportBoundRejects(9, 100, 10, 0, true));
	Transport::Label walking;
	walking.cost = 5;
	Transport graph(2, {{0, 1, 100}}, 100, walking);
	unsigned queries = 0;
	finish(graph, [&](size_t, size_t, bool) -> std::optional<Segment> { ++queries; return Segment{}; });
	assert(queries == 0 && graph.counters.boundRejects == 1);

	walking.danger = 1000;
	Transport unsafe(2, {{0, 1, 100}}, 100, walking);
	finish(unsafe, [&](size_t state, size_t, bool) -> std::optional<Segment> {
		++queries;
		return Segment{Result::Reached, state == 0 ? 2.0 : 1.0, 1, 0, 0};
	});
	assert(queries > 0 && unsafe.bestPaid);

	// Cheap but unsafe prefixes cannot dominate the viable safe alternative.
	Transport risk(2, {{0, 1, 10}, {1, 1, 10}}, 100);
	finish(risk, [](size_t state, size_t c, bool) -> std::optional<Segment> {
		if (state == 0) return Segment{Result::Reached, c == 0 ? 1.0 : 50.0, 1, 0, c == 0 ? 0.2 : 0.0};
		return Segment{Result::Reached, 1, 1, 0, 0};
	});
	assert(risk.bestPaid && risk.bestPaid->firstOffer == 1);
	assert(risk.counters.riskRejects > 0);

	Transport hazardous(2, {{0, 1, 1}}, 10);
	unsigned unsafeArrivalQueries = 0;
	finish(hazardous, [&](size_t state, size_t, bool) -> std::optional<Segment> {
		if (state == 1) ++unsafeArrivalQueries;
		return Segment{Result::Reached, 1, 1, 0, 0.2};
	});
	assert(!hazardous.bestPaid && unsafeArrivalQueries == 0);
	assert(hazardous.counters.riskRejects == 1);
}

void detailedConnectionsOnly()
{
	Transport route(2, {{0, 1, 1}}, 1);
	unsigned detailedLegs = 0;
	finish(route, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		++detailedLegs;
		assert((state == 0 && connection == 0) || (state == 1 && connection == SIZE_MAX));
		return Segment{Result::Reached, 1, 1, 0, 0};
	});
	assert(detailedLegs == 2 && route.counters.localQueries == 2 && route.bestPaid);
	assert(route.bestPaid->fare == 1 && route.bestPaid->danger == 0);

	// An unsafe detailed final leg cannot become an incumbent; the other
	// arrival still gets its detailed connection evaluated.
	Transport alternatives(3, {{0, 1, 1}, {1, 2, 1}}, 2);
	unsigned unsafeFinalChecks = 0, safeFinalChecks = 0;
	finish(alternatives, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (state == 1 && connection == SIZE_MAX) {
			++unsafeFinalChecks;
			return Segment{Result::Reached, 1, 1, 1000, 0.5};
		}
		if (state == 2 && connection == SIZE_MAX) ++safeFinalChecks;
		return Segment{Result::Reached, state == 2 ? 3.0 : 1.0, 1, 0, 0};
	});
	assert(unsafeFinalChecks == 1 && safeFinalChecks == 1);
	assert(alternatives.bestPaid && alternatives.bestPaid->firstOffer == 1);
	assert(alternatives.counters.riskRejects == 1);

	Transport::Label walking;
	walking.cost = 10;
	Transport cheaper(2, {{0, 1, 0}}, 0, walking);
	unsigned queried = 0;
	finish(cheaper, [&](size_t, size_t, bool) -> std::optional<Segment> {
		++queried;
		return Segment{Result::Reached, 1, 1, 0, 0};
	});
	assert(queried == 2 && cheaper.bestPaid && cheaper.bestPaid->cost == 3);
}

void remoteConnectionsAndBounds()
{
	Transport::Label walking;
	walking.cost = 20;
	Transport graph(3, {{0, 1, 0}, {1, 2, 0}}, 0, walking);
	bool remoteFinalVisited = false;
	finish(graph, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (state == 0 && connection == 0) return Segment{Result::Reached, 1, 1, 0, 0};
		if (state == 1 && connection == 1) return Segment{Result::Reached, 1, 1, 0, 0};
		if (state == 2 && connection == SIZE_MAX) {
			remoteFinalVisited = true;
			return Segment{Result::Reached, 1, 1, 0, 0};
		}
		return Segment{Result::Unreachable};
	});
	assert(remoteFinalVisited); // Remote multi-leg travel cannot be pruned by geometry.
	for (const double cardinal : {100.0, 400.0, 1000.0}) {
		for (unsigned moves = 0; moves < 20; ++moves) for (unsigned uses = 0; uses < 5; ++uses) {
			const unsigned danger = moves * 7;
			const unsigned units = moves * 10 + moves * 30 + uses * 20 + danger;
			const double actual = moves * cardinal / 1000 + moves * cardinal * 3 / 1000 + uses + danger / 10.0;
			assert(playerBotLocalRouteCostLowerBound(units, cardinal, cardinal * 3) <= actual + 1e-9);
			const double sellActual = playerBotTransportRouteCost(
			    moves * cardinal / 1000 + moves * cardinal * 3 / 1000 + uses, 0, danger, true);
			assert(playerBotLocalRouteCostLowerBound(units, cardinal, cardinal * 3, true) <= sellActual + 1e-9);
		}
	}
	assert(playerBotLocalRouteCostLowerBound(30, 1000, 3000) <= 1.0); // Open diagonal shovel passage.
	// A safe request-wide cutoff is neither a failed path nor exhausted nodes.
	Transport bounded(2, {{0, 1, 1}}, 10);
	finish(bounded, [](size_t, size_t, bool detailed) -> std::optional<Segment> {
		return Segment{Result::NodeLimit, 0, 0, 0, 0, detailed};
	});
	assert(!bounded.incomplete && bounded.counters.boundRejects == 1);
}

void unknownAndCache()
{
	// Offers sharing a provider share work, but NodeLimit is still unknown.
	Transport graph(3, {{0, 1, 1}, {0, 2, 2}}, 10);
	unsigned localQueries = 0;
	finish(graph, [&](size_t, size_t, bool detailed) -> std::optional<Segment> {
		if (detailed) ++localQueries;
		return Segment{Result::NodeLimit};
	});
	assert(localQueries == 1 && graph.incomplete && !graph.bestPaid);
	assert(graph.counters.unknown == 1 && graph.counters.cacheHits == 1);
	Transport nextRequest(2, {{0, 1, 1}}, 10);
	finish(nextRequest, [](size_t, size_t, bool) -> std::optional<Segment> {
		return Segment{Result::Reached, 1, 1, 0, 0};
	});
	assert(nextRequest.bestPaid && !nextRequest.incomplete);
}

void transportObjectives()
{
	assert(playerBotTransportRouteCost(120, 0, 0, true) == 20);
	assert(playerBotTransportRouteCost(10, 30, 0) == 13);
	assert(playerBotTransportRouteCost(6, 3, 20, true) == 6);
	for (const bool sell : {false, true}) {
		Transport::Label walking;
		walking.seconds = 120;
		walking.cost = playerBotTransportRouteCost(walking.seconds, 0, 0, sell);
		Transport route(2, {{0, 1, 30}}, 100, walking, {}, {}, {}, sell);
		unsigned queries = 0;
		finish(route, [&](size_t state, size_t connection, bool) -> std::optional<Segment> {
			++queries;
			if (state == 0 && connection == 0) return Segment{Result::Reached, 9};
			if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached};
			return Segment{Result::Unreachable};
		});
		if (sell) {
			assert(!route.bestPaid && queries == 0); // 20g walking beats 31 2/3g by boat.
			assert(route.counters.boundRejects == 1);
		} else {
			assert(route.bestPaid && route.bestPaid->seconds == 10 && route.bestPaid->fare == 30);
			assert(route.bestPaid->cost == 13 && route.bestPaid->cost < walking.cost);
		}

		// A cheap but unsafe walk must not prune a safe boat in either mode.
		walking.seconds = walking.cost = 0;
		walking.peak = 0.2;
		Transport unsafeWalk(2, {{0, 1, 30}}, 100, walking, {}, {}, {}, sell);
		finish(unsafeWalk, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
			if (state == 0 && connection == 0) return Segment{Result::Reached, 9};
			if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached};
			return Segment{Result::Unreachable};
		});
		assert(unsafeWalk.bestPaid && unsafeWalk.bestPaid->fare == 30);
	}
}

void sellPaidAlternatives()
{
	// Visit the fast expensive offer first. The cheapest fare is not the best
	// economic route either: fare 5 wins once travel time is included.
	for (const bool sell : {false, true}) {
		Transport route(2, {{0, 1, 30, 0}, {1, 1, 5, 1000}, {2, 1, 1, 2000}},
		    100, std::nullopt, {}, {}, {}, sell);
		finish(route, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
			if (state == 0 && connection < 3) {
				const double seconds[] = {9, 59, 119};
				return Segment{Result::Reached, seconds[connection]};
			}
			if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached};
			return Segment{Result::Unreachable};
		});
		assert(route.bestPaid && route.bestPaid->fare == (sell ? 5 : 30));
		assert(route.bestPaid->firstOffer == (sell ? 1u : 0u));
		assert(route.bestPaid->cost == (sell ? 15 : 13));
		assert(std::abs(route.bestPaid->lowerCost - route.bestPaid->cost) < 1e-9);
	}

	// Validated walking prefixes must use seconds/6 too; a hunt-cost bound
	// here would incorrectly prune the paid route against the 20g walk.
	Transport::Label walking;
	walking.seconds = 120;
	walking.cost = playerBotTransportRouteCost(120, 0, 0, true);
	Transport cheaper(2, {{0, 1, 5}}, 100, walking, {}, {}, {}, true);
	finish(cheaper, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if (state == 0 && connection == 0) return Segment{Result::Reached, 54};
		if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached, 5};
		return Segment{Result::Unreachable};
	});
	assert(cheaper.bestPaid && cheaper.bestPaid->cost == 15 && cheaper.bestPaid->fare == 5);

	// Do not round each leg: two one-second rides remain below a 3g walk.
	walking.seconds = 18;
	walking.cost = playerBotTransportRouteCost(18, 0, 0, true);
	Transport fractional(3, {{0, 1, 1}, {1, 2, 1}}, 2, walking, {}, {}, {}, true);
	finish(fractional, [](size_t state, size_t connection, bool) -> std::optional<Segment> {
		if ((state == 0 && connection == 0) || (state == 1 && connection == 1) ||
		    (state == 2 && connection == SIZE_MAX)) return Segment{Result::Reached};
		return Segment{Result::Unreachable};
	});
	assert(fractional.bestPaid && fractional.bestPaid->fare == 2 && fractional.bestPaid->seconds == 2);
	assert(std::abs(fractional.bestPaid->cost - 7.0 / 3.0) < 1e-9);
}

void paidIncumbentPruning()
{
	Transport graph(4, {{0, 1, 10}, {1, 2, 0}, {2, 3, 100}}, 200);
	unsigned expensiveDetailed = 0, cheapDetailed = 0, cheapFinal = 0;
	finish(graph, [&](size_t state, size_t connection, bool detailed) -> std::optional<Segment> {
		if (connection == 2 && detailed) ++expensiveDetailed;
		if (state == 0 && connection == 0) return Segment{Result::Reached, 0, 0, 0, 0};
		if (state == 1 && connection == SIZE_MAX) return Segment{Result::Reached, 0, 0, 0, 0};
		if (state == 0 && connection == 1) {
			++cheapDetailed;
			return Segment{Result::Reached, 5, 5, 0, 0};
		}
		if (state == 2 && connection == SIZE_MAX) {
			if (detailed) ++cheapFinal;
			return Segment{Result::Reached, 0, 0, 0, 0};
		}
		return Segment{Result::Unreachable};
	});
	assert(graph.bestPaid && graph.bestPaid->fare == 10 && graph.bestPaid->cost == 2);
	assert(expensiveDetailed == 0); // The paid result stops costly detailed work.
	assert(cheapDetailed == 1 && cheapFinal == 1); // Cost 6 > 2, but fare 0 < 10 survives.
	assert(graph.counters.boundRejects > 0);
}

void providerMovementPolicy()
{
	PlayerBotHuntRequestRestartBudget budget;
	for (unsigned retry = 0; retry < PlayerBotHuntRequestRestartBudget::maximumRestarts; ++retry) {
		assert(budget.retry());
		const auto reconstructed = budget;
		budget = reconstructed; // controller recreates its request-local work
	}
	assert(!budget.retry()); // Unknown result; never start another walking frontier.

	PlayerBotHuntProviderValidation validation;
	std::map<uint32_t, Position> live{{1, Position(100, 100, 7)}, {2, Position(200, 200, 7)}};
	validation.reset(live);
	assert(validation.use(1) == live.at(1));
	for (unsigned turn = 0; turn < 30; ++turn) {
		live[2] = Position(200 + (turn % 2 ? 3 : -3), 200, 7);
		assert(validation.update(live)); // Radius-three wandering of an unvisited NPC cannot restart work.
	}
	assert(validation.use(2) == live.at(2)); // First query uses its fresh position, not the initial snapshot.
	live[1] = Position(102, 100, 7);
	assert(validation.update(live));
	live[1] = Position(103, 100, 7);
	assert(!validation.update(live)); // Anchors do not creep along with repeated movement.
	for (unsigned restart = 0; restart <= PlayerBotHuntProviderValidation::maximumRestarts; ++restart) {
		assert(validation.restartAllowed() == (restart < PlayerBotHuntProviderValidation::maximumRestarts));
		validation.reset(live); // Rebuilding dependent transport work does not reset the request's retry count.
		validation.use(1);
		live[1].x += 3;
		assert(!validation.update(live));
	}
	assert(playerBotHuntAggregateRouteResult(Result::Reached, 1000, 0, true) == Result::NodeLimit);
	assert(playerBotHuntAggregateRouteResult(Result::Reached, 0, 0.2, true) == Result::NodeLimit);
	assert(playerBotHuntAggregateRouteResult(Result::Reached, 0, 0, true) == Result::Reached);
	assert(playerBotHuntAggregateRouteResult(Result::Reached, 1000, 0, false) == Result::Reached);
}

void cosmeticConnectivityContracts()
{
	PlayerBotRouteItemSignature cosmetic;
	cosmetic.moveable = cosmetic.pickupable = cosmetic.useable = true;
	cosmetic.passageId = 3058; // A useable corpse does not add a passage.
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, false));
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, true));
	cosmetic.passageId = 1386;
	assert(playerBotRouteItemMayOpenConnectivity(cosmetic, false));
	cosmetic.passageId = 0;
	cosmetic.blockSolid = true;
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, false));
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, true)); // movable blocker
	cosmetic.moveable = false;
	assert(playerBotRouteItemMayOpenConnectivity(cosmetic, true)); // previously blocked tile
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, true, true)); // graph already includes tile
	cosmetic.blockSolid = false;
	cosmetic.hasHeight = true;
	assert(playerBotRouteItemMayOpenConnectivity(cosmetic, false)); // fixed height
	assert(playerBotRouteItemMayOpenConnectivity(cosmetic, true, true)); // height removal can restore same-floor walking
	cosmetic.moveable = true;
	assert(!playerBotRouteItemMayOpenConnectivity(cosmetic, false)); // parcel-only climb
	PlayerBotRouteItemSignature ground;
	ground.ground = true;
	ground.passageId = 6594;
	assert(!playerBotRouteItemUpdateMayOpenConnectivity(ground, ground)); // visual ground decay
	PlayerBotRouteItemSignature pitfall;
	pitfall.passageId = 3311;
	pitfall.floorChange = 1;
	auto decayedPitfall = pitfall;
	decayedPitfall.passageId = 3310;
	assert(!playerBotRouteItemUpdateMayOpenConnectivity(pitfall, decayedPitfall, true));
	const PlayerBotRouteItemSignature empty;
	assert(playerBotRouteItemUpdateMayOpenConnectivity(empty, ground)); // new floor
	ground.passageId = 384;
	assert(playerBotRouteItemUpdateMayOpenConnectivity(ground, empty)); // rope spot
	ground.passageId = 6594;
	ground.blockSolid = true;
	assert(playerBotRouteItemUpdateMayOpenConnectivity(ground, empty)); // opens a blocked tile
	PlayerBotRouteItemSignature door;
	door.door = door.blockSolid = true;
	door.passageId = 1223;
	PlayerBotRouteItemSignature openDoor = door;
	openDoor.blockSolid = false;
	openDoor.passageId = 1224;
	assert(playerBotRouteItemUpdateMayOpenConnectivity(door, openDoor, true));
	assert(!playerBotRouteItemUpdateMayOpenConnectivity(door, openDoor, true, true));
	PlayerBotRouteItemSignature hole;
	hole.passageId = 7932;
	auto openHole = hole;
	openHole.passageId = 7933;
	openHole.floorChange = 1;
	assert(playerBotRouteItemUpdateMayOpenConnectivity(hole, openHole, true));
	assert(!playerBotRouteItemUpdateMayOpenConnectivity(hole, openHole, true, false, true));
	for (const auto [closedId, openId] : {std::pair<uint16_t, uint16_t>{468, 469},
	                                   {481, 482}, {483, 484}}) {
		hole.passageId = closedId;
		openHole.passageId = openId;
		assert(!playerBotRouteItemUpdateMayOpenConnectivity(hole, openHole, true, false, true));
	}

	const Position location(101, 201, 7);
	const auto before = PlayerBotRouteChanges::currentConnectivityRevision();
	PlayerBotRouteChanges::Watch watch;
	{ PlayerBotRouteChanges::Scope scope(watch); PlayerBotRouteChanges::read(location); }
	PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::ItemAdd, false);
	assert(watch.check() == PlayerBotRouteChanges::Reason::ChangedTile);
	assert(PlayerBotRouteChanges::currentConnectivityRevision() == before);
	PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::ItemRemove);
	assert(PlayerBotRouteChanges::currentConnectivityRevision() == before + 1);
}

void itemUpdateDependencies()
{
	PlayerBotRouteItemSignature harmless;
	harmless.moveable = true;
	harmless.pickupable = true;
	assert(!playerBotRouteItemUpdateAffectsNavigation(harmless, harmless)); // count, fluid, harmless decay
	// Different visual IDs with identical non-passage semantics do not affect a route.
	const auto cosmetic = harmless;
	assert(!playerBotRouteItemUpdateAffectsNavigation(harmless, cosmetic));
	cosmeticConnectivityContracts();
	const Position location(100, 200, 7);
	PlayerBotRouteChanges::Watch watch;
	{ PlayerBotRouteChanges::Scope scope(watch); PlayerBotRouteChanges::read(location); }
	if (playerBotRouteItemUpdateAffectsNavigation(harmless, cosmetic))
		PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::ItemUpdate);
	assert(watch.check() == PlayerBotRouteChanges::Reason::None);
	{
		PlayerBotRouteChanges::Suppress update;
		PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::TileFlag);
		PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::TileFlag);
	}
	assert(watch.check() == PlayerBotRouteChanges::Reason::None); // transient reset/reapply
	auto relevant = harmless;
	const auto check = [&](const PlayerBotRouteItemSignature& from, const PlayerBotRouteItemSignature& to) {
		assert(playerBotRouteItemUpdateAffectsNavigation(from, to));
		PlayerBotRouteChanges::Watch dependency;
		{ PlayerBotRouteChanges::Scope scope(dependency); PlayerBotRouteChanges::read(location); }
		PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::ItemUpdate);
		assert(dependency.check() == PlayerBotRouteChanges::Reason::ChangedTile);
		assert(dependency.changedCause == PlayerBotRouteChanges::Cause::ItemUpdate);
	};
	relevant.passageId = 1386; check(harmless, relevant); // ladder identity
	relevant = harmless; relevant.door = true; relevant.passageId = 1223; check(harmless, relevant);
	auto closedDoor = relevant; closedDoor.passageId = 1223;
	relevant.passageId = 1224; check(closedDoor, relevant); // opening a door changes its action identity
	relevant = harmless; relevant.magicField = true; check(relevant, relevant); // same-ID field damage
	relevant = harmless; relevant.teleport = true; check(harmless, relevant);
	relevant = harmless; relevant.ground = true; relevant.groundSpeed = 150; check(harmless, relevant);
	relevant = harmless; relevant.blockSolid = true; check(harmless, relevant);
	relevant = harmless; relevant.blockProjectile = true; check(harmless, relevant);
	relevant = harmless; relevant.hasHeight = true; check(harmless, relevant);
	relevant = harmless; relevant.actionId = 101; check(harmless, relevant);
	relevant = harmless; relevant.floorChange = 1; check(harmless, relevant);
	PlayerBotRouteChanges::Watch changedFlags;
	{ PlayerBotRouteChanges::Scope scope(changedFlags); PlayerBotRouteChanges::read(location); }
	{ PlayerBotRouteChanges::Suppress update; PlayerBotRouteChanges::changed(location); }
	PlayerBotRouteChanges::changed(location, PlayerBotRouteChanges::Cause::TileFlag); // net flags changed
	assert(changedFlags.check() == PlayerBotRouteChanges::Reason::ChangedTile);
}

void invalidationAndCancellation()
{
	const Position anchor(100, 100, 7);
	for (int x = -2; x <= 2; ++x) for (int y = -2; y <= 2; ++y) {
		const Position live(anchor.x + x, anchor.y + y, anchor.z);
		assert(playerBotNpcTravelUsesLocalApproach(anchor, live, 2));
		for (int ax = -1; ax <= 1; ++ax) for (int ay = -1; ay <= 1; ++ay) {
			const Position approach(anchor.x + ax, anchor.y + ay, anchor.z);
			assert((Position::areInRange<3, 3, 0>(approach, live)));
		}
	}
	assert(!playerBotNpcTravelUsesLocalApproach(anchor, Position(103, 100, 7), 2));
	assert(!playerBotNpcTravelUsesLocalApproach(anchor, Position(100, 100, 8), 2));
	const Position read(10, 20, 7), unrelated(20, 30, 7);
	PlayerBotRouteChanges::Watch watch;
	{
		PlayerBotRouteChanges::Scope scope(watch);
		PlayerBotRouteChanges::read(read); // Includes absent tiles.
	}
	PlayerBotRouteChanges::changed(unrelated);
	assert(watch.check() == PlayerBotRouteChanges::Reason::None);
	PlayerBotRouteChanges::changed(read);
	assert(watch.check() == PlayerBotRouteChanges::Reason::ChangedTile);
	PlayerBotRouteChanges::Watch overflow;
	for (unsigned i = 0; i < 4097; ++i) PlayerBotRouteChanges::changed(unrelated);
	assert(overflow.check() == PlayerBotRouteChanges::Reason::JournalOverflow);
	PlayerBotRouteChanges::Watch permissions;
	PlayerBotRouteChanges::invalidate();
	assert(permissions.check() == PlayerBotRouteChanges::Reason::Epoch);
	{
		auto cancelled = std::make_unique<PlayerBotRouteChanges::Watch>();
		{ PlayerBotRouteChanges::Scope scope(*cancelled); PlayerBotRouteChanges::read(read); }
		cancelled.reset();
	}
	// No observer or world callback survives cancellation.
	PlayerBotRouteChanges::read(read);
	PlayerBotRouteChanges::changed(read);
}
}

int main()
{
	resumedPaths();
	huntCoarseVerdictContracts();
	ordinaryWalkingContracts();
	peakDangerPathContracts();
	reusableSourceTreeContracts();
	remoteAlternativesAndFareLabels();
	goalDirectedTransportScheduling();
	boundsAndRisk();
	detailedConnectionsOnly();
	remoteConnectionsAndBounds();
	unknownAndCache();
	paidIncumbentPruning();
	transportObjectives();
	sellPaidAlternatives();
	providerMovementPolicy();
	itemUpdateDependencies();
	invalidationAndCancellation();
	std::cout << "playerbot routing contracts passed\n";
}
