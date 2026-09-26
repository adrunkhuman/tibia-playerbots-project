#include "playerbotroutecorridor.h"
#include "playerbottopology.h"
#include "playerbotpathsearch.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using Graph = std::vector<std::vector<PlayerBotTopologyEdge>>;

void topologyContracts()
{
	// Nodes 0 and 4 can share a conceptual sector, but no graph edge joins them.
	Graph graph(7);
	graph[0].push_back({1, {}});
	graph[1].push_back({2, {}});
	graph[2].push_back({3, {}});
	graph[4].push_back({5, {}});
	graph[3].push_back({99, {}}); // A stale/out-of-range edge cannot admit a node.
	auto outgoing = [&](uint32_t node) -> const auto& { return graph[node]; };
	PlayerBotRouteCorridor corridor(static_cast<uint32_t>(graph.size()), {0, 3, 0, 99});
	assert(corridor.nodeCount() == 7 && corridor.seedCount() == 2);
	assert(corridor.contains(0) && corridor.contains(3));
	assert(!corridor.contains(1) && !corridor.contains(4));
	assert(!corridor.contains(std::nullopt) && !corridor.contains(99));
	corridor.widen(0, outgoing);
	assert(!corridor.contains(1));
	corridor.widen(1, outgoing);
	assert(corridor.contains(1) && !corridor.contains(2));
	assert(!corridor.contains(4) && !corridor.contains(5));
	corridor.widen(3, outgoing);
	assert(corridor.contains(2) && !corridor.contains(4));
	corridor.addSeeds({4, 4, 100}); // Alternate itinerary joins the allowed set.
	assert(corridor.seedCount() == 3 && corridor.contains(4) && !corridor.contains(5));
	corridor.widen(1, outgoing);
	assert(corridor.contains(5) && corridor.contains(2)); // Widening never removes nodes.

	PlayerBotRouteCorridor directed(2, {0});
	Graph oneWay(2);
	oneWay[0].push_back({1, {}});
	directed.widen(1, [&](uint32_t node) -> const auto& { return oneWay[node]; });
	assert(directed.contains(1));
	PlayerBotRouteCorridor reverse(2, {1});
	reverse.widen(3, [&](uint32_t node) -> const auto& { return oneWay[node]; });
	assert(!reverse.contains(0));
	PlayerBotRouteCorridor empty(0, {0});
	empty.widen(std::numeric_limits<uint32_t>::max(), outgoing);
	assert(empty.seedCount() == 0 && !empty.contains(0));
}

void pathSearchContracts()
{
	const Position start(100, 100, 7), hazard(101, 100, 7), detour(100, 101, 7), goalPos(101, 101, 7);
	Graph graph(4);
	graph[0].push_back({1, {}});
	graph[0].push_back({2, {}});
	graph[1].push_back({3, {}});
	graph[2].push_back({3, {}});
	PlayerBotRouteCorridor corridor(4, {0, 1, 3});
	auto walkNode = [&](Position p) -> std::optional<uint32_t> {
		if (p == start) return 0;
		if (p == hazard) return 1;
		if (p == detour) return 2;
		if (p == goalPos) return 3;
		return std::nullopt;
	};
	auto search = [&](const PlayerBotRouteCorridor* limit, bool missingTile = false) {
		const Position uncharted(100, 99, 7);
		const Position destination = missingTile ? Position(101, 99, 7) : goalPos;
		PlayerBotNavigationGoal goal;
		goal.position = destination;
		PlayerBotPathSearch path(start, goal, 32, true);
		path.maximumPeakDanger = 0.2;
		auto arc = [](Position to, Direction direction, double peak = 0.0) {
			PlayerBotNavigationStep step;
			step.direction = direction;
			step.target = step.expectedPosition = to;
			return PlayerBotPathSearch::Arc{step, 10, 0, peak};
		};
		auto expand = [&](Position from) {
			std::vector<PlayerBotPathSearch::Arc> arcs;
			if (missingTile) {
				if (from == start) arcs.push_back(arc(uncharted, DIRECTION_NORTH));
				if (from == uncharted) arcs.push_back(arc(destination, DIRECTION_EAST));
			} else {
				if (from == start) {
					arcs.push_back(arc(hazard, DIRECTION_EAST, 0.8));
					arcs.push_back(arc(detour, DIRECTION_SOUTH));
				}
				if (from == hazard) arcs.push_back(arc(goalPos, DIRECTION_SOUTH));
				if (from == detour) arcs.push_back(arc(goalPos, DIRECTION_EAST));
			}
			if (limit) {
				for (auto it = arcs.begin(); it != arcs.end();) {
					if (!limit->contains(walkNode(it->step.expectedPosition))) it = arcs.erase(it);
					else ++it;
				}
			}
			return arcs;
		};
		while (!path.advance(2, expand, [&](Position p) { return p == destination; },
		                     [&](Position p) { return playerBotNavigationDistance(p, destination); })) {}
		return path;
	};
	const auto narrow = search(&corridor);
	assert(narrow.result == PlayerBotNavigationResult::Unreachable);
	corridor.widen(1, [&](uint32_t node) -> const auto& { return graph[node]; });
	const auto widened = search(&corridor);
	assert(widened.result == PlayerBotNavigationResult::Reached);
	assert(widened.steps.size() == 2 && widened.steps.front().expectedPosition == detour);
	assert(widened.summary.maximumHealthLossPerSecond == 0);
	corridor.widen(3, [&](uint32_t node) -> const auto& { return graph[node]; });
	assert(search(&corridor, true).result == PlayerBotNavigationResult::Unreachable);
	const auto unrestricted = search(nullptr, true);
	assert(unrestricted.result == PlayerBotNavigationResult::Reached && unrestricted.steps.size() == 2);
}
} // namespace

int main()
{
	topologyContracts();
	pathSearchContracts();
	std::cout << "playerbot route corridor contracts passed\n";
}
