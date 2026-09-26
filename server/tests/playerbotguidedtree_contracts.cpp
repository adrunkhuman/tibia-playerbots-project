#include "playerbotpathsearch.h"
#include "playerbotshortcutheuristic.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <vector>

namespace {
using Result = PlayerBotNavigationResult;
using Arc = PlayerBotPathSearch::Arc;

// Index-based, directed test graph. The oracle never uses PathSearch labels.
struct Graph {
	struct Edge { unsigned to; uint32_t movement, danger; double peak; };
	std::vector<std::vector<Edge>> edges;
	std::vector<unsigned> expansions;
	explicit Graph(unsigned count) : edges(count), expansions(count) {}
	Position pos(unsigned i) const { return Position(100 + i, 200, 7); }
	unsigned index(Position p) const {
		assert(p.y == 200 && p.z == 7 && p.x >= 100 && p.x < 100 + edges.size());
		return p.x - 100;
	}
	void add(unsigned from, unsigned to, uint32_t movement, uint32_t danger = 0, double peak = 0) {
		assert(from < edges.size() && to < edges.size() && movement + danger > 0);
		edges[from].push_back({to, movement, danger, peak});
	}
	std::vector<Arc> expand(Position from) {
		const unsigned i = index(from);
		++expansions[i];
		std::vector<Arc> result;
		for (const Edge& e : edges[i]) {
			PlayerBotNavigationStep step;
			step.target = step.expectedPosition = pos(e.to);
			result.push_back({step, e.movement, e.danger, e.peak});
		}
		return result;
	}
	std::vector<uint32_t> distances(const std::vector<unsigned>& sources, bool reverse = false) const {
		const uint32_t inf = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> dist(edges.size(), inf);
		using Entry = std::pair<uint32_t, unsigned>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
		for (unsigned source : sources) {
			assert(source < edges.size());
			dist[source] = 0;
			queue.emplace(0, source);
		}
		while (!queue.empty()) {
			const auto [cost, node] = queue.top();
			queue.pop();
			if (dist[node] != cost) continue;
			for (unsigned from = 0; from < edges.size(); ++from) for (const Edge& e : edges[from]) {
				if ((reverse ? e.to != node : from != node)) continue;
				const unsigned to = reverse ? from : e.to;
				const uint32_t next = cost + e.movement + e.danger;
				if (next < dist[to]) {
					dist[to] = next;
					queue.emplace(next, to);
				}
			}
		}
		return dist;
	}
	std::vector<uint32_t> potential(const std::vector<unsigned>& goals) const {
		auto h = distances(goals, true);
		// An unreachable node has no outgoing route into the goal region.
		// Use the largest finite distance to preserve consistency across
		// edges leaving the reachable region (zero would break it).
		uint32_t largest = 0;
		for (uint32_t value : h) if (value != UINT32_MAX) largest = std::max(largest, value);
		for (uint32_t& value : h) if (value == UINT32_MAX) value = largest;
		for (unsigned from = 0; from < edges.size(); ++from) for (const Edge& e : edges[from])
			assert(h[from] <= e.movement + e.danger + h[e.to]);
		for (unsigned goal : goals) assert(h[goal] == 0);
		return h;
	}
	PlayerBotNavigationGoal goal(const std::vector<unsigned>& indices) const {
		PlayerBotNavigationGoal result;
		result.type = PlayerBotNavigationGoalType::AnyOf;
		for (unsigned i : indices) result.positions.push_back(pos(i));
		return result;
	}
};

void verifyPath(const Graph& graph, const PlayerBotPathSearch& search, const std::vector<unsigned>& targets,
                uint32_t expected) {
	std::deque<PlayerBotNavigationStep> steps;
	PlayerBotNavigationCostSummary summary;
	Position reached;
	std::vector<Position> positions;
	for (unsigned i : targets) positions.push_back(graph.pos(i));
	assert(search.extract(positions, steps, summary, &reached));
	assert(std::find(positions.begin(), positions.end(), reached) != positions.end());
	assert(summary.movementCost + summary.dangerCost == expected);
	unsigned current = graph.index(search.start);
	uint32_t movement = 0, danger = 0;
	double peak = 0;
	for (const auto& step : steps) {
		const unsigned next = graph.index(step.expectedPosition);
		const auto& arcs = graph.edges[current];
		const auto it = std::find_if(arcs.begin(), arcs.end(), [&](const Graph::Edge& e) {
			return e.to == next;
		});
		assert(it != arcs.end() && step.target == graph.pos(next));
		movement += it->movement;
		danger += it->danger;
		peak = std::max(peak, it->peak);
		current = next;
	}
	assert(graph.pos(current) == reached);
	assert(summary.movementCost == movement && summary.dangerCost == danger);
	assert(summary.maximumHealthLossPerSecond == peak);
}

void query(Graph& graph, PlayerBotPathSearch& search, const std::vector<unsigned>& targets, uint64_t slice) {
	const auto h = graph.potential(targets);
	search.retarget(graph.goal(targets), [&](Position p) { return h[graph.index(p)]; });
	const auto fromStart = graph.distances({graph.index(search.start)});
	uint32_t best = UINT32_MAX;
	for (unsigned target : targets) best = std::min(best, fromStart[target]);
	const auto unused = [](Position) -> uint32_t { assert(false && "source trees ignore endpoint callbacks"); return 0; };
	for (unsigned turns = 0; !search.advance(slice, [&](Position p) { return graph.expand(p); },
	     unused, unused, 0, [&](Position p) { return h[graph.index(p)]; }); ++turns) {
		assert(turns < 1000 && slice != 0);
	}
	assert(search.result == (best == UINT32_MAX ? Result::Unreachable : Result::Reached));
	if (best != UINT32_MAX) verifyPath(graph, search, targets, best);
	for (unsigned count : graph.expansions) assert(count <= 1); // No restart or re-expansion after retarget.
	assert(search.expanded == std::accumulate(graph.expansions.begin(), graph.expansions.end(), uint64_t(0)));
}

void retargetAndFrontierBound() {
	Graph graph(6);
	// The expensive goal settles first under its old consistent potential;
	// its cheaper competitor is still on the frontier after retargeting.
	graph.add(0, 1, 20, 0, 0.1);
	graph.add(0, 2, 1);
	graph.add(2, 1, 100);
	graph.add(2, 3, 1, 1, 0.05);
	graph.add(1, 4, 2);
	graph.add(4, 5, 2);
	PlayerBotPathSearch tree(graph.pos(0), graph.goal({1}), 30, false, true);
	query(graph, tree, {1}, 1);
	assert(tree.expanded == 2 && graph.expansions[1] == 1); // Goal's outgoing arcs must have been generated.
	const auto h = graph.potential({1, 3});
	tree.retarget(graph.goal({1, 3}), [&](Position p) { return h[graph.index(p)]; });
	const auto unused = [](Position) -> uint32_t { assert(false); return 0; };
	assert(!tree.advance(0, [&](Position p) { return graph.expand(p); }, unused, unused, 0,
	                     [&](Position p) { return h[graph.index(p)]; }));
	query(graph, tree, {1, 3}, 1);
	assert(tree.expanded == 4);
	query(graph, tree, {5}, 2); // Continue through the previously settled goal.
	assert(tree.expanded <= graph.edges.size());
	query(graph, tree, {0}, 1); // Already settled start needs no new expansions.
}

void unreachableAndSettledEvidence() {
	Graph graph(4);
	graph.add(0, 1, 5);
	graph.add(2, 3, 1);
	PlayerBotPathSearch tree(graph.pos(0), graph.goal({1}), 4, false, true);
	query(graph, tree, {1}, 2);
	query(graph, tree, {3}, 1);
	std::deque<PlayerBotNavigationStep> steps;
	PlayerBotNavigationCostSummary summary;
	assert(!tree.extract({graph.pos(3)}, steps, summary));
	assert(steps.empty() && summary.movementCost == 0);
	query(graph, tree, {1}, 4); // Finished tree can still answer settled goals.
	assert(tree.expanded == 2);
}

void nodeLimitAndResume() {
	Graph graph(5);
	for (unsigned i = 0; i < 4; ++i) graph.add(i, i + 1, 3, 1, 0.02);
	PlayerBotPathSearch tree(graph.pos(0), graph.goal({4}), 2, false, true);
	const auto h = graph.potential({4});
	const auto unused = [](Position) -> uint32_t { assert(false); return 0; };
	while (!tree.advance(1, [&](Position p) { return graph.expand(p); }, unused, unused, 0,
	                     [&](Position p) { return h[graph.index(p)]; })) {}
	assert(tree.result == Result::NodeLimit && tree.expanded == 2);
	// Retargeting an exhausted shared source must not refill its allowance.
	const auto nearer = graph.potential({3});
	tree.retarget(graph.goal({3}), [&](Position p) { return nearer[graph.index(p)]; });
	assert(tree.advance(10, [&](Position p) { return graph.expand(p); }, unused, unused, 0,
	                    [&](Position p) { return nearer[graph.index(p)]; }) == Result::NodeLimit);
	assert(tree.expanded == 2);
	query(graph, tree, {1}, 1); // Settled evidence is still available at the cap.
	assert(tree.expanded == 2);
	tree.allowance = 5;
	query(graph, tree, {4}, 1);
	assert(tree.expanded == 5);
	query(graph, tree, {2}, 3); // Settled target needs no work after exhaustion.
	assert(tree.expanded == 5);
}

void shortcutPotentialGuidesTree() {
	const Position start(10, 10, 7), goal(70, 10, 7), entry(10, 30, 7), arrival(69, 10, 7);
	PlayerBotNavigationGoal targets;
	targets.type = PlayerBotNavigationGoalType::AnyOf;
	targets.positions = {goal, Position(70, 11, 7)};
	PlayerBotShortcutHeuristic potential(targets.positions, {{entry, arrival}}, Position(0, 0, 7), Position(99, 79, 7));
	auto expand = [&](Position from) {
		std::vector<Arc> arcs;
		auto add = [&](Position to) {
			PlayerBotNavigationStep step;
			step.target = step.expectedPosition = to;
			arcs.push_back({step, 10, 0, 0});
		};
		if (from.x) add(Position(from.x - 1, from.y, 7));
		if (from.x < 99) add(Position(from.x + 1, from.y, 7));
		if (from.y) add(Position(from.x, from.y - 1, 7));
		if (from.y < 79) add(Position(from.x, from.y + 1, 7));
		if (from == entry) add(arrival);
		return arcs;
	};
	auto run = [&](bool guided) {
		PlayerBotPathSearch tree(start, targets, 10000, false, true);
		const auto unused = [](Position) -> uint32_t { assert(false); return 0; };
		while (!tree.advance(17, expand, unused, unused, 0,
		    [&](Position p) { return guided ? potential.estimate(p) : 0u; })) {}
		assert(tree.result == Result::Reached);
		std::deque<PlayerBotNavigationStep> steps;
		PlayerBotNavigationCostSummary summary;
		assert(tree.extract(targets.positions, steps, summary));
		assert(summary.movementCost == 220); // The best path first walks away from the goal.
		assert(std::any_of(steps.begin(), steps.end(), [&](const auto& step) { return step.expectedPosition == arrival; }));
		return tree.expanded;
	};
	const auto flood = run(false), guided = run(true);
	assert(guided < flood);
	std::cout << "shortcut-guided expansions: " << guided << '/' << flood << " (guided/Dijkstra)\n";
}

void startGoalRetarget() {
	Graph graph(5);
	for (unsigned i = 0; i < 4; ++i) graph.add(i, i + 1, 3, 1, 0.02);
	PlayerBotPathSearch tree(graph.pos(0), graph.goal({0}), 1, false, true);
	query(graph, tree, {0}, 1);
	// The source is already reached at zero cost. Its unexpanded queue entry
	// must survive so a later target can continue normally.
	assert(tree.expanded == 0 && graph.expansions[0] == 0);
	const auto h = graph.potential({1});
	tree.retarget(graph.goal({1}), [&](Position p) { return h[graph.index(p)]; });
	const auto unused = [](Position) -> uint32_t { assert(false); return 0; };
	assert(tree.advance(1, [&](Position p) { return graph.expand(p); }, unused, unused, 0,
	                    [&](Position p) { return h[graph.index(p)]; }) == Result::NodeLimit);
	assert(tree.expanded == 1 && graph.expansions[0] == 1);
	tree.allowance = 5;
	query(graph, tree, {4}, 1); // retarget resets NodeLimit; labels and frontier survive.
	assert(tree.expanded == 5);
	query(graph, tree, {2}, 3); // Settled target needs no work after exhaustion.
	assert(tree.expanded == 5);
}

void directedShortcuts() {
	Graph graph(7);
	// Positions increase toward the goal; the cheap directed shortcut first
	// travels away from it. Geometric distance is not a valid potential here.
	graph.add(0, 5, 40);
	graph.add(0, 1, 2);
	graph.add(1, 6, 2, 1);
	graph.add(6, 5, 2);
	graph.add(0, 2, 2);
	graph.add(2, 3, 2);
	graph.add(3, 4, 2);
	graph.add(4, 5, 20);
	const auto h = graph.potential({5});
	assert(h[0] == 7 && h[1] == 5 && h[6] == 2);
	PlayerBotPathSearch tree(graph.pos(0), graph.goal({5}), 20, false, true);
	query(graph, tree, {5}, 1);
	query(graph, tree, {3}, 3);
	query(graph, tree, {6, 4}, 2);
}

void admissibleIsNotConsistent() {
	Graph graph(4);
	graph.add(0, 1, 20);
	graph.add(0, 2, 1);
	graph.add(2, 1, 1);
	graph.add(1, 3, 100);
	// A goal-search A* may reopen node 1 with this admissible potential.
	// A reusable tree cannot treat its first settlement (g=20) as final:
	// h(2)=101 > cost(2,1)+h(1). Do not use such a potential for trees.
	const uint32_t h[] = {0, 0, 101, 0};
	assert(h[2] <= graph.distances({2})[3]);
	assert(h[2] > 1 + h[1]);
	PlayerBotPathSearch search(graph.pos(0), graph.goal({3}), 20);
	auto reached = [&](Position p) { return p == graph.pos(3); };
	auto distance = [&](Position) { return 0u; };
	while (!search.advance(1, [&](Position p) { return graph.expand(p); }, reached, distance, 0,
	                       [&](Position p) { return h[graph.index(p)]; })) {}
	assert(search.result == Result::Reached);
	assert(search.summary.movementCost == 102);
	assert(graph.expansions[1] == 2); // Reopening is allowed for a goal search.
}

void randomizedContracts() {
	std::mt19937 rng(0x5eed);
	for (unsigned trial = 0; trial < 100; ++trial) {
		Graph graph(8 + rng() % 17);
		for (unsigned from = 0; from < graph.edges.size(); ++from) {
			if (from + 1 < graph.edges.size()) graph.add(from, from + 1, 1 + rng() % 15, rng() % 5);
			for (unsigned j = 0; j < 3; ++j) {
				unsigned to = rng() % graph.edges.size();
				if (to != from && std::none_of(graph.edges[from].begin(), graph.edges[from].end(),
				    [&](const Graph::Edge& edge) { return edge.to == to; }))
					graph.add(from, to, 1 + rng() % 30, rng() % 8);
			}
		}
		PlayerBotPathSearch tree(graph.pos(0), graph.goal({}), graph.edges.size(), false, true);
		for (unsigned turn = 0; turn < 7; ++turn) {
			std::vector<unsigned> goals{unsigned(rng() % graph.edges.size())};
			if (turn % 2 == 0) goals.push_back(rng() % graph.edges.size());
			query(graph, tree, goals, 1 + rng() % 7);
		}
		assert(tree.expanded <= graph.edges.size());
	}
}
}

int main() {
	retargetAndFrontierBound();
	unreachableAndSettledEvidence();
	directedShortcuts();
	admissibleIsNotConsistent();
	randomizedContracts();
	nodeLimitAndResume();
	startGoalRetarget();
	shortcutPotentialGuidesTree();
	std::cout << "playerbot guided tree contracts passed\n";
}
