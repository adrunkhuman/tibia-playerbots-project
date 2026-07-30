/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTCONTROLLER_H
#define FS_PLAYERBOTCONTROLLER_H

// Internal controller contract shared by the responsibility-specific playerbot implementation units.

#include "playerbot.h"
#include "playerbothuntregions.h"
#include "playerbotnavigation.h"

#include "container.h"
#include "condition.h"
#include "configmanager.h"
#include "database.h"
#include "game.h"
#include "iologindata.h"
#include "item.h"
#include "monster.h"
#include "npc.h"
#include "player.h"
#include "scheduler.h"
#include "tile.h"

#include <array>
#include <ctime>
#include <optional>
#include <set>

extern Game g_game;
extern ConfigManager g_config;

namespace playerbot {
	inline constexpr uint32_t navigationInterval = 1000;
	inline constexpr uint32_t blockedRouteRetryInterval = 500;
	inline constexpr std::chrono::seconds summaryInterval(60);
	inline constexpr std::chrono::seconds repeatedEventInterval(60);
	inline constexpr uint16_t ratCorpseItemId = 5964;
	inline constexpr uint16_t meatItemId = 2666;
	inline constexpr uint16_t smallHealthPotionItemId = 8704;
	inline constexpr uint32_t minimumSmallHealthPotions = 5;
	inline constexpr int32_t healingHealthPercent = 60;
	inline constexpr uint32_t minimumMeat = 1;
	inline constexpr int32_t meatFoodTicks = 108000;
	inline constexpr int32_t maximumFoodSeconds = 1200;
	inline constexpr uint8_t corpseContainerId = 0;
	inline constexpr uint8_t backpackContainerId = 1;
	inline constexpr uint8_t rewardContainerIdBase = 2;
	inline constexpr uint8_t maximumContainerId = 0x0F;
	inline constexpr uint32_t maxCorpseSearchAttempts = 4;
	inline constexpr uint16_t ropeItemId = 2120;
	inline constexpr std::chrono::seconds traversalCombatTimeout(60);
	inline constexpr std::chrono::seconds traversalTargetSuppression(120);
	inline constexpr std::chrono::seconds navigationBlockSuppression(10);
	inline constexpr std::chrono::minutes navigationOscillationSuppression(2);
	inline constexpr std::chrono::seconds navigationStepTimeout(2);
	inline constexpr std::chrono::seconds healingRetryInterval(2);
	inline constexpr std::chrono::minutes stableLifetimeReset(5);
	inline constexpr std::chrono::minutes huntRegionCooldown(10);
	inline constexpr std::chrono::minutes pickupRewardSuccessCooldown(5);
	inline constexpr std::chrono::seconds pickupRewardFailureCooldown(60);
	// Top-level utilities are comparable arbitration scores. Baselines encode the default priority:
	// critical healing > departure > capacity service > useful rewards > ordinary service > hunting >
	// economic pickup. Dynamic service and reward adjustments may cross these baselines. Equal scores
	// retain the candidate declaration order.
	inline constexpr int32_t serviceGoalBaseUtility = 400;
	inline constexpr int32_t pickupRewardBaseUtility = 650;
	inline constexpr int32_t economicPickupBaseUtility = 250;
	inline constexpr int32_t huntGoalUtility = 300;
	inline constexpr int32_t oracleDepartureUtility = 950;
	inline constexpr int32_t capacityServiceUtility = 900;
	inline constexpr int32_t criticalHealingServiceUtility = 1000;
	inline constexpr int32_t missingPotionUtility = 15;
	inline constexpr int32_t missingFoodUtility = 20;
	inline constexpr int32_t sellableItemUtility = 10;
	inline constexpr uint32_t returnCapacityThreshold = 30 * 100;
	inline constexpr uint32_t carriedGoldReserve = 100;
	inline constexpr uint32_t maximumServiceAttempts = 3;
	inline constexpr uint32_t maximumRelogAttempts = 3;
	inline constexpr uint32_t maximumProgressionAttempts = 3;
	inline constexpr uint16_t genericQuestChestActionId = 2000;
	inline constexpr uint16_t nonContainerQuestActionId = 2001;
	inline constexpr uint16_t doubletQuestUniqueId = 56002;
	inline constexpr uint16_t doubletItemId = 2485;
	inline constexpr uint16_t oracleMinimumLevel = 8;
	inline constexpr uint16_t oracleMaximumLevel = 10;
	inline constexpr uint16_t oracleVocationId = 4;
	inline constexpr uint32_t oracleTownId = 2;
	inline constexpr uint32_t rookgaardTownId = 6;
	inline constexpr Position rookgaardTemplePosition(32097, 32219, 7);
	inline constexpr int32_t rookgaardRewardRadius = 180;
	inline constexpr Position fakeDepotPosition(32105, 32195, 8);
	inline constexpr Position fakeDepotTilePosition(32105, 32196, 8);
	inline constexpr std::array<Position, 4> huntingLoop = {{
		Position(32084, 32144, 5),
		Position(32103, 32124, 8),
		Position(32117, 32090, 9),
		Position(32103, 32124, 8),
	}};
	inline constexpr const char* botAccountName = "bot-one";

