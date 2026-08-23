#include "otpch.h"

#include "playerbotserviceworkflow.h"

void PlayerBotServiceWorkflow::reset()
{
	npcSession.reset();
	serviceSession.reset();
	serviceStage = PlayerBotServiceStage::Discover;
	shopProviders.clear();
	bankProviders.clear();
	pendingApproach = Position();
	rejectedApproaches.clear();
	clearPendingSlottedSale();
}

bool PlayerBotServiceWorkflow::acceptNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type)
{
	return npcSession.acceptReply(playerId, replyingPlayerId, npcId, type);
}

PlayerBotNpcSessionOutcome PlayerBotServiceWorkflow::establishNpcFocus(Player& player, Npc& npc, uint32_t maximumRetries)
{
	return npcSession.establishFocus(player, npc, maximumRetries);
}

PlayerBotNpcSessionOutcome PlayerBotServiceWorkflow::openNpcShop(Player& player, Npc& npc, uint32_t maximumRetries)
{
	return npcSession.openShop(player, npc, maximumRetries);
}

PlayerBotServiceVerification PlayerBotServiceWorkflow::verifyShopTransaction(uint32_t itemCount, uint64_t money,
	uint64_t balance, bool purchase, uint32_t unitPrice, uint32_t maximumRetries)
{
	return serviceSession.verifyShopTransaction(itemCount, money, balance, purchase, unitPrice, maximumRetries);
}

PlayerBotServiceVerification PlayerBotServiceWorkflow::verifyBankDeposit(uint64_t money, uint64_t balance,
	uint32_t maximumRetries)
{
	return serviceSession.verifyBankDeposit(money, balance, maximumRetries);
}

PlayerBotServiceVerification PlayerBotServiceWorkflow::verifyBankWithdrawal(uint64_t money, uint64_t balance,
	uint32_t maximumRetries)
{
	return serviceSession.verifyBankWithdrawal(money, balance, maximumRetries);
}

