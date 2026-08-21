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

void PlayerBotServiceWorkflow::setProviders(std::vector<PlayerBotEconomyProvider> shops,
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

const PlayerBotEconomyProvider* PlayerBotServiceWorkflow::rankedProvider(const PlayerBotEconomyCatalog& catalog,
	uint16_t itemId, bool purchase, const Position& position) const
{
	return catalog.rankedProvider(shopProviders, itemId, purchase, position);
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
