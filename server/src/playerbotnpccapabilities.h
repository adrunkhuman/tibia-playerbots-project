/** Playerbot capability discovery from loaded NPC state. */
#ifndef FS_PLAYERBOTNPCCAPABILITIES_H
#define FS_PLAYERBOTNPCCAPABILITIES_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "npc.h"

enum class PlayerBotNpcCapability : uint8_t { Shop, SpellTrainer, Travel };

inline const std::string* playerBotNpcMetadata(const Npc& npc)
{
	return npc.getParameter("playerbot_service");
}

inline bool playerBotNpcDisabled(const Npc& npc)
{
	const std::string* metadata = playerBotNpcMetadata(npc);
	return metadata && *metadata == "disabled";
}

inline bool playerBotNpcHasCapability(const Npc& npc, PlayerBotNpcCapability capability)
{
	if (npc.isRemoved() || playerBotNpcDisabled(npc)) return false;
	switch (capability) {
		case PlayerBotNpcCapability::Shop: return !npc.getShopOffers().empty();
		case PlayerBotNpcCapability::SpellTrainer: return !npc.getSpellOffers().empty();
		case PlayerBotNpcCapability::Travel: return !npc.getTravelOffers().empty();
	}
	return false;
}

inline bool playerBotNpcHasRole(const Npc& npc, const char* role)
{
	const std::string* metadata = !npc.isRemoved() ? playerBotNpcMetadata(npc) : nullptr;
	return metadata && *metadata == role;
}

inline uint32_t playerBotNpcDistance(const Position& from, const Position& to)
{
	return std::max(Position::getDistanceX(from, to), Position::getDistanceY(from, to)) +
	       (from.z == to.z ? 0 : 32 * Position::getDistanceZ(from, to));
}

inline std::vector<Npc*> playerBotNpcProviders(const std::map<uint32_t, Npc*>& npcs,
	PlayerBotNpcCapability capability, const Position& from)
{
	std::vector<Npc*> providers;
	for (const auto& entry : npcs) {
		Npc* npc = entry.second;
		if (npc && playerBotNpcHasCapability(*npc, capability)) providers.push_back(npc);
	}
	std::sort(providers.begin(), providers.end(), [&from](const Npc* left, const Npc* right) {
		const uint32_t leftDistance = playerBotNpcDistance(from, left->getPosition());
		const uint32_t rightDistance = playerBotNpcDistance(from, right->getPosition());
		return leftDistance != rightDistance ? leftDistance < rightDistance : left->getID() < right->getID();
	});
	return providers;
}

#endif
