/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbottestpolicy.h"

const playerbot::PlayerBotTestPolicy& playerbot::playerBotTestPolicyFromEnvironment()
{
	static const PlayerBotTestPolicy policy = []() {
		const char* gameplayModeValue = std::getenv("PLAYERBOT_GAMEPLAY_MODE");
		const char* regressionModeValue = std::getenv("PLAYERBOT_REGRESSION_MODE");
		const char* gameplayMode = gameplayModeValue && *gameplayModeValue != '\0' ? gameplayModeValue : nullptr;
		const char* regressionMode = regressionModeValue && *regressionModeValue != '\0' ? regressionModeValue : nullptr;
		const bool progressionMode = gameplayMode &&
			(std::strcmp(gameplayMode, "progression") == 0 ||
			 std::strcmp(gameplayMode, "progression_bundle") == 0 ||
			 std::strcmp(gameplayMode, "progression_nested") == 0 ||
			 std::strcmp(gameplayMode, "progression_resume") == 0 ||
			 std::strcmp(gameplayMode, "progression_nested_resume") == 0 ||
			 std::strcmp(gameplayMode, "progression_space") == 0 ||
			 std::strcmp(gameplayMode, "mainland_reward") == 0 ||
			 std::strcmp(gameplayMode, "readiness_no_food") == 0 ||
			 std::strcmp(gameplayMode, "readiness_low_wealth") == 0 ||
			 std::strcmp(gameplayMode, "arbitration") == 0 ||
			 std::strcmp(gameplayMode, "arbitration_interrupt") == 0 ||
			 std::strcmp(gameplayMode, "departure") == 0 ||
			 std::strcmp(gameplayMode, "departure_recovery") == 0 ||
			 std::strcmp(gameplayMode, "spell_training") == 0 || std::strcmp(gameplayMode, "equipment_shadow") == 0 ||
			 std::strcmp(gameplayMode, "equipment_shadow_unaffordable") == 0 ||
			 std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") == 0 ||
			 std::strcmp(gameplayMode, "equipment_buy") == 0 ||
			 std::strcmp(gameplayMode, "equipment_buy_resume") == 0 ||
			 std::strcmp(gameplayMode, "equipment_buy_space") == 0 ||
			 std::strcmp(gameplayMode, "equipment_buy_rejected") == 0 ||
			 std::strcmp(gameplayMode, "slotted_loot_seller") == 0 ||
			 std::strcmp(gameplayMode, "slotted_loot_no_seller") == 0 ||
			 (std::strncmp(gameplayMode, "magic_training", 14) == 0 &&
			  std::strcmp(gameplayMode, "magic_training_hunt") != 0));
		const bool startInHunt = gameplayMode &&
			(std::strcmp(gameplayMode, "navigation") == 0 || std::strcmp(gameplayMode, "navigation_recovery") == 0 ||
			 (std::strcmp(gameplayMode, "corpse") == 0 || std::strcmp(gameplayMode, "corpse_inaccessible") == 0) ||
			 std::strcmp(gameplayMode, "patrol_recovery") == 0 ||
			 (std::strcmp(gameplayMode, "target_pursuit") == 0 || std::strcmp(gameplayMode, "target_pursuit_abandon") == 0) ||
			 std::strcmp(gameplayMode, "healing") == 0 || std::strcmp(gameplayMode, "healing_resupply") == 0 ||
			 std::strcmp(gameplayMode, "value") == 0 || std::strcmp(gameplayMode, "departure_interrupt") == 0 ||
			 std::strcmp(gameplayMode, "stamina_bonus") == 0 || std::strcmp(gameplayMode, "stamina_boundary") == 0 ||
			 std::strcmp(gameplayMode, "stamina_normal") == 0 || std::strcmp(gameplayMode, "hunt_planning") == 0 ||
			 std::strcmp(gameplayMode, "adaptive_challenge") == 0 ||
			 std::strcmp(gameplayMode, "readiness_ready") == 0 || std::strcmp(gameplayMode, "readiness_upgrade") == 0 ||
			 std::strcmp(gameplayMode, "readiness_missing_weapon") == 0 || std::strcmp(gameplayMode, "readiness_supplies") == 0 ||
			 std::strcmp(gameplayMode, "readiness_food_capacity") == 0 ||
			 std::strcmp(gameplayMode, "readiness_retention") == 0 || std::strcmp(gameplayMode, "spell_use") == 0 ||
			 std::strcmp(gameplayMode, "magic_training_hunt") == 0 ||
			 std::strcmp(gameplayMode, "magic_training_post_hunt") == 0 ||
			 std::strcmp(gameplayMode, "magic_training_post_hunt_no_overflow") == 0 ||
			 std::strcmp(gameplayMode, "spell_calibration") == 0);
		const bool adaptiveChallengeFixture = gameplayMode && std::strcmp(gameplayMode, "adaptive_challenge") == 0;
		const bool spellCalibrationFixture = gameplayMode && std::strcmp(gameplayMode, "spell_calibration") == 0;
		const bool magicTrainingFixture = gameplayMode && std::strncmp(gameplayMode, "magic_training", 14) == 0;
		const bool fixedFixtureRoute = gameplayMode && std::strcmp(gameplayMode, "stamina_bonus") != 0 &&
		                               std::strcmp(gameplayMode, "stamina_boundary") != 0 &&
		                               std::strcmp(gameplayMode, "stamina_normal") != 0 &&
		                               std::strcmp(gameplayMode, "hunt_planning") != 0 &&
		                               std::strcmp(gameplayMode, "adaptive_challenge") != 0 &&
		                               std::strcmp(gameplayMode, "equipment_shadow") != 0 &&
		                               std::strcmp(gameplayMode, "equipment_shadow_unaffordable") != 0 &&
		                               std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") != 0 &&
		                               std::strcmp(gameplayMode, "mainland") != 0 &&
		                               std::strcmp(gameplayMode, "mainland_reward") != 0 &&
		                               std::strcmp(gameplayMode, "spell_training") != 0 &&
		                               std::strcmp(gameplayMode, "depot") != 0 &&
		                               std::strcmp(gameplayMode, "slotted_loot_seller") != 0 &&
		                               std::strcmp(gameplayMode, "slotted_loot_no_seller") != 0;
		const char* depotRestartPhase = std::getenv("PLAYERBOT_DEPOT_RESTART_PHASE");
		const DepotRestartCheckpoint depotRestartCheckpoint = !depotRestartPhase ? DepotRestartCheckpoint::None :
			std::strcmp(depotRestartPhase, "approach") == 0 ? DepotRestartCheckpoint::Approach :
			std::strcmp(depotRestartPhase, "locker") == 0 ? DepotRestartCheckpoint::Locker :
			std::strcmp(depotRestartPhase, "chest") == 0 ? DepotRestartCheckpoint::Chest :
			std::strcmp(depotRestartPhase, "deposit") == 0 ? DepotRestartCheckpoint::Deposit :
			std::strcmp(depotRestartPhase, "depart") == 0 ? DepotRestartCheckpoint::Depart :
			DepotRestartCheckpoint::None;
		const char* depotMoveCase = std::getenv("PLAYERBOT_DEPOT_MOVE_CASE");
		const DepotMoveFixture depotMoveFixture = depotMoveCase && std::strcmp(depotMoveCase, "partial") == 0 ?
			DepotMoveFixture::Partial : depotMoveCase && std::strcmp(depotMoveCase, "rejected") == 0 ?
			DepotMoveFixture::Rejected : DepotMoveFixture::Normal;
		return PlayerBotTestPolicy{
			!regressionMode && (!gameplayMode || progressionMode),
			startInHunt,
			fixedFixtureRoute,
			gameplayMode && std::strcmp(gameplayMode, "depot") == 0,
			depotRestartCheckpoint,
			depotMoveFixture,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "hunt_planning") == 0,
			gameplayMode && std::strcmp(gameplayMode, "navigation_recovery") == 0,
			gameplayMode && std::strcmp(gameplayMode, "corpse_inaccessible") == 0,
			gameplayMode && std::strcmp(gameplayMode, "patrol_recovery") == 0,
			gameplayMode && std::strcmp(gameplayMode, "slotted_loot_no_seller") == 0,
			!gameplayMode || (std::strcmp(gameplayMode, "equipment_shadow") != 0 &&
			                  std::strcmp(gameplayMode, "equipment_shadow_unaffordable") != 0 &&
			                  std::strcmp(gameplayMode, "equipment_shadow_no_upgrade") != 0),
			gameplayMode && std::strncmp(gameplayMode, "equipment_buy", 13) == 0,
			gameplayMode && std::strcmp(gameplayMode, "equipment_buy_rejected") == 0,
			gameplayMode && std::strcmp(gameplayMode, "equipment_buy_space") == 0,
			adaptiveChallengeFixture,
			adaptiveChallengeFixture,
			gameplayMode && (std::strcmp(gameplayMode, "mainland_reward") == 0 ||
			                 std::strcmp(gameplayMode, "spell_training") == 0 ||
			                 std::strncmp(gameplayMode, "equipment_buy", 13) == 0),
			spellCalibrationFixture,
			magicTrainingFixture,
			gameplayMode && std::strcmp(gameplayMode, "magic_training_failed") == 0,
		};
	}();
	return policy;
}
