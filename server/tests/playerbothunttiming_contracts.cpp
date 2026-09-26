#include "playerbothunttiming.h"

#include <cassert>
#include <chrono>
#include <iostream>

int main()
{
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
	std::cout << "hunt timing contracts passed\n";
}
