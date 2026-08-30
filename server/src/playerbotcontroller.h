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
#include "playerbothuntcoordinator.h"
#include "playerbotdepotworkflow.h"
#include "playerboteconomy.h"
#include "playerbotequipmentpolicy.h"
#include "playerbotequipmentadapter.h"
#include "playerbotinventorypolicy.h"
#include "playerbotnavigationruntime.h"
#include "playerbottopology.h"
#include "playerbotprogressionruntime.h"
#include "playerbotprogressionplanners.h"
#include "playerbotsurvivalruntime.h"
#include "playerbottestpolicy.h"
#include "playerbotfixturedriver.h"
#include "playerbottelemetry.h"
#include "playerbotserviceworkflow.h"
#include "playerbotturnrouter.h"

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
	inline constexpr uint32_t huntRegionPathfindingCallsPerTurn = 1;
	inline constexpr uint32_t huntRegionScoringCandidatesPerTurn = 32;
	inline constexpr uint32_t blockedRouteRetryInterval = 500;
	inline constexpr uint16_t ratCorpseItemId = 5964;
	inline constexpr uint16_t meatItemId = 2666;
	inline constexpr int32_t healingHealthPercent = 60;
	inline constexpr int32_t meatFoodTicks = 108000;
	inline constexpr int32_t maximumFoodSeconds = 1200;
	inline constexpr uint32_t maximumEatFailures = 3;
	inline constexpr std::chrono::minutes eatFailureCooldown(5);
	inline constexpr uint8_t corpseContainerId = 0;
	inline constexpr uint8_t backpackContainerId = 1;
	inline constexpr uint8_t rewardContainerIdBase = 2;
	inline constexpr uint8_t depotSourceContainerId = 12;
	inline constexpr uint8_t depotChestContainerId = 13;
	inline constexpr uint8_t depotLockerContainerId = 14;
	inline constexpr uint8_t maximumContainerId = 0x0F;
	inline constexpr uint32_t maxCorpseSearchAttempts = 4;
	inline constexpr uint32_t maximumCorpseNavigationFailures = 6;
	inline constexpr uint32_t corpseNavigationSuspendThreshold = 3;
	inline constexpr uint32_t corpseNavigationRetryInterval = 2000;
	inline constexpr std::chrono::seconds corpseLootTimeout(20);
	inline constexpr std::chrono::seconds traversalCombatTimeout(60);
	inline constexpr std::chrono::seconds traversalTargetSuppression(120);
	inline constexpr std::chrono::seconds lostTargetPursuitTimeout(5);
	inline constexpr std::chrono::seconds lostTargetSuppression(120);
	inline constexpr uint32_t maximumLostTargetPursuitDistance = 6;
	inline constexpr uint32_t maximumTargetReacquisitionDistance = 6;
	inline constexpr std::chrono::seconds navigationBlockSuppression(10);
	inline constexpr std::chrono::minutes navigationOscillationSuppression(2);
	inline constexpr std::chrono::seconds navigationStepTimeout(2);
	inline constexpr uint32_t maximumRepeatedNavigationStepFailures = 3;
	inline constexpr uint32_t maximumPatrolRouteFailures = 3;
	inline constexpr std::chrono::seconds healingRetryInterval(2);
	inline constexpr std::chrono::minutes stableLifetimeReset(5);
	inline constexpr std::chrono::minutes huntRegionCooldown(10);
	inline constexpr std::chrono::seconds huntScopeReevaluationDelay(30);
	inline constexpr uint32_t maximumHuntScopeExhaustions = 3;
	inline constexpr std::chrono::minutes pickupRewardSuccessCooldown(5);
	inline constexpr std::chrono::seconds pickupRewardFailureCooldown(60);
	inline constexpr std::chrono::minutes spellTrainingSuccessCooldown(5);
	inline constexpr std::chrono::seconds spellTrainingFailureCooldown(60);
	inline constexpr std::chrono::minutes equipmentPurchaseSuccessCooldown(5);
	inline constexpr std::chrono::seconds equipmentPurchaseFailureCooldown(60);
	// Top-level utilities are comparable arbitration scores. Baselines encode the default priority:
	// critical healing > departure > capacity service > useful rewards > ordinary service > hunting >
	// economic pickup. Dynamic service and reward adjustments may cross these baselines. Equal scores
	// retain the candidate declaration order.
	inline constexpr int32_t pickupRewardBaseUtility = 650;
	inline constexpr int32_t spellTrainingGoalUtility = 550;
	inline constexpr int32_t equipmentPurchaseGoalUtility = 500;
	inline constexpr int32_t magicTrainingGoalUtility = 350;
	inline constexpr int32_t economicPickupBaseUtility = 250;
	inline constexpr int32_t huntGoalUtility = 300;
	inline constexpr int32_t oracleDepartureUtility = 950;
	inline constexpr size_t maximumEquipmentCandidateSimulations = 16;
	inline constexpr int32_t missingPotionUtility = 15;
	inline constexpr int32_t foodPreferenceUtility = 20;
	inline constexpr uint32_t returnCapacityThreshold = 30 * 100;
	inline constexpr std::chrono::minutes huntCapacityPressureGrace(5);
	inline constexpr uint32_t maximumServiceAttempts = 3;
	// Prevent a rejected slotted-item move from blocking the service/depot loop.
	inline constexpr std::chrono::seconds unavailableDispositionCooldown(60);
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
	inline constexpr uint32_t maximumDepotAttempts = 3;
	inline constexpr uint32_t maximumDepotDiscoveryAttempts = 4;
	// A depot scan can see several lockers with eight adjacent approach tiles each.
	// Keep expensive navigation planning bounded and resume the sorted queue next turn.
	inline constexpr uint32_t depotRouteValidationsPerDecision = 2;
	inline constexpr std::chrono::seconds depotApproachSuppression(2);
	inline constexpr uint32_t depotRetryInitialInterval = 1000;
	inline constexpr uint32_t depotRetryMaximumInterval = 4000;
	inline constexpr std::array<Position, 4> huntingLoop = {{
		Position(32084, 32144, 5),
		Position(32103, 32124, 8),
		Position(32117, 32090, 9),
		Position(32103, 32124, 8),
	}};
	inline constexpr const char* botAccountName = "bot-one";

	void emitPlayerbotEvent(const std::string& playerName, uint32_t playerGuid, const char* event,
	                        const Position& position, const std::string& fields = {});
}

