#include "otpch.h"

#include "playerbotturnrouter.h"

namespace {
	constexpr bool routingContract()
	{
		PlayerBotTurnRouter router;
		if (router.route({}) != PlayerBotTurnCommand::ReturnToDepot) return false;
		if (router.route({true}) != PlayerBotTurnCommand::Progression) return false;
		router.setCyclePhase(PlayerBotCyclePhase::Service);
		if (router.route({}) != PlayerBotTurnCommand::Service) return false;
		router.setCyclePhase(PlayerBotCyclePhase::DepositLoot);
		if (router.route({}) != PlayerBotTurnCommand::DepositLoot) return false;
		router.setCyclePhase(PlayerBotCyclePhase::Hunt);
		router.setScenarioStage(PlayerBotScenarioStage::LootCorpse);
		PlayerBotTurnObservation observation;
		observation.magicTrainingActive = true;
		observation.huntPlanningActive = true;
		observation.lootNavigationSuspended = true;
		if (router.route(observation) != PlayerBotTurnCommand::MagicTraining) return false;
		observation.magicTrainingActive = false;
		if (router.route(observation) != PlayerBotTurnCommand::PlanHunt) return false;
		observation.huntPlanningActive = false;
		if (router.route(observation) != PlayerBotTurnCommand::SuspendedLoot) return false;
		observation.lootNavigationSuspended = false;
		observation.huntCycleFinished = true;
		if (router.route(observation) != PlayerBotTurnCommand::Loot) return false;
		router.setScenarioStage(PlayerBotScenarioStage::TraversalCombat);
		if (router.route(observation) != PlayerBotTurnCommand::FinishHunt) return false;
		observation.huntCycleFinished = false;
		if (router.route(observation) != PlayerBotTurnCommand::TraversalCombat) return false;
		router.pause();
		if (router.route(observation) != PlayerBotTurnCommand::None || router.running()) return false;
		router.start();
		router.stop();
		return router.route(observation) == PlayerBotTurnCommand::None;
	}

	static_assert(routingContract(), "playerbot turn routing priority changed");
}

const char* PlayerBotTurnRouter::cyclePhaseName(PlayerBotCyclePhase value)
{
	switch (value) {
		case PlayerBotCyclePhase::Idle: return "idle";
		case PlayerBotCyclePhase::Service: return "service";
		case PlayerBotCyclePhase::ReturnToDepot: return "return_to_depot";
		case PlayerBotCyclePhase::DepositLoot: return "deposit_loot";
		case PlayerBotCyclePhase::Hunt: return "hunt";
	}
	return "unknown";
}

const char* PlayerBotTurnRouter::scenarioStageName(PlayerBotScenarioStage value)
{
	switch (value) {
		case PlayerBotScenarioStage::LootCorpse: return "loot_corpse";
		case PlayerBotScenarioStage::Traverse: return "traverse";
		case PlayerBotScenarioStage::TraversalCombat: return "traversal_combat";
		case PlayerBotScenarioStage::TargetPursuit: return "target_pursuit";
	}
	return "unknown";
}

const char* PlayerBotTurnRouter::stateName() const
{
	if (running()) return scenarioStageName(stage);
	return controllerLifecycle == PlayerBotControllerLifecycle::Paused ? "paused" : "stopped";
}
