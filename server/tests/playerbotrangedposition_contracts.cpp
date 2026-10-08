// Standalone pure-contract regression: see playerbotrangedposition_contracts.sh.
#include <cassert>
#include <cstring>
#include <iostream>

#include "playerbotrangedposition.h"
#include "playerbotnavigationsession.h"

namespace {
using Action = PlayerBotRangedAction;

const Position target(100, 100, 7);

// Open room, bot west of the target, neighbours listed cardinal first.
PlayerBotRangedPositionInput room(int32_t distance, uint8_t range = 3)
{
	PlayerBotRangedPositionInput input;
	input.self = Position(100 - distance, 100, 7);
	input.target = target;
	input.range = range;
	input.selfStepMs = input.targetStepMs = 650;
	input.sightClear = true;
	for (const auto& [dx, dy] : {std::pair{0, -1}, {1, 0}, {0, 1}, {-1, 0}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}}) {
		PlayerBotRangedTile tile;
		tile.position = Position(input.self.x + dx, input.self.y + dy, 7);
		tile.open = true;
		tile.sightClear = true;
		input.neighbours.push_back(tile);
	}
	return input;
}

PlayerBotRangedTile& tileAt(PlayerBotRangedPositionInput& input, int32_t dx, int32_t dy)
{
	for (auto& tile : input.neighbours) {
		if (tile.position == Position(input.self.x + dx, input.self.y + dy, 7)) return tile;
	}
	assert(false);
	return input.neighbours.front();
}

void holdsInsideTheBand()
{
	assert(playerBotRangedPosition(room(3)).action == Action::Hold);
	assert(playerBotRangedPosition(room(3, 6)).action == Action::Retreat);
	assert(playerBotRangedPosition(room(6, 6)).action == Action::Hold);
	assert(playerBotRangedPosition(room(1, 1)).action == Action::Hold);
}

void closesWhenOutOfRangeOrBlocked()
{
	auto far = playerBotRangedPosition(room(4));
	assert(far.action == Action::Close && !std::strcmp(far.reason, "out_of_range"));
	auto blind = room(3);
	blind.sightClear = false;
	auto decision = playerBotRangedPosition(blind);
	assert(decision.action == Action::Close && !std::strcmp(decision.reason, "no_line_of_sight"));
}

void retreatsAwayFromTheTarget()
{
	const auto decision = playerBotRangedPosition(room(1));
	assert(decision.action == Action::Retreat && !std::strcmp(decision.reason, "target_too_close"));
	// Farthest tile within range wins; a cardinal tile beats a diagonal one of equal distance.
	assert(decision.tile == Position(98, 100, 7));
	const auto spear = playerBotRangedPosition(room(2));
	assert(spear.action == Action::Retreat && playerBotChebyshev(spear.tile, target) == 3);
}

void neverLeavesTheBandWhileRetreating()
{
	// Bow range 6: one step at a time, never beyond the weapon range.
	const auto decision = playerBotRangedPosition(room(6, 6));
	assert(decision.action == Action::Hold);
	const auto step = playerBotRangedPosition(room(5, 6));
	assert(step.action == Action::Retreat && playerBotChebyshev(step.tile, target) == 6);
}

void stepDurationGate()
{
	auto input = room(1);
	input.targetStepMs = 600;
	auto decision = playerBotRangedPosition(input);
	assert(decision.action == Action::FightInPlace && !std::strcmp(decision.reason, "target_faster"));
	input.targetStepMs = 650;
	assert(playerBotRangedPosition(input).action == Action::Retreat);
	input.targetStepMs = 900;
	assert(playerBotRangedPosition(input).action == Action::Retreat);
}

