// Standalone pure-contract regression: see playerbotapproach_contracts.sh.
#include <cassert>
#include <cstring>
#include <iostream>

#include "playerbotapproach.h"

namespace {
using Approach = PlayerBotApproach;
using Verdict = PlayerBotApproachVerdict;
using Limits = PlayerBotApproachLimits;

const Position north(100, 99, 7), east(101, 100, 7), south(100, 101, 7), west(99, 100, 7), far(130, 100, 7);

void occupiedTilesAreLastResort()
{
	Approach approach;
	approach.offer({{north, 1, true}, {east, 5, false}, {south, 3, false}});
	// Equal tiles keep the caller's order.
	Approach ranked;
	ranked.offer({{east, 1}, {north, 1}});
	assert(ranked.next() == east);
	assert(approach.next() == south);
	approach.reject(*approach.next(), "route_unavailable");
	assert(approach.next() == east);
	approach.reject(east, "route_unavailable");
	// Only the occupied tile is left; it stays eligible.
	assert(approach.next() == north);
}

void partlyUnreachableTiles()
{
	// A depot locker or NPC with reachable and unreachable tiles: rejections
	// move on to the next tile until the bound or the tiles run out.
	Approach approach;
	approach.offer({{north, 1}, {east, 2}, {south, 3}});
	assert(approach.reject(north, "route_unavailable") == Verdict::Continue);
	assert(approach.next() == east);
	approach.select(east);
	assert(approach.selected() == east);
	assert(approach.reject(east, "navigation_stalled") == Verdict::Continue && !approach.selected());
	assert(approach.reject(south, "route_unavailable") == Verdict::Exhausted);
	assert(std::strcmp(approach.reason(), "route_unavailable") == 0);
	assert(!approach.next());

	// The rejection bound applies even while tiles remain.
	Approach crowded;
	crowded.offer({{north, 1}, {east, 2}, {south, 3}, {west, 4}, {far, 5}});
	for (const Position& tile : {north, east, south}) assert(crowded.reject(tile, "approach_occupied") == Verdict::Continue);
	assert(crowded.reject(west, "approach_occupied") == Verdict::Exhausted);
	assert(!crowded.next());
	// Repeating a rejection does not spend the bound twice.
	Approach repeated;
	repeated.offer({{north, 1}, {east, 2}});
	assert(repeated.reject(north, "route_unavailable") == Verdict::Continue);
	assert(repeated.reject(north, "route_unavailable") == Verdict::Continue);

	// A new offer keeps earlier rejections.
	Approach refreshed;
	refreshed.offer({{north, 1}, {east, 2}});
	refreshed.reject(north, "route_unavailable");
	refreshed.offer({{north, 0}, {south, 9}});
	assert(refreshed.next() == south);
}

void creatureOnThePath()
{
	Approach approach;
	approach.offer({{north, 1}});
	for (uint32_t wait = 0; wait < Limits::blockedWaits; ++wait) assert(approach.blocked() == Verdict::Wait);
	approach.unblocked();
	assert(approach.blocked() == Verdict::Wait && approach.blockedWaits() == 1);
	for (uint32_t wait = 1; wait < Limits::blockedWaits; ++wait) approach.blocked();
	assert(approach.blocked() == Verdict::Exhausted);
	assert(std::strcmp(approach.reason(), "route_blocked") == 0);
}

void wanderingProvider()
{
	// A seller or captain that moves is retargeted a bounded number of times.
	Approach approach;
	approach.offer({{north, 1}});
	approach.select(north);
	for (uint32_t move = 0; move < Limits::providerMoves; ++move) {
		assert(approach.providerMoved() == Verdict::Continue);
		assert(!approach.selected());
		approach.offer({{east, 1}});
		approach.select(east);
	}
	assert(approach.providerMoved() == Verdict::Exhausted);
	assert(std::strcmp(approach.reason(), "provider_moved") == 0);

	// At the chosen tile the provider wandered out of range. With no other tile
	// in range, wait; then release the tile; reject it after repeated releases.
	Approach waiting;
	waiting.offer({{north, 1}, {east, 2}});
	waiting.select(north);
	for (uint32_t wait = 0; wait < Limits::providerWaits; ++wait) assert(waiting.providerOutOfRange(false) == Verdict::Wait);
	assert(waiting.providerOutOfRange(false) == Verdict::Release && !waiting.selected());
	assert(waiting.next() == north); // A release keeps the tile eligible.

	Approach restless;
	restless.offer({{north, 1}, {east, 2}});
	for (uint32_t release = 0; release < Limits::providerReleases; ++release) {
		restless.select(north);
		assert(restless.providerOutOfRange(true) == Verdict::Release);
	}
	restless.select(north);
	assert(restless.providerOutOfRange(true) == Verdict::Continue);
	assert(restless.rejected(north) && restless.next() == east);
	assert(restless.providerOutOfRange(true) == Verdict::Release); // Nothing selected.
}
}

int main()
{
	occupiedTilesAreLastResort();
	partlyUnreachableTiles();
	creatureOnThePath();
	wanderingProvider();
	std::cout << "playerbot approach contracts passed\n";
}
