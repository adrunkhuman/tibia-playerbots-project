#include "../src/playerbotlifecycle.h"

#include <cassert>
#include <iostream>
#include <map>

int main()
{
	PlayerBotLifecycleClock clock;
	uint64_t first = 0;
	const uint64_t activation = clock.renew(first);
	assert(PlayerBotLifecycleClock::accepts(first, activation));
	uint64_t second = 0;
	const uint64_t otherActivation = clock.renew(second);
	const uint64_t recovery = clock.renew(first);
	assert(PlayerBotLifecycleClock::accepts(second, otherActivation));
	assert(!PlayerBotLifecycleClock::accepts(first, activation));
	assert(PlayerBotLifecycleClock::accepts(first, recovery));
	clock.renew(first); // Explicit removal invalidates queued recovery callbacks.
	assert(!PlayerBotLifecycleClock::accepts(first, recovery));
	assert(PlayerBotLifecycleClock::accepts(second, otherActivation));
	uint64_t replacement = 0; // Same GUID reserved again must not reuse tokens.
	const uint64_t nextActivation = clock.renew(replacement);
	assert(!PlayerBotLifecycleClock::accepts(replacement, activation));
	assert(!PlayerBotLifecycleClock::accepts(replacement, recovery));
	assert(PlayerBotLifecycleClock::accepts(replacement, nextActivation));
	std::map<uint32_t, PlayerBotLifecycleState> roster;
	const uint32_t firstGuid = 101;
	const uint32_t secondGuid = 102;
	auto& firstBot = roster[firstGuid];
	auto& secondBot = roster[secondGuid];
	assert(firstBot.managed() && !firstBot.active()); // Failed startup still reserves identity.
	assert(firstBot.activate());
	assert(secondBot.activate());
	assert(firstBot.beginRecovery());
	assert(firstBot.recovering() && secondBot.active());
	firstBot.suspend(); // Relog attempts exhausted: reservation survives.
	assert(firstBot.managed() && !firstBot.recovering());
	assert(firstBot.activate());
	assert(firstBot.beginRecovery());
	assert(firstBot.activate()); // Successful placement, including paused controllers.
	assert(firstBot.terminate());
	clock.renew(first);
	assert(!firstBot.managed() && roster.count(firstGuid)); // Stable record and login reservation.
	assert(&roster.at(firstGuid) == &firstBot);
	assert(!PlayerBotLifecycleClock::accepts(first, recovery));
	assert(!firstBot.terminate() && !firstBot.activate() && !firstBot.beginRecovery());
	firstBot.suspend();
	assert(firstBot.phase() == PlayerBotLifecycleState::Phase::Terminal);
	assert(secondBot.active());
	std::cout << "playerbot lifecycle contracts passed\n";
}