class PlayerBotController : public std::enable_shared_from_this<PlayerBotController>
{
	friend class PlayerBotManager;

	public:
		explicit PlayerBotController(const Player& player,
		                            std::map<uint64_t, std::chrono::steady_clock::time_point>& sharedHuntRegionCooldowns);

		void start(const Position& position, bool recovered, uint32_t recoveryCount);

	private:
		using CyclePhase = PlayerBotCyclePhase;
		using ScenarioStage = PlayerBotScenarioStage;

		using TopLevelGoal = PlayerBotGoalArbiter::TopLevelGoal;
		using GoalCandidate = PlayerBotGoalArbiter::GoalCandidate;

		using EquipmentUpgrade = PlayerBotEquipmentUpgrade;
		using EquipmentLoadout = PlayerBotEquipmentLoadout;
		using EquipmentHuntSummary = PlayerBotEquipmentHuntSummary;
		using EquipmentOfferEvaluation = PlayerBotEquipmentOfferEvaluation;

		using RewardItemInspection = PlayerBotRewardItemInspection;
		using RewardInspection = PlayerBotRewardInspection;

		using ServiceNpc = PlayerBotEconomyProvider;

		void schedule(uint32_t interval);

		static const char* stageName(ScenarioStage stage);
		void emit(const char* event, const Position& position, const std::string& fields = {}) const
		{
			telemetry.emit(event, position, fields);
		}

		void say(Player& player, const std::string& text) const;
		bool shouldEmitRepeated(const std::string& key)
		{
			return telemetry.shouldEmitRepeated(key);
		}

		void setStage(ScenarioStage stage, const Position& position);

		PlayerBotExpectedCorpse expectedCorpseFor(const Creature& target) const;
		std::optional<PlayerBotTraversalTarget> clearTraversalTarget(const Position& position, const char* reason);
		void logActionFailure(const char* action, const char* reason, const Position& position)
		{
			telemetry.logActionFailure(action, reason, position);
		}

