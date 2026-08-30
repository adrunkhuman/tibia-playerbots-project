/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "playerbotprogressionruntime.h"

PlayerBotGoalArbiter::GoalDecision PlayerBotProgressionRuntime::selectGoal(const PlayerBotGoalPlannerSnapshot& snapshot)
{
	PlayerBotGoalArbiter::GoalDecision decision = arbiter.decide(planner.candidates(snapshot));
	arbiter.apply(decision);
	return decision;
}

PlayerBotGoalArbiter::GoalDecision PlayerBotProgressionRuntime::interruptForDeparture(bool feasible, int32_t utility, std::string reason)
{
	PlayerBotGoalArbiter::GoalDecision decision = arbiter.force({PlayerBotGoalArbiter::TopLevelGoal::Departure, feasible, utility, std::move(reason)});
	arbiter.apply(decision);
	return decision;
}

PlayerBotGoalArbiter::GoalDecision PlayerBotProgressionRuntime::interruptHuntForService(std::string reason)
{
	PlayerBotGoalArbiter::GoalDecision decision = arbiter.force(planner.forcedServiceCandidate(std::move(reason)));
	arbiter.apply(decision);
	return decision;
}

void PlayerBotProgressionRuntime::enterService()
{
	arbiter.setActiveGoal(PlayerBotGoalArbiter::TopLevelGoal::Service);
}

void PlayerBotProgressionRuntime::enterHunt()
{
	arbiter.setActiveGoal(PlayerBotGoalArbiter::TopLevelGoal::Hunt);
}

void PlayerBotProgressionRuntime::completeReward(bool succeeded, std::chrono::steady_clock::duration cooldown)
{
	arbiter.setCooldown(PlayerBotGoalArbiter::TopLevelGoal::PickupReward, cooldown);
}

void PlayerBotProgressionRuntime::completeSpellTraining(bool succeeded, std::chrono::steady_clock::duration cooldown)
{
	arbiter.setCooldown(PlayerBotGoalArbiter::TopLevelGoal::LearnSpell, cooldown);
}

void PlayerBotProgressionRuntime::completeEquipmentPurchase(bool succeeded, std::chrono::steady_clock::duration cooldown)
{
	arbiter.setCooldown(PlayerBotGoalArbiter::TopLevelGoal::BuyEquipment, cooldown);
}

void PlayerBotProgressionRuntime::completeMagicTraining(std::chrono::steady_clock::duration cooldown)
{
	arbiter.setCooldown(PlayerBotGoalArbiter::TopLevelGoal::MagicTraining, cooldown);
}

void PlayerBotProgressionRuntime::completeSellLoot(std::chrono::steady_clock::duration cooldown)
{
	arbiter.setCooldown(PlayerBotGoalArbiter::TopLevelGoal::SellLoot, cooldown);
}

bool PlayerBotProgressionRuntime::isCoolingDown(PlayerBotGoalArbiter::TopLevelGoal goal,
	std::chrono::steady_clock::time_point now) const
{
	return arbiter.isCoolingDown(goal, now);
}

void PlayerBotProgressionRuntime::beginReward(PlayerBotRewardPlan plan, std::map<uint16_t, uint32_t> displacedCounts)
{
	finish();
	rewardSession.begin(std::move(plan));
	rewardSession.captureDisplacedItemCounts(std::move(displacedCounts));
	progression.begin(PlayerBotProgressionProcedure::PickupReward);
}

void PlayerBotProgressionRuntime::beginDeparture(PlayerBotOracleDeparturePlan plan)
{
	finish();
	beginNpcConversation(plan.npcId);
	departureSession.begin(std::move(plan));
	progression.begin(PlayerBotProgressionProcedure::OracleDeparture);
}

void PlayerBotProgressionRuntime::beginSpellTraining(PlayerBotSpellTrainingPlan plan)
{
	finish();
	beginNpcConversation(plan.npcId);
	spellTrainingSession.begin(std::move(plan));
	progression.begin(PlayerBotProgressionProcedure::LearnSpell);
}

void PlayerBotProgressionRuntime::beginEquipmentPurchase(PlayerBotEquipmentOfferEvaluation plan)
{
	finish();
	if (!plan.carried) beginNpcConversation(plan.npcId);
	equipmentPurchaseSession.begin(std::move(plan));
	progression.begin(PlayerBotProgressionProcedure::BuyEquipment);
}

