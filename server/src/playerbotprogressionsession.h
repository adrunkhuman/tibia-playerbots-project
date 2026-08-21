/**
 * Durable state for the independent playerbot progression procedures.
 * Planning, action dispatch, navigation, and telemetry remain in the controller.
 */
#ifndef FS_PLAYERBOTPROGRESSIONSESSION_H
#define FS_PLAYERBOTPROGRESSIONSESSION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "playerbotequipmentpolicy.h"
#include "position.h"

enum class PlayerBotProgressionProcedure : uint8_t {
	None,
	PickupReward,
	OracleDeparture,
	LearnSpell,
	BuyEquipment,
};

struct PlayerBotRewardPlan {
	uint16_t uniqueId = 0;
	uint16_t itemId = 0;
	uint16_t rootItemId = 0;
	uint16_t rootOrdinal = 0;
	Position itemPosition;
	Position approachPosition;
	slots_t slot = CONST_SLOT_WHEREEVER;
	int32_t benefit = 0;
	std::string metric;
	int32_t currentValue = 0;
	int32_t candidateValue = 0;
	uint32_t travelSteps = 0;
	uint32_t estimatedDistance = 0;
	uint32_t requiredBackpackSlots = 0;
	uint16_t replacedItemId = 0;
	uint16_t displacedLeftItemId = 0;
	uint16_t displacedRightItemId = 0;
	uint32_t knownUtility = 0;
	uint32_t itemCount = 0;
	uint32_t containerCount = 0;
	uint32_t unknownCount = 0;
	uint32_t currencyValue = 0;
	uint32_t sellValue = 0;
	uint32_t equipmentUpgradeCount = 0;
	uint64_t expandedNodes = 0;
	std::vector<uint16_t> selectedItemPath;
	std::string rootSignature;
	std::vector<std::string> rootSignatures;
	std::vector<std::string> nonStackableRootSignatures;
	std::map<uint16_t, uint32_t> stackableRootCounts;
	bool resumeEquipment = false;
};

enum class PlayerBotRewardStage : uint8_t {
	Travel,
	UseReward,
	VerifyReward,
	EquipReward,
	VerifyEquipment,
};

struct PlayerBotRewardClaimSnapshot {
	uint32_t itemCount = 0;
	uint32_t rootCount = 0;
	std::map<std::string, uint32_t> roots;
	std::map<uint16_t, uint32_t> stackables;
};

struct PlayerBotNestedContainerAccessState {
	size_t depth = SIZE_MAX;
	uint32_t openAttempts = 0;
};

class PlayerBotRewardSession
{
	public:
	void begin(PlayerBotRewardPlan reward);
	void reset();

	const PlayerBotRewardPlan& plan() const { return reward; }
	PlayerBotRewardPlan& plan() { return reward; }
	PlayerBotRewardStage stage() const { return currentStage; }
	void setStage(PlayerBotRewardStage stage) { currentStage = stage; }
	uint32_t retries() const { return attempts; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	PlayerBotRewardClaimSnapshot& claimSnapshot() { return claim; }
	const PlayerBotRewardClaimSnapshot& claimSnapshot() const { return claim; }
	PlayerBotNestedContainerAccessState& containerAccess() { return nestedContainer; }
	std::map<uint16_t, uint32_t>& displacedItemCounts() { return displaced; }
	const std::map<uint16_t, uint32_t>& displacedItemCounts() const { return displaced; }

	private:
	PlayerBotRewardPlan reward;
	PlayerBotRewardStage currentStage = PlayerBotRewardStage::Travel;
	uint32_t attempts = 0;
	PlayerBotRewardClaimSnapshot claim;
	PlayerBotNestedContainerAccessState nestedContainer;
	std::map<uint16_t, uint32_t> displaced;
};

struct PlayerBotOracleDeparturePlan {
	uint32_t npcId = 0;
	Position npcPosition;
	Position approachPosition;
	uint32_t travelSteps = 0;
	uint64_t expandedNodes = 0;
};

enum class PlayerBotOracleDepartureStage : uint8_t {
	Travel,
	Greet,
	ConfirmReady,
	ChooseTown,
	ChooseVocation,
	ConfirmVocation,
	Verify,
};

class PlayerBotOracleDepartureSession
{
	public:
	void begin(PlayerBotOracleDeparturePlan departure);
	void reset();

