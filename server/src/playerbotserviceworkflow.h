/** Service state machine. The controller supplies value observations and dispatches commands. */
#ifndef FS_PLAYERBOTSERVICEWORKFLOW_H
#define FS_PLAYERBOTSERVICEWORKFLOW_H

#include "playerboteconomy.h"
#include "playerbotnpcsession.h"
#include "playerbotservicesession.h"

#include "player.h"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>

enum class PlayerBotServiceStage : uint8_t { Discover, SellLoot, BuyPotions, Bank, Complete, Failed };
enum class PlayerBotServiceIntent : uint8_t { Resupply, LiquidateCarriedLoot };
enum class PlayerBotServiceCommandType : uint8_t {
	None, ValidateProviderRoute, NavigateProvider, Speak, Sell, Buy, DepositAll, Withdraw, OpenBackpack, MoveSlottedSale, Complete, Fail, Wait,
};
enum class PlayerBotServiceOutcome : uint8_t { Pending, Success, Partial, Retry, Rejected, Unavailable, InsufficientFunds };
enum class PlayerBotServiceRouteResult : uint8_t { NotObserved, Reached, Unreachable };

struct PlayerBotServiceSlottedItem {
	uint16_t itemId = 0;
	slots_t slot = CONST_SLOT_WHEREEVER;
	uint32_t count = 0;
};

struct PlayerBotServiceProviderObservation {
	bool available = false;
	bool inRange = false;
	bool shopOpen = false;
	bool approachesObserved = false;
	struct Approach {
		Position position;
		uint32_t distance = 0;
	};
	std::vector<Approach> approaches;
};

struct PlayerBotServiceRouteObservation {
	uint32_t providerId = 0;
	Position destination;
	PlayerBotServiceRouteResult result = PlayerBotServiceRouteResult::NotObserved;
	uint32_t steps = 0;
	uint64_t expandedNodes = 0;
	uint32_t dangerCost = 0;
	double maximumDanger = 0;
	bool requiresNpcTravel = false;
};

struct PlayerBotServiceDiscovery {
	uint32_t npcId = 0;
	std::string npcName;
	std::string capability;
	uint32_t offers = 0;
};

struct PlayerBotServiceCommand {
	PlayerBotServiceCommandType type = PlayerBotServiceCommandType::None;
	PlayerBotServiceOutcome outcome = PlayerBotServiceOutcome::Pending;
	uint32_t providerId = 0;
	uint16_t itemId = 0;
	uint32_t amount = 0;
	slots_t sourceSlot = CONST_SLOT_WHEREEVER;
	const char* speech = nullptr;
	uint8_t subType = 0;
	uint32_t attempt = 0;
	uint32_t cooldownMs = 0;
	Position destination;
	bool providerAvailable = false;
	std::optional<PlayerBotServiceTransaction> transaction;
	std::optional<PlayerBotServiceVerification> verification;
	std::vector<PlayerBotServiceDiscovery> discoveries;
};

// This is deliberately a value snapshot. It contains no Item, Container, Npc,
// or Player pointers, so workflow decisions remain reproducible in fixtures.
struct PlayerBotServiceObservation {
	Position currentPosition;
	std::vector<PlayerBotEconomyProvider> shops;
	std::vector<PlayerBotEconomyProvider> bankers;
	std::map<uint16_t, uint32_t> inventoryCounts;
	std::map<uint16_t, uint32_t> backpackSaleCounts;
	std::vector<PlayerBotServiceSlottedItem> slottedSaleItems;
	bool actionAvailable = false;
	bool backpackAvailable = false;
	bool backpackOpen = false;
	uint32_t freeCapacity = 0;
	uint64_t money = 0;
	uint64_t bankBalance = 0;
	uint32_t goldCoinWeight = 0;
	uint16_t healthPotionItemId = 0;
	uint32_t healthPotionWeight = 0;
	uint32_t healthPotionReturnThreshold = PlayerBotDispositionPolicy::potionReturnThreshold;
	uint32_t healthPotionRestockTarget = PlayerBotDispositionPolicy::potionRestockTarget;
	std::map<uint32_t, PlayerBotServiceProviderObservation> providers;
	std::vector<PlayerBotServiceDiscovery> discoveries;
	uint32_t maximumAttempts = 0;
	uint32_t slottedSaleCooldownMs = 0;
	std::chrono::steady_clock::time_point now;
	PlayerBotServiceRouteObservation approachRoute;
};

struct PlayerBotServiceSnapshot {
	PlayerBotServiceStage stage = PlayerBotServiceStage::Discover;
	uint32_t npcId = 0;
};

