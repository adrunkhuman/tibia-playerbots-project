/** Durable equipment decisions over immutable caller-provided observations. */
#ifndef FS_PLAYERBOTEQUIPMENTPOLICY_H
#define FS_PLAYERBOTEQUIPMENTPOLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdint.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "playerbotcombatprofile.h"
#include "position.h"

enum slots_t : uint8_t;

// Standard 8.60 inventory slots are numbered 0 through 10.
inline constexpr size_t playerBotEquipmentSlotCount = 11;

enum class PlayerBotEquipmentWeaponType : uint8_t {
	None, Shield, Sword, Club, Axe, Distance, Ammo, Other,
};

struct PlayerBotEquipmentItemSnapshot {
	uint16_t itemId = 0;
	uint16_t minimumLevel = 0;
	uint16_t minimumMagicLevel = 0;
	uint32_t weight = 0;
	uint32_t count = 0;
	int32_t armor = 0;
	int32_t defense = 0;
	int32_t extraDefense = 0;
	int32_t attack = 0;
	std::vector<uint16_t> vocationIds;
	PlayerBotEquipmentWeaponType weaponType = PlayerBotEquipmentWeaponType::None;
	bool pickupable = false;
	bool removed = false;
	bool premiumRequired = false;
	bool head = false;
	bool armorSlot = false;
	bool legs = false;
	bool feet = false;
	bool left = false;
	bool right = false;
	bool twoHanded = false;
	bool container = false;
	bool inContainer = false;
	bool equipped = false;
};

struct PlayerBotEquipmentPlayerSnapshot {
	uint32_t level = 0;
	uint32_t magicLevel = 0;
	uint16_t vocationId = 0;
	bool premium = false;
	int32_t maximumHealth = 0;
	int32_t fistSkill = 0;
	int32_t swordSkill = 0;
	int32_t clubSkill = 0;
	int32_t axeSkill = 0;
	int32_t distanceSkill = 0;
	int32_t shieldSkill = 0;
	float attackFactor = 1.0f;
	float defenseFactor = 1.0f;
	double armorMultiplier = 1.0;
	double defenseMultiplier = 1.0;
};

struct PlayerBotEquipmentUpgrade {
	slots_t slot = static_cast<slots_t>(0);
	int32_t benefit = 0;
	const char* metric = nullptr;
	int32_t currentValue = 0;
	int32_t candidateValue = 0;
};

struct PlayerBotEquipmentLoadout {
	std::array<uint16_t, playerBotEquipmentSlotCount> itemIds{};
	std::array<PlayerBotEquipmentItemSnapshot, playerBotEquipmentSlotCount> items{};
};

struct PlayerBotEquipmentCarriedCandidate {
	PlayerBotEquipmentItemSnapshot item;
	bool actionable = false;
};

struct PlayerBotEquipmentCarriedUpgrade {
	size_t index = 0;
	PlayerBotEquipmentUpgrade upgrade;
};

struct PlayerBotEquipmentHuntSummary {
	uint32_t suitableRegions = 0;
	double bestProjectedExperience = 0;
	double lowestThreatRatio = 0;
	uint32_t evaluatedRegions = 0;
	bool truncated = false;
};

enum class PlayerBotEquipmentDecisionRule : uint8_t {
	None, ParetoImprovement, UnlocksHunt, ReadinessRepair,
};

struct PlayerBotEquipmentOfferEvaluation {
	uint32_t npcId = 0;
	Position npcPosition;
	Position approachPosition;
	uint16_t itemId = 0;
	uint32_t price = 0;
	slots_t slot = static_cast<slots_t>(0);
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
		using HuntSummaryEvaluator = std::function<PlayerBotEquipmentHuntSummary(const PlayerBotCombatProfile&)>;

		explicit PlayerBotEquipmentPolicy(uint16_t combatReadinessVocationId);
		bool requiresKnightCombatReadiness(const PlayerBotEquipmentPlayerSnapshot& player) const;
		bool isLegalEquipmentItem(const PlayerBotEquipmentPlayerSnapshot& player,
		                          const PlayerBotEquipmentItemSnapshot& item) const;
		bool isKnightMeleeWeapon(const PlayerBotEquipmentPlayerSnapshot& player,
		                         const PlayerBotEquipmentItemSnapshot& item) const;
		bool isCombatEquipment(const PlayerBotEquipmentItemSnapshot& item) const;
		std::optional<PlayerBotEquipmentUpgrade> evaluateUpgrade(const PlayerBotEquipmentPlayerSnapshot& player,
		                                                         const PlayerBotEquipmentLoadout& loadout,
		                                                         const PlayerBotEquipmentItemSnapshot& candidate) const;
		std::optional<PlayerBotEquipmentCarriedUpgrade> findCarriedUpgrade(const PlayerBotEquipmentPlayerSnapshot& player,
		                                                                   const PlayerBotEquipmentLoadout& loadout,
		                                                                   const std::vector<PlayerBotEquipmentCarriedCandidate>& candidates) const;
		PlayerBotCombatProfile combatProfile(const PlayerBotEquipmentPlayerSnapshot& player,
		                                     const PlayerBotEquipmentLoadout& loadout) const;
		bool loadoutReady(const PlayerBotEquipmentPlayerSnapshot& player, const PlayerBotEquipmentLoadout& loadout,
		                  const PlayerBotEquipmentReadinessInput& readiness, uint32_t additionalWeight = 0) const;
		PlayerBotEquipmentReadiness combatReadiness(const PlayerBotEquipmentPlayerSnapshot& player,
		                                            const PlayerBotEquipmentLoadout& loadout, bool carriedUpgrade,
		                                            const PlayerBotEquipmentReadinessInput& readiness) const;
		PlayerBotEquipmentOfferEvaluation evaluateCandidate(const PlayerBotEquipmentPlayerSnapshot& player,
		                                                    const PlayerBotEquipmentItemSnapshot& candidate,
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
		bool applyOffer(const PlayerBotEquipmentPlayerSnapshot& player, PlayerBotEquipmentLoadout& loadout,
		                const PlayerBotEquipmentItemSnapshot& candidate, slots_t& slot, uint16_t& replacedItemId,
		                uint16_t& displacedLeftItemId, uint16_t& displacedRightItemId, std::string& rejection) const;
		const uint16_t combatReadinessVocationId;
};

#endif