void PlayerBotProgressionRuntime::finish()
{
	switch (progression.active()) {
		case PlayerBotProgressionProcedure::PickupReward: rewardSession.reset(); break;
		case PlayerBotProgressionProcedure::OracleDeparture: departureSession.reset(); break;
		case PlayerBotProgressionProcedure::LearnSpell: spellTrainingSession.reset(); break;
		case PlayerBotProgressionProcedure::BuyEquipment: equipmentPurchaseSession.reset(); break;
		case PlayerBotProgressionProcedure::None: break;
	}
	progression.reset();
	finishNpcConversation();
}

void PlayerBotProgressionRuntime::beginNpcConversation(uint32_t npcId)
{
	npcSession.reset(npcId);
	equipmentTransaction.reset();
}

void PlayerBotProgressionRuntime::finishNpcConversation()
{
	npcSession.reset();
	equipmentTransaction.reset();
}

bool PlayerBotProgressionRuntime::reportNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type)
{
	return npcSession.acceptReply(playerId, replyingPlayerId, npcId, type);
}

void PlayerBotProgressionRuntime::restartDepartureConversation()
{
	npcSession.reset(departureSession.plan().npcId);
	departureSession.setStage(PlayerBotOracleDepartureStage::Greet);
	departureSession.resetRetries();
}

void PlayerBotProgressionRuntime::restartSpellTrainingConversation()
{
	npcSession.reset(spellTrainingSession.plan().npcId);
	spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Greet);
	spellTrainingSession.resetRetries();
}

void PlayerBotProgressionRuntime::restartEquipmentConversation()
{
	npcSession.reset(equipmentPurchaseSession.plan().npcId);
}

PlayerBotEquipmentShopCommand PlayerBotProgressionRuntime::advanceEquipmentShop(const PlayerBotNpcShopObservation& observation,
                                                                                  uint32_t maximumRetries)
{
	PlayerBotNpcShopObservation current = observation;
	current.greetingAcknowledged = npcSession.isGreetingAcknowledged();
	const PlayerBotNpcSessionOutcome outcome = npcSession.advanceShop(current, maximumRetries);
	const char* speech = nullptr;
	if (outcome.actionsIssued != 0) speech = npcSession.step() == PlayerBotNpcConversationStep::Request ? "hi" : "trade";
	const char* failureReason = outcome.result == PlayerBotNpcSessionResult::Failed ?
		npcSession.step() == PlayerBotNpcConversationStep::Request ? "npc_focus_unconfirmed" : "shop_window_unavailable" : nullptr;
	return {outcome.result, speech, observation.otherShopOpen && speech && npcSession.step() == PlayerBotNpcConversationStep::Request,
	        npcSession.nextDelay(), failureReason};
}

PlayerBotProgressionCommand PlayerBotProgressionRuntime::command(PlayerBotProgressionCommandType type, const char* reason) const
{
	return {type, progression.active(), 0, 0, reason};
}

PlayerBotReadinessEquipmentCommand PlayerBotProgressionRuntime::beginReadinessEquipment(
	const PlayerBotReadinessEquipmentObservation& observation, bool resumeService, uint32_t maximumRetries)
{
	readinessEquipment.resumeService = readinessEquipment.resumeService || resumeService;
	if (!observation.upgradeAvailable || !observation.actionAvailable) return {};
	if (readinessEquipment.itemId != observation.itemId || readinessEquipment.slot != observation.slot) {
		readinessEquipment = {};
		readinessEquipment.itemId = observation.itemId;
		readinessEquipment.slot = observation.slot;
		readinessEquipment.resumeService = resumeService;
	}
	if (observation.openContainerRequired) {
		if (!observation.containerAccessAvailable || ++readinessEquipment.attempts >= maximumRetries) {
			const PlayerBotReadinessEquipmentCommand command{PlayerBotReadinessEquipmentCommandType::ServiceFallback, 0,
				CONST_SLOT_WHEREEVER, readinessEquipment.attempts, "access_attempts_exhausted"};
			readinessEquipment = {};
			return command;
		}
		return {PlayerBotReadinessEquipmentCommandType::OpenContainer, observation.itemId, observation.slot,
		        readinessEquipment.attempts, "open_readiness_container"};
	}
	readinessEquipment.pending = true;
	return {PlayerBotReadinessEquipmentCommandType::Equip, observation.itemId, observation.slot,
	        readinessEquipment.attempts, "equip_readiness"};
}

