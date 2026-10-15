#include "playerbottravelsafety.h"
#include "playerbotturnrouter.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <type_traits>

namespace {
using Memory = PlayerBotTravelSafetyMemory;
using Field = PlayerBotTravelDangerField;
using namespace std::chrono_literals;
const auto epoch = Memory::Clock::time_point{} + 1000s;
const Position tile{100, 200, 7};

void near(double actual, double expected) {
	assert(std::abs(actual - expected) < 1e-10);
}

void evidence() {
	Memory memory, other;
	static_assert(Memory::MaxThreats == 64 && Memory::MaxClearTiles == 512);
	static_assert(Memory::ThreatLifetime == 120s && Memory::ClearLifetime == 5s);
	static_assert(std::is_same_v<Memory::Clock, std::chrono::steady_clock>);
	assert(memory.revision() == 0);
	memory.expire(epoch);
	memory.observeEmpty(tile, epoch);
	assert(memory.revision() == 0);

	memory.observeThreat(1, tile, "Dragon", epoch);
	const auto firstRevision = memory.revision();
	assert(firstRevision > 0);
	memory.observeThreat(1, tile, "Dragon", epoch);
	assert(memory.revision() == firstRevision);
	const auto snapshot = memory.threats(epoch);
	assert(snapshot.size() == 1 && snapshot[0].id == 1 && snapshot[0].position == tile);
	assert(snapshot[0].species == "Dragon" && snapshot[0].seen == epoch);
	for (int i = 0; i < 100; ++i) assert(memory.threats(epoch + 1s).size() == 1);
	assert(memory.revision() == firstRevision); // Reads are not new sightings.
	assert(other.threats(epoch + 1s).empty()); // No singleton/shared per-bot state.

	memory.observeThreat(1, {101, 200, 7}, "Dragon Lord", epoch + 2s);
	assert(memory.threats(epoch + 2s).size() == 1); // ID moves, not duplicates.
	assert(memory.threats(epoch + 2s)[0].species == "Dragon Lord");
	assert(snapshot[0].position == tile && snapshot[0].species == "Dragon");
	const auto movedRevision = memory.revision();
	memory.observeThreat(1, tile, "old sighting", epoch + 1s);
	memory.observeEmpty({101, 200, 7}, epoch + 1s);
	memory.observeClear({101, 200, 7}, 1, epoch + 1s);
	assert(memory.revision() == movedRevision); // Delayed evidence cannot undo new evidence.

	memory.observeThreat(2, tile, "Rat", epoch + 2s);
	memory.observeThreat(3, {101, 200, 8}, "Rat", epoch + 2s);
	memory.observeEmpty({101, 200, 7}, epoch + 3s);
	auto threats = memory.threats(epoch + 3s);
	assert(threats.size() == 2 && threats[0].id == 2 && threats[1].id == 3);
	assert(!memory.clearAt({101, 200, 7}, 1, epoch + 3s)); // Empty is not automatically raid clearance.
	memory.observeEmpty({100, 201, 7}, epoch + 3s);
	assert(memory.threats(epoch + 3s).size() == 2);
	memory.observeThreat(1, {102, 200, 7}, "Dragon", epoch + 4s); // Absence was not death.
	assert(memory.threats(epoch + 4s).size() == 3);

	assert(memory.threats(epoch + 122s - 1ns).size() == 3);
	const auto beforeExpiry = memory.revision();
	assert(memory.threats(epoch + 122s).size() == 1); // Exact TTL boundary.
	assert(memory.revision() == beforeExpiry + 1);
	assert(memory.threats(epoch + 124s).empty());
	const auto expiredRevision = memory.revision();
	memory.expire(epoch + 200s);
	memory.observeThreat(1, tile, "delayed", epoch + 5s);
	assert(memory.revision() == expiredRevision && memory.threats(epoch + 200s).empty());
}

void clearance() {
	Memory memory;
	memory.observeClear(tile, 10, epoch);
	const auto revision = memory.revision();
	memory.observeClear(tile, 10, epoch);
	assert(memory.revision() == revision);
	const Memory& view = memory;
	assert(view.clearAt(tile, 10, epoch));
	assert(!view.clearAt(tile, 10, epoch - 1ns)); // Future evidence is not proof.
	assert(!view.clearAt({101, 200, 7}, 10, epoch));
	assert(!view.clearAt({100, 200, 8}, 10, epoch));
	assert(!view.clearAt(tile, 11, epoch)); // Raid activation invalidates old clearance.
	assert(view.clearPositions(11, epoch).empty());
	assert(view.clearPositions(10, epoch) == std::vector<Position>{tile});
	assert(view.clearAt(tile, 10, epoch + 5s - 1ns));
	assert(!view.clearAt(tile, 10, epoch + 5s));
	assert(view.clearPositions(10, epoch + 5s).empty());
	assert(memory.revision() == revision); // Const lookup filters; expire owns removal.
	memory.expire(epoch + 5s);
	assert(memory.revision() == revision + 1);
	memory.expire(epoch + 5s);
	assert(memory.revision() == revision + 1);

	memory.observeClear(tile, 11, epoch + 6s);
	memory.observeClear({101, 200, 7}, 11, epoch + 6s);
	memory.observeThreat(9, tile, "Dragon", epoch + 7s);
	assert(!memory.clearAt(tile, 11, epoch + 7s)); // Reacquisition revokes this tile only.
	assert(memory.clearAt({101, 200, 7}, 11, epoch + 7s));
	memory.observeClear(tile, 11, epoch + 8s);
	assert(!memory.clearAt(tile, 11, epoch + 8s)); // Contradictory clearance is rejected.
	memory.observeEmpty(tile, epoch + 9s);
	memory.observeClear(tile, 11, epoch + 9s);
	assert(memory.clearAt(tile, 11, epoch + 9s));
	memory.observeClear(tile, 12, epoch + 10s);
	assert(!memory.clearAt(tile, 11, epoch + 10s));
	assert(memory.clearAt(tile, 12, epoch + 10s));

	Memory simultaneousExpiry;
	simultaneousExpiry.observeThreat(1, tile, "Rat", epoch);
	simultaneousExpiry.observeClear({101, 200, 7}, 1, epoch);
	const auto before = simultaneousExpiry.revision();
	simultaneousExpiry.expire(epoch + 120s);
	assert(simultaneousExpiry.revision() == before + 1);
}

void refreshRevisions() {
	Memory threats;
	threats.observeThreat(1, tile, "Dragon", epoch);
	const auto initialRevision = threats.revision();
	const auto original = threats.threats(epoch);
	for (int seconds = 1; seconds <= 119; ++seconds) {
		const auto now = epoch + std::chrono::seconds(seconds);
		threats.observeThreat(1, tile, "Dragon", now);
		assert(threats.revision() == initialRevision);
		assert(threats.threats(now)[0].seen == now);
	}
	assert(original[0].seen == epoch); // A timestamp refresh cannot mutate snapshots.
	assert(threats.threats(epoch + 120s).size() == 1); // Refresh extended the TTL.
	assert(threats.revision() == initialRevision);
	threats.observeThreat(1, tile, "Dragon Lord", epoch + 121s);
	assert(threats.revision() == initialRevision + 1); // Species alone is semantic.
	threats.observeThreat(1, {101, 200, 7}, "Dragon Lord", epoch + 122s);
	assert(threats.revision() == initialRevision + 2); // Position alone is semantic.
	threats.observeThreat(1, {101, 200, 7}, "Dragon Lord", epoch + 123s);
	assert(threats.revision() == initialRevision + 2);
	threats.expire(epoch + 243s - 1ns);
	assert(threats.revision() == initialRevision + 2);
	threats.expire(epoch + 243s);
	assert(threats.revision() == initialRevision + 3);
	threats.observeThreat(1, {101, 200, 7}, "Dragon Lord", epoch + 243s);
	assert(threats.revision() == initialRevision + 4); // Reappearance after expiry is new evidence.

	Memory clears;
	clears.observeClear(tile, 7, epoch);
	const auto clearRevision = clears.revision();
	for (int seconds = 1; seconds <= 4; ++seconds) {
		clears.observeClear(tile, 7, epoch + std::chrono::seconds(seconds));
		assert(clears.revision() == clearRevision);
	}
	assert(clears.clearAt(tile, 7, epoch + 5s)); // Still fresh past the original TTL.
	clears.expire(epoch + 9s - 1ns);
	assert(clears.revision() == clearRevision);
	clears.expire(epoch + 9s);
	assert(clears.revision() == clearRevision + 1);
	assert(!clears.clearAt(tile, 7, epoch + 9s));
	clears.observeClear(tile, 7, epoch + 9s);
	assert(clears.revision() == clearRevision + 2);
	clears.observeClear(tile, 8, epoch + 10s);
	assert(clears.revision() == clearRevision + 3); // Raid revision alone is semantic.
	clears.observeClear(tile, 8, epoch + 11s);
	assert(clears.revision() == clearRevision + 3);
	assert(clears.clearAt(tile, 8, epoch + 15s));
}

void capacity() {
	Memory memory, isolated;
	for (uint32_t i = 0; i < Memory::MaxThreats; ++i) {
		memory.observeThreat(i, {uint16_t(i), 10, 7}, "Rat", epoch);
	}
	const auto beforeThreatRefresh = memory.revision();
	memory.observeThreat(0, {0, 10, 7}, "Rat", epoch + 1s); // Refresh oldest before overflowing.
	assert(memory.revision() == beforeThreatRefresh);
	memory.observeThreat(64, {64, 10, 7}, "Rat", epoch + 2s);
	assert(memory.revision() == beforeThreatRefresh + 1);
	auto snapshot = memory.threats(epoch + 2s);
	assert(snapshot.size() == Memory::MaxThreats);
	assert(std::none_of(snapshot.begin(), snapshot.end(), [](const auto& threat) { return threat.id == 1; }));
	assert(std::any_of(snapshot.begin(), snapshot.end(), [](const auto& threat) { return threat.id == 0; }));
	for (uint32_t i = 65; i < 1000; ++i) memory.observeThreat(i, {uint16_t(i), 10, 7}, "Rat", epoch + 2s);
	assert(memory.threats(epoch + 2s).size() == Memory::MaxThreats);
	assert(isolated.threats(epoch + 2s).empty());

	for (uint16_t i = 0; i < Memory::MaxClearTiles; ++i) memory.observeClear({i, 20, 7}, 3, epoch + 2s);
	const auto beforeClearRefresh = memory.revision();
	memory.observeClear({0, 20, 7}, 3, epoch + 3s);
	assert(memory.revision() == beforeClearRefresh);
	memory.observeClear({512, 20, 7}, 3, epoch + 4s);
	assert(memory.revision() == beforeClearRefresh + 1);
	assert(memory.clearPositions(3, epoch + 4s).size() == Memory::MaxClearTiles);
	assert(memory.clearAt({0, 20, 7}, 3, epoch + 4s));
	assert(!memory.clearAt({1, 20, 7}, 3, epoch + 4s));
	for (uint16_t i = 513; i < 2000; ++i) memory.observeClear({i, 20, 7}, 3, epoch + 4s);
	assert(memory.clearPositions(3, epoch + 4s).size() == Memory::MaxClearTiles);
	assert(isolated.clearPositions(3, epoch + 4s).empty());
}

void fieldSamples() {
	near(Field({}, {}, {}).dangerAt(tile), 0);
	const std::vector<Field::Rectangle> rectangles{
		{{110, 210, 8}, {90, 190, 7}, 0.2}, // Reversed inclusive bounds.
		{{100, 200, 7}, {110, 210, 7}, 0.4},
	};
	const Field footprints(rectangles, {}, {});
	near(footprints.dangerAt(tile), 0.4); // Overlap is maximum, not sum.
	near(footprints.dangerAt({90, 190, 7}), 0.2);
	near(footprints.dangerAt({110, 210, 8}), 0.2);
	near(footprints.dangerAt({89, 190, 7}), 0);
	near(footprints.dangerAt({110, 211, 8}), 0);
	near(footprints.dangerAt({100, 200, 6}), 0);
	near(footprints.dangerAt({100, 200, 9}), 0);

	const Field local({}, {{tile, 0.5}}, {});
	near(local.dangerAt(tile), 0.5);
	near(local.dangerAt({101, 201, 7}), 0.5); // Chebyshev distance <=1: full strength.
	near(local.dangerAt({102, 200, 7}), 0.4);
	near(local.dangerAt({103, 203, 7}), 0.3);
	near(local.dangerAt({105, 200, 7}), 0.1);
	near(local.dangerAt({106, 200, 7}), 0);
	near(local.dangerAt({107, 200, 7}), 0);
	near(local.dangerAt({100, 200, 8}), 0);
	near(local.dangerAt({100, 200, 6}), 0);

	// Rank contributions after falloff, not raw danger; count distinct co-located
	// threats separately, but each input entry only once across bucket boundaries.
	const Field ranked(rectangles, {{tile, 0.1}, {tile, 0.2}, {tile, 0.3}, {{105, 200, 7}, 1.0}}, {});
	near(ranked.dangerAt(tile), 0.4 + 0.3 + 0.2 + 0.2);
	const Field boundary({}, {{{16, 16, 7}, 0.5}}, {});
	near(boundary.dangerAt({15, 15, 7}), 0.5);
	near(boundary.dangerAt({17, 17, 7}), 0.5);
	const Field edges({}, {{{0, 0, 0}, 1}, {{65535, 65535, 255}, 2}}, {});
	near(edges.dangerAt({0, 0, 0}), 1);
	near(edges.dangerAt({1, 1, 0}), 1);
	near(edges.dangerAt({65535, 65535, 255}), 2);
	near(edges.dangerAt({65534, 65534, 255}), 2);
	near(edges.dangerAt({0, 0, 255}), 0); // No wraparound at map edges.

	const Field cleared(rectangles, {{tile, 0.3}}, {tile});
	near(cleared.dangerAt(tile), 0.3); // Clearance never suppresses the local component.
	near(cleared.dangerAt({101, 200, 7}), 0.7); // Does not clear the whole footprint.
	near(cleared.dangerAt({100, 200, 8}), 0.2);
	near(std::max(cleared.dangerAt(tile), 0.8), 0.8); // Caller combines ordinary static danger via max.

	Memory memory;
	memory.observeClear(tile, 7, epoch);
	const Field before(rectangles, {}, memory.clearPositions(7, epoch));
	near(before.dangerAt(tile), 0);
	near(Field(rectangles, {}, memory.clearPositions(8, epoch)).dangerAt(tile), 0.4);
	near(Field(rectangles, {}, memory.clearPositions(7, epoch + 5s)).dangerAt(tile), 0.4);
	memory.observeThreat(1, tile, "Dragon", epoch + 1s);
	const Field acquired(rectangles, {{tile, 0.3}}, memory.clearPositions(7, epoch + 1s));
	near(acquired.dangerAt(tile), 0.7);
	near(before.dangerAt(tile), 0); // Snapshot has no hidden live memory/world access.
	assert(memory.threats(epoch + 121s).empty());
	near(Field(rectangles, {}, memory.clearPositions(7, epoch + 121s)).dangerAt(tile), 0.4); // Raid persists.

	const double nan = std::numeric_limits<double>::quiet_NaN();
	near(Field({{tile, tile, -1}, {tile, tile, nan}}, {{tile, -1}, {tile, nan}}, {}).dangerAt(tile), 0);
	const Field infinite({}, {{tile, std::numeric_limits<double>::infinity()}}, {});
	assert(std::isinf(infinite.dangerAt(tile)));
	near(infinite.dangerAt({106, 200, 7}), 0); // No infinity * zero NaN at radius boundary.
}

void withdrawal() {
	using Candidate = PlayerBotTravelWithdrawalCandidate;
	const Position north{100, 199, 7}, east{101, 200, 7}, south{100, 201, 7}, west{99, 200, 7};
	static_assert(PlayerBotTravelMaxWithdrawalNeighbours == 8);
	const double epsilon = PlayerBotTravelWithdrawalEpsilon;
	assert(!playerBotChooseTravelWithdrawal(1, {}));
	assert(!playerBotChooseTravelWithdrawal(1, {{north, 1}, {east, 2}, {south, 1, true}}));
	assert(!playerBotChooseTravelWithdrawal(0, {{north, 0, true}}));
	assert(!playerBotChooseTravelWithdrawal(1, {{north, 1 - epsilon / 2}}));
	assert(!playerBotChooseTravelWithdrawal(2 * epsilon, {{north, epsilon}}));
	assert(playerBotChooseTravelWithdrawal(3 * epsilon, {{north, epsilon}}) == north);
	assert(playerBotChooseTravelWithdrawal(1, {{north, 0.8}, {east, 0.2}, {south, 0.5}}) == east);
	assert(playerBotChooseTravelWithdrawal(1, {{north, 0.1}, {east, 0.8, true}, {south, 0.5, true}}) == south);
	assert(playerBotChooseTravelWithdrawal(1, {{north, 0.8, true}, {east, 0.1}}) == north);
	assert(playerBotChooseTravelWithdrawal(1, {{north, 0.2}, {east, 1, true}}) == north); // Refuge cannot excuse danger.
	assert(playerBotChooseTravelWithdrawal(1, {{north, 0.2}, {east, 0.2}}) == north); // Stable caller tie-break.
	assert(playerBotChooseTravelWithdrawal(1, {{east, 0.2}, {north, 0.2}}) == east);
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double infinity = std::numeric_limits<double>::infinity();
	assert(!playerBotChooseTravelWithdrawal(nan, {{north, 0}}));
	assert(!playerBotChooseTravelWithdrawal(1, {{north, nan}, {east, -1}, {south, infinity}}));
	assert(playerBotChooseTravelWithdrawal(infinity, {{north, infinity}, {east, 1}}) == east);

	std::vector<Candidate> bounded(8, {north, 1});
	bounded.push_back({east, 0, true});
	assert(!playerBotChooseTravelWithdrawal(1, bounded)); // A ninth entry is not examined.
	bounded[7] = {west, 0.3};
	assert(playerBotChooseTravelWithdrawal(1, bounded) == west);

	const Field local({}, {{tile, 0.5}}, {});
	std::vector<Candidate> plateau;
	for (int y = 199; y <= 201; ++y) {
		for (int x = 99; x <= 101; ++x) {
			const Position p{uint16_t(x), uint16_t(y), 7};
			if (p != tile) plateau.push_back({p, local.dangerAt(p)});
		}
	}
	assert(!playerBotChooseTravelWithdrawal(local.dangerAt(tile), plateau)); // Equal danger stays caller-owned.
	assert(playerBotChooseTravelWithdrawal(local.dangerAt(east),
	                                      {{{102, 200, 7}, local.dangerAt({102, 200, 7})}}) == Position(102, 200, 7));
}

// Independent full-scan oracle checks indexed sampling, including huge rectangles.
// Production sampling must not rasterize the world or scan all local threats.
double reference(const std::vector<Field::Rectangle>& rectangles, const std::vector<Field::LocalThreat>& threats,
                 const std::vector<Position>& clears, Position p) {
	double result = 0;
	if (std::find(clears.begin(), clears.end(), p) == clears.end()) {
		for (const auto& r : rectangles) {
			if (p.x >= std::min(r.from.x, r.to.x) && p.x <= std::max(r.from.x, r.to.x) &&
			    p.y >= std::min(r.from.y, r.to.y) && p.y <= std::max(r.from.y, r.to.y) &&
			    p.z >= std::min(r.from.z, r.to.z) && p.z <= std::max(r.from.z, r.to.z)) result = std::max(result, r.danger);
		}
	}
	std::vector<double> contributions;
	for (const auto& threat : threats) {
		if (p.z != threat.position.z) continue;
		const auto d = std::max(std::abs(int(p.x) - threat.position.x), std::abs(int(p.y) - threat.position.y));
		if (d < 6) contributions.push_back(threat.danger * (d <= 1 ? 1 : (6 - d) / 5.0));
	}
	std::sort(contributions.begin(), contributions.end(), std::greater<double>());
	for (size_t i = 0; i < std::min(size_t(3), contributions.size()); ++i) result += contributions[i];
	return result;
}

void indexedOracle() {
	std::mt19937 random(267);
	const auto coordinate = [&]() { return uint16_t(random() % 128); };
	const auto position = [&]() { return Position{coordinate(), coordinate(), uint8_t(random() % 3 + 6)}; };
	std::vector<Field::Rectangle> rectangles{{{0, 0, 0}, {65535, 65535, 255}, 0.03}};
	std::vector<Field::LocalThreat> threats;
	std::vector<Position> clears;
	for (int i = 0; i < 200; ++i) {
		const auto p = position();
		rectangles.push_back({p, {uint16_t(p.x + 4), uint16_t(p.y + 4), p.z}, (random() % 100) / 100.0});
		threats.push_back({position(), (random() % 100) / 100.0});
		clears.push_back(position());
	}
	const Field field(rectangles, threats, clears);
	for (int z = 6; z <= 8; ++z) {
		for (int y = 0; y < 132; ++y) {
			for (int x = 0; x < 132; ++x) {
				const Position p{uint16_t(x), uint16_t(y), uint8_t(z)};
				near(field.dangerAt(p), reference(rectangles, threats, clears, p));
			}
		}
	}
	near(field.dangerAt({65535, 65535, 255}), 0.03);
	// Capacity overflow may overestimate by merging the tail but must not forget
	// any warning, including on another floor or far outside the small buckets.
	for (int i = 0; i < 1000; ++i) {
		const Position p{uint16_t(i), uint16_t(i), uint8_t(i % 16)};
		rectangles.push_back({p, p, 0.9});
	}
	const Field overflow(rectangles, {}, {});
	for (const auto& rectangle : rectangles) assert(overflow.dangerAt(rectangle.from) >= rectangle.danger);
}
}

