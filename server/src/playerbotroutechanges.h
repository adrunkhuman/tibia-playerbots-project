#ifndef FS_PLAYERBOTROUTECHANGES_H
#define FS_PLAYERBOTROUTECHANGES_H

#include <cstdint>
#include <cstdlib>
#include <iosfwd>

#include "position.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

// Dispatcher-only dependency tracking, not a map snapshot or topology revision.
// The bounded journal also detects changes to previously absent tiles. Overflow
// invalidates conservatively. No observer remains installed between turns.
class PlayerBotRouteChanges {
public:
	enum class Reason : uint8_t { None, Epoch, JournalOverflow, ChangedTile };
	enum class Cause : uint8_t { Other, ItemAdd, ItemUpdate, ItemRemove, TileFlag, TileReplace, Attribute, Teleport };
	static const char* causeName(Cause cause) {
		switch (cause) {
			case Cause::ItemAdd: return "item_add";
			case Cause::ItemUpdate: return "item_update";
			case Cause::ItemRemove: return "item_remove";
			case Cause::TileFlag: return "tile_flag";
			case Cause::TileReplace: return "tile_replace";
			case Cause::Attribute: return "attribute";
			case Cause::Teleport: return "teleport";
			default: return "other";
		}
	}
	static const char* reasonName(Reason reason) {
		switch (reason) {
			case Reason::Epoch: return "epoch";
			case Reason::JournalOverflow: return "journal_overflow";
			case Reason::ChangedTile: return "changed_tile";
			default: return "none";
		}
	}
	struct Watch {
		Watch() = default;
		Watch(const Watch& other)
		    : revision(other.revision), epoch(other.epoch), tiles(other.tiles), changedCause(other.changedCause),
		      changedTile(other.changedTile), delta(other.delta) {}
		Watch(Watch&&) = default;
		Watch& operator=(Watch&&) = default;
		Watch& operator=(const Watch& other) {
			if (this != &other) { Watch copy(other); *this = std::move(copy); }
			return *this;
		}
		uint64_t identity = ++nextWatchIdentity;
		// Tile dependencies only grow between assignments. Remember large imports
		// so repeated goals sharing a source tree don't recopy its entire flood.
		std::unordered_map<uint64_t, size_t> imports;
		uint64_t revision = PlayerBotRouteChanges::revision;
		uint64_t epoch = PlayerBotRouteChanges::epoch;
		std::unordered_set<uint64_t> tiles;
		Cause changedCause = Cause::Other;
		uint64_t changedTile = 0;
		uint64_t delta = 0;
		Reason check() {
			delta = PlayerBotRouteChanges::revision - revision;
			if (epoch != PlayerBotRouteChanges::epoch) return Reason::Epoch;
			if (!changes.empty() && revision < changes.front().revision - 1) return Reason::JournalOverflow;
			for (auto it = changes.rbegin(); it != changes.rend() && it->revision > revision; ++it) {
				if (tiles.count(it->tile)) {
					changedTile = it->tile;
					changedCause = it->cause;
					return Reason::ChangedTile;
				}
			}
			revision = PlayerBotRouteChanges::revision;
			return Reason::None;
		}
		bool valid() { return check() == Reason::None; }
	};
	class Scope {
	public:
		explicit Scope(Watch& watch) : watch(watch), previous(observer) { observer = this; }
		~Scope() { observer = previous; }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	private:
		Watch& watch;
		Scope* previous;
		friend class PlayerBotRouteChanges;
	};
	class Suppress {
	public:
		Suppress() { ++suppressChanges; }
		~Suppress() { --suppressChanges; }
		Suppress(const Suppress&) = delete;
		Suppress& operator=(const Suppress&) = delete;
	};
	// One dispatcher-owned topology observer. Notifications precede some map
	// writes, so the observer records coordinates and inspects them lazily.
	using GeometryObserver = void (*)(Position, bool);
	static void setGeometryObserver(GeometryObserver callback) { geometryObserver = callback; }
	using WalkableObserver = bool (*)(Position);
	static void setWalkableObserver(WalkableObserver callback) { walkableObserver = callback; }
	static bool wasWalkableAtBuild(Position position) {
		return walkableObserver && walkableObserver(position);
	}
	static uint64_t currentRevision() { return revision; }
	static uint64_t currentConnectivityRevision() { return connectivityRevision; }
	static uint64_t currentEpoch() { return epoch; }
	class IgnoreReads {
	public:
		IgnoreReads() { ++ignoredReads; }
		~IgnoreReads() { --ignoredReads; }
		IgnoreReads(const IgnoreReads&) = delete;
		IgnoreReads& operator=(const IgnoreReads&) = delete;
	};
	static void invalidate() {
		++epoch;
		if (geometryObserver) geometryObserver({}, true);
	}
	static void read(Position position) {
		if (ignoredReads) return;
		const uint64_t tile = key(position);
		for (Scope* scope = observer; scope; scope = scope->previous) scope->watch.tiles.insert(tile);
	}
	// A cached computation must remain a dependency of each enclosing resumed search.
	static void absorb(const Watch& watch) {
		for (Scope* scope = observer; scope; scope = scope->previous) {
			auto& destination = scope->watch;
			if (&destination == &watch) continue;
			if (watch.tiles.size() >= 64) {
				const auto prior = destination.imports.find(watch.identity);
				if (prior != destination.imports.end() && prior->second == watch.tiles.size()) continue;
				if (destination.imports.size() >= 64) destination.imports.clear();
				destination.imports[watch.identity] = watch.tiles.size();
			}
			destination.tiles.insert(watch.tiles.begin(), watch.tiles.end());
		}
	}
	static void changed(Position position, Cause cause = Cause::Other, bool mayOpenConnectivity = true) {
		if (suppressChanges) return;
		if (geometryObserver) geometryObserver(position, false);
		if (mayOpenConnectivity) ++connectivityRevision;
		changes.push_back({++revision, key(position), cause});
		if (changes.size() > 4096) changes.pop_front();
	}
private:
	static uint64_t key(Position p) { return (uint64_t(p.z) << 32) | (uint64_t(p.x) << 16) | p.y; }
	inline static uint64_t nextWatchIdentity = 0;
	inline static uint64_t revision = 0;
	inline static uint64_t connectivityRevision = 0;
	inline static uint64_t epoch = 0;
	inline static uint32_t suppressChanges = 0;
	inline static uint32_t ignoredReads = 0;
	inline static GeometryObserver geometryObserver = nullptr;
	inline static WalkableObserver walkableObserver = nullptr;
	struct Change { uint64_t revision, tile; Cause cause; };
	inline static std::deque<Change> changes;
	inline static Scope* observer = nullptr;
};

#endif
