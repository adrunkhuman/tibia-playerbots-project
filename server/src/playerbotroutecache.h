#ifndef FS_PLAYERBOTROUTECACHE_H
#define FS_PLAYERBOTROUTECACHE_H

#include "playerbotroutechanges.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

// Dispatcher-owned cache for completed, successful route connections. Key must
// include every non-tile input (actor, permissions, risk, topology generation,
// etc.). Callers decide whether a result is cacheable and supply its dynamic
// result size; topology invalidation is the caller's responsibility.
// Pointers returned by lookup remain valid only until the next cache mutation.
template<typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
class PlayerBotRouteCache {
public:
	static constexpr size_t DefaultMaxBytes = 64 * 1024 * 1024;
	static constexpr size_t DefaultMaxEntries = 256;

	explicit PlayerBotRouteCache(size_t maxBytes = DefaultMaxBytes, size_t maxEntries = DefaultMaxEntries)
	    : maxBytes(maxBytes), maxEntries(maxEntries) {}

	const Value* lookup(const Key& key) {
		auto found = index.find(key);
		if (found == index.end()) return nullptr;
		auto entry = found->second;
		if (!entry->watch.valid()) {
			erase(entry);
			return nullptr;
		}
		PlayerBotRouteChanges::absorb(entry->watch);
		entries.splice(entries.begin(), entries, entry);
		return &entries.front().value;
	}

	// Check a mutable value out before growing it. Its old reservation must
	// not remain in the cache while callers allocate beyond the recorded size.
	std::optional<Value> take(const Key& key) {
		if (!lookup(key)) return std::nullopt;
		auto found = index.find(key);
		Value value = std::move(found->second->value);
		erase(found->second);
		return value;
	}

	// resultBytes accounts for allocations owned by Value (and dynamic Key data,
	// if any); fixed entry/index overhead and watched tiles are charged here.
	// Reject stale or oversized entries without evicting useful cached results.
	bool insert(const Key& key, Value value, PlayerBotRouteChanges::Watch watch, size_t resultBytes) {
		if (!watch.valid()) return false;
		constexpr size_t overhead = sizeof(Entry) + sizeof(typename Index::value_type) + 4 * sizeof(void*);
		if (resultBytes > maxBytes || overhead > maxBytes - resultBytes) return false;
		const size_t base = overhead + resultBytes;
		const size_t dependencyCount = watch.tiles.size() + watch.imports.size();
		if (dependencyCount > (maxBytes - base) / BytesPerTile || maxEntries == 0) return false;
		const size_t bytes = base + dependencyCount * BytesPerTile;
		auto old = index.find(key);
		if (old != index.end()) erase(old->second);
		entries.push_front({key, std::move(value), std::move(watch), bytes});
		index.emplace(entries.front().key, entries.begin());
		weight += bytes;
		while (entries.size() > maxEntries || weight > maxBytes) erase(std::prev(entries.end()));
		return true;
	}

	void clear() { index.clear(); entries.clear(); weight = 0; }
	size_t size() const { return entries.size(); }
	size_t weightBytes() const { return weight; }

private:
	static constexpr size_t BytesPerTile = 64; // conservative hash-set node and bucket allowance
	struct Entry {
		Key key;
		Value value;
		PlayerBotRouteChanges::Watch watch;
		size_t bytes;
	};
	using Entries = std::list<Entry>;
	using Index = std::unordered_map<Key, typename Entries::iterator, Hash, Equal>;

	void erase(typename Entries::iterator entry) {
		weight -= entry->bytes;
		index.erase(entry->key);
		entries.erase(entry);
	}

	const size_t maxBytes;
	const size_t maxEntries;
	size_t weight = 0;
	Entries entries;
	Index index;
};

#endif
