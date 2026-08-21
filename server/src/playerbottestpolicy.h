/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTTESTPOLICY_H
#define FS_PLAYERBOTTESTPOLICY_H

#include <cstdint>

class Player;

namespace playerbot {
	// Gameplay fixtures are an integration-test boundary, not playerbot behavior.
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

	const PlayerBotTestPolicy& playerBotTestPolicyFromEnvironment();

	enum class PlayerBotFixtureInitialization : uint8_t {
		NotPending,
		Waiting,
		Cancelled,
		Ready,
	};

	enum class PlayerBotFixtureRouteFailure : uint8_t {
		None,
		Unreachable,
		NodeLimit,
	};

	class PlayerBotFixtureRuntime {
		public:
			explicit PlayerBotFixtureRuntime(const PlayerBotTestPolicy& policy);

			bool progressionEnabled() const { return testPolicy.progressionEnabled; }
			bool startInHunt() const { return testPolicy.startInHunt; }
			bool fixedFixtureRoute() const { return testPolicy.fixedFixtureRoute; }
			bool depotFixture() const { return testPolicy.depotFixture; }
			bool spellCalibrationFixture() const { return testPolicy.spellCalibrationFixture; }
			bool magicTrainingFixture() const { return testPolicy.magicTrainingFixture; }
			bool deferProgressionFixtureInitialization() const { return testPolicy.deferProgressionFixtureInitialization; }
			bool forceHuntScopeExhaustion() const { return testPolicy.forceHuntScopeExhaustion; }
			bool suppressSlottedLootSeller() const { return testPolicy.suppressSlottedLootSeller; }
			bool equipmentPurchasesEnabled() const { return testPolicy.equipmentPurchasesEnabled; }
			bool equipmentPurchaseFixture() const { return testPolicy.equipmentPurchaseFixture; }
			bool forceEquipmentPurchaseRejected() const { return testPolicy.forceEquipmentPurchaseRejected; }
			bool pauseAfterEquipmentStorageRejection() const { return testPolicy.pauseAfterEquipmentStorageRejection; }
			bool forceMagicTrainingVerificationFailure() const { return testPolicy.forceMagicTrainingVerificationFailure; }
			DepotMoveFixture depotMoveFixture() const { return testPolicy.depotMoveFixture; }
			bool consumeNavigationStepFailure();
			bool forceNavigationPlanFailure() const;
			void consumeNavigationPlanFailure(bool attempted);
			bool forcedStepRecoveryPending() const;
			void resetHuntPlanningRouteFailures();
			bool consumeAdaptiveChallengeFixture();
			bool consumeHuntPlanningCancellation();
			bool consumeHuntPlanningStaleRevision();
			PlayerBotFixtureRouteFailure consumeHuntPlanningRouteFailure();
			void beginDelayedInitialization();
			PlayerBotFixtureInitialization delayedInitializationStatus(Player& player);
			bool consumeDepotRestartCheckpoint(Player& player, DepotRestartCheckpoint checkpoint);
			bool completeEquipmentPurchaseFixture(Player& player) const;

		private:
			const PlayerBotTestPolicy testPolicy;
			uint32_t forcedNavigationStepFailuresRemaining = 0;
			uint32_t forcedNavigationPlanFailuresRemaining = 0;
			bool adaptiveChallengeFixtureRun = false;
			bool huntPlanningFixtureCancelled = false;
			bool huntPlanningFixtureStaleRevisionTriggered = false;
			bool huntPlanningFixtureForcedUnreachable = false;
			bool huntPlanningFixtureForcedNodeLimit = false;
			bool delayedInitializationPending = false;
	};
}

#endif
