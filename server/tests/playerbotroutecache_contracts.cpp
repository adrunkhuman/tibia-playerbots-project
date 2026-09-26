#include "playerbotroutecache.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {
struct Key {
	int connection;
	int actor;
	int permission;
	int risk;
	int generation;
	bool operator==(const Key& other) const {
		return connection == other.connection && actor == other.actor && permission == other.permission &&
		       risk == other.risk && generation == other.generation;
	}
};
struct Hash {
	size_t operator()(const Key& key) const {
		size_t hash = 0;
		for (int value : {key.connection, key.actor, key.permission, key.risk, key.generation}) {
			hash = hash * 31 + std::hash<int>{}(value);
		}
		return hash;
	}
};
using Cache = PlayerBotRouteCache<Key, std::string, Hash>;
using Changes = PlayerBotRouteChanges;

Changes::Watch watching(Position tile) {
	Changes::Watch watch;
	Changes::Scope scope(watch);
	Changes::read(tile);
	return watch;
}
} // namespace

int main() {
	const Position door(123, 456, 7), other(124, 456, 7);
	const Key primary{1, 1, 1, 2, 3};
	Cache shared;
	static_assert(Cache::DefaultMaxBytes == 64 * 1024 * 1024);
	static_assert(Cache::DefaultMaxEntries == 256);
	assert(shared.insert(primary, "route", watching(door), 5));
	// Separate callers use the same entry; neither takes ownership of its result.
	assert(*shared.lookup(primary) == "route");
	assert(*shared.lookup(primary) == "route");
	assert(!shared.lookup({1, 2, 1, 2, 3})); // actor
	assert(!shared.lookup({1, 1, 0, 2, 3})); // permission
	assert(!shared.lookup({1, 1, 1, 3, 3})); // risk
	assert(!shared.lookup({1, 1, 1, 2, 4})); // topology generation
	Changes::changed(other);
	assert(*shared.lookup(primary) == "route");
	Changes::changed(door);
	assert(!shared.lookup(primary) && shared.size() == 0 && shared.weightBytes() == 0);

	assert(shared.insert(primary, "epoch", watching(door), 5));
	Changes::invalidate();
	assert(!shared.lookup(primary));
	assert(shared.insert(primary, "overflow", watching(door), 8));
	for (int n = 0; n < 4097; ++n) Changes::changed(other);
	assert(!shared.lookup(primary));
	// Stale data cannot be inserted after an observed tile changed.
	auto stale = watching(door);
	Changes::changed(door);
	assert(!shared.insert(primary, "stale", std::move(stale), 5));

	// Reads by an inner scope belong to all enclosing watches.
	Changes::Watch outer, inner;
	{
		Changes::Scope outerScope(outer);
		{
			Changes::Scope innerScope(inner);
			Changes::read(door);
		}
		Changes::read(other);
	}
	assert(outer.tiles.size() == 2 && inner.tiles.size() == 1);
	assert(inner.tiles == watching(door).tiles);
	Changes::changed(door);
	assert(outer.check() == Changes::Reason::ChangedTile);
	assert(inner.check() == Changes::Reason::ChangedTile);

	// A cached result hit in nested resumed work propagates its dependencies to
	// both scopes, even though neither performed the original tile read.
	assert(shared.insert(primary, "nested", watching(door), 6));
	Changes::Watch resumedOuter, resumedInner;
	{
		Changes::Scope outerScope(resumedOuter);
		Changes::Scope innerScope(resumedInner);
		assert(*shared.lookup(primary) == "nested");
	}
	assert(resumedOuter.tiles.size() == 1 && resumedInner.tiles.size() == 1);
	Changes::changed(door);
	assert(resumedOuter.check() == Changes::Reason::ChangedTile);
	assert(resumedInner.check() == Changes::Reason::ChangedTile);

	Cache capped(4096, 2);
	const Key second{2, 1, 1, 2, 3}, third{3, 1, 1, 2, 3};
	assert(capped.insert(primary, "a", watching(door), 2));
	const size_t oneWeight = capped.weightBytes();
	assert(oneWeight > 2);
	assert(capped.insert(second, "b", watching(other), 2));
	assert(capped.size() == 2 && capped.weightBytes() == 2 * oneWeight);
	assert(capped.lookup(primary)); // promote primary to most recently used
	assert(capped.insert(third, "c", watching(other), 2));
	assert(!capped.lookup(second) && capped.lookup(primary) && capped.lookup(third));
	assert(capped.size() == 2 && capped.weightBytes() <= 4096);
	assert(!capped.insert(second, "huge", watching(other), 4096));
	assert(capped.size() == 2 && capped.lookup(primary));
	Cache byteCapped(oneWeight + oneWeight / 2, 256);
	assert(byteCapped.insert(primary, "a", watching(door), 2));
	assert(byteCapped.insert(second, "b", watching(other), 2));
	assert(byteCapped.size() == 1 && byteCapped.weightBytes() <= oneWeight + oneWeight / 2);
	assert(!byteCapped.lookup(primary) && byteCapped.lookup(second));
	Changes::Watch large;
	{
		Changes::Scope scope(large);
		for (int n = 0; n < 65; ++n) Changes::read(Position(n, 700, 7));
	}
	Changes::Watch imported;
	{
		Changes::Scope scope(imported);
		Changes::absorb(large);
		Changes::absorb(large);
		assert(imported.imports.size() == 1 && imported.tiles.size() == 65);
	}
	Changes::Watch copy = large;
	assert(copy.identity != large.identity);
	{
		Changes::Scope scope(large);
		Changes::read(door);
	}
	{
		Changes::Scope scope(copy);
		Changes::read(other);
	}
	{
		Changes::Scope scope(imported);
		Changes::absorb(large);
		Changes::absorb(copy);
	}
	assert(imported.tiles.size() == 67);
	Changes::changed(door);
	assert(!imported.valid());
	assert(!capped.insert(second, "many tiles", std::move(large), 0));
	assert(capped.size() == 2);
	Cache single(4096, 1);
	assert(single.insert(primary, "first", watching(door), 0));
	assert(single.insert(second, "second", watching(other), 0));
	assert(!single.lookup(primary) && single.lookup(second));
	capped.clear();
	assert(capped.size() == 0 && capped.weightBytes() == 0);
	assert(capped.insert(primary, "mutable", watching(door), 7));
	Changes::Watch borrower;
	{
		Changes::Scope scope(borrower);
		auto value = capped.take(primary);
		assert(value && *value == "mutable");
		assert(!capped.lookup(primary) && capped.weightBytes() == 0);
		value->resize(8192);
		assert(!capped.insert(primary, std::move(*value), watching(door), 8192));
	}
	assert(capped.size() == 0 && capped.weightBytes() == 0);
	Changes::changed(door);
	assert(!borrower.valid());
	assert(!capped.take(primary));
	Cache defaultCapped;
	for (int n = 0; n <= 256; ++n) {
		assert(defaultCapped.insert({n, 1, 1, 2, 3}, "ok", watching(other), 2));
	}
	assert(defaultCapped.size() == 256 && !defaultCapped.lookup({0, 1, 1, 2, 3}));
	std::cout << "playerbot route cache contracts passed\n";
}
