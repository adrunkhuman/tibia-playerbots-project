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
#include "playerbotdepotsession.h"
#include "playerbotequipmentpolicy.h"
#include "playerbotgoalarbiter.h"
#include "playerbothuntregions.h"
#include "playerbothuntplanningsession.h"
#include "playerbothuntpolicy.h"
#include "playerbotinventorypolicy.h"
#include "playerbotlootsession.h"
#include "playerbotnavigationruntime.h"
#include "playerbotnpcsession.h"
#include "playerbotprogressionsession.h"
#include "playerbotrecoverysession.h"
#include "playerbotspellcalibration.h"
#include "playerbotspellruntime.h"
#include "playerbottelemetry.h"
#include "playerbotservicesession.h"
#include "playerbottargetingsession.h"

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
	inline constexpr int32_t serviceGoalBaseUtility = 400;
	inline constexpr int32_t pickupRewardBaseUtility = 650;
	inline constexpr int32_t spellTrainingGoalUtility = 550;
	inline constexpr int32_t equipmentPurchaseGoalUtility = 500;
	inline constexpr int32_t magicTrainingGoalUtility = 350;
	inline constexpr int32_t economicPickupBaseUtility = 250;
	inline constexpr int32_t huntGoalUtility = 300;
	inline constexpr int32_t oracleDepartureUtility = 950;
	inline constexpr int32_t capacityServiceUtility = 900;
	inline constexpr int32_t criticalHealingServiceUtility = 1000;
	inline constexpr size_t maximumEquipmentCandidateSimulations = 16;
	inline constexpr int32_t missingPotionUtility = 15;
	inline constexpr int32_t foodPreferenceUtility = 20;
	inline constexpr int32_t sellableItemUtility = 10;
	inline constexpr uint32_t returnCapacityThreshold = 30 * 100;
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
	inline constexpr Position rookgaardTemplePosition(32097, 32219, 7);
	inline constexpr int32_t rookgaardRewardRadius = 180;
	inline constexpr Position fakeDepotPosition(32105, 32195, 8);
	inline constexpr Position fakeDepotTilePosition(32105, 32196, 8);
	inline constexpr uint32_t maximumDepotAttempts = 3;
	inline constexpr uint32_t maximumDepotDiscoveryAttempts = 4;
	// A depot scan can see several lockers with eight adjacent approach tiles each.
	// Keep expensive navigation planning bounded and resume the sorted queue next turn.
	inline constexpr uint32_t depotRouteValidationsPerDecision = 2;
	inline constexpr std::chrono::seconds depotApproachSuppression(2);
	inline constexpr uint32_t depotRetryInitialInterval = 1000;
	inline constexpr uint32_t depotRetryMaximumInterval = 4000;
	inline constexpr uint32_t depotRestartCheckpointStorage = 50096;
	inline constexpr uint32_t gameplayFixtureReadyStorage = 50099;
	inline constexpr std::array<Position, 4> huntingLoop = {{
		Position(32084, 32144, 5),
		Position(32103, 32124, 8),
		Position(32117, 32090, 9),
		Position(32103, 32124, 8),
	}};
	inline constexpr const char* botAccountName = "bot-one";

	enum class DepotRestartCheckpoint : uint8_t {
		None,
		Approach,
		Locker,
		Chest,
		Deposit,
		Depart,
	};
	enum class DepotMoveFixture : uint8_t {
		Normal,
		Partial,
		Rejected,
	};

	struct PlayerBotTestPolicy {
		bool progressionEnabled;
		bool startInHunt;
		bool fixedFixtureRoute;
		bool depotFixture;
		DepotRestartCheckpoint depotRestartCheckpoint;
		DepotMoveFixture depotMoveFixture;
		bool forceFirstHuntCandidateUnreachable;
		bool forceSecondHuntCandidateNodeLimit;
		bool cancelHuntPlanningAtScoreBarrier;
		bool forceRepeatedNavigationStepFailures;
		bool forceCorpseNavigationFailures;
		bool forcePatrolRouteFailures;
		bool suppressSlottedLootSeller;
		bool equipmentPurchasesEnabled;
		bool equipmentPurchaseFixture;
		bool forceEquipmentPurchaseRejected;
		bool pauseAfterEquipmentStorageRejection;
		bool adaptiveChallengeFixture;
		bool forceHuntScopeExhaustion;
		bool deferProgressionFixtureInitialization;
		bool spellCalibrationFixture;
		bool magicTrainingFixture;
		bool forceMagicTrainingVerificationFailure;
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
			Idle,
			Service,
			ReturnToDepot,
			DepositLoot,
			Hunt,
		};

		enum class ServiceStage : uint8_t {
			Discover,
			SellLoot,
			BuyPotions,
			Bank,
			Complete,
		};

		using TopLevelGoal = PlayerBotGoalArbiter::TopLevelGoal;
		using GoalCandidate = PlayerBotGoalArbiter::GoalCandidate;

		using EquipmentUpgrade = PlayerBotEquipmentUpgrade;
		using EquipmentLoadout = PlayerBotEquipmentLoadout;
		using EquipmentHuntSummary = PlayerBotEquipmentHuntSummary;
		using EquipmentOfferEvaluation = PlayerBotEquipmentOfferEvaluation;

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
			std::optional<EquipmentOfferEvaluation> bestEquipment;
			std::string equipmentRejection;
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

		struct ServiceNpc {
			uint32_t id;
			Position position;
		};

		enum class ScenarioStage : uint8_t {
			LootCorpse,
			Traverse,
			TraversalCombat,
			TargetPursuit,
			Stopped,
		};

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

		int32_t getFoodTicks(const Player& player) const;

		bool canEatCheese(const Player& player) const;

		bool needsHealing(const Player& player) const;
		void emitCombatReadiness(const Player& player, const Position& position, const char* result,
		                         const std::string& recovery, const std::string& terminalReason) const;
		PlayerBotEquipmentReadinessInput equipmentReadinessInput(const Player& player) const;
		bool beginReadinessEquipment(Player* player, const Position& position, const char* reason);
		void processReadinessEquipment(Player* player, const Position& position);
		bool ensureCombatReady(Player* player, const Position& position, const char* reason);

		void logHealResult(const char* result, const char* reason, const PlayerBotPotionAttempt& before,
		                   const PlayerBotPotionAttempt& after, const Position& position);

		bool handleHealing(Player* player, const Position& currentPosition);
		bool handleSpellHealing(Player* player, const Position& currentPosition);
		bool trySupportSpell(Player* player, const Position& currentPosition);
		bool tryOffensiveSpell(Player* player, const Position& currentPosition);
		bool startSpellCast(Player& player, const Position& position, const char* spellName, const char* need,
		                    Creature* target = nullptr);
		void verifySpellCast(Player& player, const Position& position);
		void emitSpellCastEvent(const Position& position, const char* spellName, const char* words, const char* role,
		                        const char* need, const char* result, const char* engineResult, const char* reason,
		                        const PlayerBotSpellPendingCast* pending, const Player* player, const char* fallback) const;

		void logEatSuccess(uint16_t itemId, uint32_t inventoryCount, int32_t foodTicks, const Position& position);

		bool handleFood(Player* player, const Position& currentPosition);

		playerbot::PlayerBotTelemetrySummary telemetrySummary() const;

		void stop(const char* reason, const Position& position);

		bool findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams);

		void setTraversalTarget(Creature* target, const Position& position);

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

		std::string rewardItemSignature(const Item& item) const;

		void inspectRewardItem(Player& player, const Item& item, uint16_t rootOrdinal,
		                       std::vector<uint16_t>& path, const std::string& rootSignature,
		                       const EquipmentLoadout& currentLoadout,
		                       const PlayerBotCombatProfile& currentProfile,
		                       const EquipmentHuntSummary& currentHunts, bool currentReady,
		                       uint32_t additionalWeight,
		                       std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
		                       size_t& simulatedItems, RewardInspection& inspection) const;

		RewardInspection inspectRewardBundle(Player& player, const Container& contents,
		                                        const EquipmentLoadout& currentLoadout,
		                                        const PlayerBotCombatProfile& currentProfile,
		                                        const EquipmentHuntSummary& currentHunts, bool currentReady,
		                                        uint32_t additionalWeight,
		                                        std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
		                                        size_t& simulatedItems) const;
		RewardInspection inspectKnownReward(Player& player, const Item& item,
		                                       const EquipmentLoadout& currentLoadout,
		                                       const PlayerBotCombatProfile& currentProfile,
		                                       const EquipmentHuntSummary& currentHunts, bool currentReady,
		                                       uint32_t additionalWeight,
		                                       std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation>& equipmentEvaluations,
		                                       size_t& simulatedItems) const;
		void finalizeRewardInspection(Player& player, RewardInspection& inspection) const;

		std::string rewardInspectionItemsJson(const RewardInspection& inspection) const;

		int32_t estimatedPickupUtility(const PlayerBotRewardPlan& reward) const;

		Container* playerBackpack(Player& player) const;

		uint32_t matchingRewardRootCount(Player& player, const std::string& signature) const;

		bool allRewardRootsAdded(Player& player) const;

		Item* findMatchingRewardRoot(Player& player, const std::string& signature) const;

		Item* resolveRewardPath(Item* root, const std::vector<uint16_t>& path, size_t length) const;

		bool prepareRewardItemAccess(Player& player, const Position& position, Item*& selectedItem, std::string& failure);

		bool isRewardPosition(const Player& player, const Position& position) const;

		bool isRewardClaimed(const Player& player, uint16_t uniqueId) const;

		bool planSimpleRewardApproach(Player& player, const Position& rewardPosition, Position& approachPosition,
		                              std::deque<PlayerBotNavigationStep>& approachSteps, uint64_t& expandedNodes);

		void emitRewardCandidate(const PlayerBotRewardPlan& candidate, const Position& position, const char* result,
		                         const char* reason = nullptr) const;

		void emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
		                          const RewardInspection& inspection, const Position& position);

		bool findPickupReward(Player& player, const Position& position, PlayerBotRewardPlan& reward,
		                      std::deque<PlayerBotNavigationStep>& rewardSteps);

		bool hasCompletedRookgaardDeparture(const Player& player) const;
		bool requiresRookgaardDeparture(const Player& player) const;

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
		const char* magicTrainingCandidateReason(const Player& player) const;
		bool magicTrainingSafe(const Player& player) const;
		const char* magicTrainingSafetyReason(const Player& player) const;
		void finishMagicTraining(Player& player, const Position& position, const char* result, const char* reason);

		uint32_t saleableItemCount(const Player& player) const;

		GoalCandidate serviceGoalCandidate(const Player& player) const;

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

		void clearNavigation();
		void resetPatrolRouteFailures();

		void beginReturn(Player* player, const Position& position, const char* reason);

		void onNpcReply(uint32_t replyingPlayerId, uint32_t npcId, uint8_t type, const std::string& text);

		void onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller);

		void beginService(Player* player, const Position& position, const char* reason);

		void finishHuntAndSelectGoal(Player* player, const Position& position, const char* reason);

		void discoverServices(const Position& position);

		bool approachServiceNpc(Player* player, ServiceNpc& service, const Position& currentPosition);

		void refreshItemValues();

		const ShopInfo* findOffer(const ServiceNpc& service, uint16_t itemId, bool buying) const;

		uint32_t serviceDistance(const Position& from, const ServiceNpc& service) const;

		ServiceNpc* findNearestService(std::vector<ServiceNpc>& services, const Position& position);

		ServiceNpc* findShopFor(uint16_t itemId, bool buying, const Position& position);

		ServiceNpc* findLootSeller(Player* player, const Position& position, uint16_t& itemId);
		bool prepareSlottedSaleItem(Player* player, uint16_t itemId, const Position& position);

		void completeServiceAction(Player* player, const char* action, const PlayerBotServiceTransaction& transaction,
		                           const Position& position);

		void processServiceShop(Player* player, const Position& currentPosition, ServiceNpc& service, const char* action,
		                        uint16_t itemId, uint32_t amount, bool purchase);

		void processBank(Player* player, const Position& currentPosition, ServiceNpc& banker);

		void processService(Player* player, const Position& currentPosition);

		Item* findNavigationItem(const PlayerBotNavigationStep& step) const;

		bool executeNavigationStep(Player* player, const PlayerBotNavigationStep& step);

		uint32_t navigationDecisionDelay(const Player& player) const;

		void onHealthDrain(const Player& player, uint32_t damage);
		void onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage);
		void onHealthGain(Creature* healer, const Creature& target, uint32_t gain);

		bool processNavigation(Player* player, const Position& currentPosition, const Position& destination);
		void adoptNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps);

		bool findDepositableItem(const Player& player, Container* container, Container*& source,
		                         Item*& depositItem, uint8_t& count) const;
		bool findDepotLocker(const Position& position, uint16_t expectedDepotId, uint16_t& lockerItemId) const;
		void clearDepotDiscovery();
		bool discoverDepot(Player& player, const Position& currentPosition);
		bool openDepotLocker(Player& player, const Position& currentPosition);
		bool openDepotChest(Player& player, const Position& currentPosition);
		bool pauseDepotFixtureForRestart(Player& player, playerbot::DepotRestartCheckpoint checkpoint,
		                                 const Position& currentPosition);
		bool openContainer(Player& player, Container& container, uint8_t containerId, const Position& currentPosition);
		uint8_t containerDestinationIndex(const Container& container, const Item& item) const;
		void processFixtureDeposit(Player* player, const Position& currentPosition);

		void emitHuntRegionCandidate(const PlayerBotHuntRegion& region, const Position& position) const;
		bool isActiveHuntCombat(const Player& player) const;
		void recordActiveHuntCombat(const Player& player);
		void recordHuntRecovery(bool potion);
		void emitChallengeFrontier(const PlayerBotHuntChallengeUpdate& update, const Position& position,
		                           const char* reason) const;
		void runAdaptiveChallengeFixture(Player& player, const Position& position);
		void runSpellCalibrationFixture(Player& player, const Position& position);
		void runMagicTrainingFixture(Player& player, const Position& position);
		void cancelHuntRegionPlanning();
		void emitHuntRegionPlanning(const PlayerBotHuntPlanningSession& planning, const Position& position, const char* phase) const;

		void finishHuntRegion(const Player& player, const Position& position, const char* reason);

		bool selectHuntRegion(Player& player, const Position& position, const char* reason);
		void beginHuntCycle(Player* player, const Position& position, const char* reason);

		void startHunt(Player* player, const Position& position, const char* reason);

		void processDeposit(Player* player, const Position& currentPosition);

		void processTraversal(Player* player, const Position& currentPosition);

		void navigate();

		void beginLoot(Player* player, const Position& currentPosition);

		void finishLoot(Player* player, const Position& currentPosition);
		void finishLootFailure(Player* player, const Position& currentPosition, const char* reason);

		Container* findCorpse(Player* player, const Position& searchPosition);

		uint8_t backpackDestinationIndex(const Container& backpack, const Item& item) const;

		bool isReplaceableCargo(const Item& item) const;

		void discardCargoForLoot(Player* player, Container* backpack, Item* incoming, uint8_t incomingCount,
		                         const Position& currentPosition);

		void verifyPendingLootMoves(Player* player, const Position& currentPosition);

		void lootCorpse(Player* player, const Position& currentPosition);

		uint32_t playerId;
		uint32_t playerGuid;
		std::string playerName;
		const playerbot::PlayerBotTestPolicy testPolicy;
		playerbot::PlayerBotTelemetry telemetry;
		Position lastPosition;
		ScenarioStage scenarioStage = ScenarioStage::Traverse;
		uint32_t fixedTargetRouteFailureCount = 0;
		uint32_t patrolRouteFailureCount = 0;
		uint64_t patrolRouteFailureExpandedNodes = 0;
		Position patrolRouteFailureTarget;
		std::chrono::steady_clock::time_point patrolRouteFailureStarted;
		uint64_t lastNavigationExpandedNodes = 0;
		uint32_t forcedNavigationStepFailuresRemaining = 0;
		uint32_t forcedNavigationPlanFailuresRemaining = 0;
		bool lastNavigationRouteUnavailable = false;
		std::map<uint16_t, uint32_t> itemSellValues;
		PlayerBotEquipmentPolicy equipmentPolicy;
		playerbot::PlayerBotInventoryPolicy inventoryPolicy;
		PlayerBotRecoverySession recoverySession;
		PlayerBotDepotSession depotSession;
		PlayerBotSpellRuntime spellRuntime;
		PlayerBotSpellCalibration spellCalibration;
		PlayerBotTargetingSession targetingSession;
		PlayerBotLootSession lootSession;
		CyclePhase cyclePhase = CyclePhase::ReturnToDepot;
		ServiceStage serviceStage = ServiceStage::Discover;
		PlayerBotProgressionSession progressionSession;
		PlayerBotRewardSession rewardSession;
		PlayerBotOracleDepartureSession departureSession;
		PlayerBotSpellTrainingSession spellTrainingSession;
		PlayerBotEquipmentPurchaseSession equipmentPurchaseSession;
		PlayerBotGoalArbiter goalArbiter;
		std::map<uint16_t, std::string> rewardInspectionFingerprints;
		uint16_t pendingReadinessItemId = 0;
		slots_t pendingReadinessSlot = CONST_SLOT_WHEREEVER;
		uint32_t pendingReadinessAttempts = 0;
		bool readinessEquipmentPending = false;
		bool readinessResumeService = false;
		std::vector<ServiceNpc> serviceShops;
		std::vector<ServiceNpc> serviceBankers;
		Position serviceApproachTarget;
		std::set<Position> serviceRejectedApproaches;
		uint16_t pendingSlottedSaleItemId = 0;
		slots_t pendingSlottedSaleSourceSlot = CONST_SLOT_WHEREEVER;
		uint32_t pendingSlottedSaleBackpackCount = 0;
		uint32_t slottedSaleMoveAttempts = 0;
		std::map<std::pair<uint16_t, slots_t>, std::chrono::steady_clock::time_point> unavailableSlottedSales;
		PlayerBotNpcSession npcSession;
		PlayerBotServiceSession serviceSession;
		size_t huntRouteIndex = 0;
		uint32_t completedCycles = 0;
		std::chrono::steady_clock::time_point huntDeadline;
		PlayerBotNavigationRuntime navigationRuntime;
		PlayerBotHuntRegionPlanner huntRegionPlanner;
		std::optional<PlayerBotHuntPlanningSession> huntRegionPlanning;
		std::optional<PlayerBotHuntRegion> activeHuntRegion;
		std::map<Position, std::chrono::steady_clock::time_point>& huntRegionCooldowns;
		PlayerBotHuntPolicy huntPolicy;
		std::chrono::steady_clock::time_point huntScopeReevaluationAfter;
		uint32_t consecutiveHuntScopeExhaustions = 0;
		std::chrono::steady_clock::time_point huntRegionStarted;
		uint64_t huntRegionStartExperience = 0;
		uint32_t huntRegionStartLevel = 0;
		bool adaptiveChallengeFixtureRun = false;
		bool huntPlanningFixtureCancelled = false;
		bool huntPlanningFixtureStaleRevisionTriggered = false;
		bool huntPlanningFixtureForcedUnreachable = false;
		bool huntPlanningFixtureForcedNodeLimit = false;
		bool fixtureInitializationPending = false;
		bool deathObserved = false;
};
#endif
