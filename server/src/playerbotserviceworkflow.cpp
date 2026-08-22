#include "otpch.h"

#include "playerbotserviceworkflow.h"

namespace {
	constexpr uint32_t npcReplyDelay = 1000;
}

void PlayerBotServiceWorkflow::reset()
{
	npcSession.reset();
	serviceSession.reset();
	serviceStage = PlayerBotServiceStage::Discover;
	shopProviders.clear();
	bankProviders.clear();
	pendingSlottedItem = 0;
	pendingSlottedSlot = CONST_SLOT_WHEREEVER;
	pendingSlottedBackpackItems = 0;
	slottedMoveAttempts = 0;
	unavailableSlottedSales.clear();
	providerApproaches.clear();
	pendingApproachRoute.reset();
	selectedApproach.reset();
	rejectedApproaches.clear();
}

bool PlayerBotServiceWorkflow::reportNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type)
{
	return npcSession.acceptReply(playerId, replyingPlayerId, npcId, type);
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

const PlayerBotEconomyProvider* PlayerBotServiceWorkflow::offerProvider(uint16_t itemId, bool purchase, const Position& position,
	const PlayerBotEconomyCatalog& catalog) const
{
	return catalog.rankedProvider(shopProviders, itemId, purchase, position);
}

void PlayerBotServiceWorkflow::targetProvider(uint32_t id)
{
	if (!npcSession.targets(id)) {
		npcSession.reset(id);
		serviceSession.reset();
		providerApproaches.clear();
		pendingApproachRoute.reset();
		selectedApproach.reset();
		rejectedApproaches.clear();
	}
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::approachProvider(const PlayerBotServiceObservation& observation,
	const PlayerBotServiceProviderObservation& provider)
{
	if (provider.inRange) {
		pendingApproachRoute.reset();
		selectedApproach.reset();
		return {};
	}
	if (selectedApproach) {
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::NavigateProvider, PlayerBotServiceOutcome::Pending,
		                                npcSession.targetId()};
		command.destination = *selectedApproach;
		return command;
	}
	if (providerApproaches.empty()) {
		providerApproaches = provider.approaches;
	}
	if (pendingApproachRoute) {
		if (observation.approachRoute.providerId != npcSession.targetId() ||
		    observation.approachRoute.destination != *pendingApproachRoute ||
		    observation.approachRoute.result == PlayerBotServiceRouteResult::NotObserved) {
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::ValidateProviderRoute, PlayerBotServiceOutcome::Pending,
			                                npcSession.targetId()};
			command.destination = *pendingApproachRoute;
			return command;
		}
		if (observation.approachRoute.result == PlayerBotServiceRouteResult::Reached) {
			selectedApproach = *pendingApproachRoute;
			pendingApproachRoute.reset();
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::NavigateProvider, PlayerBotServiceOutcome::Pending,
			                                npcSession.targetId()};
			command.destination = *selectedApproach;
			return command;
		}
		rejectedApproaches.insert(*pendingApproachRoute);
		pendingApproachRoute.reset();
	}
	for (const auto& candidate : providerApproaches) {
		if (rejectedApproaches.find(candidate.position) != rejectedApproaches.end()) continue;
		pendingApproachRoute = candidate.position;
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::ValidateProviderRoute, PlayerBotServiceOutcome::Pending,
		                                npcSession.targetId()};
		command.destination = candidate.position;
		return command;
	}
	serviceStage = PlayerBotServiceStage::Failed;
	return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable};
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::establishNpc(const PlayerBotServiceObservation& observation, bool shop)
{
	const auto state = observation.providers.find(npcSession.targetId());
	if (state == observation.providers.end() || !state->second.available) {
		serviceStage = PlayerBotServiceStage::Failed;
		return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable};
	}
	if (PlayerBotServiceCommand approach = approachProvider(observation, state->second); approach.type != PlayerBotServiceCommandType::None) {
		return approach;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Greet) {
		npcSession.setStep(PlayerBotNpcConversationStep::Request);
		return {PlayerBotServiceCommandType::Speak, PlayerBotServiceOutcome::Pending, npcSession.targetId(), 0, 0,
		        CONST_SLOT_WHEREEVER, "hi"};
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Request) {
		if (!npcSession.isGreetingAcknowledged()) {
			if (npcSession.retryLimitReached(observation.maximumAttempts)) {
				serviceStage = PlayerBotServiceStage::Failed;
				return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected};
			}
			npcSession.setStep(PlayerBotNpcConversationStep::Greet);
			return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Retry};
		}
		npcSession.setStep(shop ? PlayerBotNpcConversationStep::Confirm : PlayerBotNpcConversationStep::Ready);
		if (!shop) return {PlayerBotServiceCommandType::Wait};
		return {PlayerBotServiceCommandType::Speak, PlayerBotServiceOutcome::Pending, npcSession.targetId(), 0, 0,
		        CONST_SLOT_WHEREEVER, "trade"};
	}
	if (shop && npcSession.step() == PlayerBotNpcConversationStep::Confirm) {
		if (!state->second.shopOpen) {
			if (npcSession.retryLimitReached(observation.maximumAttempts)) {
				serviceStage = PlayerBotServiceStage::Failed;
				return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected};
			}
			npcSession.resetGreetingAcknowledgement();
			npcSession.setStep(PlayerBotNpcConversationStep::Greet);
			return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Retry};
		}
		npcSession.setStep(PlayerBotNpcConversationStep::Ready);
		npcSession.resetRetries();
	}
	return {PlayerBotServiceCommandType::None};
}

