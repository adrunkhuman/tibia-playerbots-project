#ifndef TFS_PLAYERBOTSELLLOOTBASKET_H
#define TFS_PLAYERBOTSELLLOOTBASKET_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace playerbot {

struct SellLootBasketItem {
	uint32_t available = 0;
	uint32_t weight = 0;
	bool stackable = false;
	uint32_t mergeRoom = 0;
};

// Items are ordered by the caller's sale priority. Slots and weight are shared
// across the whole trip, not reset for each item type.
inline std::vector<uint32_t> playerBotSellLootBasket(const std::vector<SellLootBasketItem>& items,
                                                     uint64_t freeWeight, uint32_t freeSlots,
                                                     uint32_t perItemLimit)
{
	std::vector<uint32_t> counts;
	counts.reserve(items.size());
	for (const SellLootBasketItem& item : items) {
		const uint64_t slotRoom = item.stackable ? uint64_t(item.mergeRoom) + uint64_t(freeSlots) * 100 : freeSlots;
		const uint64_t weightRoom = item.weight ? freeWeight / item.weight : perItemLimit;
		const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>({item.available, perItemLimit, slotRoom, weightRoom}));
		counts.push_back(count);
		const uint32_t newUnits = item.stackable && count <= item.mergeRoom ? 0 :
			item.stackable ? count - item.mergeRoom : count;
		freeSlots -= item.stackable ? (newUnits + 99) / 100 : newUnits;
		freeWeight -= uint64_t(item.weight) * count;
	}
	return counts;
}

}

#endif