void fallsBackWithAnExplicitReason()
{
	auto cornered = room(1);
	for (auto [dx, dy] : {std::pair{-1, 0}, {-1, -1}, {-1, 1}}) tileAt(cornered, dx, dy).open = false;
	auto decision = playerBotRangedPosition(cornered);
	assert(decision.action == Action::FightInPlace && !std::strcmp(decision.reason, "retreat_blocked"));

	auto hostile = room(1);
	tileAt(hostile, -1, 0).hostileNearby = true;
	tileAt(hostile, -1, -1).hostileNearby = true;
	tileAt(hostile, -1, 1).hazardous = true;
	assert(!std::strcmp(playerBotRangedPosition(hostile).reason, "retreat_hostile"));

	auto hazard = room(1);
	for (auto [dx, dy] : {std::pair{-1, 0}, {-1, -1}, {-1, 1}}) tileAt(hazard, dx, dy).hazardous = true;
	assert(!std::strcmp(playerBotRangedPosition(hazard).reason, "retreat_hazard"));

	auto outside = room(1);
	for (auto [dx, dy] : {std::pair{-1, 0}, {-1, -1}, {-1, 1}}) tileAt(outside, dx, dy).insideArea = false;
	assert(!std::strcmp(playerBotRangedPosition(outside).reason, "retreat_outside_region"));
}

void skipsUnusableTilesButKeepsLookingForOne()
{
	auto input = room(1);
	tileAt(input, -1, 0).hostileNearby = true;
	// The straight-back tile is vetoed; a diagonal one is still a valid retreat.
	const auto decision = playerBotRangedPosition(input);
	assert(decision.action == Action::Retreat && decision.tile != Position(98, 100, 7));
	assert(playerBotChebyshev(decision.tile, target) == 2);
}

void retreatMovementMustBeObserved()
{
	const auto now = std::chrono::steady_clock::time_point{};
	const auto timeout = std::chrono::seconds(2);
	const auto suppression = std::chrono::seconds(10);
	auto input = room(1);
	const auto retreat = playerBotRangedPosition(input);
	PlayerBotNavigationStep step;
	step.target = step.expectedPosition = retreat.tile;
	PlayerBotNavigationSession movement;
	movement.beginMovement(step, now);
	assert(movement.observeMovement(input.self, true, now, timeout, suppression) == PlayerBotPendingMovementResult::Waiting);
	// Dispatch is not arrival; even a permanently pending action must expire.
	assert(movement.observeMovement(input.self, true, now + timeout, timeout, suppression) == PlayerBotPendingMovementResult::Mismatch);
	const auto blocked = movement.activeBlockedPositions(now + timeout);
	assert(blocked.count(retreat.tile));
	for (auto& tile : input.neighbours) if (blocked.count(tile.position)) tile.open = false;
	const auto alternative = playerBotRangedPosition(input);
	assert(alternative.action == Action::Retreat && alternative.tile != retreat.tile);
	step.target = step.expectedPosition = alternative.tile;
	movement.beginMovement(step, now + timeout);
	assert(movement.observeMovement(alternative.tile, true, now + timeout, timeout, suppression) == PlayerBotPendingMovementResult::Completed);
	assert(!movement.pendingMoveTarget());
	assert(movement.activeBlockedPositions(now + timeout + suppression).empty());

	// An approach abandoned for local positioning must not poison the next
	// closing route with its obsolete expected landing.
	PlayerBotNavigationSession approach;
	step.target = step.expectedPosition = Position(100, 100, 7);
	approach.beginMovement(step, now);
	approach.clear();
	assert(approach.observeMovement(alternative.tile, false, now, timeout, suppression) == PlayerBotPendingMovementResult::None);
	assert(approach.activeBlockedPositions(now).empty());
	movement.beginMovement(step, now);
	movement.clear();
	movement.clearBlockedPositions();
	assert(!movement.hasPendingWork());
}

void meleeStyleNeverMoves()
{
	auto input = room(1, 1);
	const auto decision = playerBotRangedPosition(input);
	assert(decision.action == Action::Hold && !std::strcmp(decision.reason, "melee_style"));
	input.range = 0;
	assert(!std::strcmp(playerBotRangedPosition(input).reason, "melee_style"));
}
} // namespace

int main()
{
	holdsInsideTheBand();
	closesWhenOutOfRangeOrBlocked();
	retreatsAwayFromTheTarget();
	neverLeavesTheBandWhileRetreating();
	stepDurationGate();
	fallsBackWithAnExplicitReason();
	skipsUnusableTilesButKeepsLookingForOne();
	meleeStyleNeverMoves();
	retreatMovementMustBeObserved();
	std::cout << "playerbot ranged position contracts passed\n";
}
