#ifndef FS_PLAYERBOTRAIDHAZARDS_H
#define FS_PLAYERBOTRAIDHAZARDS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "position.h"

struct PlayerBotRaidMonster {
	std::string name;
	uint32_t count;
};

struct PlayerBotRaidHazard {
	uint64_t id;
	std::string raid;
	Position from, to;
	std::vector<PlayerBotRaidMonster> monsters;
	// A configured wave is still expected here; local emptiness cannot clear it.
	bool pending = false;
};

struct PlayerBotRaidFootprint {
	Position from, to;
	std::vector<PlayerBotRaidMonster> monsters;
};

// Dispatcher-owned shared knowledge, not a live creature/occupancy index. Counts
// are configured upper bounds, including future waves; rectangles describe spawn
// origins, never monster leashes or proof that tiles outside them are safe.
// Script-only spawns have no static footprint and are not represented here.
class PlayerBotRaidHazards {
public:
	static constexpr size_t MaxFootprints = 256;

	static PlayerBotRaidHazards& instance() {
		static PlayerBotRaidHazards hazards;
		return hazards;
	}

	uint64_t revision() const { return generation; }
	std::shared_ptr<const std::vector<PlayerBotRaidHazard>> snapshot() const { return records; }

	// Publish one activation atomically. Equal raid/rectangle pairs in this call
	// combine wave counts (saturating); matching older records are replaced with
	// new IDs so a bot's observed clearance cannot clear a later activation/wave.
	// Start and wave signals publish the full configured activation forecast:
	// earlier monsters may survive later waves. Never publish only a weaker later
	// wave here. setPending() tracks the remaining sequence independently, without
	// weakening species/counts or adding them again across publications.
	// Unmatched records retain their IDs. At capacity the oldest record is evicted.
	void activate(const std::string& raid, const std::vector<PlayerBotRaidFootprint>& footprints, bool pending = false) {
		std::shared_ptr<std::vector<PlayerBotRaidHazard>> updated;
		uint64_t candidateId = nextId;
		for (const auto& footprint : footprints) {
			if (!hasMonsters(footprint)) continue;

			const Position from = lowerCorner(footprint);
			const Position to = upperCorner(footprint);
			if (!updated) updated = std::make_shared<std::vector<PlayerBotRaidHazard>>(*records);
			auto found = std::find_if(updated->begin(), updated->end(), [&](const auto& hazard) {
				return hazard.raid == raid && hazard.from == from && hazard.to == to;
			});
			if (found != updated->end() && found->id < nextId) {
				updated->erase(found);
				found = updated->end();
			}
			if (found == updated->end()) {
				if (updated->size() == MaxFootprints) updated->erase(updated->begin());
				updated->push_back({candidateId++, raid, from, to, {}, pending});
				found = std::prev(updated->end());
			}
			for (const auto& monster : footprint.monsters) {
				if (monster.name.empty() || monster.count == 0) continue;
				auto species = std::find_if(found->monsters.begin(), found->monsters.end(), [&](const auto& entry) {
					return entry.name == monster.name;
				});
				if (species == found->monsters.end()) {
					found->monsters.push_back(monster);
				} else {
					const uint32_t room = std::numeric_limits<uint32_t>::max() - species->count;
					species->count += std::min(room, monster.count);
				}
			}
		}
		if (!updated) return;
		records = std::move(updated);
		nextId = candidateId;
		++generation;
	}

	// Pending means a future configured wave, not surviving occupancy. Changing
	// that fact retains the full forecast and IDs; only a new spawn signal needs
	// to invalidate prior observed clearance. No-op updates do not copy or revise.
	void setPending(const std::string& raid, const std::vector<PlayerBotRaidFootprint>& remaining) {
		std::shared_ptr<std::vector<PlayerBotRaidHazard>> updated;
		for (size_t i = 0; i < records->size(); ++i) {
			const auto& hazard = (*records)[i];
			if (hazard.raid != raid) continue;
			const bool pending = std::any_of(remaining.begin(), remaining.end(), [&](const auto& footprint) {
				return hazard.from == lowerCorner(footprint) && hazard.to == upperCorner(footprint) && hasMonsters(footprint);
			});
			if (hazard.pending == pending) continue;
			if (!updated) updated = std::make_shared<std::vector<PlayerBotRaidHazard>>(*records);
			(*updated)[i].pending = pending;
		}
		if (!updated) return;
		records = std::move(updated);
		++generation;
	}

	// Completion, failure, cancellation and reload all stop promising future waves.
	// Existing monsters may survive any of them, so do not erase their warnings.
	void retirePending(const std::string& raid) { setPending(raid, {}); }

	// Explicit knowledge reset for tests, not evidence of monster removal. Raid
	// completion and definition clear/reload must not call this: spawned monsters
	// can outlive both their event sequence and the objects that configured them.
	// IDs are never reused, even across clears. Empty clears are no-ops.
	void clear() {
		if (records->empty()) return;
		records = emptySnapshot();
		++generation;
	}

private:
	static bool hasMonsters(const PlayerBotRaidFootprint& footprint) {
		return std::any_of(footprint.monsters.begin(), footprint.monsters.end(), [](const auto& monster) {
			return !monster.name.empty() && monster.count != 0;
		});
	}

	static Position lowerCorner(const PlayerBotRaidFootprint& footprint) {
		return {std::min(footprint.from.x, footprint.to.x), std::min(footprint.from.y, footprint.to.y),
		        std::min(footprint.from.z, footprint.to.z)};
	}

	static Position upperCorner(const PlayerBotRaidFootprint& footprint) {
		return {std::max(footprint.from.x, footprint.to.x), std::max(footprint.from.y, footprint.to.y),
		        std::max(footprint.from.z, footprint.to.z)};
	}

	static std::shared_ptr<const std::vector<PlayerBotRaidHazard>> emptySnapshot() {
		static const auto empty = std::make_shared<const std::vector<PlayerBotRaidHazard>>();
		return empty;
	}

	PlayerBotRaidHazards() = default;
	PlayerBotRaidHazards(const PlayerBotRaidHazards&) = delete;
	PlayerBotRaidHazards& operator=(const PlayerBotRaidHazards&) = delete;

	std::shared_ptr<const std::vector<PlayerBotRaidHazard>> records = emptySnapshot();
	uint64_t generation = 0;
	uint64_t nextId = 1;
};

#endif