int main() {
	for (const auto phase : {PlayerBotCyclePhase::Idle, PlayerBotCyclePhase::Service, PlayerBotCyclePhase::ReturnToDepot,
	     PlayerBotCyclePhase::DepositLoot, PlayerBotCyclePhase::Hunt}) {
		for (const auto stage : {PlayerBotScenarioStage::Traverse, PlayerBotScenarioStage::LootCorpse, PlayerBotScenarioStage::TraversalCombat}) {
			PlayerBotTurnRouter router;
			router.setCyclePhase(phase);
			router.setScenarioStage(stage);
			PlayerBotTurnObservation turn;
			turn.travelSafetyRequired = turn.progressionActive = turn.trainerDiscoveryActive = turn.preparationActive = true;
			turn.magicTrainingActive = turn.huntRegionSelectionRequired = turn.huntPlanningActive = true;
			assert(router.route(turn) == PlayerBotTurnCommand::TravelSafety);
			router.pause();
			assert(router.route(turn) == PlayerBotTurnCommand::None);
			router.stop();
			assert(router.route(turn) == PlayerBotTurnCommand::None);
		}
	}
	evidence();
	clearance();
	refreshRevisions();
	capacity();
	fieldSamples();
	withdrawal();
	indexedOracle();
	std::cout << "playerbot travel safety contracts passed\n";
}