	struct PlayerBotTestPolicy {
		bool progressionEnabled;
		bool startInHunt;
		bool fixedFixtureRoute;
	};

	std::string jsonString(const std::string& value);
	std::string utcTimestamp();
	void emitPlayerbotEvent(const std::string& playerName, uint32_t playerGuid, const char* event,
	                        const Position& position, const std::string& fields = {});
	const PlayerBotTestPolicy& testPolicyFromEnvironment();
}

class PlayerBotController : public std::enable_shared_from_this<PlayerBotController>
{
	friend class PlayerBotManager;

	public:
		explicit PlayerBotController(const Player& player,
		                            std::map<Position, std::chrono::steady_clock::time_point>& sharedHuntRegionCooldowns,
		                            const playerbot::PlayerBotTestPolicy& testPolicy);

		void start(const Position& position, bool recovered, uint32_t recoveryCount);

	private:
		enum class CyclePhase : uint8_t {
			Service,
			ReturnToDepot,
			DepositLoot,
			Hunt,
		};

		enum class ServiceStage : uint8_t {
			Discover,
			SellLoot,
			BuyPotions,
			BuyMeat,
			Bank,
			Complete,
		};

		enum class ConversationStep : uint8_t {
			Greet,
			Request,
			Ready,
			Confirm,
			Verify,
		};

		enum class ProgressionObjective : uint8_t {
			None,
			PickupReward,
			OracleDeparture,
		};

		enum class TopLevelGoal : uint8_t {
			Departure,
			Service,
			PickupReward,
			Hunt,
		};

		struct GoalCandidate {
			TopLevelGoal goal;
			bool feasible;
			int32_t utility;
			std::string reason;
		};

		enum class ProgressionStage : uint8_t {
			Travel,
			UseReward,
			VerifyReward,
			EquipReward,
			VerifyEquipment,
		};

		enum class DepartureStage : uint8_t {
			Travel,
			Greet,
			ConfirmReady,
			ChooseTown,
			ChooseVocation,
			ConfirmVocation,
			Verify,
		};

		struct EquipmentUpgrade {
			slots_t slot;
			int32_t benefit;
			const char* metric;
			int32_t currentValue;
			int32_t candidateValue;
		};

		struct RewardItemInspection {
			uint16_t itemId;
			uint32_t count;
			uint32_t depth;
			uint16_t rootOrdinal;
			std::vector<uint16_t> path;
			std::vector<std::string> classes;
			uint32_t worth = 0;
			uint32_t sellValue = 0;
		};

		struct RewardInspection {
			std::vector<RewardItemInspection> items;
			std::vector<std::string> rootSignatures;
			std::vector<std::string> nonStackableRootSignatures;
			std::map<uint16_t, uint32_t> stackableRootCounts;
			std::optional<EquipmentUpgrade> bestUpgrade;
			uint16_t bestItemId = 0;
			uint16_t bestRootOrdinal = 0;
			std::vector<uint16_t> bestItemPath;
			std::string bestRootSignature;
			uint16_t primaryKnownItemId = 0;
			uint16_t primaryKnownRootOrdinal = 0;
			std::string primaryKnownRootSignature;
			uint32_t primaryKnownItemUtility = 0;
			uint32_t itemCount = 0;
			uint32_t containerCount = 0;
			uint32_t unknownCount = 0;
			uint32_t equipmentUpgradeCount = 0;
			uint32_t currencyValue = 0;
			uint32_t sellValue = 0;
			uint32_t potionCount = 0;
			uint32_t foodCount = 0;
			uint32_t ropeCount = 0;
			uint32_t shovelCount = 0;
			int32_t knownUtility = 0;
		};

