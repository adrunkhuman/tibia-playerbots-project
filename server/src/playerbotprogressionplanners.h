/**
 * Pure progression candidate policy. Controller adapters build these immutable
 * observations from the game world and execute only the selected plan.
 */
#ifndef FS_PLAYERBOTPROGRESSIONPLANNERS_H
#define FS_PLAYERBOTPROGRESSIONPLANNERS_H

#include "playerboteconomy.h"
#include "playerbotinventorypolicy.h"
#include "playerbotprogressionsession.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

struct PlayerBotRouteEstimate {
	bool reachable = false;
	bool nodeLimitReached = false;
	Position approachPosition;
	uint32_t steps = 0;
	uint64_t expandedNodes = 0;
};

struct PlayerBotRewardItemInspection {
	uint16_t itemId = 0;
	uint32_t count = 0;
	uint32_t depth = 0;
	uint16_t rootOrdinal = 0;
	std::vector<uint16_t> path;
	std::vector<std::string> classes;
	uint32_t worth = 0;
	uint32_t sellValue = 0;
};

struct PlayerBotRewardInspection {
	std::vector<PlayerBotRewardItemInspection> items;
	std::vector<uint16_t> rootItemIds;
	std::vector<std::string> rootSignatures;
	std::vector<std::string> nonStackableRootSignatures;
	std::map<uint16_t, uint32_t> stackableRootCounts;
	std::optional<PlayerBotEquipmentUpgrade> bestUpgrade;
	std::optional<PlayerBotEquipmentOfferEvaluation> bestEquipment;
	std::string equipmentRejection;
	uint16_t bestItemId = 0;
	uint16_t bestRootOrdinal = 0;
	std::vector<uint16_t> bestItemPath;
	std::string bestRootSignature;
	uint16_t primaryKnownItemId = 0;
	uint16_t primaryKnownRootOrdinal = 0;
	std::string primaryKnownRootSignature;
	uint32_t primaryKnownItemUtility = 0;
	uint32_t itemCount = 0;
	uint32_t containerCount = 0;
	uint32_t unknownCount = 0;
	uint32_t equipmentUpgradeCount = 0;
	uint32_t currencyValue = 0;
	uint32_t sellValue = 0;
	uint32_t potionCount = 0;
	uint32_t foodCount = 0;
	uint32_t ropeCount = 0;
	uint32_t shovelCount = 0;
	int32_t knownUtility = 0;
};

struct PlayerBotRewardInspectionContext {
	const PlayerBotEquipmentLoadout& currentLoadout;
	uint32_t heldPotions = 0;
	uint32_t heldFood = 0;
	bool ownsRope = false;
	bool ownsShovel = false;
	uint16_t potionItemId;
	uint32_t potionRestockTarget;
	uint32_t preferredFoodCount;
	uint16_t ropeItemId;
	uint16_t shovelItemId;
	int32_t missingPotionUtility;
	int32_t foodPreferenceUtility;
};

struct PlayerBotRewardItemObservation {
	uint16_t itemId = 0;
	uint32_t count = 0;
	uint32_t depth = 0;
	uint16_t rootOrdinal = 0;
	std::vector<uint16_t> path;
	std::string rootSignature;
	uint32_t worth = 0;
	uint32_t sellValue = 0;
	bool container = false;
	bool equipmentCandidate = false;
	std::optional<PlayerBotEquipmentOfferEvaluation> equipment;
	int32_t currentValue = 0;
	int32_t candidateValue = 0;
	const char* metric = nullptr;
	bool potion = false;
	bool food = false;
	bool rope = false;
	bool shovel = false;
};

struct PlayerBotRewardInspectionSnapshot {
	std::vector<uint16_t> rootItemIds;
	std::vector<std::string> rootSignatures;
	std::vector<std::string> nonStackableRootSignatures;
	std::map<uint16_t, uint32_t> stackableRootCounts;
	std::vector<PlayerBotRewardItemObservation> items;
};

struct PlayerBotDepartureProviderSnapshot {
	uint32_t npcId = 0;
	Position npcPosition;
	PlayerBotRouteEstimate route;
};

struct PlayerBotDeparturePlannerSnapshot {
	uint32_t level = 0;
	uint16_t vocationId = 0;
	uint32_t townId = 0;
	uint16_t minimumLevel = 0;
	uint16_t maximumLevel = 0;
	uint32_t rookgaardTownId = 0;
	std::vector<PlayerBotDepartureProviderSnapshot> providers;
};

class PlayerBotDeparturePlanner
{
	public:
		bool hasCompleted(const PlayerBotDeparturePlannerSnapshot& snapshot) const;
		bool required(const PlayerBotDeparturePlannerSnapshot& snapshot) const;
		std::optional<PlayerBotOracleDeparturePlan> select(const PlayerBotDeparturePlannerSnapshot& snapshot) const;
};