void PlayerBotServiceWorkflow::observeProviders(std::vector<PlayerBotEconomyProvider> shops,
	std::vector<PlayerBotEconomyProvider> bankers)
{
	shopProviders = std::move(shops);
	bankProviders = std::move(bankers);
	std::sort(shopProviders.begin(), shopProviders.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
	std::sort(bankProviders.begin(), bankProviders.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
}

const PlayerBotEconomyProvider* PlayerBotServiceWorkflow::provider(uint32_t id, bool shop) const
{
	const auto& providers = shop ? shopProviders : bankProviders;
	auto it = std::find_if(providers.begin(), providers.end(), [id](const auto& candidate) { return candidate.id == id; });
	return it == providers.end() ? nullptr : &*it;
}

const PlayerBotEconomyProvider* PlayerBotServiceWorkflow::nearestBanker(const Position& position) const
{
	auto it = std::min_element(bankProviders.begin(), bankProviders.end(), [&position](const auto& left, const auto& right) {
		const uint32_t leftDistance = std::max(Position::getDistanceX(position, left.position), Position::getDistanceY(position, left.position)) +
		                              (position.z == left.position.z ? 0 : 32 * Position::getDistanceZ(position, left.position));
		const uint32_t rightDistance = std::max(Position::getDistanceX(position, right.position), Position::getDistanceY(position, right.position)) +
		                               (position.z == right.position.z ? 0 : 32 * Position::getDistanceZ(position, right.position));
		return leftDistance < rightDistance;
	});
	return it == bankProviders.end() ? nullptr : &*it;
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::advance(const PlayerBotServiceObservation& observation,
	const PlayerBotEconomyCatalog& catalog, const PlayerBotDispositionPolicy& disposition)
{
	if (serviceStage == PlayerBotServiceStage::Failed) return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected};
	if (serviceStage == PlayerBotServiceStage::Complete) return {PlayerBotServiceCommandType::Complete, PlayerBotServiceOutcome::Success};
	if (serviceSession.hasShopTransaction()) {
		const PlayerBotServiceTransaction& transaction = *serviceSession.shopTransaction();
		return {serviceStage == PlayerBotServiceStage::SellLoot ? PlayerBotServiceCommandType::Sell : PlayerBotServiceCommandType::Buy,
		        PlayerBotServiceOutcome::Pending, npcSession.targetId(), transaction.itemId, transaction.amount};
	}
	if (serviceStage == PlayerBotServiceStage::Discover) {
		if (!observation.discoveryObserved) return {PlayerBotServiceCommandType::RequestDiscoverySnapshot};
		observeProviders(observation.shops, observation.bankers);
		if (shopProviders.empty() || bankProviders.empty()) {
			serviceStage = PlayerBotServiceStage::Failed;
			return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable};
		}
		serviceStage = PlayerBotServiceStage::SellLoot;
	}
	if (serviceStage == PlayerBotServiceStage::SellLoot) {
		const PlayerBotEconomyProvider* selected = nullptr;
		uint16_t itemId = 0;
		uint32_t price = 0;
		for (const auto& provider : shopProviders) for (const auto& offer : provider.offers) {
			auto count = observation.inventoryCounts.find(offer.itemId);
			if (offer.sellPrice == 0 || count == observation.inventoryCounts.end() || count->second == 0) continue;
			if (!selected || offer.sellPrice > price || (offer.sellPrice == price &&
			    (std::max(Position::getDistanceX(observation.currentPosition, provider.position), Position::getDistanceY(observation.currentPosition, provider.position)) <
			     std::max(Position::getDistanceX(observation.currentPosition, selected->position), Position::getDistanceY(observation.currentPosition, selected->position))))) {
				selected = &provider; itemId = offer.itemId; price = offer.sellPrice;
			}
		}
		if (!selected) { serviceStage = PlayerBotServiceStage::BuyPotions; return advance(observation, catalog, disposition); }
		const uint32_t backpack = observation.backpackSaleCounts.count(itemId) ? observation.backpackSaleCounts.at(itemId) : 0;
		return {backpack == 0 ? PlayerBotServiceCommandType::Wait : PlayerBotServiceCommandType::Sell,
		        PlayerBotServiceOutcome::Pending, selected->id, itemId, std::min<uint32_t>(100, backpack)};
	}
	if (serviceStage == PlayerBotServiceStage::BuyPotions) {
		const uint16_t itemId = PlayerBotDispositionPolicy::smallHealthPotionItemId;
		const uint32_t count = observation.inventoryCounts.count(itemId) ? observation.inventoryCounts.at(itemId) : 0;
		if (count >= PlayerBotDispositionPolicy::potionRestockTarget) { serviceStage = PlayerBotServiceStage::Bank; return advance(observation, catalog, disposition); }
		const PlayerBotEconomyProvider* provider = catalog.rankedProvider(shopProviders, itemId, true, observation.currentPosition);
		if (!provider) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		auto offer = std::find_if(provider->offers.begin(), provider->offers.end(), [itemId](const auto& candidate) { return candidate.itemId == itemId && candidate.buyPrice != 0; });
		if (offer == provider->offers.end()) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		const auto restock = disposition.restock({count, observation.freeCapacity, observation.money, observation.bankBalance}, offer->buyPrice, Item::items[itemId].weight);
		if (restock.insufficientFunds) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::InsufficientFunds}; }
		if (restock.amount == 0) { serviceStage = PlayerBotServiceStage::Bank; return advance(observation, catalog, disposition); }
		return {PlayerBotServiceCommandType::Buy, PlayerBotServiceOutcome::Pending, provider->id, itemId, restock.amount};
	}
	const PlayerBotEconomyProvider* banker = nearestBanker(observation.currentPosition);
	if (!banker) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
	if (!serviceSession.bankDepositComplete()) return {PlayerBotServiceCommandType::DepositAll, PlayerBotServiceOutcome::Pending, banker->id};
	if (!serviceSession.hasBankWithdrawal()) {
		const uint32_t amount = disposition.bankWithdrawal({0, observation.freeCapacity, observation.money, observation.bankBalance}, Item::items[ITEM_GOLD_COIN].weight);
		if (amount == 0) { serviceStage = PlayerBotServiceStage::Complete; return {PlayerBotServiceCommandType::Complete, PlayerBotServiceOutcome::Success}; }
		return {PlayerBotServiceCommandType::Withdraw, PlayerBotServiceOutcome::Pending, banker->id, 0, amount};
	}
	return {PlayerBotServiceCommandType::Withdraw, PlayerBotServiceOutcome::Pending, banker->id};
}

