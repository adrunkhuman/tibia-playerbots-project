/** Service orchestration. Engine interaction is deliberately supplied by the controller. */
#ifndef FS_PLAYERBOTSERVICEWORKFLOW_H
#define FS_PLAYERBOTSERVICEWORKFLOW_H

#include "playerboteconomy.h"
#include "playerbotnpcsession.h"
#include "playerbotservicesession.h"

#include "creature.h"

#include <chrono>
#include <set>

enum class PlayerBotServiceStage : uint8_t { Discover, SellLoot, BuyPotions, Bank, Complete };
enum class PlayerBotServiceCommandType : uint8_t { None, Sell, BuyPotions, DepositAll, Withdraw, Complete };
enum class PlayerBotServiceOutcome : uint8_t { Pending, Success, Partial, Retry, Rejected, Unavailable };

struct PlayerBotServiceCommand {
	PlayerBotServiceCommandType type = PlayerBotServiceCommandType::None;
	uint32_t providerId = 0;
	uint16_t itemId = 0;
	uint32_t amount = 0;
};

enum class PlayerBotSlottedSaleObservation : uint8_t { Pending, Moved, Retry, Deferred };

struct PlayerBotSlottedSaleState {
	uint16_t itemId = 0;
	slots_t sourceSlot = CONST_SLOT_WHEREEVER;
	uint32_t backpackCount = 0;
	uint32_t attempts = 0;
};

class PlayerBotServiceWorkflow
{
	public:
		void reset();
		PlayerBotServiceStage stage() const { return serviceStage; }
		void setStage(PlayerBotServiceStage stage) { serviceStage = stage; }
		void resetNpc(uint32_t targetId = 0) { npcSession.reset(targetId); }
		bool npcTargets(uint32_t npcId) const { return npcSession.targets(npcId); }
		uint32_t npcTargetId() const { return npcSession.targetId(); }
		PlayerBotNpcConversationStep npcStep() const { return npcSession.step(); }
		void setNpcStep(PlayerBotNpcConversationStep step) { npcSession.setStep(step); }
		void resetNpcRetries() { npcSession.resetRetries(); }
		void resetGreetingAcknowledgement() { npcSession.resetGreetingAcknowledgement(); }
		bool isGreetingAcknowledged() const { return npcSession.isGreetingAcknowledged(); }
		bool acceptNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type);
		PlayerBotNpcSessionOutcome establishNpcFocus(Player& player, Npc& npc, uint32_t maximumRetries);
		PlayerBotNpcSessionOutcome openNpcShop(Player& player, Npc& npc, uint32_t maximumRetries);
		uint32_t npcNextDelay() const { return npcSession.nextDelay(); }

		void resetTransactions() { serviceSession.reset(); }
		bool hasShopTransaction() const { return serviceSession.hasShopTransaction(); }
		const PlayerBotServiceTransaction* shopTransaction() const { return serviceSession.shopTransaction(); }
		void beginShopTransaction(PlayerBotServiceTransaction transaction) { serviceSession.beginShopTransaction(transaction); }
		PlayerBotServiceVerification verifyShopTransaction(uint32_t itemCount, uint64_t money, uint64_t balance,
		                                                   bool purchase, uint32_t unitPrice, uint32_t maximumRetries);
		bool bankDepositComplete() const { return serviceSession.bankDepositComplete(); }
		void setBankDepositComplete(bool complete) { serviceSession.setBankDepositComplete(complete); }
		bool hasBankDeposit() const { return serviceSession.hasBankDeposit(); }
		void beginBankDeposit(uint64_t money, uint64_t balance) { serviceSession.beginBankDeposit(money, balance); }
		PlayerBotServiceVerification verifyBankDeposit(uint64_t money, uint64_t balance, uint32_t maximumRetries);
		bool hasBankWithdrawal() const { return serviceSession.hasBankWithdrawal(); }
		const PlayerBotServiceTransaction& bankTransaction() const { return serviceSession.bankTransaction(); }
		void beginBankWithdrawal(uint64_t balance, uint32_t amount) { serviceSession.beginBankWithdrawal(balance, amount); }
		PlayerBotServiceVerification verifyBankWithdrawal(uint64_t money, uint64_t balance, uint32_t maximumRetries);

		void setProviders(std::vector<PlayerBotEconomyProvider> shops, std::vector<PlayerBotEconomyProvider> bankers);
		const std::vector<PlayerBotEconomyProvider>& shops() const { return shopProviders; }
		const std::vector<PlayerBotEconomyProvider>& bankers() const { return bankProviders; }
		const PlayerBotEconomyProvider* provider(uint32_t id, bool shop) const;
		const PlayerBotEconomyProvider* rankedProvider(const PlayerBotEconomyCatalog& catalog, uint16_t itemId,
		                                               bool purchase, const Position& position) const;

		const Position& approachTarget() const { return pendingApproach; }
		void setApproachTarget(Position target) { pendingApproach = target; }
		void clearApproach() { pendingApproach = Position(); }
		bool isApproachRejected(const Position& position) const;
		void rejectApproach(const Position& position);
		void clearRejectedApproaches();

		bool hasPendingSlottedSale() const { return pendingSlottedItem != 0; }
		uint16_t pendingSlottedItemId() const { return pendingSlottedItem; }
		slots_t pendingSlottedSourceSlot() const { return pendingSlottedSlot; }
		uint32_t pendingSlottedBackpackCount() const { return pendingSlottedBackpackItems; }
		uint32_t slottedSaleAttempts() const { return slottedMoveAttempts; }
		std::optional<PlayerBotSlottedSaleState> pendingSlottedSale() const;
		void beginSlottedSale(uint16_t itemId, slots_t slot, uint32_t backpackCount);
		void clearPendingSlottedSale();
		PlayerBotSlottedSaleObservation observeSlottedSale(bool moved, uint32_t maximumAttempts,
		                                                  std::chrono::steady_clock::time_point now,
		                                                  std::chrono::steady_clock::duration cooldown);
		bool slottedSaleUnavailable(uint16_t itemId, slots_t slot, std::chrono::steady_clock::time_point now) const;
		void deferSlottedSale(uint16_t itemId, slots_t slot, std::chrono::steady_clock::time_point expires);
		std::optional<std::chrono::steady_clock::time_point> nextDeferredSlottedSale(std::chrono::steady_clock::time_point now) const;

	private:
		PlayerBotNpcSession npcSession;
		PlayerBotServiceSession serviceSession;
		PlayerBotServiceStage serviceStage = PlayerBotServiceStage::Discover;
		std::vector<PlayerBotEconomyProvider> shopProviders;
		std::vector<PlayerBotEconomyProvider> bankProviders;
		Position pendingApproach;
		std::set<Position> rejectedApproaches;
		uint16_t pendingSlottedItem = 0;
		slots_t pendingSlottedSlot = CONST_SLOT_WHEREEVER;
		uint32_t pendingSlottedBackpackItems = 0;
		uint32_t slottedMoveAttempts = 0;
		std::map<std::pair<uint16_t, slots_t>, std::chrono::steady_clock::time_point> unavailableSlottedSales;
};

#endif
