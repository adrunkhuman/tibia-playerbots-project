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
#include "playerbotspellcalibration.h"

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
	inline constexpr std::chrono::seconds summaryInterval(60);
	inline constexpr std::chrono::seconds repeatedEventInterval(60);
	inline constexpr uint16_t ratCorpseItemId = 5964;
	inline constexpr uint16_t meatItemId = 2666;
	inline constexpr uint16_t smallHealthPotionItemId = 8704;
	inline constexpr uint32_t smallHealthPotionReturnThreshold = 1;
	inline constexpr uint32_t smallHealthPotionRestockTarget = 10;
	inline constexpr int32_t healingHealthPercent = 60;
	inline constexpr uint32_t preferredFoodCount = 2;
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
	inline constexpr uint16_t ropeItemId = 2120;
	inline constexpr std::chrono::seconds traversalCombatTimeout(60);
	inline constexpr std::chrono::seconds traversalTargetSuppression(120);
	inline constexpr std::chrono::seconds lostTargetPursuitTimeout(5);
	inline constexpr std::chrono::seconds lostTargetSuppression(10);
	inline constexpr uint32_t maximumLostTargetPursuitDistance = 6;
	inline constexpr uint32_t maximumTargetReacquisitionDistance = 6;
	inline constexpr std::chrono::seconds navigationBlockSuppression(10);
	inline constexpr std::chrono::minutes navigationOscillationSuppression(2);
	inline constexpr std::chrono::seconds navigationStepTimeout(2);
	inline constexpr uint32_t maximumRepeatedNavigationStepFailures = 3;
	inline constexpr std::chrono::seconds healingRetryInterval(2);
	inline constexpr std::chrono::minutes stableLifetimeReset(5);
	inline constexpr std::chrono::minutes huntRegionCooldown(10);
	inline constexpr std::chrono::seconds huntScopeReevaluationDelay(30);
	inline constexpr uint32_t maximumHuntScopeExhaustions = 3;
	inline constexpr double initialChallengeFrontier = 0.20;
	inline constexpr double minimumChallengeFrontier = 0.10;
	inline constexpr double maximumChallengeFrontier = 0.40;
	inline constexpr double challengeEscalation = 0.025;
	inline constexpr double challengeBackoff = 0.05;
	inline constexpr double challengeHealthSafetyPercent = 85;
	inline constexpr double minimumChallengeActiveSeconds = 30;
	inline constexpr uint32_t minimumChallengeKills = 1;
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
	inline constexpr uint32_t maximumDepotAttempts = 3;
	inline constexpr uint32_t maximumDepotDiscoveryAttempts = 4;
	// A depot scan can see several lockers with eight adjacent approach tiles each.
	// Keep expensive navigation planning bounded and resume the sorted queue next turn.
	inline constexpr uint32_t depotRouteValidationsPerDecision = 2;
	inline constexpr std::chrono::seconds depotApproachSuppression(2);
	inline constexpr uint32_t depotRetryInitialInterval = 1000;
	inline constexpr uint32_t depotRetryMaximumInterval = 4000;
	inline constexpr uint32_t depotRestartCheckpointStorage = 50096;
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
		bool equipmentPurchasesEnabled;
		bool forceEquipmentPurchaseRejected;
		bool adaptiveChallengeFixture;
		bool forceHuntScopeExhaustion;
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
			LearnSpell,
			BuyEquipment,
		};

		enum class TopLevelGoal : uint8_t {
			Departure,
			Service,
			PickupReward,
			LearnSpell,
			BuyEquipment,
			MagicTraining,
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

		enum class SpellTrainingStage : uint8_t {
			Travel,
			Greet,
			Request,
			Confirm,
			Verify,
		};

		struct EquipmentUpgrade {
			slots_t slot;
			int32_t benefit;
			const char* metric;
			int32_t currentValue;
			int32_t candidateValue;
		};

		struct EquipmentLoadout {
			std::array<uint16_t, CONST_SLOT_LAST + 1> itemIds{};
		};

		struct EquipmentHuntSummary {
			uint32_t suitableRegions = 0;
			double bestProjectedExperience = 0;
			double lowestThreatRatio = 0;
			uint32_t evaluatedRegions = 0;
			bool truncated = false;
		};

		enum class EquipmentDecisionRule : uint8_t {
			None,
			ParetoImprovement,
			UnlocksHunt,
			ReadinessRepair,
		};

		struct EquipmentOfferEvaluation {
			uint32_t npcId = 0;
			Position npcPosition;
			Position approachPosition;
			uint16_t itemId = 0;
			uint32_t price = 0;
			slots_t slot = CONST_SLOT_WHEREEVER;
			uint16_t replacedItemId = 0;
			uint16_t displacedLeftItemId = 0;
			uint16_t displacedRightItemId = 0;
			PlayerBotCombatProfile profile;
			EquipmentHuntSummary hunts;
			bool currentReady = false;
			bool candidateReady = false;
			bool carried = false;
			bool simulated = false;
			std::string rejection;
			EquipmentDecisionRule rule = EquipmentDecisionRule::None;
			uint32_t travelSteps = 0;
		};

		enum class EquipmentPurchaseStage : uint8_t {
			Travel,
			Purchase,
			VerifyPurchase,
			Equip,
			VerifyEquipment,
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
			uint16_t replacedItemId = 0;
			uint16_t displacedLeftItemId = 0;
			uint16_t displacedRightItemId = 0;
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

		struct SpellTrainingPlan {
			uint32_t npcId = 0;
			Position npcPosition;
			Position approachPosition;
			std::string spellName;
			std::string keyword;
			uint32_t price = 0;
			uint32_t level = 0;
			uint32_t travelSteps = 0;
			uint64_t reserve = 0;
			uint64_t moneyBefore = 0;
		};

		struct PendingSpellCast {
			std::string name;
			std::string role;
			std::string need;
			uint32_t manaBefore = 0;
			uint32_t manaReserve = 0;
			int32_t healthBefore = 0;
			uint32_t targetId = 0;
			int32_t targetHealthBefore = 0;
			int32_t missingHealth = 0;
			int32_t hasteTicksBefore = 0;
			int32_t hasteTicksAfterCast = 0;
			int32_t hasteTicksObserved = 0;
			int32_t hasteDurationMeasured = 0;
			int64_t hasteEndTimeAfterCast = 0;
			PlayerBotSpellEnvelope envelope;
			std::string targetClass;
			uint32_t observedSpellHealing = 0;
			uint32_t observedSpellDamage = 0;
			bool concurrentDamage = false;
			bool otherRecovery = false;
			bool otherAttacker = false;
			bool meleeOrOtherBotDamage = false;
			std::array<uint32_t, 4> spellVictimIds{};
			uint8_t spellVictimCount = 0;
			bool spellVictimOverflow = false;
			std::chrono::steady_clock::time_point observedAt;
		};

		enum class ScenarioStage : uint8_t {
			LootCorpse,
			Traverse,
			TraversalCombat,
			TargetPursuit,
			Stopped,
		};

		struct CargoCandidate {
			Item* item;
			Container* source;
			uint8_t index;
			uint32_t unitValue;
			uint32_t unitWeight;
			uint32_t availableCount;
		};

		struct FoodInventory {
			uint32_t count = 0;
			uint32_t weight = 0;
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

		enum class DepotStage : uint8_t {
			Discover,
			Approach,
			OpenLocker,
			OpenChest,
			Deposit,
			VerifyMove,
			Depart,
		};

		struct HuntRegionPlanning {
			enum class Phase : uint8_t {
				Scoring,
				Reachability,
			};

			std::vector<PlayerBotHuntRegion> regions;
			std::vector<size_t> candidateIndices;
			std::string reason;
			std::chrono::steady_clock::time_point started;
			size_t nextCandidate = 0;
			size_t nextScoringCandidate = 0;
			Phase phase = Phase::Scoring;
			uint32_t pathfindingCalls = 0;
			uint32_t batchPathfindingCalls = 0;
			uint64_t expandedNodes = 0;
			uint32_t yields = 0;
			uint32_t suitableCandidates = 0;
			uint32_t scoredCandidates = 0;
			uint32_t totalCandidates = 0;
			bool cacheHit = false;
			uint64_t snapshotTimeUs = 0;
			uint64_t clusteringTimeUs = 0;
			uint64_t scoringTimeUs = 0;
			Position playerPosition;
			uint32_t playerLevel = 0;
			uint16_t staminaMinutes = 0;
			bool fixtureForcedUnreachable = false;
			bool fixtureForcedNodeLimit = false;
			uint64_t cacheRevision = 0;
			std::set<Position> excludedRegions;
			PlayerBotHuntPlanningProfile profile;
		};

		struct DepotCandidate {
			uint16_t depotId = 0;
			uint16_t lockerItemId = 0;
			Position lockerPosition;
			Position approachPosition;
			uint32_t distance = 0;
		};

		struct ChallengeFrontier {
			double target = playerbot::initialChallengeFrontier;
			uint8_t qualifyingHuntsToHold = 0;
		};

		struct HuntCombatEvidence {
			double activeSeconds = 0;
			uint32_t damageTaken = 0;
			uint32_t potionRecoveries = 0;
			uint32_t spellRecoveries = 0;
			uint32_t maximumAttackerOverlap = 0;
			int32_t minimumHealth = std::numeric_limits<int32_t>::max();
			bool dangerObserved = false;
			bool deathObserved = false;
			std::array<uint32_t, 101> healthPercentSamples{};
			std::chrono::steady_clock::time_point lastSample;
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
		uint64_t desiredCarriedGold(const Player& player) const;
		static bool isFoodItem(uint16_t itemId);
		FoodInventory getFoodInventory(const Player& player) const;
		uint32_t effectiveFreeCapacity(const Player& player) const;

		uint32_t itemUnitValue(uint16_t itemId) const;

		uint32_t protectedItemReserve(uint16_t itemId) const;

		uint32_t getSaleItemCount(const Player& player, uint16_t itemId) const;

		int32_t getFoodTicks(const Player& player) const;

		bool canEatCheese(const Player& player) const;

		bool needsHealing(const Player& player) const;
		bool requiresKnightCombatReadiness(const Player& player) const;
		bool isLegalEquipmentType(const Player& player, const ItemType& type) const;
		bool isLegalEquipmentItem(const Player& player, const Item& item) const;
		bool isKnightMeleeWeapon(const Player& player, const Item& item) const;
		bool isCombatEquipment(const Item& item) const;
		bool isProtectedInventoryItem(const Item& item) const;
		bool isCombatReady(const Player& player, std::string& recovery, std::string& terminalReason) const;
		void emitCombatReadiness(const Player& player, const Position& position, const char* result,
		                         const std::string& recovery, const std::string& terminalReason) const;
		bool findCarriedEquipmentUpgrade(Player& player, Item*& item, EquipmentUpgrade& upgrade) const;
		bool beginReadinessEquipment(Player* player, const Position& position, const char* reason);
		void processReadinessEquipment(Player* player, const Position& position);
		bool ensureCombatReady(Player* player, const Position& position, const char* reason);

		void logHealResult(const char* result, const char* reason, int32_t healthAfter,
		                   uint32_t potionCountAfter, const Position& position);

		bool handleHealing(Player* player, const Position& currentPosition);
		bool handleSpellHealing(Player* player, const Position& currentPosition);
		bool trySupportSpell(Player* player, const Position& currentPosition);
		bool tryOffensiveSpell(Player* player, const Position& currentPosition);
		bool startSpellCast(Player& player, const Position& position, const char* spellName, const char* need,
		                    Creature* target = nullptr);
		void verifySpellCast(Player& player, const Position& position);
		void emitSpellCastEvent(const Position& position, const char* spellName, const char* words, const char* role,
		                        const char* need, const char* result, const char* engineResult, const char* reason,
		                        const PendingSpellCast* pending, const Player* player, const char* fallback) const;

		void logEatSuccess(uint16_t itemId, uint32_t inventoryCount, int32_t foodTicks, const Position& position);

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
		void beginTargetPursuit(Player* player, const Position& currentPosition);
		void finishTargetPursuit(const Position& currentPosition, const char* reason);
		void processTargetPursuit(Player* player, const Position& currentPosition);

		std::optional<EquipmentUpgrade> evaluateEquipmentUpgrade(const Player& player, const Item& candidate) const;
		EquipmentLoadout equipmentLoadout(const Player& player) const;
		bool applyEquipmentOffer(const Player& player, EquipmentLoadout& loadout, uint16_t itemId, slots_t& slot,
		                         uint16_t& replacedItemId, uint16_t& displacedLeftItemId, uint16_t& displacedRightItemId,
		                         std::string& rejection) const;
		PlayerBotCombatProfile equipmentCombatProfile(const Player& player, const EquipmentLoadout& loadout) const;
		bool equipmentLoadoutReady(const Player& player, const EquipmentLoadout& loadout,
		                           uint32_t additionalWeight = 0) const;
		EquipmentHuntSummary equipmentHuntSummary(Player& player, const PlayerBotCombatProfile& profile) const;
		EquipmentOfferEvaluation evaluateEquipmentCandidate(Player& player, uint16_t itemId,
		                                                    const EquipmentLoadout& currentLoadout,
		                                                    const PlayerBotCombatProfile& currentProfile,
		                                                    const EquipmentHuntSummary& currentHunts,
		                                                    bool currentReady,
		                                                    uint32_t additionalWeight = 0,
		                                                    bool allowSimulation = true) const;
		const char* equipmentDecisionRuleName(EquipmentDecisionRule rule) const;
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

		int32_t estimatedPickupUtility(const PickupReward& reward) const;

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

		void emitRewardCandidate(const PickupReward& candidate, const Position& position, const char* result,
		                         const char* reason = nullptr) const;

		void emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
		                          const RewardInspection& inspection, const Position& position);

		bool findPickupReward(Player& player, const Position& position, PickupReward& reward,
		                      std::deque<PlayerBotNavigationStep>& rewardSteps);

		bool hasCompletedRookgaardDeparture(const Player& player) const;
		bool requiresRookgaardDeparture(const Player& player) const;

		bool findOracleDeparture(Player& player, const Position& position, DeparturePlan& plan,
		                         std::deque<PlayerBotNavigationStep>& departureSteps);
		bool forceOracleDeparture(Player& player, const Position& position, const char* decisionReason);

		void beginOracleDeparture(Player& player, const Position& position, DeparturePlan plan,
		                          std::deque<PlayerBotNavigationStep> departureSteps);

		void processOracleDeparture(Player* player, const Position& currentPosition);

		void finishOracleDeparture(Player* player, const Position& position, const char* result, const char* reason);

		uint64_t spellTrainingReserve(const Player& player) const;
		void emitSpellCandidate(const Npc& npc, const NpcSpellOffer& offer, const Position& position, const char* result,
		                        const char* reason, uint64_t reserve = 0, uint32_t travelSteps = 0) const;
		bool findSpellTraining(Player& player, const Position& position, SpellTrainingPlan& plan,
		                       std::deque<PlayerBotNavigationStep>& steps);
		void beginSpellTraining(Player& player, const Position& position, SpellTrainingPlan plan,
		                        std::deque<PlayerBotNavigationStep> steps);
		void finishSpellTraining(Player* player, const Position& position, const char* result, const char* reason);
		void processSpellTraining(Player* player, const Position& currentPosition);

		bool processMagicTraining(Player& player, const Position& position);
		const char* magicTrainingCandidateReason(const Player& player) const;
		bool magicTrainingSafe(const Player& player) const;
		const char* magicTrainingSafetyReason(const Player& player) const;
		void finishMagicTraining(Player& player, const Position& position, const char* result, const char* reason);

		const char* topLevelGoalName(TopLevelGoal goal) const;

		uint32_t saleableItemCount(const Player& player) const;

		GoalCandidate serviceGoalCandidate(const Player& player) const;

		void emitGoalCandidate(const Player& player, const GoalCandidate& candidate, const Position& position, const char* decisionReason,
		                       const PickupReward* reward = nullptr, const DeparturePlan* departure = nullptr,
		                       const EquipmentOfferEvaluation* equipment = nullptr) const;

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
		void onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage);
		void onHealthGain(Creature* healer, const Creature& target, uint32_t gain);

		uint32_t navigationDistance(const Position& from, const Position& destination) const;

		bool detectNavigationOscillation(const Position& currentPosition, const Position& destination);

		bool processNavigation(Player* player, const Position& currentPosition, const Position& destination);
		void adoptNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps);

		bool isProtectedDepositItem(const Player& player, const Item& item) const;
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
		void recordHuntCombatObservation(bool active, double elapsedSeconds, int32_t health, int32_t maximumHealth,
		                                 uint32_t attackers);
		void recordActiveHuntCombat(const Player& player);
		uint8_t p10HuntCombatHealthPercent() const;
		void recordHuntRecovery(bool potion);
		void updateChallengeFrontier(const Player& player, const Position& position, uint64_t huntDurationSeconds,
		                             const char* reason);
		void runAdaptiveChallengeFixture(Player& player, const Position& position);
		void runSpellCalibrationFixture(Player& player, const Position& position);
		void runMagicTrainingFixture(Player& player, const Position& position);
		void cancelHuntRegionPlanning();
		void emitHuntRegionPlanning(const HuntRegionPlanning& planning, const Position& position, const char* phase) const;

		void finishHuntRegion(const Player& player, const Position& position, const char* reason);

		bool selectHuntRegion(Player& player, const Position& position, const char* reason);
		void beginHuntCycle(Player* player, const Position& position, const char* reason);

		void startHunt(Player* player, const Position& position, const char* reason);

		void processDeposit(Player* player, const Position& currentPosition);

		void processTraversal(Player* player, const Position& currentPosition);

		void navigate();

		void beginLoot(Player* player, const Position& currentPosition);

		void finishLoot(Player* player, const Position& currentPosition);

		Container* findCorpse(Player* player, const Position& searchPosition);

		uint8_t backpackDestinationIndex(const Container& backpack, const Item& item) const;

		bool isReplaceableCargo(const Item& item) const;

		bool chooseCargoReplacement(const Container& backpack, const Item& incoming, uint8_t incomingCount, uint32_t freeCapacity,
		                            CargoCandidate& replacement, uint8_t& replacementCount) const;

		void discardCargoForLoot(Player* player, Container* backpack, Item* incoming, uint8_t incomingCount,
		                         const Position& currentPosition);

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
		uint32_t forcedNavigationStepFailuresRemaining = 0;
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
		uint32_t pendingDepositInventoryCount = 0;
		uint8_t pendingDepositRequestedCount = 0;
		uint32_t depotAttempts = 0;
		uint16_t depotId = 0;
		uint16_t depotLockerItemId = 0;
		Position depotLockerPosition;
		Position depotApproachPosition;
		std::vector<DepotCandidate> depotCandidates;
		std::map<Position, std::chrono::steady_clock::time_point> rejectedDepotApproaches;
		size_t nextDepotCandidate = 0;
		uint32_t depotIndexedCandidateCount = 0;
		uint32_t depotInScopeCandidateCount = 0;
		uint32_t depotStandableCandidateCount = 0;
		uint32_t depotSuppressedApproachCount = 0;
		Position depotDiscoveryAnchor;
		bool depotCandidatesPrepared = false;
		DepotStage depotStage = DepotStage::Discover;
		std::set<uint16_t> unavailableLootItemIds;
		std::map<uint16_t, uint32_t> itemSellValues;
		bool pendingHeal = false;
		int32_t pendingHealHealth = 0;
		int32_t pendingHealHealthMax = 0;
		uint32_t pendingHealPotionCount = 0;
		std::chrono::steady_clock::time_point healRetryAfter;
		PendingSpellCast pendingSpellCast;
		bool spellCastExecuting = false;
		std::chrono::steady_clock::time_point spellRetryAfter;
		PlayerBotSpellCalibration spellCalibration;
		bool pendingEat = false;
		uint16_t pendingEatItemId = 0;
		uint32_t pendingEatInventoryCount = 0;
		int32_t pendingEatFoodTicks = 0;
		uint32_t eatFailures = 0;
		std::chrono::steady_clock::time_point eatRetryAfter;
		std::chrono::steady_clock::time_point combatStarted;
		std::chrono::steady_clock::time_point defensiveCombatStarted;
		std::chrono::steady_clock::time_point targetPursuitStarted;
		Position targetPursuitStartPosition;
		Position targetPursuitDestination;
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> suppressedTraversalTargets;
		CyclePhase cyclePhase = CyclePhase::ReturnToDepot;
		ServiceStage serviceStage = ServiceStage::Discover;
		ConversationStep conversationStep = ConversationStep::Greet;
		ProgressionObjective progressionObjective = ProgressionObjective::None;
		ProgressionStage progressionStage = ProgressionStage::Travel;
		DepartureStage departureStage = DepartureStage::Travel;
		SpellTrainingStage spellTrainingStage = SpellTrainingStage::Travel;
		PickupReward pickupReward;
		DeparturePlan departurePlan;
		SpellTrainingPlan spellTrainingPlan;
		EquipmentOfferEvaluation equipmentPurchase;
		EquipmentPurchaseStage equipmentPurchaseStage = EquipmentPurchaseStage::Travel;
		TopLevelGoal activeGoal = TopLevelGoal::Service;
		uint64_t goalDecisionId = 0;
		std::chrono::steady_clock::time_point pickupRewardCooldownUntil;
		std::chrono::steady_clock::time_point spellTrainingCooldownUntil;
		std::chrono::steady_clock::time_point equipmentPurchaseCooldownUntil;
		std::chrono::steady_clock::time_point magicTrainingCooldownUntil;
		uint32_t progressionAttempts = 0;
		uint32_t pendingRewardItemCount = 0;
		uint32_t pendingRewardRootCount = 0;
		std::map<std::string, uint32_t> pendingRewardRootCounts;
		std::map<uint16_t, uint32_t> pendingRewardStackableCounts;
		size_t pendingRewardContainerDepth = SIZE_MAX;
		uint32_t pendingRewardContainerOpenAttempts = 0;
		std::map<uint16_t, std::string> rewardInspectionFingerprints;
		std::map<uint16_t, uint32_t> pendingEquipmentDisplacedCounts;
		uint16_t pendingReadinessItemId = 0;
		slots_t pendingReadinessSlot = CONST_SLOT_WHEREEVER;
		uint32_t pendingReadinessAttempts = 0;
		bool readinessEquipmentPending = false;
		bool readinessResumeService = false;
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
		std::optional<HuntRegionPlanning> huntRegionPlanning;
		std::optional<PlayerBotHuntRegion> activeHuntRegion;
		std::map<Position, std::chrono::steady_clock::time_point>& huntRegionCooldowns;
		std::map<Position, PlayerBotHuntRegionPerformance> huntRegionPerformance;
		ChallengeFrontier challengeFrontier;
		HuntCombatEvidence huntCombatEvidence;
		std::chrono::steady_clock::time_point huntScopeReevaluationAfter;
		uint32_t consecutiveHuntScopeExhaustions = 0;
		std::chrono::steady_clock::time_point huntRegionStarted;
		uint64_t huntRegionStartExperience = 0;
		uint32_t huntRegionStartLevel = 0;
		uint32_t huntRegionKills = 0;
		uint32_t huntRegionDamageTaken = 0;
		bool adaptiveChallengeFixtureRun = false;
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
		bool huntPlanningFixtureCancelled = false;
		bool huntPlanningFixtureStaleRevisionTriggered = false;
		Position blockedNavigationTarget;
		std::chrono::steady_clock::time_point navigationStepStarted;
		std::chrono::steady_clock::time_point blockedNavigationTargetExpires;
		PlayerBotNavigationStep worldChangeStep;
		std::map<Position, std::chrono::steady_clock::time_point> temporarilyBlockedPositions;
		bool navigationPending = false;
		bool worldChangePending = false;
		bool magicTrainingFixtureInitializationPending = false;
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