		struct PickupReward {
			uint16_t uniqueId = 0;
			uint16_t itemId = 0;
			uint16_t rootItemId = 0;
			uint16_t rootOrdinal = 0;
			Position itemPosition;
			Position approachPosition;
			slots_t slot = CONST_SLOT_WHEREEVER;
			int32_t benefit = 0;
			std::string metric;
			int32_t currentValue = 0;
			int32_t candidateValue = 0;
			uint32_t travelSteps = 0;
			uint32_t estimatedDistance = 0;
			uint32_t requiredBackpackSlots = 0;
			uint32_t knownUtility = 0;
			uint32_t itemCount = 0;
			uint32_t containerCount = 0;
			uint32_t unknownCount = 0;
			uint32_t currencyValue = 0;
			uint32_t sellValue = 0;
			uint32_t equipmentUpgradeCount = 0;
			uint64_t expandedNodes = 0;
			std::vector<uint16_t> selectedItemPath;
			std::string rootSignature;
			std::vector<std::string> rootSignatures;
			std::vector<std::string> nonStackableRootSignatures;
			std::map<uint16_t, uint32_t> stackableRootCounts;
			bool resumeEquipment = false;
		};

		struct DeparturePlan {
			uint32_t npcId = 0;
			Position npcPosition;
			Position approachPosition;
			uint32_t travelSteps = 0;
			uint64_t expandedNodes = 0;
		};

		struct ServiceNpc {
			uint32_t id;
			Position position;
		};

		enum class ScenarioStage : uint8_t {
			LootCorpse,
			Traverse,
			TraversalCombat,
			Stopped,
		};

		struct CargoCandidate {
			Item* item;
			uint8_t index;
			uint32_t unitValue;
			uint32_t unitWeight;
			uint32_t availableCount;
		};

		struct Counters {
			uint64_t decisions = 0;
			uint64_t decisionTimeUs = 0;
			uint64_t pathfindingCalls = 0;
			uint64_t pathfindingFailures = 0;
			uint64_t pathfindingTimeUs = 0;
			uint64_t actionsAttempted = 0;
			uint64_t actionsFailed = 0;
			uint64_t stuckEvents = 0;
			uint64_t suppressedEvents = 0;
		};

		class DecisionTimer
		{
			public:
				explicit DecisionTimer(PlayerBotController& controller) : controller(controller)
				{
					controller.decisionStarted = std::chrono::steady_clock::now();
					controller.decisionActive = true;
					++controller.counters.decisions;
				}
				~DecisionTimer()
				{
					controller.counters.decisionTimeUs += std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - controller.decisionStarted).count();
					controller.decisionActive = false;
				}

