/** Economy knowledge and pure disposition decisions for one playerbot. */
#ifndef FS_PLAYERBOTECONOMY_H
#define FS_PLAYERBOTECONOMY_H

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "playerbotinventorypolicy.h"
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

struct PlayerBotEconomySupplyRequest {
	uint32_t itemCount = 0;
	uint32_t unitPrice = 0;
	uint32_t unitWeight = 0;
	uint32_t returnThreshold = 0;
	uint32_t restockTarget = 0;
	uint32_t safetyFloor = 0;
};

struct PlayerBotProviderUtilityProfile {
	uint32_t serviceValueWeight = 10;
	uint32_t routeCostWeight = 1;
};

class PlayerBotProviderUtilityPolicy
{
	public:
		int64_t score(uint64_t serviceValue, uint32_t estimatedRouteCost,
		              const PlayerBotProviderUtilityProfile& profile) const;
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
		static constexpr uint32_t potionReturnThreshold = 1;
		static constexpr uint32_t potionRestockTarget = playerbot::healthPotionAmmoTarget;
		static constexpr uint32_t carriedGoldReserve = 100;

		uint32_t protectedReserve(uint16_t itemId, bool food, uint16_t potionItemId) const;
		uint32_t sellQuantity(const PlayerBotEconomyInventorySnapshot& inventory, uint16_t reserve) const;
		PlayerBotEconomyRestockDecision restock(const PlayerBotEconomyInventorySnapshot& inventory,
		                                        uint32_t unitPrice, uint32_t unitWeight, uint32_t returnThreshold = potionReturnThreshold,
		                                        uint32_t restockTarget = potionRestockTarget, bool survival = false) const
		{
			if (inventory.itemCount >= restockTarget || unitPrice == 0) {
				return {};
			}
			const uint32_t targetGap = restockTarget - inventory.itemCount;
			const uint64_t totalMoney = inventory.money + inventory.bankBalance;
			// Survival restock drops the unusable-partial-reserve guard and buys
			// what the available gold affords, possibly nothing, instead of
			// failing service.
			const uint32_t requiredGap = !survival && inventory.itemCount <= returnThreshold ?
			    returnThreshold + 1 - inventory.itemCount : 0;
			if (totalMoney / unitPrice < requiredGap) {
				return {0, true};
			}
			uint32_t amount = survival ? static_cast<uint32_t>(std::min<uint64_t>(targetGap, totalMoney / unitPrice)) :
			    totalMoney / unitPrice >= targetGap ? targetGap : static_cast<uint32_t>(std::min<uint64_t>(
			    targetGap, totalMoney > carriedGoldReserve ? (totalMoney - carriedGoldReserve) / unitPrice : 0));
			if (inventory.itemCount <= returnThreshold) {
				amount = std::max(amount, static_cast<uint32_t>(std::min<uint64_t>(requiredGap, totalMoney / unitPrice)));
			}
			if (unitWeight != 0) {
				amount = std::min(amount, inventory.freeCapacity / unitWeight);
			}
			return {amount, false};
		}
		// Several supply kinds share one cash and capacity budget. Every kind's
		// floor (its safety floor, and at least one above its return threshold)
		// is funded in priority order before any kind tops up to its target.
		// Survival buys partial floors instead of failing. With one request and
		// no safety floor this equals restock().
		std::vector<PlayerBotEconomyRestockDecision> restockSupplies(
		    const std::vector<PlayerBotEconomySupplyRequest>& requests, uint32_t freeCapacity, uint64_t money,
		    uint64_t bankBalance, bool survival = false) const
		{
			std::vector<PlayerBotEconomyRestockDecision> decisions(requests.size());
			uint64_t funds = money + bankBalance;
			uint32_t capacity = freeCapacity;
			auto take = [&](size_t index, uint32_t amount) {
				const PlayerBotEconomySupplyRequest& request = requests[index];
				if (request.unitWeight != 0) amount = std::min(amount, capacity / request.unitWeight);
				decisions[index].amount += amount;
				funds -= static_cast<uint64_t>(amount) * request.unitPrice;
				capacity -= amount * request.unitWeight;
			};
			auto needed = [&](const PlayerBotEconomySupplyRequest& request) {
				return request.unitPrice != 0 && request.itemCount < request.restockTarget;
			};
			for (size_t index = 0; index < requests.size(); ++index) {
				const PlayerBotEconomySupplyRequest& request = requests[index];
				if (!needed(request)) continue;
				// An unbounded reserve keeps restock()'s unsigned arithmetic.
				const bool unbounded = request.returnThreshold == UINT32_MAX;
				const uint32_t floor = unbounded ? 0 : std::max(request.safetyFloor, request.returnThreshold + 1);
				if (!unbounded && request.itemCount >= floor) continue;
				const uint32_t requiredGap = unbounded ? request.returnThreshold + 1 - request.itemCount :
				                                         floor - request.itemCount;
				if (survival) {
					take(index, static_cast<uint32_t>(std::min<uint64_t>({requiredGap,
					    request.restockTarget - request.itemCount, funds / request.unitPrice})));
					continue;
				}
				if (funds / request.unitPrice < requiredGap) {
					decisions[index].insufficientFunds = true;
					continue;
				}
				take(index, requiredGap);
			}
			for (size_t index = 0; index < requests.size(); ++index) {
				const PlayerBotEconomySupplyRequest& request = requests[index];
				const uint32_t count = request.itemCount + decisions[index].amount;
				if (!needed(request) || decisions[index].insufficientFunds || count >= request.restockTarget) continue;
				const uint32_t gap = request.restockTarget - count;
				const uint64_t affordable = funds / request.unitPrice;
				take(index, survival ? static_cast<uint32_t>(std::min<uint64_t>(gap, affordable)) :
				    affordable >= gap ? gap : static_cast<uint32_t>(std::min<uint64_t>(
				    gap, funds > carriedGoldReserve ? (funds - carriedGoldReserve) / request.unitPrice : 0)));
			}
			return decisions;
		}
		uint32_t bankWithdrawal(const PlayerBotEconomyInventorySnapshot& inventory, uint32_t coinWeight) const;
};

#endif
