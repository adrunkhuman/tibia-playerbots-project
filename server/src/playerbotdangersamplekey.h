#ifndef FS_PLAYERBOTDANGERSAMPLEKEY_H
#define FS_PLAYERBOTDANGERSAMPLEKEY_H

#include "playerbotcombatprofile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iosfwd>

#include "position.h"

// Include the entire combat profile: two bots may share a sample only when
// their modeled damage and health inputs are identical. Float bits, rather
// than float equality, preserve the exact attack factor (including signed 0).
struct PlayerBotDangerSampleKey {
	uint64_t atlasRevision;
	Position position;
	std::array<uint32_t, 7> combat;

	PlayerBotDangerSampleKey(uint64_t revision, Position tile, const PlayerBotCombatProfile& profile)
	    : atlasRevision(revision), position(tile), combat{profile.level, static_cast<uint32_t>(profile.maximumHealth),
	      static_cast<uint32_t>(profile.armor), static_cast<uint32_t>(profile.defense),
	      static_cast<uint32_t>(profile.attack), static_cast<uint32_t>(profile.attackSkill), factorBits(profile.attackFactor)} {}

	bool operator==(const PlayerBotDangerSampleKey& other) const {
		return atlasRevision == other.atlasRevision && position == other.position && combat == other.combat;
	}

private:
	static uint32_t factorBits(float factor) {
		static_assert(sizeof(float) == sizeof(uint32_t));
		uint32_t bits;
		std::memcpy(&bits, &factor, sizeof(bits));
		return bits;
	}
};

struct PlayerBotDangerSampleKeyHash {
	size_t operator()(const PlayerBotDangerSampleKey& key) const {
		size_t hash = std::hash<uint64_t>{}(key.atlasRevision);
		auto combine = [&hash](uint32_t value) {
			hash ^= std::hash<uint32_t>{}(value) + size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
		};
		combine(key.position.x);
		combine(key.position.y);
		combine(key.position.z);
		for (uint32_t field : key.combat) combine(field);
		return hash;
	}
};

#endif
