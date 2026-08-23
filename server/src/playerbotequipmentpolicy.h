/**
 * Durable playerbot equipment decisions. World discovery, execution, and
 * telemetry are deliberately supplied by callers.
 */
#ifndef FS_PLAYERBOTEQUIPMENTPOLICY_H
#define FS_PLAYERBOTEQUIPMENTPOLICY_H

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "creature.h"
#include "playerbothuntregions.h"

class Item;
class ItemType;
class Player;

struct PlayerBotEquipmentUpgrade {
	slots_t slot = CONST_SLOT_WHEREEVER;
	int32_t benefit = 0;
	const char* metric = nullptr;
	int32_t currentValue = 0;
	int32_t candidateValue = 0;
};

struct PlayerBotEquipmentLoadout {
	std::array<uint16_t, CONST_SLOT_LAST + 1> itemIds{};
};

struct PlayerBotEquipmentHuntSummary {
	uint32_t suitableRegions = 0;
	double bestProjectedExperience = 0;
	double lowestThreatRatio = 0;
	uint32_t evaluatedRegions = 0;
	bool truncated = false;
};

enum class PlayerBotEquipmentDecisionRule : uint8_t {
	None,
	ParetoImprovement,
	UnlocksHunt,
	ReadinessRepair,
};

struct PlayerBotEquipmentOfferEvaluation {
	uint32_t npcId = 0;
	Position npcPosition;
	Position approachPosition;
	uint16_t itemId = 0;
	uint32_t price = 0;
	slots_t slot = CONST_SLOT_WHEREEVER;
	uint16_t replacedItemId = 0;
	uint16_t displacedLeftItemId = 0;
	uint16_t displacedRightItemId = 0;
	PlayerBotCombatProfile profile;
	PlayerBotEquipmentHuntSummary hunts;
	bool currentReady = false;
	bool candidateReady = false;
	bool carried = false;
	bool simulated = false;
	std::string rejection;
	PlayerBotEquipmentDecisionRule rule = PlayerBotEquipmentDecisionRule::None;
	uint32_t travelSteps = 0;
};

struct PlayerBotEquipmentReadiness {
	bool ready = false;
	std::string recovery;
	std::string terminalReason;
};

struct PlayerBotEquipmentReadinessInput {
	bool backpackReady = false;
	bool suppliesReady = false;
	uint32_t effectiveFreeCapacity = 0;
	uint32_t minimumFreeCapacity = 0;
};

class PlayerBotEquipmentPolicy
{
	public:
		using HuntSummaryEvaluator = std::function<PlayerBotEquipmentHuntSummary(Player&, const PlayerBotCombatProfile&)>;

		explicit PlayerBotEquipmentPolicy(uint16_t combatReadinessVocationId);
		bool requiresKnightCombatReadiness(const Player& player) const;
		bool isLegalEquipmentType(const Player& player, const ItemType& type) const;
		bool isLegalEquipmentItem(const Player& player, const Item& item) const;
		bool isKnightMeleeWeapon(const Player& player, const Item& item) const;
		bool isCombatEquipment(const Item& item) const;

		std::optional<PlayerBotEquipmentUpgrade> evaluateUpgrade(const Player& player, const Item& candidate) const;
		bool findCarriedUpgrade(Player& player, Item*& selectedItem, PlayerBotEquipmentUpgrade& selectedUpgrade) const;
		PlayerBotEquipmentLoadout loadout(const Player& player) const;
		bool applyOffer(const Player& player, PlayerBotEquipmentLoadout& loadout, uint16_t itemId, slots_t& slot,
		                uint16_t& replacedItemId, uint16_t& displacedLeftItemId, uint16_t& displacedRightItemId,
		                std::string& rejection) const;
		PlayerBotCombatProfile combatProfile(const Player& player, const PlayerBotEquipmentLoadout& loadout) const;
		bool loadoutReady(const Player& player, const PlayerBotEquipmentLoadout& loadout,
		                  const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight = 0) const;
		PlayerBotEquipmentReadiness combatReadiness(const Player& player, bool carriedUpgrade,
		                                            const PlayerBotEquipmentReadinessInput& readiness) const;
		PlayerBotEquipmentOfferEvaluation evaluateCandidate(Player& player, uint16_t itemId,
		                                                    const PlayerBotEquipmentLoadout& currentLoadout,
		                                                    const PlayerBotCombatProfile& currentProfile,
		                                                    const PlayerBotEquipmentHuntSummary& currentHunts,
		                                                    bool currentReady,
		                                                    const PlayerBotEquipmentReadinessInput& readiness,
		                                                    uint32_t additionalWeight, bool allowSimulation,
		                                                    const HuntSummaryEvaluator& huntSummary) const;

		static const char* decisionRuleName(PlayerBotEquipmentDecisionRule rule);
		static bool prefers(const PlayerBotEquipmentOfferEvaluation& candidate,
		                    const PlayerBotEquipmentOfferEvaluation& current);

	private:
		const uint16_t combatReadinessVocationId;
};

#endif
