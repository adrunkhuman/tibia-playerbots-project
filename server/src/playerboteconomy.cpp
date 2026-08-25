#include "otpch.h"

#include "playerboteconomy.h"

#include "playerbotinventorypolicy.h"

namespace {
	uint32_t providerDistance(const Position& from, const Position& to)
	{
		return std::max(Position::getDistanceX(from, to), Position::getDistanceY(from, to)) +
		       (from.z == to.z ? 0 : 32 * Position::getDistanceZ(from, to));
	}
}

int64_t PlayerBotProviderUtilityPolicy::score(uint64_t serviceValue, uint32_t estimatedRouteCost,
	const PlayerBotProviderUtilityProfile& profile) const
{
	return static_cast<int64_t>(serviceValue) * profile.serviceValueWeight -
	       static_cast<int64_t>(estimatedRouteCost) * profile.routeCostWeight;
}

void PlayerBotEconomyCatalog::learn(const std::vector<PlayerBotEconomyProvider>& providers)
{
	learnedSellValues.clear();
	for (const PlayerBotEconomyProvider& provider : providers) {
		for (const PlayerBotEconomyOffer& offer : provider.offers) {
			if (offer.sellPrice != 0) {
				learnedSellValues[offer.itemId] = std::max(learnedSellValues[offer.itemId], offer.sellPrice);
			}
		}
	}
}

uint32_t PlayerBotEconomyCatalog::sellValue(uint16_t itemId) const
{
	auto it = learnedSellValues.find(itemId);
	return it == learnedSellValues.end() ? 0 : it->second;
}

const PlayerBotEconomyProvider* PlayerBotEconomyCatalog::rankedProvider(const std::vector<PlayerBotEconomyProvider>& providers,
	uint16_t itemId, bool purchase, const Position& from) const
{
	const PlayerBotEconomyProvider* selected = nullptr;
	uint32_t selectedPrice = 0;
	for (const PlayerBotEconomyProvider& provider : providers) {
		auto offer = std::find_if(provider.offers.begin(), provider.offers.end(), [itemId, purchase](const auto& candidate) {
			return candidate.itemId == itemId && (purchase ? candidate.buyPrice != 0 : candidate.sellPrice != 0);
		});
		if (offer == provider.offers.end()) {
			continue;
		}
		const uint32_t price = purchase ? offer->buyPrice : offer->sellPrice;
		if (!selected || (!purchase && price > selectedPrice) ||
		    (!purchase && price == selectedPrice && providerDistance(from, provider.position) < providerDistance(from, selected->position)) ||
		    (!purchase && price == selectedPrice && providerDistance(from, provider.position) == providerDistance(from, selected->position) && provider.id < selected->id) ||
		    (purchase && providerDistance(from, provider.position) < providerDistance(from, selected->position)) ||
		    (purchase && providerDistance(from, provider.position) == providerDistance(from, selected->position) && provider.id < selected->id)) {
			selected = &provider;
			selectedPrice = price;
		}
	}
	return selected;
}

uint32_t PlayerBotDispositionPolicy::protectedReserve(uint16_t itemId, bool food, uint16_t potionItemId) const
{
	if (itemId == 2120 || itemId == 2554) {
		return 1;
	}
	if (food) {
		return 2;
	}
	return itemId == potionItemId ? potionRestockTarget : 0;
}

uint32_t PlayerBotDispositionPolicy::sellQuantity(const PlayerBotEconomyInventorySnapshot& inventory, uint16_t reserve) const
{
	return inventory.itemCount > reserve ? inventory.itemCount - reserve : 0;
}

PlayerBotEconomyRestockDecision PlayerBotDispositionPolicy::restock(const PlayerBotEconomyInventorySnapshot& inventory,
	uint32_t unitPrice, uint32_t unitWeight) const
{
	if (inventory.itemCount >= potionRestockTarget || unitPrice == 0) {
		return {};
	}
	const uint32_t targetGap = potionRestockTarget - inventory.itemCount;
	const uint64_t totalMoney = inventory.money + inventory.bankBalance;
	const uint32_t requiredGap = inventory.itemCount <= potionReturnThreshold ? potionReturnThreshold + 1 - inventory.itemCount : 0;
	if (totalMoney / unitPrice < requiredGap) {
		return {0, true};
	}
	uint32_t amount = totalMoney / unitPrice >= targetGap ? targetGap : static_cast<uint32_t>(std::min<uint64_t>(
		targetGap, totalMoney > carriedGoldReserve ? (totalMoney - carriedGoldReserve) / unitPrice : 0));
	if (inventory.itemCount <= potionReturnThreshold) {
		amount = std::max(amount, static_cast<uint32_t>(std::min<uint64_t>(requiredGap, totalMoney / unitPrice)));
	}
	if (unitWeight != 0) {
		amount = std::min(amount, inventory.freeCapacity / unitWeight);
	}
	return {amount, false};
}

uint32_t PlayerBotDispositionPolicy::bankWithdrawal(const PlayerBotEconomyInventorySnapshot& inventory, uint32_t coinWeight) const
{
	uint32_t amount = static_cast<uint32_t>(std::min<uint64_t>(carriedGoldReserve, inventory.bankBalance));
	if (coinWeight != 0) {
		amount = std::min(amount, inventory.freeCapacity / coinWeight);
	}
	return amount;
}
