/** Fixture-only driver for integration scenarios. It is inert outside test policy. */
#ifndef FS_PLAYERBOTFIXTUREDRIVER_H
#define FS_PLAYERBOTFIXTUREDRIVER_H

#include "playerbottestpolicy.h"

#include <string>
#include <vector>

class Player;
class PlayerBotHuntRuntime;
class PlayerBotSurvivalRuntime;

namespace playerbot {
	enum class PlayerBotFixtureInitialization : uint8_t { NotPending, Waiting, Cancelled, Ready };
	enum class PlayerBotFixtureRouteFailure : uint8_t { None, Unreachable, NodeLimit };

	struct PlayerBotFixtureEvent {
		const char* name;
		std::string fields;
	};

	class PlayerBotFixtureDriver {
		public:
			explicit PlayerBotFixtureDriver(const PlayerBotTestPolicy& policy);
			bool progressionEnabled() const { return policy.progressionEnabled; }
			bool startInHunt() const { return policy.startInHunt; }
			bool fixedRoute() const { return policy.fixedFixtureRoute; }
			bool depotScenario() const { return policy.depotFixture; }
			bool spellCalibrationScenario() const { return policy.spellCalibrationFixture; }
			bool magicTrainingScenario() const { return policy.magicTrainingFixture; }
			bool deferInitialization() const { return policy.deferProgressionFixtureInitialization; }
			bool equipmentPurchasesEnabled() const { return policy.equipmentPurchasesEnabled; }
			bool suppressSlottedLootSeller() const { return policy.suppressSlottedLootSeller; }
			bool forceEquipmentPurchaseRejected() const { return policy.forceEquipmentPurchaseRejected; }
			bool pauseAfterEquipmentStorageRejection() const { return policy.pauseAfterEquipmentStorageRejection; }
			DepotMoveFixture depotMoveScenario() const { return policy.depotMoveFixture; }
			bool consumeNavigationStepFailure();
			bool forceNavigationPlanFailure() const;
			void observeNavigationPlan(bool attempted);
			bool forcedStepRecoveryPending() const;
			void resetHuntPlanningRouteFailures();
			PlayerBotFixtureRouteFailure nextHuntPlanningRouteFailure();
			std::vector<PlayerBotFixtureEvent> applyHuntPlanningHooks(PlayerBotHuntRuntime& runtime);
			void beginDelayedInitialization();
			PlayerBotFixtureInitialization delayedInitializationStatus(Player& player);
			bool consumeDepotRestartCheckpoint(Player& player, DepotRestartCheckpoint checkpoint);
			bool completeEquipmentPurchase(Player& player) const;
			uint64_t observedMagicTrainingMana(uint64_t engineObservation) const;
			std::vector<PlayerBotFixtureEvent> runAdaptiveChallenge(Player& player, PlayerBotHuntRuntime& runtime);
			std::vector<PlayerBotFixtureEvent> runSpellCalibration(Player& player, PlayerBotSurvivalRuntime& runtime);
			std::vector<PlayerBotFixtureEvent> runMagicTraining(Player& player);

		private:
			const PlayerBotTestPolicy policy;
			uint32_t forcedNavigationStepFailuresRemaining = 0;
			uint32_t forcedNavigationPlanFailuresRemaining = 0;
			bool adaptiveChallengeRun = false;
			bool planningCancelled = false;
			bool planningRevisionInvalidated = false;
			bool forcedUnreachable = false;
			bool forcedNodeLimit = false;
			bool delayedInitializationPending = false;
	};
}

#endif