PlayerBotReadinessEquipmentCommand PlayerBotProgressionRuntime::advanceReadinessEquipment(
	const PlayerBotReadinessEquipmentObservation& observation, uint32_t maximumRetries)
{
	if (!readinessEquipment.pending) return {};
	if (observation.equipmentVerified) {
		const PlayerBotReadinessEquipmentCommandType type = readinessEquipment.resumeService ?
			PlayerBotReadinessEquipmentCommandType::ResumeService : observation.combatReady ?
			PlayerBotReadinessEquipmentCommandType::StartHunt : PlayerBotReadinessEquipmentCommandType::Retry;
		const PlayerBotReadinessEquipmentCommand command{type, readinessEquipment.itemId, readinessEquipment.slot,
			readinessEquipment.attempts, type == PlayerBotReadinessEquipmentCommandType::Retry ? "readiness_upgrade_incomplete" : nullptr};
		readinessEquipment = {};
		return command;
	}
	if (++readinessEquipment.attempts >= maximumRetries) {
		const PlayerBotReadinessEquipmentCommand command{PlayerBotReadinessEquipmentCommandType::ServiceFallback,
			readinessEquipment.itemId, readinessEquipment.slot, readinessEquipment.attempts, "move_not_verified"};
		readinessEquipment = {};
		return command;
	}
	return {PlayerBotReadinessEquipmentCommandType::Retry, readinessEquipment.itemId, readinessEquipment.slot,
	        readinessEquipment.attempts, "readiness_retry"};
}

PlayerBotProgressionOutcome PlayerBotProgressionRuntime::advanceDeparture(const PlayerBotDepartureObservation& observation)
{
	const auto outcome = [this](PlayerBotProgressionCommandType type, PlayerBotProgressionOutcomeType result, const char* reason) {
		return PlayerBotProgressionOutcome{{type, progression.active(), static_cast<uint8_t>(departureSession.stage()), departureSession.retries(), reason},
		                                   result, departureSession.retries(), reason};
	};
	switch (departureSession.stage()) {
		case PlayerBotOracleDepartureStage::Travel:
			if (observation.navigationFailed) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "route_unavailable");
			if (!observation.navigationReached) return outcome(PlayerBotProgressionCommandType::Navigate, PlayerBotProgressionOutcomeType::Pending, nullptr);
			departureSession.setStage(PlayerBotOracleDepartureStage::ConfirmReady);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "hi");
		case PlayerBotOracleDepartureStage::Greet:
			if (!observation.npcAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "oracle_unavailable");
			departureSession.setStage(PlayerBotOracleDepartureStage::ConfirmReady);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "hi");
		case PlayerBotOracleDepartureStage::ConfirmReady:
			if (!observation.npcAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "oracle_unavailable");
			if (!observation.greetingAcknowledged) {
				if (departureSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "oracle_focus_unconfirmed");
				departureSession.setStage(PlayerBotOracleDepartureStage::ConfirmReady);
				return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Retry, "hi");
			}
			departureSession.setStage(PlayerBotOracleDepartureStage::ChooseTown);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "yes");
		case PlayerBotOracleDepartureStage::ChooseTown:
			departureSession.setStage(PlayerBotOracleDepartureStage::ChooseVocation);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "thais");
		case PlayerBotOracleDepartureStage::ChooseVocation:
			departureSession.setStage(PlayerBotOracleDepartureStage::ConfirmVocation);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "knight");
		case PlayerBotOracleDepartureStage::ConfirmVocation:
			departureSession.setStage(PlayerBotOracleDepartureStage::Verify);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "yes");
		case PlayerBotOracleDepartureStage::Verify:
			return outcome(PlayerBotProgressionCommandType::Finish,
			               observation.departureVerified ? PlayerBotProgressionOutcomeType::Succeeded : PlayerBotProgressionOutcomeType::Failed,
			               observation.departureVerified ? "vocation_town_and_teleport_verified" : "departure_not_verified");
	}
	return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "invalid_departure_stage");
}

