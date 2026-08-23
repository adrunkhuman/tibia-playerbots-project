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

}

#endif