bool PlayerBotServiceWorkflow::hasSlottedItem(const PlayerBotServiceObservation& observation, uint16_t itemId, slots_t slot) const
{
	return std::any_of(observation.slottedSaleItems.begin(), observation.slottedSaleItems.end(), [itemId, slot](const auto& item) {
		return item.itemId == itemId && item.slot == slot && item.count != 0;
	});
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::advanceSlottedSale(const PlayerBotServiceObservation& observation, uint16_t itemId)
{
	if (pendingSlottedItem != 0) {
		const uint32_t backpack = observation.backpackSaleCounts.count(pendingSlottedItem) ? observation.backpackSaleCounts.at(pendingSlottedItem) : 0;
		const bool moved = !hasSlottedItem(observation, pendingSlottedItem, pendingSlottedSlot) && backpack > pendingSlottedBackpackItems;
		if (moved) {
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Success};
			command.itemId = pendingSlottedItem;
			command.sourceSlot = pendingSlottedSlot;
			command.providerAvailable = true;
			pendingSlottedItem = 0;
			pendingSlottedSlot = CONST_SLOT_WHEREEVER;
			slottedMoveAttempts = 0;
			return command;
		}
		if (slottedMoveAttempts >= observation.maximumAttempts) {
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Unavailable};
			command.itemId = pendingSlottedItem;
			command.sourceSlot = pendingSlottedSlot;
			command.cooldownMs = observation.slottedSaleCooldownMs;
			command.providerAvailable = true;
			unavailableSlottedSales[{pendingSlottedItem, pendingSlottedSlot}] = observation.now +
			    std::chrono::milliseconds(observation.slottedSaleCooldownMs);
			pendingSlottedItem = 0;
			pendingSlottedSlot = CONST_SLOT_WHEREEVER;
			slottedMoveAttempts = 0;
			serviceStage = PlayerBotServiceStage::BuyPotions;
			return command;
		}
	}
	for (const auto& item : observation.slottedSaleItems) {
		if (item.itemId != itemId || item.count == 0) continue;
		auto unavailable = unavailableSlottedSales.find({item.itemId, item.slot});
		if (unavailable != unavailableSlottedSales.end() && unavailable->second > observation.now) continue;
		pendingSlottedItem = item.itemId;
		pendingSlottedSlot = item.slot;
		pendingSlottedBackpackItems = observation.backpackSaleCounts.count(itemId) ? observation.backpackSaleCounts.at(itemId) : 0;
		++slottedMoveAttempts;
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::MoveSlottedSale, PlayerBotServiceOutcome::Pending, 0,
		                                item.itemId, item.count, item.slot};
		command.attempt = slottedMoveAttempts;
		command.providerAvailable = true;
		return command;
	}
	return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Unavailable};
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::verifyShop(const PlayerBotServiceObservation& observation, bool purchase)
{
	const PlayerBotServiceTransaction* transaction = serviceSession.shopTransaction();
	if (!transaction) return {PlayerBotServiceCommandType::None};
	const PlayerBotEconomyProvider* shop = provider(npcSession.targetId(), true);
	if (!shop) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
	auto offer = std::find_if(shop->offers.begin(), shop->offers.end(), [transaction](const auto& value) { return value.itemId == transaction->itemId; });
	if (offer == shop->offers.end()) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
	const uint32_t count = observation.inventoryCounts.count(transaction->itemId) ? observation.inventoryCounts.at(transaction->itemId) : 0;
	const PlayerBotServiceVerification result = serviceSession.verifyShopTransaction(count, observation.money, observation.bankBalance,
		purchase, purchase ? offer->buyPrice : offer->sellPrice, observation.maximumAttempts);
	if (result.result == PlayerBotServiceVerificationResult::Success) {
		npcSession.setStep(PlayerBotNpcConversationStep::Ready);
		npcSession.resetRetries();
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Success};
		command.transaction = result.before;
		command.verification = result;
		return command;
	}
	if (result.result == PlayerBotServiceVerificationResult::Retry) return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Retry};
	serviceStage = PlayerBotServiceStage::Failed;
	return {PlayerBotServiceCommandType::Fail, result.result == PlayerBotServiceVerificationResult::Mismatch ?
	        PlayerBotServiceOutcome::Rejected : PlayerBotServiceOutcome::Unavailable};
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::advanceBank(const PlayerBotServiceObservation& observation,
	const PlayerBotDispositionPolicy& disposition)
{
	const PlayerBotEconomyProvider* banker = nearestBanker(observation.currentPosition);
	if (!banker) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
	targetProvider(banker->id);
	PlayerBotServiceCommand focus = establishNpc(observation, false);
	if (focus.type != PlayerBotServiceCommandType::None) return focus;
	if (!serviceSession.bankDepositComplete()) {
		if (!serviceSession.hasBankDeposit()) serviceSession.beginBankDeposit(observation.money, observation.bankBalance);
		if (serviceSession.bankTransaction().money == 0) {
			serviceSession.markBankDepositComplete();
			return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Success};
		}
		if (npcSession.step() == PlayerBotNpcConversationStep::Ready) {
			npcSession.setStep(PlayerBotNpcConversationStep::Confirm);
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::DepositAll, PlayerBotServiceOutcome::Pending, banker->id};
			command.transaction = serviceSession.bankTransaction();
			return command;
		}
		if (npcSession.step() == PlayerBotNpcConversationStep::Confirm) {
			npcSession.setStep(PlayerBotNpcConversationStep::Verify);
			return {PlayerBotServiceCommandType::Speak, PlayerBotServiceOutcome::Pending, banker->id, 0, 0, CONST_SLOT_WHEREEVER, "yes"};
		}
		const PlayerBotServiceVerification result = serviceSession.verifyBankDeposit(observation.money, observation.bankBalance, observation.maximumAttempts);
		if (result.result == PlayerBotServiceVerificationResult::Success) {
			serviceSession.markBankDepositComplete();
			npcSession.setStep(PlayerBotNpcConversationStep::Ready);
			PlayerBotServiceCommand command{PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Success};
			command.transaction = result.before;
			command.verification = result;
			return command;
		}
		if (result.result == PlayerBotServiceVerificationResult::Rejected) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected}; }
		return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Retry};
	}
	if (!serviceSession.hasBankWithdrawal()) {
		const uint32_t amount = disposition.bankWithdrawal({0, observation.freeCapacity, observation.money, observation.bankBalance}, observation.goldCoinWeight);
		if (amount == 0) { serviceStage = PlayerBotServiceStage::Complete; return {PlayerBotServiceCommandType::Complete, PlayerBotServiceOutcome::Success}; }
		serviceSession.beginBankWithdrawal(observation.bankBalance, amount);
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Ready) {
		npcSession.setStep(PlayerBotNpcConversationStep::Confirm);
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::Withdraw, PlayerBotServiceOutcome::Pending, banker->id, 0,
		                                serviceSession.bankTransaction().amount};
		command.transaction = serviceSession.bankTransaction();
		return command;
	}
	if (npcSession.step() == PlayerBotNpcConversationStep::Confirm) {
		npcSession.setStep(PlayerBotNpcConversationStep::Verify);
		return {PlayerBotServiceCommandType::Speak, PlayerBotServiceOutcome::Pending, banker->id, 0, 0, CONST_SLOT_WHEREEVER, "yes"};
	}
	const PlayerBotServiceVerification result = serviceSession.verifyBankWithdrawal(observation.money, observation.bankBalance, observation.maximumAttempts);
	if (result.result == PlayerBotServiceVerificationResult::Success) {
		serviceStage = PlayerBotServiceStage::Complete;
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::Complete, PlayerBotServiceOutcome::Success};
		command.transaction = result.before;
		command.verification = result;
		return command;
	}
	if (result.result == PlayerBotServiceVerificationResult::Rejected) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected}; }
	return {PlayerBotServiceCommandType::Wait, PlayerBotServiceOutcome::Retry};
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::advance(const PlayerBotServiceObservation& observation,
	const PlayerBotEconomyCatalog& catalog, const PlayerBotDispositionPolicy& disposition)
{
	const bool discovered = serviceStage == PlayerBotServiceStage::Discover;
	PlayerBotServiceCommand command = advanceImpl(observation, catalog, disposition);
	if (discovered) command.discoveries = observation.discoveries;
	return command;
}

