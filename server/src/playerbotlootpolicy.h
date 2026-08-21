/** Explicit loot and cargo decisions over authoritative item snapshots. */
#ifndef FS_PLAYERBOTLOOTPOLICY_H
#define FS_PLAYERBOTLOOTPOLICY_H

#include <cstdint>
#include <optional>
#include <map>
#include <set>
#include <vector>

struct PlayerBotLootItemSnapshot {
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint8_t count = 0;
	uint8_t availableCount = 0;
	uint8_t index = 0;
	uint32_t unitWeight = 0;
	uint32_t unitValue = 0;
	uint32_t inventoryCount = 0;
	bool food = false;
};

struct PlayerBotLootCargoSnapshot {
	const void* source = nullptr;
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint8_t count = 0;
	uint8_t index = 0;
	uint32_t unitWeight = 0;
	uint32_t unitValue = 0;
	bool replaceable = false;
	int8_t containerId = -1;
};

struct PlayerBotLootInventorySnapshot {
	uint32_t freeCapacity = 0;
	uint32_t heldFood = 0;
	std::map<uint16_t, uint32_t> itemCounts;
	std::vector<PlayerBotLootCargoSnapshot> cargo;
};

enum class PlayerBotLootSelectionResult : uint8_t {
	Selected,
	FoodPreferenceSatisfied,
	NoEligibleLoot,
};

struct PlayerBotLootSelection {
	PlayerBotLootSelectionResult result = PlayerBotLootSelectionResult::NoEligibleLoot;
	PlayerBotLootItemSnapshot item;
};

struct PlayerBotLootReplacement {
	PlayerBotLootCargoSnapshot cargo;
	uint8_t count = 0;
	uint64_t discardedValue = 0;
	bool viable = false;
};

class PlayerBotLootPolicy
{
	public:
		explicit PlayerBotLootPolicy(uint32_t preferredFoodCount) : preferredFoodCount(preferredFoodCount) {}

		PlayerBotLootSelection select(const std::vector<PlayerBotLootItemSnapshot>& items,
		                              const PlayerBotLootInventorySnapshot& inventory,
		                              const std::set<uint16_t>& unavailableItems) const;
		PlayerBotLootReplacement replacementFor(const PlayerBotLootItemSnapshot& incoming,
		                                        const PlayerBotLootInventorySnapshot& inventory) const;

	private:
		uint32_t preferredFoodCount;
};

#endif
