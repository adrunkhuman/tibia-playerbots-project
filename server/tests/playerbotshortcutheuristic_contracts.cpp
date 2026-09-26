#include "playerbotshortcutheuristic.h"
#include "playerbotroutechanges.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>

namespace {
struct Arc { Position from, to; uint32_t cost; };

void compareOracle(const std::vector<Arc>& arcs, const std::vector<Position>& goals,
                   const PlayerBotShortcutHeuristic& heuristic)
{
	std::map<Position, std::vector<std::pair<Position, uint32_t>>> incoming;
	std::map<Position, uint32_t> costs;
	for (const Arc& arc : arcs) incoming[arc.to].push_back({arc.from, arc.cost});
	using Entry = std::pair<uint32_t, Position>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
	for (Position goal : goals) { costs[goal] = 0; open.emplace(0, goal); }
	while (!open.empty()) {
		const auto [cost, current] = open.top();
		open.pop();
		if (costs[current] != cost) continue;
		for (const auto& [source, weight] : incoming[current]) {
			auto it = costs.find(source);
			if (it != costs.end() && it->second <= cost + weight) continue;
			costs[source] = cost + weight;
			open.emplace(cost + weight, source);
		}
	}
	for (const auto& [position, cost] : costs) assert(heuristic.estimate(position) <= cost);
	for (const Arc& arc : arcs) {
		assert(heuristic.estimate(arc.from) <= arc.cost + heuristic.estimate(arc.to));
	}
	for (Position goal : goals) assert(heuristic.estimate(goal) == 0);
}

std::vector<Arc> walkingGrid(const std::set<Position>& blocked = {})
{
	std::vector<Arc> arcs;
	for (int z = 7; z <= 8; ++z) for (int y = 10; y < 40; ++y) for (int x = 10; x < 40; ++x) {
		Position p(x, y, z);
		if (blocked.count(p)) continue;
		for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
			if ((!dx && !dy) || x + dx < 10 || x + dx >= 40 || y + dy < 10 || y + dy >= 40) continue;
			Position q(x + dx, y + dy, z);
			if (!blocked.count(q)) arcs.push_back({p, q, dx && dy ? 30u : 10u});
		}
	}
	return arcs;
}

void directedAndDetour()
{
	const Position start(12, 12, 7), entry(11, 12, 7), exit(35, 35, 8), goal(36, 35, 8);
	std::vector<PlayerBotShortcut> shortcuts{{entry, exit}};
	std::set<Position> wall;
	for (int y = 10; y < 39; ++y) wall.insert(Position(25, y, 7));
	auto arcs = walkingGrid(wall);
	arcs.push_back({start, exit, 10}); // Move away from the goal into the directed teleport.
	PlayerBotShortcutHeuristic h({goal}, shortcuts, Position(0, 0, 0), Position(63, 63, 0));
	compareOracle(arcs, {goal}, h);
	assert(h.estimate(start) == 20);
	PlayerBotShortcutHeuristic reversed({start}, shortcuts, Position(0, 0, 0), Position(63, 63, 0));
	assert(reversed.estimate(exit) > 20); // No invented reverse teleport.
	compareOracle(arcs, {start}, reversed);

	const std::vector<Position> goals{goal, Position(12, 18, 7)};
	PlayerBotShortcutHeuristic any(goals, shortcuts, Position(0, 0, 0), Position(63, 63, 0));
	compareOracle(arcs, goals, any);
	assert(any.estimate(Position(12, 17, 7)) == 10);
	PlayerBotShortcutHeuristic noShortcut({goal}, {}, Position(0, 0, 0), Position(63, 63, 0));
	assert(noShortcut.estimate(start) > h.estimate(start));
}

