/** Economy knowledge and pure disposition decisions for one playerbot. */
#ifndef FS_PLAYERBOTECONOMY_H
#define FS_PLAYERBOTECONOMY_H

#include <cstdint>
#include <map>
#include <vector>

#include "position.h"

struct PlayerBotEconomyOffer {
	uint16_t itemId = 0;
	uint32_t buyPrice = 0;
	uint32_t sellPrice = 0;
	uint8_t subType = 0;
};

struct PlayerBotEconomyProvider {
	uint32_t id = 0;
	Position position;
	std::vector<PlayerBotEconomyOffer> offers;
};

struct PlayerBotEconomyInventorySnapshot {
	uint32_t itemCount = 0;
	uint32_t freeCapacity = 0;
	uint64_t money = 0;
	uint64_t bankBalance = 0;
};

struct PlayerBotEconomyRestockDecision {
	uint32_t amount = 0;
	bool insufficientFunds = false;
};

class PlayerBotEconomyCatalog
{
	public:
		void learn(const std::vector<PlayerBotEconomyProvider>& providers);
		uint32_t sellValue(uint16_t itemId) const;
		const PlayerBotEconomyProvider* rankedProvider(const std::vector<PlayerBotEconomyProvider>& providers,
		                                               uint16_t itemId, bool purchase, const Position& from) const;
		const std::map<uint16_t, uint32_t>& sellValues() const { return learnedSellValues; }

	private:
		std::map<uint16_t, uint32_t> learnedSellValues;
};

class PlayerBotDispositionPolicy
{
	public:
		static constexpr uint16_t smallHealthPotionItemId = 8704;
		static constexpr uint32_t potionReturnThreshold = 1;
		static constexpr uint32_t potionRestockTarget = 10;
		static constexpr uint32_t carriedGoldReserve = 100;

		uint32_t protectedReserve(uint16_t itemId, bool food) const;
		uint32_t sellQuantity(const PlayerBotEconomyInventorySnapshot& inventory, uint16_t reserve) const;
		PlayerBotEconomyRestockDecision restock(const PlayerBotEconomyInventorySnapshot& inventory,
		                                         uint32_t unitPrice, uint32_t unitWeight) const;
		uint32_t bankWithdrawal(const PlayerBotEconomyInventorySnapshot& inventory, uint32_t coinWeight) const;
};

#endif
