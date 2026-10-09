#include "playerbothunttiming.h"
#include "playerbothunttriptiming.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main()
{
	// Terrain comes from the route, not the fast town tile at planning time.
	assert(playerBotHuntStepMilliseconds(70, 220) == 350);
	assert(playerBotHuntStepMilliseconds(150, 220) == 700);
	assert(playerBotHuntStepMilliseconds(200, 220) == 950);
	assert(playerBotHuntStepMilliseconds(150, 220, true) == 1400);
	assert(playerBotHuntStepMilliseconds(150, 220, true, true) == 1400);
	assert(playerBotHuntStepMilliseconds(0, 220) == 700);
	assert(playerBotHuntCoarseTravelSeconds(100, 220) == 70);
	assert(playerBotHuntActionMilliseconds(700, 200) == 1000);
	assert(playerBotHuntActionMilliseconds(700, 1500) == 1500);
	assert(playerBotHuntActionMilliseconds(1900, 1500) == 1900);
	assert(playerBotHuntTransitSeconds(200) == 460);
	assert(playerBotHuntTransitSeconds(0) == 60);
	assert(playerBotHuntTransitSeconds(1e12) == 3600);
	using namespace std::chrono;
	using Timing = PlayerBotHuntSliceTiming;
	const Timing::Time start{};
	Timing timing(start);
	timing.mark(PlayerBotHuntSlicePart::Transport, start + microseconds(12000));
	timing.mark(PlayerBotHuntSlicePart::Score, start + microseconds(12007));
	timing.mark(PlayerBotHuntSlicePart::Evidence, start + microseconds(14008));
	timing.mark(PlayerBotHuntSlicePart::Other, start + microseconds(14109));
	assert(timing.us(PlayerBotHuntSlicePart::Setup) == 12000);
	assert(timing.us(PlayerBotHuntSlicePart::Transport) == 7);
	assert(timing.us(PlayerBotHuntSlicePart::Score) == 2001);
	assert(timing.us(PlayerBotHuntSlicePart::Evidence) == 101);
	assert(timing.elapsedUs() == 14109);
	assert(timing.elapsedUs() >= Timing::overrunThresholdUs);
	assert(Timing::latenessUs(start + milliseconds(500), start + milliseconds(501)) == 1000);
	assert(Timing::latenessUs(start + milliseconds(500), start + milliseconds(499)) == 0);
	PlayerBotHuntSliceAttribution attribution;
	attribution.update(12, 4, 0, 0);
	assert(attribution.planningPass == 12 && attribution.scoringRevision == 4);
	attribution.update(13, 5, 12, 4);
	assert(attribution.planningPass == 13 && attribution.scoringRevision == 5);
	assert(attribution.invalidatedPass == 12 && attribution.invalidatedRevision == 4);
	attribution.update(0, 0, 0, 0); // Cleared session retains the last current pass.
	assert(attribution.planningPass == 13 && attribution.scoringRevision == 5);
	PlayerBotHuntSliceAttribution cancelled;
	cancelled.update(0, 0, 12, 4);
	assert(cancelled.planningPass == 12 && cancelled.scoringRevision == 4);
	PlayerBotHuntSliceCounters merged;
	merged.elapsedUs = merged.maxElapsedUs = 1200;
	merged.partUs[static_cast<size_t>(PlayerBotHuntSlicePart::RouteChecks)] = 1000;
	merged.outboundChecks = 1;
	merged.routes.localExpandedNodes = 300;
	merged.routes.requestSequence = 4;
	merged.routes.invalidations = 1;
	merged.routes.invalidationReason = "tile_changed";
	PlayerBotHuntSliceCounters later = merged;
	later.elapsedUs = later.maxElapsedUs = 11000;
	later.routes.requestSequence = 5;
	later.routes.invalidations = 0;
	later.routes.invalidationReason = "none";
	later.routes.walkingTime = microseconds(700);
	merged.add(later);
	assert(merged.slices == 2 && merged.elapsedUs == 12200 && merged.maxElapsedUs == 11000);
	assert(merged.us(PlayerBotHuntSlicePart::RouteChecks) == 2000 && merged.outboundChecks == 2);
	assert(merged.routes.localExpandedNodes == 600 && merged.routes.walkingTime == microseconds(700));
	// Request identity is the latest; the last invalidation survives quiet slices.
	assert(merged.routes.requestSequence == 5 && merged.routes.invalidations == 1);
	assert(std::string(merged.routes.invalidationReason) == "tile_changed");
	std::cout << "hunt timing contracts passed\n";
}