	const PlayerBotOracleDeparturePlan& plan() const { return departure; }
	PlayerBotOracleDepartureStage stage() const { return currentStage; }
	void setStage(PlayerBotOracleDepartureStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }

	private:
	PlayerBotOracleDeparturePlan departure;
	PlayerBotOracleDepartureStage currentStage = PlayerBotOracleDepartureStage::Travel;
	uint32_t attempts = 0;
};

struct PlayerBotSpellTrainingPlan {
	uint32_t npcId = 0;
	Position npcPosition;
	Position approachPosition;
	std::string spellName;
	std::string keyword;
	uint32_t price = 0;
	uint32_t level = 0;
	uint32_t travelSteps = 0;
	uint64_t reserve = 0;
};

enum class PlayerBotSpellTrainingStage : uint8_t {
	Travel,
	Greet,
	Request,
	Confirm,
	Verify,
};

class PlayerBotSpellTrainingSession
{
	public:
	void begin(PlayerBotSpellTrainingPlan training);
	void reset();

	const PlayerBotSpellTrainingPlan& plan() const { return training; }
	PlayerBotSpellTrainingStage stage() const { return currentStage; }
	void setStage(PlayerBotSpellTrainingStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	uint64_t moneyBefore() const { return beforeMoney; }
	void setMoneyBefore(uint64_t money) { beforeMoney = money; }

	private:
	PlayerBotSpellTrainingPlan training;
	PlayerBotSpellTrainingStage currentStage = PlayerBotSpellTrainingStage::Travel;
	uint32_t attempts = 0;
	uint64_t beforeMoney = 0;
};

enum class PlayerBotEquipmentPurchaseStage : uint8_t {
	Travel,
	Purchase,
	VerifyPurchase,
	Equip,
	VerifyEquipment,
};

class PlayerBotEquipmentPurchaseSession
{
	public:
	void begin(PlayerBotEquipmentOfferEvaluation purchase);
	void reset();

	const PlayerBotEquipmentOfferEvaluation& plan() const { return purchase; }
	PlayerBotEquipmentOfferEvaluation& plan() { return purchase; }
	PlayerBotEquipmentPurchaseStage stage() const { return currentStage; }
	void setStage(PlayerBotEquipmentPurchaseStage stage) { currentStage = stage; }
	uint32_t retries() const { return attempts; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	std::map<uint16_t, uint32_t>& displacedItemCounts() { return displaced; }
	const std::map<uint16_t, uint32_t>& displacedItemCounts() const { return displaced; }
	PlayerBotNestedContainerAccessState& containerAccess() { return nestedContainer; }

	private:
	PlayerBotEquipmentOfferEvaluation purchase;
	PlayerBotEquipmentPurchaseStage currentStage = PlayerBotEquipmentPurchaseStage::Travel;
	uint32_t attempts = 0;
	std::map<uint16_t, uint32_t> displaced;
	PlayerBotNestedContainerAccessState nestedContainer;
};

class PlayerBotProgressionSession
{
	public:
	PlayerBotProgressionProcedure active() const { return procedure; }
	bool active(PlayerBotProgressionProcedure value) const { return procedure == value; }
	void begin(PlayerBotProgressionProcedure value) { procedure = value; }
	void reset() { procedure = PlayerBotProgressionProcedure::None; }

	private:
	PlayerBotProgressionProcedure procedure = PlayerBotProgressionProcedure::None;
};

#endif