bool PlayerBotServiceWorkflow::isApproachRejected(const Position& position) const
{
	return rejectedApproaches.find(position) != rejectedApproaches.end();
}

void PlayerBotServiceWorkflow::rejectApproach(const Position& position)
{
	rejectedApproaches.insert(position);
}

void PlayerBotServiceWorkflow::clearRejectedApproaches()
{
	rejectedApproaches.clear();
}

void PlayerBotServiceWorkflow::beginSlottedSale(uint16_t itemId, slots_t slot, uint32_t backpackCount)
{
	pendingSlottedItem = itemId;
	pendingSlottedSlot = slot;
	pendingSlottedBackpackItems = backpackCount;
	++slottedMoveAttempts;
}

void PlayerBotServiceWorkflow::clearPendingSlottedSale()
{
	pendingSlottedItem = 0;
	pendingSlottedSlot = CONST_SLOT_WHEREEVER;
	pendingSlottedBackpackItems = 0;
	slottedMoveAttempts = 0;
}

std::optional<PlayerBotSlottedSaleState> PlayerBotServiceWorkflow::pendingSlottedSale() const
{
	if (!hasPendingSlottedSale()) {
		return std::nullopt;
	}
	return PlayerBotSlottedSaleState{pendingSlottedItem, pendingSlottedSlot, pendingSlottedBackpackItems, slottedMoveAttempts};
}

PlayerBotSlottedSaleObservation PlayerBotServiceWorkflow::observeSlottedSale(bool moved, uint32_t maximumAttempts,
	std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration cooldown)
{
	if (!hasPendingSlottedSale()) {
		return PlayerBotSlottedSaleObservation::Pending;
	}
	if (moved) {
		clearPendingSlottedSale();
		return PlayerBotSlottedSaleObservation::Moved;
	}
	const PlayerBotSlottedSaleState pending = *pendingSlottedSale();
	clearPendingSlottedSale();
	if (pending.attempts >= maximumAttempts) {
		deferSlottedSale(pending.itemId, pending.sourceSlot, now + cooldown);
		return PlayerBotSlottedSaleObservation::Deferred;
	}
	return PlayerBotSlottedSaleObservation::Retry;
}

bool PlayerBotServiceWorkflow::slottedSaleUnavailable(uint16_t itemId, slots_t slot,
	std::chrono::steady_clock::time_point now) const
{
	auto it = unavailableSlottedSales.find({itemId, slot});
	return it != unavailableSlottedSales.end() && it->second > now;
}

void PlayerBotServiceWorkflow::deferSlottedSale(uint16_t itemId, slots_t slot,
	std::chrono::steady_clock::time_point expires)
{
	unavailableSlottedSales[{itemId, slot}] = expires;
}

std::optional<std::chrono::steady_clock::time_point> PlayerBotServiceWorkflow::nextDeferredSlottedSale(
	std::chrono::steady_clock::time_point now) const
{
	std::optional<std::chrono::steady_clock::time_point> earliest;
	for (const auto& entry : unavailableSlottedSales) {
		if (entry.second > now && (!earliest || entry.second < *earliest)) {
			earliest = entry.second;
		}
	}
	return earliest;
}