void transitions()
{
	auto arcs = walkingGrid();
	// Every possible upstairs preference can differ from the approach by two
	// tiles on either axis. Missing ground, new ladders and rope tools need no
	// extra index entries: these transitions are universally relaxed.
	for (int dx = -2; dx <= 2; ++dx) for (int dy = -2; dy <= 2; ++dy) {
		arcs.push_back({Position(20, 20, 8), Position(20 + dx, 20 + dy, 7), 20});
	}
	arcs.push_back({Position(22, 22, 7), Position(23, 23, 8), 20}); // Shovel/down-use.
	arcs.push_back({Position(22, 22, 7), Position(23, 22, 8), 10}); // Height/drop.
	std::vector<PlayerBotShortcut> shortcuts;
	// All combinations of lower tile direction flags, including alternate
	// double shifts. Changed/previously missing exit geometry is already covered.
	for (int mask = 0; mask < 64; ++mask) {
		const int dx = -(!!(mask & 1)) - 2 * (!!(mask & 2)) + (!!(mask & 4));
		const int dy = -(!!(mask & 8)) - 2 * (!!(mask & 16)) + (!!(mask & 32));
		const Position source(30, 30, 7), target(30 + dx, 30 + dy, 8);
		auto down = playerBotFloorShortcuts(source, true, 0, 0);
		assert(std::find(down.begin(), down.end(), PlayerBotShortcut{source, target}) != down.end());
		shortcuts.insert(shortcuts.end(), down.begin(), down.end());
		arcs.push_back({Position(29, 30, 7), target, 10});
	}
	const auto up = playerBotFloorShortcuts(Position(16, 16, 8), false, 3, 3);
	shortcuts.insert(shortcuts.end(), up.begin(), up.end());
	arcs.push_back({Position(15, 16, 8), Position(19, 19, 7), 10});
	// A single movement cost can cover a whole redirected chain.
	shortcuts.push_back({Position(19, 19, 7), Position(21, 21, 6)});
	arcs.push_back({Position(15, 16, 8), Position(21, 21, 6), 10});
	const std::vector<Position> goals{Position(34, 34, 8), Position(21, 21, 6)};
	PlayerBotShortcutHeuristic h(goals, shortcuts, Position(0, 0, 0), Position(63, 63, 0));
	compareOracle(arcs, goals, h);
}

PlayerBotShortcutIndex* observedIndex = nullptr;
void notify(Position p, bool all) {
	if (all) observedIndex->invalidate();
	else observedIndex->changed(p);
}

void mutations()
{
	PlayerBotShortcutIndex index;
	observedIndex = &index;
	PlayerBotRouteChanges::setGeometryObserver(notify);
	std::map<Position, std::vector<PlayerBotShortcut>> world;
	const Position source(20, 20, 7), destination(80, 80, 8), goal(81, 80, 8);
	world[source] = {{source, destination}};
	size_t scans = 0, descriptions = 0;
	auto refresh = [&] {
		index.refresh([&](auto visit) {
			++scans;
			for (const auto& entry : world) visit(entry.first);
		}, [&](Position p) {
			++descriptions;
			auto it = world.find(p);
			return it == world.end() ? std::vector<PlayerBotShortcut>{} : it->second;
		});
	};
	refresh();
	const auto original = index.revision();
	PlayerBotShortcutHeuristic before({goal}, index.shortcuts(), Position(0, 0, 0), Position(127, 127, 0));
	for (int i = 0; i < 6000; ++i) {
		// More notifications than the route journal retains, including an
		// ordinary update at a shortcut source. They must not disable guidance.
		PlayerBotRouteChanges::changed(Position(i % 100, i / 100, 7));
	}
	PlayerBotRouteChanges::changed(source);
	refresh();
	assert(index.revision() == original && scans == 1);
	assert(before.estimate(Position(19, 20, 7)) == 20);
	const auto reads = descriptions;
	refresh();
	assert(descriptions == reads && scans == 1);

	// Existing teleport retargeted, then one added at a previously absent tile.
	PlayerBotRouteChanges::changed(source, PlayerBotRouteChanges::Cause::Teleport);
	world[source] = {{source, goal}};
	refresh();
	assert(index.revision() != original && scans == 1);
	PlayerBotShortcutHeuristic changed({goal}, index.shortcuts(), Position(0, 0, 0), Position(127, 127, 0));
	assert(changed.estimate(Position(19, 20, 7)) == 10);
	const auto retargeted = index.revision();
	const Position newSource(60, 60, 7);
	PlayerBotRouteChanges::changed(newSource, PlayerBotRouteChanges::Cause::TileReplace);
	world[newSource] = {{newSource, goal}};
	refresh();
	assert(index.revision() != retargeted && scans == 1);
	PlayerBotShortcutHeuristic added({goal}, index.shortcuts(), Position(0, 0, 0), Position(127, 127, 0));
	assert(added.estimate(Position(59, 60, 7)) == 10);
	assert(changed.estimate(Position(59, 60, 7)) > 10); // Old potentials really are unsafe here.
	compareOracle({{Position(59, 60, 7), goal, 10}}, {goal}, added);

	PlayerBotRouteChanges::changed(source);
	world.erase(source);
	refresh();
	assert(index.shortcuts().size() == 1);
	const auto removed = index.revision();
	PlayerBotRouteChanges::invalidate(); // Ordinary bulk/permission epoch: same shortcuts.
	refresh();
	assert(scans == 2 && index.revision() == removed);
	for (uint32_t i = 0; i <= PlayerBotShortcutIndex::maximumDirtyTiles; ++i) {
		PlayerBotRouteChanges::changed(Position(uint16_t(i), uint16_t(i >> 16), 7));
	}
	world[newSource] = {{newSource, destination}};
	refresh(); // Dirty-set overflow never silently loses the update.
	assert(scans == 3 && index.revision() != removed);
	PlayerBotRouteChanges::setGeometryObserver(nullptr);
	observedIndex = nullptr;

	PlayerBotRouteChanges::Watch watch;
	{
		PlayerBotRouteChanges::Scope scope(watch);
		{
			PlayerBotRouteChanges::IgnoreReads ignore;
			PlayerBotRouteChanges::read(source);
		}
		PlayerBotRouteChanges::read(destination);
	}
	assert(watch.tiles.size() == 1);
}