struct PlayerBotServiceLiquidationPlan {
	uint32_t providerId = 0;
	uint16_t itemId = 0;
	uint32_t count = 0;
	uint32_t price = 0;
	uint8_t subType = 0;
	uint32_t maximumRouteSteps = 0;
};

class PlayerBotServiceWorkflow
{
	public:
		void reset(PlayerBotServiceIntent intent = PlayerBotServiceIntent::Resupply);
		void setLiquidationPlan(PlayerBotServiceLiquidationPlan plan) { liquidationPlan = plan; }
		const std::optional<PlayerBotServiceLiquidationPlan>& liquidation() const { return liquidationPlan; }
		PlayerBotServiceStage stage() const { return serviceStage; }
		PlayerBotServiceIntent intent() const { return serviceIntent; }
		PlayerBotServiceSnapshot snapshot() const { return {serviceStage, npcSession.targetId()}; }
		const std::set<uint32_t>& unavailableProviders() const { return unavailableProviderIds; }
		void setProviderUtilityProfile(PlayerBotProviderUtilityProfile profile) { providerUtilityProfile = profile; }
		bool reportNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type);
		std::optional<Position> rejectSelectedApproach();
		PlayerBotServiceCommand advance(const PlayerBotServiceObservation& observation,
		                               const PlayerBotEconomyCatalog& catalog,
		                               const PlayerBotDispositionPolicy& disposition);

	private:
		void observeProviders(std::vector<PlayerBotEconomyProvider> shops, std::vector<PlayerBotEconomyProvider> bankers);
		const PlayerBotEconomyProvider* provider(uint32_t id, bool shop) const;
		const PlayerBotEconomyProvider* nearestBanker(const Position& position) const;
		const PlayerBotEconomyProvider* offerProvider(uint16_t itemId, bool purchase, const Position& position,
		                                             const PlayerBotEconomyCatalog& catalog) const;
		void targetProvider(uint32_t id);
		PlayerBotServiceCommand rejectCurrentProvider();
		PlayerBotServiceCommand establishNpc(const PlayerBotServiceObservation& observation, bool shop);
		PlayerBotServiceCommand verifyShop(const PlayerBotServiceObservation& observation, bool purchase);
		PlayerBotServiceCommand advanceImpl(const PlayerBotServiceObservation& observation,
		                                    const PlayerBotEconomyCatalog& catalog,
		                                    const PlayerBotDispositionPolicy& disposition);
		PlayerBotServiceCommand advanceBank(const PlayerBotServiceObservation& observation,
		                                  const PlayerBotDispositionPolicy& disposition);
		PlayerBotServiceCommand advanceSlottedSale(const PlayerBotServiceObservation& observation, uint16_t itemId);
		bool hasSlottedItem(const PlayerBotServiceObservation& observation, uint16_t itemId, slots_t slot) const;
		PlayerBotServiceCommand approachProvider(const PlayerBotServiceObservation& observation,
		                                         const PlayerBotServiceProviderObservation& provider);

		PlayerBotNpcSession npcSession;
		PlayerBotServiceSession serviceSession;
		PlayerBotProviderUtilityPolicy providerUtilityPolicy;
		PlayerBotProviderUtilityProfile providerUtilityProfile;
		PlayerBotServiceStage serviceStage = PlayerBotServiceStage::Discover;
		PlayerBotServiceIntent serviceIntent = PlayerBotServiceIntent::Resupply;
		std::vector<PlayerBotEconomyProvider> shopProviders;
		std::vector<PlayerBotEconomyProvider> bankProviders;
		uint16_t pendingSlottedItem = 0;
		slots_t pendingSlottedSlot = CONST_SLOT_WHEREEVER;
		uint32_t pendingSlottedBackpackItems = 0;
		uint32_t slottedMoveAttempts = 0;
		uint32_t shopOpenAttempts = 0;
		std::map<std::pair<uint16_t, slots_t>, std::chrono::steady_clock::time_point> unavailableSlottedSales;
		std::vector<PlayerBotServiceProviderObservation::Approach> providerApproaches;
		std::optional<Position> pendingApproachRoute;
		std::optional<Position> selectedApproach;
		std::set<Position> rejectedApproaches;
		std::set<uint32_t> unavailableProviderIds;
		std::map<uint32_t, uint32_t> providerRouteCosts;
		std::set<uint32_t> providersRequiringNpcTravel;
		std::optional<PlayerBotServiceLiquidationPlan> liquidationPlan;
};

#endif