		void logLootSuccess(uint16_t itemId, uint32_t count, uint32_t inventoryCount, const Position& position);

		uint32_t getSaleItemCount(const Player& player, uint16_t itemId) const;
		Item* findActionableSlottedItem(const Player& player, uint16_t itemId, slots_t& slot) const;

		PlayerBotSurvivalSnapshot survivalSnapshot(const Player& player, const Creature* target = nullptr) const;
		void emitCombatReadiness(const Player& player, const Position& position, const char* result,
		                         const std::string& recovery, const std::string& terminalReason,
		                         uint32_t minimumFreeCapacity = playerbot::returnCapacityThreshold) const;
		PlayerBotEquipmentReadinessInput equipmentReadinessInput(const Player& player) const;
		bool beginReadinessEquipment(Player* player, const Position& position, const char* reason, bool resumeService = false);
		void processReadinessEquipment(Player* player, const Position& position);
		bool ensureCombatReady(Player* player, const Position& position, const char* reason, bool requireCapacity = true);

		void logHealResult(uint16_t itemId, const char* result, const char* reason, const PlayerBotPotionAttempt& before,
		                   const PlayerBotPotionAttempt& after, const Position& position);

		bool handleHealing(Player* player, const Position& currentPosition);
		bool trySupportSpell(Player* player, const Position& currentPosition);
		bool tryOffensiveSpell(Player* player, const Position& currentPosition);
		bool dispatchSpellCommand(Player& player, const Position& position, PlayerBotSurvivalCommand command);
		void verifySpellCast(Player& player, const Position& position);
		void emitSpellCastEvent(const Position& position, const char* spellName, const char* words, const char* role,
		                        const char* need, const char* result, const char* engineResult, const char* reason,
		                        const PlayerBotSpellPendingCast* pending, const Player* player, const char* fallback) const;

		void logEatSuccess(uint16_t itemId, uint32_t inventoryCount, int32_t foodTicks, const Position& position);

		bool handleFood(Player* player, const Position& currentPosition);

		playerbot::PlayerBotTelemetrySummary telemetrySummary() const;

		void stop(const char* reason, const Position& position);
		void pause(const Position& position);