void randomOracles()
{
	std::mt19937 random(42);
	for (int trial = 0; trial < 30; ++trial) {
		std::set<Position> blocked;
		for (int i = 0; i < 200; ++i) blocked.insert(Position(10 + random() % 30, 10 + random() % 30, 7));
		auto arcs = walkingGrid(blocked);
		std::vector<PlayerBotShortcut> shortcuts;
		for (int i = 0; i < 20; ++i) {
			Position entry(11 + random() % 28, 11 + random() % 28, 7 + random() % 2);
			Position exit(11 + random() % 28, 11 + random() % 28, 7 + random() % 2);
			shortcuts.push_back({entry, exit});
			arcs.push_back({Position(entry.x - 1, entry.y, entry.z), exit, 10});
		}
		std::vector<Position> goals{Position(15, 15, 7), Position(35, 35, 8)};
		PlayerBotShortcutHeuristic h(goals, shortcuts, Position(0, 0, 0), Position(63, 63, 0));
		compareOracle(arcs, goals, h);
	}
}

void boundsAndBudget()
{
	PlayerBotShortcutHeuristic empty({}, {}, Position(), Position());
	assert(empty.estimate(Position(200, 200, 7)) == 0);
	const auto start = std::chrono::steady_clock::now();
	PlayerBotShortcutHeuristic large({Position(32768, 32768, 7)}, {}, Position(0, 0, 0), Position(65535, 65535, 0));
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	assert(large.cellCount() <= PlayerBotShortcutHeuristic::maximumCells);
	assert(large.resolution() == 64);
	assert(large.estimate(Position(32000, 32000, 8)) > 0);
	std::vector<Arc> arcs;
	for (uint16_t x : {uint16_t(0), uint16_t(63), uint16_t(64), uint16_t(65535)}) {
		arcs.push_back({Position(x, x, 7), Position(uint16_t(x + 1), x, 7), 10});
		arcs.push_back({Position(x, x, 8), Position(uint16_t(x + 2), uint16_t(x + 2), 7), 20});
	}
	compareOracle(arcs, {Position(32768, 32768, 7)}, large);
	PlayerBotShortcutHeuristic clipped({Position(20, 20, 7)}, {}, Position(10, 10, 0), Position(30, 30, 0));
	compareOracle(arcs, {Position(20, 20, 7)}, clipped);
	std::cout << "bounded grid: " << large.cellCount() << " cells, " << ms << " ms\n";
}
}

int main()
{
	directedAndDetour();
	transitions();
	mutations();
	randomOracles();
	boundsAndBudget();
	std::cout << "playerbot shortcut heuristic contracts passed\n";
}
