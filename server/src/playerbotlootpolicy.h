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
	bool currency = false;
	const void* source = nullptr;
	const void* item = nullptr;
	bool stackable = false;
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
	const void* item = nullptr;
	bool stackable = false;
	bool wholeStackReplaceable = false;
	uint8_t sourceCount = 0;
};

struct PlayerBotLootContainerSnapshot {
	const void* container = nullptr;
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint8_t size = 0;
	uint8_t capacity = 0;
	int8_t containerId = -1;
	bool contentsComplete = true;
};

struct PlayerBotLootContainerAccessSnapshot {
	const void* container = nullptr;
	const void* parent = nullptr;
	const void* item = nullptr;
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint8_t index = 0;
	int8_t parentContainerId = -1;
};

struct PlayerBotLootInventorySnapshot {
	uint32_t freeCapacity = 0;
	uint32_t heldFood = 0;
	std::map<uint16_t, uint32_t> itemCounts;
	std::vector<PlayerBotLootCargoSnapshot> cargo;
	std::vector<PlayerBotLootContainerSnapshot> containers;
	std::vector<PlayerBotLootContainerAccessSnapshot> containerAccess;
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

enum class PlayerBotLootPlacementResult : uint8_t {
	Placed,
	NoCapacity,
	NoSlot,
};

struct PlayerBotLootDestinationSnapshot {
	const void* container = nullptr;
	const void* item = nullptr;
	uint16_t containerItemId = 0;
	uint16_t containerClientId = 0;
	uint16_t itemId = 0;
	uint16_t clientId = 0;
	uint8_t index = 0;
	uint8_t count = 0;
	uint8_t availableCount = 0;
	int8_t containerId = -1;
	bool merge = false;
};

struct PlayerBotLootPlacement {
	PlayerBotLootPlacementResult result = PlayerBotLootPlacementResult::NoSlot;
	PlayerBotLootDestinationSnapshot destination;
	uint8_t count = 0;
};

struct PlayerBotLootReplacement {
	PlayerBotLootItemSnapshot incoming;
	PlayerBotLootCargoSnapshot cargo;
	PlayerBotLootDestinationSnapshot destination;
	uint8_t count = 0;
	uint8_t incomingCount = 0;
	uint64_t discardedValue = 0;
	bool slotReplacement = false;
	bool viable = false;
};

class PlayerBotLootPolicy
{
	public:
		explicit PlayerBotLootPolicy(uint32_t preferredFoodCount) : preferredFoodCount(preferredFoodCount) {}

		PlayerBotLootSelection select(const std::vector<PlayerBotLootItemSnapshot>& items,
		                              const PlayerBotLootInventorySnapshot& inventory,
		                              const std::set<uint16_t>& unavailableItems) const;
		PlayerBotLootPlacement placementFor(const PlayerBotLootItemSnapshot& incoming,
		                                    const PlayerBotLootInventorySnapshot& inventory) const;
		PlayerBotLootReplacement replacementFor(const PlayerBotLootItemSnapshot& incoming,
		                                        const PlayerBotLootInventorySnapshot& inventory) const;

	private:
		uint32_t preferredFoodCount;
};

#endif
