#include "playerbotdangersamplekey.h"
#include "playerbotroutecache.h"

#include <cassert>
#include <iostream>

int main() {
	using Key = PlayerBotDangerSampleKey;
	using Cache = PlayerBotRouteCache<Key, double, PlayerBotDangerSampleKeyHash>;
	PlayerBotCombatProfile combat{30, 200, 20, 40, 50, 60, 1.0f};
	const Position sample(100, 200, 7), sightTile(99, 200, 7);
	const Key original(3, sample, combat);
	Cache shared(4096, 8);
	PlayerBotRouteChanges::Watch los;
	{
		PlayerBotRouteChanges::Scope scope(los);
		PlayerBotRouteChanges::read(sightTile);
	}
	assert(shared.insert(original, 0.25, std::move(los), 0));
	assert(*shared.lookup(Key(3, sample, combat)) == 0.25); // another bot, identical profile
	assert(!shared.lookup(Key(4, sample, combat))); // atlas rebuilt or monsters reloaded
	assert(!shared.lookup(Key(3, Position(101, 200, 7), combat)));
	auto different = combat;
	++different.level;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; ++different.maximumHealth;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; ++different.armor;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; ++different.defense;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; ++different.attack;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; ++different.attackSkill;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; different.attackFactor = 0.5f;
	assert(!shared.lookup(Key(3, sample, different)));
	different = combat; different.attackFactor = -0.0f;
	combat.attackFactor = 0.0f;
	assert(!(Key(3, sample, combat) == Key(3, sample, different)));

	PlayerBotRouteChanges::Watch outer;
	{
		PlayerBotRouteChanges::Scope scope(outer);
		assert(*shared.lookup(original) == 0.25);
	}
	PlayerBotRouteChanges::changed(sightTile);
	assert(outer.check() == PlayerBotRouteChanges::Reason::ChangedTile);
	assert(!shared.lookup(original)); // blocked/unblocked LOS invalidates the sample
	std::cout << "playerbot danger sample key contracts passed\n";
}