PlayerBotProgressionOutcome PlayerBotProgressionRuntime::advanceSpellTraining(const PlayerBotSpellTrainingObservation& observation)
{
	const auto outcome = [this](PlayerBotProgressionCommandType type, PlayerBotProgressionOutcomeType result, const char* reason) {
		return PlayerBotProgressionOutcome{{type, progression.active(), static_cast<uint8_t>(spellTrainingSession.stage()), spellTrainingSession.retries(), reason},
		                                   result, spellTrainingSession.retries(), reason};
	};
	switch (spellTrainingSession.stage()) {
		case PlayerBotSpellTrainingStage::Travel:
			if (observation.navigationFailed) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "route_unavailable");
			if (!observation.navigationReached) return outcome(PlayerBotProgressionCommandType::Navigate, PlayerBotProgressionOutcomeType::Pending, nullptr);
			spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Request);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "hi");
		case PlayerBotSpellTrainingStage::Greet:
			if (!observation.npcAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "trainer_unavailable");
			spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Request);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "hi");
		case PlayerBotSpellTrainingStage::Request:
			if (!observation.npcAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "trainer_unavailable");
			if (!observation.greetingAcknowledged) {
				if (spellTrainingSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "trainer_focus_unconfirmed");
				spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Request);
				return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Retry, "hi");
			}
			spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Confirm);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "request");
		case PlayerBotSpellTrainingStage::Confirm:
			spellTrainingSession.setMoneyBefore(observation.totalMoney);
			spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Verify);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Pending, "yes");
		case PlayerBotSpellTrainingStage::Verify:
			if (observation.learned && spellTrainingSession.moneyBefore() >= spellTrainingSession.plan().price &&
			    observation.totalMoney == spellTrainingSession.moneyBefore() - spellTrainingSession.plan().price) {
				return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Succeeded, "learned_state_and_payment_verified");
			}
			if (observation.learned || observation.totalMoney != spellTrainingSession.moneyBefore()) {
				return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "transaction_delta_mismatch");
			}
			if (spellTrainingSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "learning_not_verified");
			spellTrainingSession.setStage(PlayerBotSpellTrainingStage::Greet);
			return outcome(PlayerBotProgressionCommandType::Speak, PlayerBotProgressionOutcomeType::Retry, "hi");
	}
	return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "invalid_spell_stage");
}

