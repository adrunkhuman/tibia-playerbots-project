#ifndef FS_PLAYERBOTTURNROUTER_H
#define FS_PLAYERBOTTURNROUTER_H

#include <cstdint>

enum class PlayerBotCyclePhase : uint8_t {
	Idle,
	Service,
	ReturnToDepot,
	DepositLoot,
	Hunt,
};

enum class PlayerBotScenarioStage : uint8_t {
	LootCorpse,
	Traverse,
	TraversalCombat,
};

enum class PlayerBotControllerLifecycle : uint8_t {
	Running,
	Paused,
	Stopped,
};

enum class PlayerBotTurnCommand : uint8_t {
	None,
	Progression,
	MagicTraining,
	StartHunt,
	PlanHunt,
	SuspendedLoot,
	Loot,
	FinishHunt,
	Service,
	ReturnToDepot,
	DepositLoot,
	TraversalCombat,
	Hunt,
};

struct PlayerBotTurnObservation {
	bool progressionActive = false;
	bool magicTrainingActive = false;
	bool huntRegionSelectionRequired = false;
	bool huntPlanningActive = false;
	bool lootNavigationSuspended = false;
	bool huntCycleFinished = false;
};

class PlayerBotTurnRouter
{
	public:
		constexpr PlayerBotControllerLifecycle lifecycle() const { return controllerLifecycle; }
		constexpr bool running() const { return controllerLifecycle == PlayerBotControllerLifecycle::Running; }
		constexpr void start() { controllerLifecycle = PlayerBotControllerLifecycle::Running; }
		constexpr void pause() { controllerLifecycle = PlayerBotControllerLifecycle::Paused; }
		constexpr void stop() { controllerLifecycle = PlayerBotControllerLifecycle::Stopped; }

		constexpr PlayerBotCyclePhase cyclePhase() const { return phase; }
		constexpr void setCyclePhase(PlayerBotCyclePhase value) { phase = value; }
		constexpr PlayerBotScenarioStage scenarioStage() const { return stage; }
		constexpr void setScenarioStage(PlayerBotScenarioStage value) { stage = value; }

		static const char* cyclePhaseName(PlayerBotCyclePhase value);
		static const char* scenarioStageName(PlayerBotScenarioStage value);
		const char* stateName() const;

		// This order is the controller's preemption contract; keep command selection pure.
		constexpr PlayerBotTurnCommand route(const PlayerBotTurnObservation& observation) const
		{
			if (!running()) return PlayerBotTurnCommand::None;
			if (observation.progressionActive) return PlayerBotTurnCommand::Progression;
			if (observation.magicTrainingActive) return PlayerBotTurnCommand::MagicTraining;
			if (observation.huntRegionSelectionRequired) return PlayerBotTurnCommand::StartHunt;
			if (observation.huntPlanningActive) return PlayerBotTurnCommand::PlanHunt;
			if (stage == PlayerBotScenarioStage::LootCorpse && observation.lootNavigationSuspended) {
				return PlayerBotTurnCommand::SuspendedLoot;
			}
			if (stage == PlayerBotScenarioStage::LootCorpse) return PlayerBotTurnCommand::Loot;
			if (phase == PlayerBotCyclePhase::Hunt && observation.huntCycleFinished) {
				return PlayerBotTurnCommand::FinishHunt;
			}
			if (phase == PlayerBotCyclePhase::Service) return PlayerBotTurnCommand::Service;
			if (phase == PlayerBotCyclePhase::ReturnToDepot) return PlayerBotTurnCommand::ReturnToDepot;
			if (phase == PlayerBotCyclePhase::DepositLoot) return PlayerBotTurnCommand::DepositLoot;
			if (stage == PlayerBotScenarioStage::TraversalCombat) return PlayerBotTurnCommand::TraversalCombat;
			return phase == PlayerBotCyclePhase::Hunt ? PlayerBotTurnCommand::Hunt : PlayerBotTurnCommand::None;
		}

	private:
		PlayerBotControllerLifecycle controllerLifecycle = PlayerBotControllerLifecycle::Running;
		PlayerBotCyclePhase phase = PlayerBotCyclePhase::ReturnToDepot;
		PlayerBotScenarioStage stage = PlayerBotScenarioStage::Traverse;
};

#endif
