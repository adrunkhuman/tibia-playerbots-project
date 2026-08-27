/**
 * Owns progression selection and durable procedure state. Controllers only
 * observe it and execute the resulting game actions.
 */
#ifndef FS_PLAYERBOTPROGRESSIONRUNTIME_H
#define FS_PLAYERBOTPROGRESSIONRUNTIME_H

#include "playerbotgoalplanner.h"
#include "playerbotnpcsession.h"
#include "playerbotprogressionsession.h"
#include "playerbotservicesession.h"

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
	PlayerBotServiceTransaction transaction;
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
	bool otherShopOpen = false;
	bool fundingAvailable = true;
	uint32_t itemCount = 0;
	uint64_t money = 0;
	uint64_t bankBalance = 0;
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

struct PlayerBotReadinessEquipmentObservation {
	bool actionAvailable = false;
	bool upgradeAvailable = false;
	uint16_t itemId = 0;
	slots_t slot = CONST_SLOT_WHEREEVER;
	bool openContainerRequired = false;
	bool containerAccessAvailable = true;
	bool equipmentVerified = false;
	bool combatReady = false;
};

enum class PlayerBotReadinessEquipmentCommandType : uint8_t {
	None, OpenContainer, Equip, Retry, ResumeService, StartHunt, ServiceFallback,
};

struct PlayerBotReadinessEquipmentCommand {
	PlayerBotReadinessEquipmentCommandType type = PlayerBotReadinessEquipmentCommandType::None;
	uint16_t itemId = 0;
	slots_t slot = CONST_SLOT_WHEREEVER;
	uint32_t attempts = 0;
	const char* reason = nullptr;
};

struct PlayerBotReadinessEquipmentSnapshot {
	uint16_t itemId = 0;
	slots_t slot = CONST_SLOT_WHEREEVER;
	uint32_t attempts = 0;
	bool pending = false;
};

struct PlayerBotEquipmentShopCommand {
	PlayerBotNpcSessionResult result = PlayerBotNpcSessionResult::Pending;
	const char* speech = nullptr;
	bool closeOtherShop = false;
	uint32_t delay = 0;
	const char* failureReason = nullptr;
};

class PlayerBotProgressionRuntime {
	public:
		PlayerBotGoalArbiter::GoalDecision selectGoal(const PlayerBotGoalPlannerSnapshot& snapshot);
		PlayerBotGoalArbiter::GoalDecision interruptForDeparture(bool feasible, int32_t utility, std::string reason);
		PlayerBotGoalArbiter::GoalDecision interruptHuntForService(std::string reason);
		void enterService();
		void enterHunt();
		void completeReward(bool succeeded, std::chrono::steady_clock::duration cooldown);
		void completeSpellTraining(bool succeeded, std::chrono::steady_clock::duration cooldown);
		void completeEquipmentPurchase(bool succeeded, std::chrono::steady_clock::duration cooldown);
		void completeMagicTraining(std::chrono::steady_clock::duration cooldown);

		PlayerBotProgressionProcedure active() const { return progression.active(); }
		bool active(PlayerBotProgressionProcedure procedure) const { return progression.active(procedure); }
		PlayerBotGoalArbiter::TopLevelGoal activeGoal() const { return arbiter.activeGoal(); }
		uint64_t decisionId() const { return arbiter.decisionId(); }
		bool isCoolingDown(PlayerBotGoalArbiter::TopLevelGoal goal, std::chrono::steady_clock::time_point now) const;
		const PlayerBotProgressionSession& session() const { return progression; }

		void beginReward(PlayerBotRewardPlan plan, std::map<uint16_t, uint32_t> displacedCounts);
		void beginDeparture(PlayerBotOracleDeparturePlan plan);
		void beginSpellTraining(PlayerBotSpellTrainingPlan plan);
		void beginEquipmentPurchase(PlayerBotEquipmentOfferEvaluation plan);
		void finish();
		bool reportNpcReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type);
		bool greetingAcknowledged() const { return npcSession.isGreetingAcknowledged(); }
		void clearGreetingAcknowledgement() { npcSession.resetGreetingAcknowledgement(); }
		void restartDepartureConversation();
		void restartSpellTrainingConversation();
		void restartEquipmentConversation();
		PlayerBotEquipmentShopCommand advanceEquipmentShop(const PlayerBotNpcShopObservation& observation,
		                                                  uint32_t maximumRetries);
		bool readinessEquipmentPending() const { return readinessEquipment.pending; }
		PlayerBotReadinessEquipmentSnapshot readinessEquipmentSnapshot() const
		{
			return {readinessEquipment.itemId, readinessEquipment.slot, readinessEquipment.attempts, readinessEquipment.pending};
		}
		PlayerBotReadinessEquipmentCommand beginReadinessEquipment(const PlayerBotReadinessEquipmentObservation& observation,
		                                                            bool resumeService, uint32_t maximumRetries);
		PlayerBotReadinessEquipmentCommand advanceReadinessEquipment(const PlayerBotReadinessEquipmentObservation& observation,
		                                                              uint32_t maximumRetries);

		const PlayerBotRewardSession& reward() const { return rewardSession; }
		const PlayerBotOracleDepartureSession& departure() const { return departureSession; }
		const PlayerBotSpellTrainingSession& spellTraining() const { return spellTrainingSession; }
		const PlayerBotEquipmentPurchaseSession& equipmentPurchase() const { return equipmentPurchaseSession; }

		PlayerBotProgressionOutcome advanceReward(const PlayerBotRewardObservation& observation);
		PlayerBotProgressionOutcome advanceDeparture(const PlayerBotDepartureObservation& observation);
		PlayerBotProgressionOutcome advanceSpellTraining(const PlayerBotSpellTrainingObservation& observation);
		PlayerBotProgressionOutcome advanceEquipmentPurchase(const PlayerBotEquipmentPurchaseObservation& observation,
		                                                     uint32_t maximumRetries);
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
		void beginNpcConversation(uint32_t npcId);
		void finishNpcConversation();
		PlayerBotGoalPlanner planner;
		PlayerBotGoalArbiter arbiter;
		PlayerBotProgressionSession progression;
		PlayerBotRewardSession rewardSession;
		PlayerBotOracleDepartureSession departureSession;
		PlayerBotSpellTrainingSession spellTrainingSession;
		PlayerBotEquipmentPurchaseSession equipmentPurchaseSession;
		PlayerBotNpcSession npcSession;
		PlayerBotServiceSession equipmentTransaction;
		struct {
			uint16_t itemId = 0;
			slots_t slot = CONST_SLOT_WHEREEVER;
			uint32_t attempts = 0;
			bool pending = false;
			bool resumeService = false;
		} readinessEquipment;
};

#endif