PlayerBotProgressionOutcome PlayerBotProgressionRuntime::advanceReward(const PlayerBotRewardObservation& observation)
{
	const auto outcome = [this](PlayerBotProgressionCommandType type, PlayerBotProgressionOutcomeType result, const char* reason) {
		return PlayerBotProgressionOutcome{{type, progression.active(), static_cast<uint8_t>(rewardSession.stage()), rewardSession.retries(), reason},
		                                   result, rewardSession.retries(), reason};
	};
	switch (rewardSession.stage()) {
		case PlayerBotRewardStage::Travel:
			if (observation.navigationFailed) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "route_unavailable");
			if (!observation.navigationReached) return outcome(PlayerBotProgressionCommandType::Navigate, PlayerBotProgressionOutcomeType::Pending, nullptr);
			rewardSession.setStage(PlayerBotRewardStage::UseReward);
			return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, nullptr);
		case PlayerBotRewardStage::UseReward:
			if (!observation.rewardObjectAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_object_unavailable");
			if (!observation.inRange) {
				rewardSession.setStage(PlayerBotRewardStage::Travel);
				return outcome(PlayerBotProgressionCommandType::Navigate, PlayerBotProgressionOutcomeType::Retry, "reward_out_of_range");
			}
			if (!observation.actionAvailable) return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "action_unavailable");
			rewardSession.captureClaimSnapshot(observation.currentClaim);
			rewardSession.setStage(PlayerBotRewardStage::VerifyReward);
			return outcome(PlayerBotProgressionCommandType::Use, PlayerBotProgressionOutcomeType::Pending, "claim_reward");
		case PlayerBotRewardStage::VerifyReward:
			{
				bool rootsAdded = true;
				std::map<std::string, uint32_t> expectedRoots;
				for (const std::string& signature : rewardSession.plan().nonStackableRootSignatures) ++expectedRoots[signature];
				for (const auto& [signature, expected] : expectedRoots) {
					const auto before = rewardSession.claimSnapshot().roots.find(signature);
					const uint32_t count = before == rewardSession.claimSnapshot().roots.end() ? 0 : before->second;
					const auto current = observation.currentClaim.roots.find(signature);
					if (current == observation.currentClaim.roots.end() || current->second < count + expected) rootsAdded = false;
				}
				for (const auto& [itemId, expected] : rewardSession.plan().stackableRootCounts) {
					const auto before = rewardSession.claimSnapshot().stackables.find(itemId);
					const uint32_t count = before == rewardSession.claimSnapshot().stackables.end() ? 0 : before->second;
					const auto current = observation.currentClaim.stackables.find(itemId);
					if (current == observation.currentClaim.stackables.end() || current->second < count + expected) rootsAdded = false;
				}
				const bool claimVerified = observation.claimed &&
					observation.currentClaim.itemCount > rewardSession.claimSnapshot().itemCount && rootsAdded;
				if (!claimVerified) {
				if (rewardSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "claim_not_verified");
				rewardSession.setStage(PlayerBotRewardStage::UseReward);
				return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Retry, "claim_reward");
				}
			}
			rewardSession.resetRetries();
			if (rewardSession.plan().slot == CONST_SLOT_WHEREEVER) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Succeeded, "reward_bundle_claimed");
			rewardSession.setStage(PlayerBotRewardStage::EquipReward);
			return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "reward_claimed");
		case PlayerBotRewardStage::EquipReward:
			if (observation.equipmentVerified) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Succeeded, "reward_equipped");
			if (observation.rootRelocationRequired || observation.displacedMoveRequired) {
				if (observation.rootRelocationRequired && !observation.rootRelocationSpaceAvailable) {
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "insufficient_reward_root_space");
				}
				if (observation.displacedMoveRequired && !observation.displacedMoveSpaceAvailable) {
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "insufficient_displaced_item_space");
				}
				if (!observation.actionAvailable) return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "action_unavailable");
				if (rewardSession.retries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed,
					observation.rootRelocationRequired ? "reward_root_move_not_verified" : "displaced_item_move_not_verified");
				rewardSession.incrementRetries();
				return outcome(PlayerBotProgressionCommandType::Equip, PlayerBotProgressionOutcomeType::Retry,
					observation.rootRelocationRequired ? "relocate_reward_root" : "preserve_displaced_equipment");
			}
			switch (observation.itemAccess) {
				case PlayerBotRewardObservation::ItemAccess::Ready: rewardSession.observeContainerOpen(observation.containerDepth); break;
				case PlayerBotRewardObservation::ItemAccess::ActionUnavailable:
					return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "action_unavailable");
				case PlayerBotRewardObservation::ItemAccess::BackpackUnavailable:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "backpack_unavailable");
				case PlayerBotRewardObservation::ItemAccess::BackpackClosed:
					return outcome(PlayerBotProgressionCommandType::Open, PlayerBotProgressionOutcomeType::Pending, "open_reward_backpack");
				case PlayerBotRewardObservation::ItemAccess::ContainerOpenRequired:
					if (rewardSession.beginContainerAccess(observation.containerDepth) >= 3) {
						return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_container_open_failed");
					}
					return outcome(PlayerBotProgressionCommandType::Open, PlayerBotProgressionOutcomeType::Pending, "open_reward_container");
				case PlayerBotRewardObservation::ItemAccess::RootUnavailable:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_bundle_unavailable");
				case PlayerBotRewardObservation::ItemAccess::DepthUnsupported:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_container_depth_unsupported");
				case PlayerBotRewardObservation::ItemAccess::PathInvalid:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_container_path_invalid");
				case PlayerBotRewardObservation::ItemAccess::ContainerPositionUnavailable:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_container_position_unavailable");
				case PlayerBotRewardObservation::ItemAccess::ItemPathInvalid:
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reward_item_path_invalid");
			}
			if (!observation.actionAvailable) return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "action_unavailable");
			rewardSession.setStage(PlayerBotRewardStage::VerifyEquipment);
			return outcome(PlayerBotProgressionCommandType::Equip, PlayerBotProgressionOutcomeType::Pending, "equip_reward");
		case PlayerBotRewardStage::VerifyEquipment:
			if (observation.equipmentVerified && rewardSession.displacedItemsPreserved(observation.displacedCounts)) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Succeeded, "reward_equipped");
			if (rewardSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed,
				rewardSession.displacedItemsPreserved(observation.displacedCounts) ? "equip_not_verified" : "displaced_item_lost");
			rewardSession.setStage(PlayerBotRewardStage::EquipReward);
			return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Retry, "equip_reward");
	}
	return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "invalid_reward_stage");
}