			private:
				PlayerBotController& controller;
		};

		void schedule(uint32_t interval);

		static const char* stageName(ScenarioStage stage);

		void emit(const char* event, const Position& position, const std::string& fields = {}) const;

		void say(Player& player, const std::string& text) const;

		bool shouldEmitRepeated(const std::string& key);

		void setStage(ScenarioStage stage, const Position& position);

		void setExpectedCorpse(const Creature& target);

		void clearRatTarget(const Position& position, const char* reason);

		void logActionFailure(const char* action, const char* reason, const Position& position);

		void logLootSuccess(uint16_t itemId, uint32_t count, uint32_t inventoryCount, const Position& position);

		uint32_t getInventoryItemCount(const Player& player, uint16_t itemId) const;

		uint32_t itemUnitValue(uint16_t itemId) const;

		uint32_t protectedItemReserve(uint16_t itemId) const;

		uint32_t getSaleItemCount(const Player& player, uint16_t itemId) const;

		int32_t getFoodTicks(const Player& player) const;

		bool canEatCheese(const Player& player) const;

		bool needsHealing(const Player& player) const;

		void logHealResult(const char* result, const char* reason, int32_t healthAfter,
		                   uint32_t potionCountAfter, const Position& position);

		bool handleHealing(Player* player, const Position& currentPosition);

		void logEatSuccess(uint32_t inventoryCount, int32_t foodTicks, const Position& position);

		bool handleFood(Player* player, const Position& currentPosition);

		void logSummary(const Position& position, bool final);

		void maybeLogSummary(const Position& position);

		void stop(const char* reason, const Position& position);

		bool findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams);

		void setTraversalTarget(Creature* target, const Position& position);

		bool attackVisibleMonster(Player* player, const Position& currentPosition);

		bool attackDefensiveThreat(Player* player, const Position& currentPosition);

		void finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason);

		void processDefensiveCombat(Player* player, const Position& currentPosition);

		void finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason);

		void processTraversalCombat(Player* player, const Position& currentPosition);

		std::optional<EquipmentUpgrade> evaluateEquipmentUpgrade(const Player& player, const Item& candidate) const;

		std::string rewardItemSignature(const Item& item) const;

		void inspectRewardItem(const Player& player, const Item& item, uint16_t rootOrdinal,
		                       std::vector<uint16_t>& path, const std::string& rootSignature,
		                       RewardInspection& inspection) const;

		RewardInspection inspectRewardBundle(Player& player, const Container& contents) const;
		RewardInspection inspectKnownReward(Player& player, const Item& item) const;
		void finalizeRewardInspection(Player& player, RewardInspection& inspection) const;

		std::string rewardInspectionItemsJson(const RewardInspection& inspection) const;

		int32_t estimatedPickupUtility(const PickupReward& reward) const;

		Container* playerBackpack(Player& player) const;

		uint32_t matchingRewardRootCount(Player& player, const std::string& signature) const;

		bool allRewardRootsAdded(Player& player) const;

		Item* findMatchingRewardRoot(Player& player, const std::string& signature) const;

		Item* resolveRewardPath(Item* root, const std::vector<uint16_t>& path, size_t length) const;

		bool prepareRewardItemAccess(Player& player, const Position& position, Item*& selectedItem, std::string& failure);

		bool isRookgaardRewardPosition(const Position& position) const;

		bool isRewardClaimed(const Player& player, uint16_t uniqueId) const;

		bool planSimpleRewardApproach(Player& player, const Position& rewardPosition, Position& approachPosition,
		                              std::deque<PlayerBotNavigationStep>& approachSteps, uint64_t& expandedNodes);

		void emitRewardCandidate(const PickupReward& candidate, const Position& position, const char* result,
		                         const char* reason = nullptr) const;

		void emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
		                          const RewardInspection& inspection, const Position& position);

		bool findPickupReward(Player& player, const Position& position, PickupReward& reward,
		                      std::deque<PlayerBotNavigationStep>& rewardSteps);

		bool hasCompletedRookgaardDeparture(const Player& player) const;

		bool findOracleDeparture(Player& player, const Position& position, DeparturePlan& plan,
		                         std::deque<PlayerBotNavigationStep>& departureSteps);

		void beginOracleDeparture(Player& player, const Position& position, DeparturePlan plan,
		                          std::deque<PlayerBotNavigationStep> departureSteps);

		void processOracleDeparture(Player* player, const Position& currentPosition);

		void finishOracleDeparture(Player* player, const Position& position, const char* result, const char* reason);

		const char* topLevelGoalName(TopLevelGoal goal) const;

		uint32_t saleableItemCount(const Player& player) const;

		GoalCandidate serviceGoalCandidate(const Player& player) const;

		void emitGoalCandidate(const Player& player, const GoalCandidate& candidate, const Position& position, const char* decisionReason,
		                       const PickupReward* reward = nullptr, const DeparturePlan* departure = nullptr) const;

		void beginPickupReward(Player& player, const Position& position, PickupReward reward,
		                       std::deque<PlayerBotNavigationStep> rewardSteps);

		bool selectTopLevelGoal(Player& player, const Position& position, const char* decisionReason);

		const char* objectiveName() const;

		void finishProgressionObjective(Player* player, const Position& position, const char* result, const char* reason,
		                                bool scheduleNext = true);

		void processPickupReward(Player* player, const Position& currentPosition);

		void processProgression(Player* player, const Position& currentPosition);

		const char* cyclePhaseName() const;

		void setCyclePhase(CyclePhase phase, const Position& position, const char* reason);

		void clearNavigation();

		void beginReturn(Player* player, const Position& position, const char* reason);

		void onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text);

		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller);

		void beginService(Player* player, const Position& position, const char* reason);

		void finishHuntAndSelectGoal(Player* player, const Position& position, const char* reason);

		void discoverServices(const Position& position);

		bool approachServiceNpc(Player* player, ServiceNpc& service, const Position& currentPosition);

		void resetConversation(uint32_t targetId);

		bool openServiceShop(Player* player, ServiceNpc& service, const Position& position);

		void refreshItemValues();

		const ShopInfo* findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const;

		uint32_t serviceDistance(const Position& from, const ServiceNpc& service) const;

		ServiceNpc* findNearestService(std::vector<ServiceNpc>& services, const Position& position);

		ServiceNpc* findShopFor(uint16_t itemId, bool buying, const Position& position);

		ServiceNpc* findLootSeller(Player* player, const Position& position, uint16_t& itemId);

		void completeServiceAction(Player* player, const char* action, uint16_t itemId, uint32_t amount, const Position& position);

		void processServiceShop(Player* player, const Position& currentPosition, ServiceNpc& service, const char* action,
		                        uint16_t itemId, uint32_t amount, bool purchase);

		void processBank(Player* player, const Position& currentPosition, ServiceNpc& banker);

		void processService(Player* player, const Position& currentPosition);

		Item* findNavigationItem(const PlayerBotNavigationStep& step) const;

		bool executeNavigationStep(Player* player, const PlayerBotNavigationStep& step);

		uint32_t navigationDecisionDelay(const Player& player) const;

		void onHealthDrain(const Player& player, uint32_t damage);

		uint32_t navigationDistance(const Position& from, const Position& destination) const;

		bool detectNavigationOscillation(const Position& currentPosition, const Position& destination);

		bool processNavigation(Player* player, const Position& currentPosition, const Position& destination);

		bool isProtectedDepositItem(const Item& item) const;

		bool findDepositableItem(Container* container, Container*& source, Item*& depositItem) const;

		void emitHuntRegionCandidate(const PlayerBotHuntRegion& region, const Position& position) const;

		void finishHuntRegion(const Player& player, const Position& position, const char* reason);

		bool selectHuntRegion(Player& player, const Position& position, const char* reason);

		void startHunt(Player* player, const Position& position, const char* reason);

		void processDeposit(Player* player, const Position& currentPosition);

		void processTraversal(Player* player, const Position& currentPosition);

		void navigate();

		void beginLoot(Player* player, const Position& currentPosition);

		void finishLoot(Player* player, const Position& currentPosition);

		Container* findCorpse(Player* player, const Position& searchPosition);

		uint8_t backpackDestinationIndex(const Container& backpack, const Item& item) const;

		bool isReplaceableCargo(const Item& item) const;

		bool chooseCargoReplacement(const Container& backpack, const Item& incoming, uint32_t freeCapacity,
		                            CargoCandidate& replacement, uint8_t& replacementCount) const;

		void discardCargoForLoot(Player* player, Container* backpack, Item* incoming, const Position& currentPosition);

		void verifyPendingLootMoves(Player* player, const Position& currentPosition);

		void lootCorpse(Player* player, const Position& currentPosition);

		uint32_t playerId;
		uint32_t playerGuid;
		std::string playerName;
		const playerbot::PlayerBotTestPolicy testPolicy;
		uint32_t ratId = 0;
		uint32_t defensiveTargetId = 0;
		Position lastPosition;
		Position ratPosition;
		Position defensiveTargetPosition;
		Position lootPosition;
		ScenarioStage scenarioStage = ScenarioStage::Traverse;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t blockedStepCount = 0;
		uint32_t corpseSearchAttempts = 0;
		uint32_t corpseOpenAttempts = 0;
		uint16_t pendingLootItemId = 0;
		uint16_t pendingDiscardItemId = 0;
		uint16_t expectedCorpseItemId = 0;
		bool expectedCorpseLootable = false;
		bool lootedCurrentCorpse = false;
		uint16_t pendingDepositItemId = 0;
		uint32_t pendingLootInventoryCount = 0;
		uint8_t pendingDiscardCount = 0;
		uint32_t pendingDiscardInventoryCount = 0;
		uint32_t pendingDiscardValue = 0;
		uint16_t pendingDiscardIncomingItemId = 0;
		uint32_t pendingDepositDestinationCount = 0;
		std::set<uint16_t> unavailableLootItemIds;
		std::map<uint16_t, uint32_t> itemSellValues;
		bool pendingHeal = false;
		int32_t pendingHealHealth = 0;
		int32_t pendingHealHealthMax = 0;
		uint32_t pendingHealPotionCount = 0;
		std::chrono::steady_clock::time_point healRetryAfter;
		bool pendingEat = false;
		uint32_t pendingEatInventoryCount = 0;
		int32_t pendingEatFoodTicks = 0;
		std::chrono::steady_clock::time_point eatRetryAfter;
		std::chrono::steady_clock::time_point combatStarted;
		std::chrono::steady_clock::time_point defensiveCombatStarted;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> suppressedTraversalTargets;
		CyclePhase cyclePhase = CyclePhase::ReturnToDepot;
		ServiceStage serviceStage = ServiceStage::Discover;
		ConversationStep conversationStep = ConversationStep::Greet;
		ProgressionObjective progressionObjective = ProgressionObjective::None;
		ProgressionStage progressionStage = ProgressionStage::Travel;
		DepartureStage departureStage = DepartureStage::Travel;
		PickupReward pickupReward;
		DeparturePlan departurePlan;
		TopLevelGoal activeGoal = TopLevelGoal::Service;
		uint64_t goalDecisionId = 0;
		std::chrono::steady_clock::time_point pickupRewardCooldownUntil;
		uint32_t progressionAttempts = 0;
		uint32_t pendingRewardItemCount = 0;
		uint32_t pendingRewardRootCount = 0;
		std::map<std::string, uint32_t> pendingRewardRootCounts;
		std::map<uint16_t, uint32_t> pendingRewardStackableCounts;
		size_t pendingRewardContainerDepth = SIZE_MAX;
		uint32_t pendingRewardContainerOpenAttempts = 0;
		std::map<uint16_t, std::string> rewardInspectionFingerprints;
		uint16_t pendingEquipmentItemId = 0;
		uint32_t pendingEquipmentItemCount = 0;
		std::vector<ServiceNpc> serviceShops;
		std::vector<ServiceNpc> serviceBankers;
		uint32_t serviceTargetId = 0;
		Position serviceApproachTarget;
		std::set<Position> serviceRejectedApproaches;
		uint32_t serviceAttempts = 0;
		uint16_t serviceItemId = 0;
		uint32_t serviceAmount = 0;
		uint32_t serviceBeforeItemCount = 0;
		uint64_t serviceBeforeMoney = 0;
		uint64_t serviceBeforeBalance = 0;
		bool bankDepositComplete = false;
		bool serviceGreetingAcknowledged = false;
		size_t huntRouteIndex = 0;
		uint32_t completedCycles = 0;
		std::chrono::steady_clock::time_point huntDeadline;
		PlayerBotNavigator navigator;
		PlayerBotHuntRegionPlanner huntRegionPlanner;
		std::optional<PlayerBotHuntRegion> activeHuntRegion;
		std::map<Position, std::chrono::steady_clock::time_point>& huntRegionCooldowns;
		std::map<Position, PlayerBotHuntRegionPerformance> huntRegionPerformance;
		std::chrono::steady_clock::time_point huntRegionStarted;
		uint64_t huntRegionStartExperience = 0;
		uint32_t huntRegionStartLevel = 0;
		uint32_t huntRegionKills = 0;
		uint32_t huntRegionDamageTaken = 0;
		std::deque<PlayerBotNavigationStep> navigationSteps;
		Position navigationTarget;
		Position navigationExpectedPosition;
		Position navigationStepTarget;
		Position navigationProgressTarget;
		Position navigationProgressPrevious;
		Position navigationProgressTwoAgo;
		uint32_t navigationBestDistance = std::numeric_limits<uint32_t>::max();
		uint32_t navigationOscillationCount = 0;
		bool navigationOscillationDetected = false;
		Position blockedNavigationTarget;
		std::chrono::steady_clock::time_point navigationStepStarted;
		std::chrono::steady_clock::time_point blockedNavigationTargetExpires;
		PlayerBotNavigationStep worldChangeStep;
		std::map<Position, std::chrono::steady_clock::time_point> temporarilyBlockedPositions;
		bool navigationPending = false;
		bool worldChangePending = false;
		Counters counters;
		std::unordered_map<std::string, std::chrono::steady_clock::time_point> repeatedEventTimes;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point lastSummary = started;
		std::chrono::steady_clock::time_point decisionStarted;
		bool decisionActive = false;
		bool terminalLogged = false;
		bool deathObserved = false;
};
#endif
