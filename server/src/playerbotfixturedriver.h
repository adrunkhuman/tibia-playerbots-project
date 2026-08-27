/** Fixture-only driver for integration scenarios. It is inert outside test policy. */
#ifndef FS_PLAYERBOTFIXTUREDRIVER_H
#define FS_PLAYERBOTFIXTUREDRIVER_H

#include "playerbottestpolicy.h"
#include "position.h"

#include <string>
#include <vector>

class Player;
class DepotChest;
struct PlayerBotHuntPlanningObservation;
struct PlayerBotHuntRuntimeOutcome;

namespace playerbot {
	enum class PlayerBotFixtureInitialization : uint8_t { NotPending, Waiting, Cancelled, Ready };
	struct PlayerBotFixtureRoutePlan {
		bool forceFailure = false;
		uint64_t maximumExpandedNodes = 0;
	};
	struct PlayerBotFixtureEngineCommand {
		bool dispatch = true;
		uint8_t count = 0;
	};
	struct PlayerBotFixtureProviderObservation {
		bool available = false;
		uint32_t inventoryCount = 0;
	};
	struct PlayerBotFixtureStorageObservation {
		bool pause = false;
		bool selectGoal = false;
	};
	struct PlayerBotFixtureHuntObservation {
		bool selectRegion = true;
	};
	struct PlayerBotFixtureDepotEndpoint {
		bool synthetic = false;
		Position lockerPosition;
		Position approachPosition;
		Position destinationPosition;
		uint16_t depotId = 0;
		uint16_t lockerItemId = 0;
	};

	struct PlayerBotFixtureEvent {
		const char* name;
		std::string fields;
	};

	class PlayerBotFixtureDriver {
		public:
			explicit PlayerBotFixtureDriver(const PlayerBotTestPolicy& policy);
			PlayerBotFixtureStorageObservation goalLoop(bool engineSelectGoal) const;
			PlayerBotFixtureStorageObservation progressionGoalLoop(bool engineSelectGoal) const;
			bool startWithGoalSelection() const { return policy.progressionEnabled; }
			bool mapRewardsEnabled() const { return !policy.equipmentPurchaseFixture; }
			PlayerBotFixtureHuntObservation huntObservation() const;
			bool startInHunt() const { return policy.startInHunt; }
			std::vector<Position> huntPatrol() const;
			bool depotScenario() const { return policy.depotFixture; }
			bool spellCalibrationScenario() const { return policy.spellCalibrationFixture; }
			bool magicTrainingScenario() const { return policy.magicTrainingFixture; }
			bool deferInitialization() const { return policy.deferProgressionFixtureInitialization; }
			PlayerBotFixtureProviderObservation observeProvider(bool engineAvailable, uint16_t itemId, bool buying,
			                                                     uint32_t engineInventoryCount = 0) const;
			PlayerBotFixtureProviderObservation observeEquipmentOffer(bool engineAvailable) const;
			PlayerBotFixtureStorageObservation equipmentStorageObservation() const;
			PlayerBotFixtureRoutePlan navigationPlan(uint64_t engineMaximumExpandedNodes, bool npcApproach) const;
			PlayerBotFixtureEngineCommand navigationStepCommand();
			void observeNavigationPlan(bool attempted);
			PlayerBotFixtureStorageObservation navigationRecovery(bool routeUnavailable) const;
			void resetHuntPlanningRouteFailures();
			PlayerBotFixtureRoutePlan huntRoutePlan(uint64_t engineMaximumExpandedNodes);
			PlayerBotHuntPlanningObservation huntPlanningObservation() const;
			void observeHuntPlanning(const PlayerBotHuntRuntimeOutcome& outcome);
			void beginDelayedInitialization();
			PlayerBotFixtureInitialization delayedInitializationStatus(Player& player);
			PlayerBotFixtureStorageObservation depotRestartObservation(Player& player, DepotRestartCheckpoint checkpoint);
			PlayerBotFixtureDepotEndpoint depotEndpoint() const;
			PlayerBotFixtureEngineCommand depotMoveCommand(uint8_t requestedCount) const;
			void prepareDepotMoveDestination(DepotChest& chest) const;
			PlayerBotFixtureEngineCommand equipmentPurchaseCommand() const;
			PlayerBotFixtureStorageObservation equipmentPurchaseCompletion(Player& player) const;
			uint64_t observedMagicTrainingMana(uint64_t engineObservation) const;
			std::vector<PlayerBotFixtureEvent> runAdaptiveChallenge(Player& player);
			std::vector<PlayerBotFixtureEvent> runSpellCalibration(Player& player);
			std::vector<PlayerBotFixtureEvent> runMagicTraining(Player& player);

		private:
			const PlayerBotTestPolicy policy;
			uint32_t forcedNavigationStepFailuresRemaining = 0;
			uint32_t forcedNavigationPlanFailuresRemaining = 0;
			bool adaptiveChallengeRun = false;
			uint8_t huntPlanningStarts = 0;
			bool planningCancelled = false;
			bool planningRevisionInvalidated = false;
			bool forcedUnreachable = false;
			bool forcedNodeLimit = false;
			bool delayedInitializationPending = false;
	};
}

#endif
