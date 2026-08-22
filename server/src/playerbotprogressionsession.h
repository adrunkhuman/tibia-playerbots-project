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

#include "creature.h"
#include "playerbotequipmentpolicy.h"
#include "position.h"

class PlayerBotProgressionRuntime;

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
	const PlayerBotRewardPlan& plan() const { return reward; }
	PlayerBotRewardStage stage() const { return currentStage; }
	uint32_t retries() const { return attempts; }
	const PlayerBotRewardClaimSnapshot& claimSnapshot() const { return claim; }

	private:
	friend class PlayerBotProgressionRuntime;
	void begin(PlayerBotRewardPlan reward);
	void reset();
	void setStage(PlayerBotRewardStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	void captureClaimSnapshot(PlayerBotRewardClaimSnapshot snapshot) { claim = std::move(snapshot); }
	uint32_t beginContainerAccess(size_t depth);
	void observeContainerOpen(size_t depth);
	bool displacedItemsPreserved(const std::map<uint16_t, uint32_t>& counts) const;
	void captureDisplacedItemCounts(std::map<uint16_t, uint32_t> counts) { displaced = std::move(counts); }
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
	const PlayerBotOracleDeparturePlan& plan() const { return departure; }
	PlayerBotOracleDepartureStage stage() const { return currentStage; }
	uint32_t retries() const { return attempts; }

	private:
	friend class PlayerBotProgressionRuntime;
	void begin(PlayerBotOracleDeparturePlan departure);
	void reset();
	void setStage(PlayerBotOracleDepartureStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
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
	const PlayerBotSpellTrainingPlan& plan() const { return training; }
	PlayerBotSpellTrainingStage stage() const { return currentStage; }
	uint32_t retries() const { return attempts; }
	uint64_t moneyBefore() const { return beforeMoney; }

	private:
	friend class PlayerBotProgressionRuntime;
	void begin(PlayerBotSpellTrainingPlan training);
	void reset();
	void setStage(PlayerBotSpellTrainingStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	void setMoneyBefore(uint64_t money) { beforeMoney = money; }
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
	const PlayerBotEquipmentOfferEvaluation& plan() const { return purchase; }
	PlayerBotEquipmentPurchaseStage stage() const { return currentStage; }
	uint32_t retries() const { return attempts; }

	private:
	friend class PlayerBotProgressionRuntime;
	void begin(PlayerBotEquipmentOfferEvaluation purchase);
	void reset();
	void setStage(PlayerBotEquipmentPurchaseStage stage) { currentStage = stage; }
	uint32_t incrementRetries() { return ++attempts; }
	void resetRetries() { attempts = 0; }
	void captureDisplacedItemCounts(std::map<uint16_t, uint32_t> counts) { displaced = std::move(counts); }
	bool displacedItemsPreserved(const std::map<uint16_t, uint32_t>& counts) const;
	uint32_t beginContainerAccess(size_t depth);
	void observeContainerOpen(size_t depth);
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

	private:
	friend class PlayerBotProgressionRuntime;
	void begin(PlayerBotProgressionProcedure value) { procedure = value; }
	void reset() { procedure = PlayerBotProgressionProcedure::None; }
	PlayerBotProgressionProcedure procedure = PlayerBotProgressionProcedure::None;
};

#endif