PlayerBotServiceCommand PlayerBotServiceWorkflow::advanceImpl(const PlayerBotServiceObservation& observation,
	const PlayerBotEconomyCatalog& catalog, const PlayerBotDispositionPolicy& disposition)
{
	if (serviceStage == PlayerBotServiceStage::Failed) return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Rejected};
	if (serviceStage == PlayerBotServiceStage::Complete) return {PlayerBotServiceCommandType::Complete, PlayerBotServiceOutcome::Success};
	observeProviders(observation.shops, observation.bankers);
	if (serviceStage == PlayerBotServiceStage::Discover) {
		if (shopProviders.empty() || bankProviders.empty()) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		serviceStage = PlayerBotServiceStage::SellLoot;
	}
	if (serviceStage == PlayerBotServiceStage::SellLoot) {
		const PlayerBotEconomyProvider* selected = nullptr;
		uint16_t itemId = 0;
		uint32_t price = 0;
		for (const auto& shop : shopProviders) for (const auto& offer : shop.offers) {
			auto count = observation.inventoryCounts.find(offer.itemId);
			if (offer.sellPrice == 0 || count == observation.inventoryCounts.end() || count->second == 0) continue;
			if (!selected || offer.sellPrice > price) { selected = &shop; itemId = offer.itemId; price = offer.sellPrice; }
		}
		if (!selected) { serviceStage = PlayerBotServiceStage::BuyPotions; return advanceImpl(observation, catalog, disposition); }
		targetProvider(selected->id);
		PlayerBotServiceCommand focus = establishNpc(observation, true);
		if (focus.type != PlayerBotServiceCommandType::None) return focus;
		const uint32_t backpack = observation.backpackSaleCounts.count(itemId) ? observation.backpackSaleCounts.at(itemId) : 0;
		if (backpack == 0) return advanceSlottedSale(observation, itemId);
		if (serviceSession.hasShopTransaction()) return verifyShop(observation, false);
		auto offer = std::find_if(selected->offers.begin(), selected->offers.end(), [itemId](const auto& value) {
			return value.itemId == itemId && value.sellPrice != 0;
		});
		if (offer == selected->offers.end()) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		const PlayerBotServiceTransaction transaction{itemId, std::min<uint32_t>(100, backpack), observation.inventoryCounts.at(itemId), observation.money, observation.bankBalance};
		serviceSession.beginShopTransaction(transaction);
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::Sell, PlayerBotServiceOutcome::Pending, selected->id, itemId, transaction.amount};
		command.subType = offer->subType;
		command.transaction = transaction;
		return command;
	}
	if (serviceStage == PlayerBotServiceStage::BuyPotions) {
		const uint16_t itemId = PlayerBotDispositionPolicy::smallHealthPotionItemId;
		const uint32_t count = observation.inventoryCounts.count(itemId) ? observation.inventoryCounts.at(itemId) : 0;
		if (count >= PlayerBotDispositionPolicy::potionRestockTarget) { serviceStage = PlayerBotServiceStage::Bank; return advanceImpl(observation, catalog, disposition); }
		const PlayerBotEconomyProvider* selected = offerProvider(itemId, true, observation.currentPosition, catalog);
		if (!selected) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		auto offer = std::find_if(selected->offers.begin(), selected->offers.end(), [itemId](const auto& value) { return value.itemId == itemId && value.buyPrice != 0; });
		if (offer == selected->offers.end()) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::Unavailable}; }
		const auto restock = disposition.restock({count, observation.freeCapacity, observation.money, observation.bankBalance}, offer->buyPrice,
		                                         observation.smallHealthPotionWeight);
		if (restock.insufficientFunds) { serviceStage = PlayerBotServiceStage::Failed; return {PlayerBotServiceCommandType::Fail, PlayerBotServiceOutcome::InsufficientFunds}; }
		if (restock.amount == 0) { serviceStage = PlayerBotServiceStage::Bank; return advanceImpl(observation, catalog, disposition); }
		targetProvider(selected->id);
		PlayerBotServiceCommand focus = establishNpc(observation, true);
		if (focus.type != PlayerBotServiceCommandType::None) return focus;
		if (serviceSession.hasShopTransaction()) return verifyShop(observation, true);
		const PlayerBotServiceTransaction transaction{itemId, restock.amount, count, observation.money, observation.bankBalance};
		serviceSession.beginShopTransaction(transaction);
		PlayerBotServiceCommand command{PlayerBotServiceCommandType::Buy, PlayerBotServiceOutcome::Pending, selected->id, itemId, restock.amount};
		command.subType = offer->subType;
		command.transaction = transaction;
		return command;
	}
	return advanceBank(observation, disposition);
}
