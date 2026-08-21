/**
 * Owns progression selection and durable procedure state. Controllers only
 * observe it and execute the resulting game actions.
 */
#ifndef FS_PLAYERBOTPROGRESSIONRUNTIME_H
#define FS_PLAYERBOTPROGRESSIONRUNTIME_H

#include "playerbotgoalplanner.h"
#include "playerbotprogressionsession.h"

enum class PlayerBotProgressionCommandType : uint8_t {
	None, Navigate, Use, Open, Equip, Speak, Shop, Finish, ServiceFallback,
};

struct PlayerBotProgressionCommand {
	PlayerBotProgressionCommandType type = PlayerBotProgressionCommandType::None;
	PlayerBotProgressionProcedure procedure = PlayerBotProgressionProcedure::None;
	uint8_t stage = 0;
	uint32_t attempts = 0;
	const char* reason = nullptr;
};

enum class PlayerBotProgressionOutcomeType : uint8_t {
	Pending, Succeeded, Retry, Failed, ServiceFallback,
};

struct PlayerBotProgressionOutcome {
	PlayerBotProgressionCommand command;
	PlayerBotProgressionOutcomeType type = PlayerBotProgressionOutcomeType::Pending;
	uint32_t attempts = 0;
	const char* reason = nullptr;
};

struct PlayerBotRewardObservation {
	bool navigationReached = false;
	bool navigationFailed = false;
	bool rewardObjectAvailable = true;
	bool inRange = true;
	bool actionAvailable = false;
	bool claimed = false;
	bool equipmentVerified = false;
	bool rootRelocationRequired = false;
	bool rootRelocationSpaceAvailable = true;
	bool displacedMoveRequired = false;
	bool displacedMoveSpaceAvailable = true;
	enum class ItemAccess : uint8_t {
		Ready, ActionUnavailable, BackpackUnavailable, BackpackClosed, RootUnavailable, DepthUnsupported,
		PathInvalid, ContainerPositionUnavailable, ContainerOpenRequired, ItemPathInvalid,
	};
	ItemAccess itemAccess = ItemAccess::Ready;
	size_t containerDepth = 0;
	PlayerBotRewardClaimSnapshot currentClaim;
	std::map<uint16_t, uint32_t> displacedCounts;
};

struct PlayerBotDepartureObservation {
	bool navigationReached = false;
	bool navigationFailed = false;
	bool npcAvailable = true;
	bool greetingAcknowledged = false;
	bool departureVerified = false;
};

struct PlayerBotSpellTrainingObservation {
	bool navigationReached = false;
	bool navigationFailed = false;
	bool npcAvailable = true;
	bool greetingAcknowledged = false;
	bool learned = false;
	uint64_t totalMoney = 0;
};

struct PlayerBotEquipmentPurchaseObservation {
	bool navigationReached = false;
	bool navigationFailed = false;
	bool providerAvailable = true;
	bool providerInRange = true;
	bool offerAvailable = true;
	bool shopReady = false;
	bool fundingAvailable = true;
	bool transactionSucceeded = false;
	bool transactionRejected = false;
	bool transactionMismatch = false;
	bool equipmentAvailable = false;
	bool equipmentPositionAvailable = true;
	bool actionAvailable = false;
	bool equipmentVerified = false;
	bool openContainerRequired = false;
	bool containerAccessAvailable = true;
	size_t containerDepth = 0;
	bool displacedMoveRequired = false;
	std::map<uint16_t, uint32_t> displacedCounts;
};

class PlayerBotProgressionRuntime {
	public:
		PlayerBotGoalArbiter::GoalDecision decide(const PlayerBotGoalPlannerSnapshot& snapshot);
		PlayerBotGoalArbiter::GoalDecision force(PlayerBotGoalArbiter::GoalCandidate candidate);
		void apply(const PlayerBotGoalArbiter::GoalDecision& decision);

		PlayerBotProgressionProcedure active() const { return progression.active(); }
		bool active(PlayerBotProgressionProcedure procedure) const { return progression.active(procedure); }
		PlayerBotGoalArbiter::TopLevelGoal activeGoal() const { return arbiter.activeGoal(); }
		void setActiveGoal(PlayerBotGoalArbiter::TopLevelGoal goal) { arbiter.setActiveGoal(goal); }
		uint64_t decisionId() const { return arbiter.decisionId(); }
		bool isCoolingDown(PlayerBotGoalArbiter::TopLevelGoal goal, std::chrono::steady_clock::time_point now) const;
		void setCooldown(PlayerBotGoalArbiter::TopLevelGoal goal, std::chrono::steady_clock::duration duration);
		const PlayerBotGoalArbiter& goalArbiter() const { return arbiter; }
		const PlayerBotProgressionSession& session() const { return progression; }

		void beginReward(PlayerBotRewardPlan plan, std::map<uint16_t, uint32_t> displacedCounts);
		void beginDeparture(PlayerBotOracleDeparturePlan plan);
		void beginSpellTraining(PlayerBotSpellTrainingPlan plan);
		void beginEquipmentPurchase(PlayerBotEquipmentOfferEvaluation plan);
		void finish();

		const PlayerBotRewardSession& reward() const { return rewardSession; }
		const PlayerBotOracleDepartureSession& departure() const { return departureSession; }
		const PlayerBotSpellTrainingSession& spellTraining() const { return spellTrainingSession; }
		const PlayerBotEquipmentPurchaseSession& equipmentPurchase() const { return equipmentPurchaseSession; }

		PlayerBotProgressionOutcome advanceReward(const PlayerBotRewardObservation& observation);
		PlayerBotProgressionOutcome advanceDeparture(const PlayerBotDepartureObservation& observation);
		PlayerBotProgressionOutcome advanceSpellTraining(const PlayerBotSpellTrainingObservation& observation);
		PlayerBotProgressionOutcome advanceEquipmentPurchase(const PlayerBotEquipmentPurchaseObservation& observation);
		PlayerBotProgressionCommand command(PlayerBotProgressionCommandType type, const char* reason = nullptr) const;
		PlayerBotProgressionCommand navigate(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Navigate, reason); }
		PlayerBotProgressionCommand use(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Use, reason); }
		PlayerBotProgressionCommand open(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Open, reason); }
		PlayerBotProgressionCommand equip(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Equip, reason); }
		PlayerBotProgressionCommand speak(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Speak, reason); }
		PlayerBotProgressionCommand shop(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Shop, reason); }
		PlayerBotProgressionCommand finishCommand(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::Finish, reason); }
		PlayerBotProgressionCommand serviceFallback(const char* reason = nullptr) const { return command(PlayerBotProgressionCommandType::ServiceFallback, reason); }
		PlayerBotProgressionOutcome outcome(PlayerBotProgressionCommand command, PlayerBotProgressionOutcomeType type,
		                                    uint32_t attempts = 0, const char* reason = nullptr) const
		{
			return {command, type, attempts, reason};
		}

	private:
		PlayerBotGoalPlanner planner;
		PlayerBotGoalArbiter arbiter;
		PlayerBotProgressionSession progression;
		PlayerBotRewardSession rewardSession;
		PlayerBotOracleDepartureSession departureSession;
		PlayerBotSpellTrainingSession spellTrainingSession;
		PlayerBotEquipmentPurchaseSession equipmentPurchaseSession;
};

#endif