struct PlayerBotSpellOfferSnapshot {
	uint32_t npcId = 0;
	Position npcPosition;
	std::string npcName;
	std::string spellName;
	std::string keyword;
	uint32_t price = 0;
	uint32_t level = 0;
	bool premium = false;
	bool inScope = false;
	bool registryMatches = false;
	bool vocationEligible = false;
	bool levelEligible = false;
	bool premiumEligible = false;
	bool known = false;
	bool suppliesReady = false;
	PlayerBotRouteEstimate route;
};

struct PlayerBotSpellTrainingPlannerSnapshot {
	uint64_t reserve = 0;
	uint64_t totalMoney = 0;
	bool reserveAvailable = false;
	std::vector<PlayerBotSpellOfferSnapshot> offers;
};

struct PlayerBotPlannerOfferRejection {
	size_t offerIndex = 0;
	std::string reason;
};

struct PlayerBotSpellTrainingDecision {
	std::optional<PlayerBotSpellTrainingPlan> selected;
	std::optional<size_t> selectedOfferIndex;
	std::vector<PlayerBotPlannerOfferRejection> rejections;
};

class PlayerBotSpellTrainingPlanner
{
	public:
		PlayerBotSpellTrainingDecision select(const PlayerBotSpellTrainingPlannerSnapshot& snapshot) const;
};

struct PlayerBotEquipmentProviderOfferSnapshot {
	PlayerBotEquipmentOfferEvaluation evaluation;
	uint32_t itemWeight = 0;
	uint32_t freeBackpackSlots = 0;
	bool backpackAvailable = false;
	bool purchaseAvailable = false;
	bool routeBudgetExhausted = false;
	PlayerBotRouteEstimate route;
};

struct PlayerBotEquipmentProviderPlannerSnapshot {
	bool enabled = false;
	uint64_t reserve = 0;
	uint64_t totalMoney = 0;
	bool reserveAvailable = false;
	uint32_t freeCapacity = 0;
	std::vector<PlayerBotEquipmentProviderOfferSnapshot> offers;
};

struct PlayerBotEquipmentProviderDecision {
	bool evaluated = false;
	std::optional<PlayerBotEquipmentOfferEvaluation> selected;
	std::optional<size_t> selectedOfferIndex;
	std::vector<PlayerBotPlannerOfferRejection> rejections;
};

class PlayerBotEquipmentProviderPlanner
{
	public:
		PlayerBotEquipmentProviderDecision select(const PlayerBotEquipmentProviderPlannerSnapshot& snapshot) const;
};

struct PlayerBotRewardCandidateSnapshot {
	PlayerBotRewardPlan plan;
	bool claimed = false;
	bool ownedUpgrade = false;
	uint32_t totalWeight = 0;
	bool backpackAvailable = false;
	uint32_t freeBackpackSlots = 0;
	PlayerBotRouteEstimate route;
};

struct PlayerBotRewardPlannerSnapshot {
	uint32_t freeCapacity = 0;
	int32_t pickupBaseUtility = 0;
	int32_t economicBaseUtility = 0;
	int32_t huntUtility = 0;
	std::vector<PlayerBotRewardCandidateSnapshot> candidates;
};

struct PlayerBotRewardDecision {
	std::optional<PlayerBotRewardPlan> selected;
	struct Outcome {
		PlayerBotRewardPlan plan;
		const char* result = nullptr;
		const char* reason = nullptr;
	};
	std::vector<Outcome> outcomes;
};

class PlayerBotRewardPlanner
{
	public:
		PlayerBotRewardInspection inspect(const PlayerBotRewardInspectionSnapshot& snapshot,
		                                  const PlayerBotRewardInspectionContext& context) const;
		std::optional<PlayerBotRewardPlan> plan(uint16_t uniqueId, const Position& itemPosition,
		                                        uint32_t estimatedDistance, const PlayerBotRewardInspection& inspection) const;
		int32_t utility(const PlayerBotRewardPlan& plan, const PlayerBotRewardPlannerSnapshot& snapshot) const;
		int32_t estimatedUtility(const PlayerBotRewardPlan& plan,
		                         const PlayerBotRewardPlannerSnapshot& snapshot) const;
		std::vector<size_t> routeCandidates(const PlayerBotRewardPlannerSnapshot& snapshot) const;
		PlayerBotRewardDecision select(const PlayerBotRewardPlannerSnapshot& snapshot) const;

	private:
		void finalizeInspection(const PlayerBotRewardInspectionContext& context,
		                       PlayerBotRewardInspection& inspection) const;
};

#endif