		bool findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams);
		PlayerBotNavigationCostPolicy navigationCostPolicy(const Player& player) const;

		bool attackVisibleMonster(Player* player, const Position& currentPosition);

		bool attackDefensiveThreat(Player* player, const Position& currentPosition);

		void finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason);

		void processDefensiveCombat(Player* player, const Position& currentPosition);

		void finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason);

		void processTraversalCombat(Player* player, const Position& currentPosition);
		void beginTargetPursuit(Player* player, const Position& currentPosition);
		void finishTargetPursuit(const Position& currentPosition, const char* reason);
		void processTargetPursuit(Player* player, const Position& currentPosition);

		EquipmentHuntSummary equipmentHuntSummary(Player& player, const PlayerBotCombatProfile& profile) const;
		void emitEquipmentOffer(const Player& player, const EquipmentOfferEvaluation& evaluation,
		                       const PlayerBotCombatProfile& currentProfile, const EquipmentHuntSummary& currentHunts,
		                       uint64_t reserve, const Position& position, const char* result, const char* reason) const;
		std::optional<EquipmentOfferEvaluation> evaluateEquipmentOffers(Player& player, const Position& position);
		void beginEquipmentPurchase(Player& player, const Position& position, EquipmentOfferEvaluation evaluation);
		void processEquipmentPurchase(Player* player, const Position& position);
		void finishEquipmentPurchase(Player* player, const Position& position, const char* result, const char* reason);

		std::string rewardInspectionItemsJson(const RewardInspection& inspection) const;

		Container* playerBackpack(Player& player) const;

		uint32_t matchingRewardRootCount(Player& player, const std::string& signature) const;

		Item* findMatchingRewardRoot(Player& player, const std::string& signature) const;

		Item* resolveRewardPath(Item* root, const std::vector<uint16_t>& path, size_t length) const;

		PlayerBotRewardObservation::ItemAccess observeRewardItemAccess(Player& player, size_t& containerDepth) const;
		Item* rewardItemForAccess(Player& player) const;
		void openRewardBackpack(Player& player);
		void openRewardContainer(Player& player, const Position& position, size_t depth);

		bool isRewardPosition(const Player& player, const Position& position) const;

		bool isRewardClaimed(const Player& player, uint16_t uniqueId) const;

		bool planSimpleRewardApproach(Player& player, const Position& rewardPosition, Position& approachPosition,
		                              std::deque<PlayerBotNavigationStep>& approachSteps, uint64_t& expandedNodes,
		                              uint64_t maximumExpandedNodes, uint32_t& routeSteps, uint32_t& dangerCost,
		                              double& maximumDanger);

		void emitRewardCandidate(const PlayerBotRewardPlan& candidate, const Position& position, const char* result,
		                         const char* reason = nullptr) const;

		void emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
		                          const RewardInspection& inspection, const Position& position);

		bool findPickupReward(Player& player, const Position& position, PlayerBotRewardPlan& reward,
		                      std::deque<PlayerBotNavigationStep>& rewardSteps);

		PlayerBotDeparturePlannerSnapshot departureSnapshot(const Player& player) const;

		bool findOracleDeparture(Player& player, const Position& position, PlayerBotOracleDeparturePlan& plan,
		                         std::deque<PlayerBotNavigationStep>& departureSteps);
		bool forceOracleDeparture(Player& player, const Position& position, const char* decisionReason);

		void beginOracleDeparture(Player& player, const Position& position, PlayerBotOracleDeparturePlan plan,
		                          std::deque<PlayerBotNavigationStep> departureSteps);

		void processOracleDeparture(Player* player, const Position& currentPosition);

		void finishOracleDeparture(Player* player, const Position& position, const char* result, const char* reason);

		uint64_t spellTrainingReserve(const Player& player) const;
		void emitSpellCandidate(const Npc& npc, const NpcSpellOffer& offer, const Position& position, const char* result,
		                        const char* reason, uint64_t reserve = 0, uint32_t travelSteps = 0) const;
		bool findSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan& plan,
		                       std::deque<PlayerBotNavigationStep>& steps);
		void beginSpellTraining(Player& player, const Position& position, PlayerBotSpellTrainingPlan plan,
		                        std::deque<PlayerBotNavigationStep> steps);
		void finishSpellTraining(Player* player, const Position& position, const char* result, const char* reason);
		void processSpellTraining(Player* player, const Position& currentPosition);

		bool processMagicTraining(Player& player, const Position& position);
		void finishMagicTraining(Player& player, const Position& position, const char* result, const char* reason);

		uint32_t saleableItemCount(const Player& player) const;
		bool planLocalSellLoot(Player& player, Container& chest, const Position& position);
		bool processSellLootWithdrawal(Player& player, const Position& position);
		void deferSellLoot(Player& player, const Position& position, const char* reason);

		void emitGoalCandidate(const Player& player, const GoalCandidate& candidate, uint64_t decisionId, const Position& position, const char* decisionReason,
		                       const PlayerBotRewardPlan* reward = nullptr, const PlayerBotOracleDeparturePlan* departure = nullptr,
		                       const EquipmentOfferEvaluation* equipment = nullptr) const;

		void beginPickupReward(Player& player, const Position& position, PlayerBotRewardPlan reward,
		                       std::deque<PlayerBotNavigationStep> rewardSteps);

		bool selectTopLevelGoal(Player& player, const Position& position, const char* decisionReason);

		const char* objectiveName() const;

		void finishProgressionObjective(Player* player, const Position& position, const char* result, const char* reason,
		                                bool scheduleNext = true);

		void processPickupReward(Player* player, const Position& currentPosition);

		void processProgression(Player* player, const Position& currentPosition);

		const char* cyclePhaseName() const;

		void setCyclePhase(CyclePhase phase, const Position& position, const char* reason);

		void resetNavigation();

		void beginReturn(Player* player, const Position& position, const char* reason);

		void onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text);

		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller);

		void beginService(Player* player, const Position& position, const char* reason);

		void finishHuntAndReturn(Player* player, const Position& position, const char* reason);

		void refreshItemValues();

		const ShopInfo* findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const;

		uint32_t serviceDistance(const Position& from, const ServiceNpc& service) const;

		const ServiceNpc* findNearestService(const std::vector<ServiceNpc>& services, const Position& position) const;

		void processService(Player* player, const Position& currentPosition);

		Item* findNavigationItem(const PlayerBotNavigationStep& step) const;
		PlayerBotNavigationStep resolveTopologyPortal(Player& player, const PlayerBotNavigationStep& portal,
		                                                const std::set<Position>& blockedPositions) const;

		bool executeNavigationStep(Player* player, const PlayerBotNavigationStep& step);
		PlayerBotNavigationRoutePlan planNavigationRoute(Player& player, const Position& destination,
		                                                const std::set<Position>& blockedPositions = {},
		                                                uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes) const;
		PlayerBotNavigationRoutePlan planCompleteNavigationRoute(Player& player, const Position& destination,
		                                                        const std::set<Position>& blockedPositions = {},
		                                                        uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes) const;
		PlayerBotNavigationRoutePlan planCompleteNavigationRoute(Player& player, const Position& start,
		                                                        const Position& destination,
		                                                        const std::set<Position>& blockedPositions = {},
		                                                        uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes) const;
		PlayerBotNavigationRoutePlan planNavigationRoute(Player& player, const PlayerBotNavigationGoal& goal,
		                                                const std::set<Position>& blockedPositions = {},
		                                                uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes) const;
		std::optional<PlayerBotNavigationRoutePlan> planNpcTravelRoute(Player& player, const Position& destination,
		                                                               const std::set<Position>& blockedPositions,
		                                                               uint64_t maximumExpandedNodes) const;

		uint32_t navigationDecisionDelay(const Player& player) const;

		void onHealthDrain(const Player& player, uint32_t damage);
		void onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage);
		void onHealthGain(Creature* healer, const Creature& target, uint32_t gain);

		bool processNavigation(Player* player, const Position& currentPosition, const Position& destination,
		                       PlayerBotNavigationRuntimeOutcome* navigationOutcome = nullptr,
		                       uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes);
		bool processNavigation(Player* player, const Position& currentPosition, const PlayerBotNavigationGoal& goal,
		                       PlayerBotNavigationRuntimeOutcome* navigationOutcome = nullptr,
		                       uint64_t maximumExpandedNodes = playerBotNavigationMaximumExpandedNodes,
		                       bool npcApproach = false);
		bool processNpcApproach(Player* player, const Position& currentPosition, Npc* npc,
		                        const Position& coarseDestination, bool& unavailable);
		void observeNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps);

		bool findDepositableItem(const Player& player, Container* container, Container*& source,
		                         Item*& depositItem, uint8_t& count) const;
		bool findDepotLocker(const Position& position, uint16_t expectedDepotId, uint16_t& lockerItemId) const;
		bool discoverDepot(Player& player, const Position& currentPosition);
		bool openDepotLocker(Player& player, const PlayerBotDepotSnapshot& depot, const Position& currentPosition);
		bool openDepotChest(Player& player, const PlayerBotDepotSnapshot& depot, const Position& currentPosition);
		bool pauseDepotFixtureForRestart(Player& player, playerbot::DepotRestartCheckpoint checkpoint,
		                                 const Position& currentPosition);
		bool openContainer(Player& player, Container& container, uint8_t containerId, const Position& currentPosition);
		uint8_t containerDestinationIndex(const Container& container, const Item& item) const;

		void emitHuntRegionCandidate(const PlayerBotHuntRegion& region, const Position& position) const;
		bool isActiveHuntCombat(const Player& player) const;
		void recordActiveHuntCombat(const Player& player);
		void recordHuntRecovery(bool potion);
		void emitChallengeFrontier(const PlayerBotHuntChallengeUpdate& update, const Position& position,
		                           const char* reason) const;
		void emitFixtureEvents(const std::vector<playerbot::PlayerBotFixtureEvent>& events, const Position& position) const;
		void emitHuntRegionPlanning(const PlayerBotHuntPlanningSession& planning, const Position& position, const char* phase) const;
		void finishHuntRegion(const Player& player, const Position& position, const char* reason);

		bool selectHuntRegion(Player& player, const Position& position, const char* reason,
		                      std::chrono::steady_clock::duration* retryAfter = nullptr);
		void beginHuntCycle(Player* player, const Position& position, const char* reason);

		void startHunt(Player* player, const Position& position, const char* reason);

		void processDeposit(Player* player, const Position& currentPosition);

		void processTraversal(Player* player, const Position& currentPosition);

		void navigate();

		void beginLoot(Player* player, const Position& currentPosition, const PlayerBotCombatDecision& defeatedTarget);

		void finishLoot(Player* player, const Position& currentPosition);
		void finishLootFailure(Player* player, const Position& currentPosition, const char* reason);

		Container* findCorpse(Player* player, const Position& searchPosition);

		void lootCorpse(Player* player, const Position& currentPosition);

		uint32_t playerId;
		uint32_t playerGuid;
		std::string playerName;
		PlayerBotTurnRouter turnRouter;
		uint32_t scheduledTurnEvent = 0;
		uint64_t scheduledTurnGeneration = 0;
		std::chrono::steady_clock::time_point scheduledTurnDeadline;
		playerbot::PlayerBotFixtureDriver fixtureDriver;
		playerbot::PlayerBotTelemetry telemetry;
		Position lastPosition;
		PlayerBotEconomyCatalog economyCatalog;
		PlayerBotDispositionPolicy dispositionPolicy;
		PlayerBotEquipmentPolicy equipmentPolicy;
		playerbot::PlayerBotInventoryPolicy inventoryPolicy;
		PlayerBotSurvivalRuntime survivalRuntime;
		PlayerBotDepotWorkflow depotWorkflow;
		PlayerBotHuntCoordinator huntCoordinator;
		PlayerBotProgressionRuntime progressionRuntime;
		PlayerBotRewardPlanner rewardPlanner;
		PlayerBotEquipmentProviderPlanner equipmentProviderPlanner;
		size_t equipmentProviderScanOffset = 0;
		size_t equipmentOfferScanOffset = 0;
		PlayerBotSpellTrainingPlanner spellTrainingPlanner;
		size_t spellTrainerScanOffset = 0;
		PlayerBotDeparturePlanner departurePlanner;
		std::map<uint16_t, std::string> rewardInspectionFingerprints;
		PlayerBotServiceWorkflow serviceWorkflow;
		struct LocalSellLootPlan {
			uint32_t providerId = 0;
			uint16_t itemId = 0;
			uint32_t count = 0;
			uint32_t price = 0;
			uint8_t subType = 0;
			uint32_t routeSteps = 0;
			uint32_t routeDanger = 0;
			uint32_t expectedRevenue = 0;
			uint32_t liquidityUrgency = 0;
			uint32_t fare = 0;
			uint32_t roundTripRisk = 0;
			uint32_t roundTripTime = 0;
			uint32_t foregoneHuntProfit = 0;
			int32_t utility = 0;
			uint32_t scannedItems = 0;
			uint32_t routeValidations = 0;
			uint32_t withdrawn = 0;
			bool withdrawalPending = false;
			uint32_t withdrawalInventoryBefore = 0;
			uint32_t withdrawalDepotBefore = 0;
			uint8_t withdrawalRequested = 0;
		};
		std::optional<LocalSellLootPlan> sellLootPlan;
		size_t sellLootItemScanOffset = 0;
		std::optional<PlayerBotTopologyDistances> serviceTopologyDistances;
		Position serviceTopologyOrigin;
		bool serviceTopologyCanUseRope = false;
		bool serviceTopologyCanUseShovel = false;
		uint32_t serviceTopologyLevel = 0;
		uint64_t serviceTopologyGeneration = 0;
		PlayerBotNavigationRuntime navigationRuntime;
		bool huntRegionReached = false;
		uint32_t huntPotionReturnThreshold = playerbot::healthPotionReturnThreshold;
		uint32_t huntPotionRestockTarget = playerbot::healthPotionRestockTarget;
		struct {
			uint32_t npcId = 0;
			Position coarseDestination;
			std::optional<Position> localDestination;
			std::optional<Position> initialNpcPosition;
			std::optional<Position> observedNpcPosition;
			size_t replans = 0;
			bool local = false;
		} npcApproach;
		mutable std::map<std::pair<uint32_t, Position>, std::chrono::steady_clock::time_point> unavailableTravelOffers;
		bool deathObserved = false;
};

#endif