PlayerBotProgressionOutcome PlayerBotProgressionRuntime::advanceEquipmentPurchase(
	const PlayerBotEquipmentPurchaseObservation& observation, uint32_t maximumRetries)
{
	const auto outcome = [this](PlayerBotProgressionCommandType type, PlayerBotProgressionOutcomeType result, const char* reason) {
		return PlayerBotProgressionOutcome{{type, progression.active(), static_cast<uint8_t>(equipmentPurchaseSession.stage()), equipmentPurchaseSession.retries(), reason},
		                                   result, equipmentPurchaseSession.retries(), reason};
	};
	switch (equipmentPurchaseSession.stage()) {
		case PlayerBotEquipmentPurchaseStage::Travel:
			if (observation.navigationFailed) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "route_unavailable");
			if (!observation.navigationReached) return outcome(PlayerBotProgressionCommandType::Navigate, PlayerBotProgressionOutcomeType::Pending, nullptr);
			equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::Purchase);
			return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, nullptr);
		case PlayerBotEquipmentPurchaseStage::Purchase:
			if (!observation.providerAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "provider_unavailable");
			if (!observation.offerAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "offer_changed");
			if (!observation.providerInRange) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "provider_moved");
			if (!observation.shopReady) return outcome(PlayerBotProgressionCommandType::Shop, PlayerBotProgressionOutcomeType::Pending, "open_shop");
			if (!observation.fundingAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "reserve_changed");
			equipmentTransaction.beginShopTransaction({equipmentPurchaseSession.plan().itemId, 1, observation.itemCount,
				observation.money, observation.bankBalance, equipmentPurchaseSession.plan().price, 0});
			equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::VerifyPurchase);
			return outcome(PlayerBotProgressionCommandType::Shop, PlayerBotProgressionOutcomeType::Pending, "purchase_equipment");
		case PlayerBotEquipmentPurchaseStage::VerifyPurchase:
			{
				const PlayerBotServiceVerification verification = equipmentTransaction.verifyShopTransaction(observation.itemCount,
					observation.money, observation.bankBalance, true, maximumRetries);
				if (verification.result == PlayerBotServiceVerificationResult::Mismatch) {
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "transaction_delta_mismatch");
				}
				if (verification.result == PlayerBotServiceVerificationResult::Rejected) {
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "transaction_rejected");
				}
				if (verification.result == PlayerBotServiceVerificationResult::Success) {
					equipmentPurchaseSession.resetRetries();
					equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::Equip);
					return {command(PlayerBotProgressionCommandType::None), PlayerBotProgressionOutcomeType::Pending, 0, nullptr,
					        verification.before};
				}
				return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Retry, "purchase_equipment");
			}
		case PlayerBotEquipmentPurchaseStage::Equip:
			if (observation.equipmentVerified) {
				equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::VerifyEquipment);
				return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, nullptr);
			}
			if (!observation.equipmentAvailable) {
				if (equipmentPurchaseSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "purchased_item_unavailable");
				return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Retry, "purchased_item_unavailable");
			}
			if (!observation.openContainerRequired) equipmentPurchaseSession.observeContainerOpen(observation.containerDepth);
			if (observation.displacedMoveRequired || observation.openContainerRequired) {
				if (observation.openContainerRequired && !observation.containerAccessAvailable) {
					return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "purchased_item_container_unavailable");
				}
				if (equipmentPurchaseSession.retries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed,
					observation.displacedMoveRequired ? "displaced_item_move_not_verified" : "purchased_item_container_unavailable");
				equipmentPurchaseSession.incrementRetries();
				if (observation.openContainerRequired) equipmentPurchaseSession.beginContainerAccess(observation.containerDepth);
				return outcome(observation.openContainerRequired ? PlayerBotProgressionCommandType::Open : PlayerBotProgressionCommandType::Equip,
					PlayerBotProgressionOutcomeType::Retry, observation.openContainerRequired ? "open_equipment_container" : "preserve_displaced_equipment");
			}
			if (!observation.actionAvailable) return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Pending, "action_unavailable");
			if (!observation.equipmentPositionAvailable) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "purchased_item_position_unavailable");
			equipmentPurchaseSession.captureDisplacedItemCounts(observation.displacedCounts);
			equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::VerifyEquipment);
			return outcome(PlayerBotProgressionCommandType::Equip, PlayerBotProgressionOutcomeType::Pending, "equip_equipment");
		case PlayerBotEquipmentPurchaseStage::VerifyEquipment:
			if (observation.equipmentVerified && equipmentPurchaseSession.displacedItemsPreserved(observation.displacedCounts)) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Succeeded, "upgrade_equipped");
			if (equipmentPurchaseSession.incrementRetries() >= 3) return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed,
				equipmentPurchaseSession.displacedItemsPreserved(observation.displacedCounts) ? "equip_not_verified" : "displaced_item_lost");
			equipmentPurchaseSession.setStage(PlayerBotEquipmentPurchaseStage::Equip);
			return outcome(PlayerBotProgressionCommandType::None, PlayerBotProgressionOutcomeType::Retry, "equip_equipment");
	}
	return outcome(PlayerBotProgressionCommandType::Finish, PlayerBotProgressionOutcomeType::Failed, "invalid_equipment_stage");
}
