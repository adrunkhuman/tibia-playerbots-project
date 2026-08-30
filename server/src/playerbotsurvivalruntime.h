/**
 * Per-bot survival policy. The controller supplies authoritative snapshots and
 * executes the returned engine command; this class owns recovery and spell policy.
 */
#ifndef FS_PLAYERBOTSURVIVALRUNTIME_H
#define FS_PLAYERBOTSURVIVALRUNTIME_H

#include "playerbotrecoverysession.h"
#include "playerbotspellruntime.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PlayerBotSurvivalTargetObservation {
	uint32_t id = 0;
	int32_t health = 0;
	std::string targetClass = "self";
	bool valid = false;
};

struct PlayerBotSurvivalSpellObservation {
	std::string name;
	bool metadataMatches = false;
	bool learned = false;
	bool targetReachable = false;
	bool magicTrainingEligible = false;
	uint32_t manaCost = 0;
	PlayerBotSpellEnvelope envelope;
};

struct PlayerBotSurvivalSnapshot {
	int32_t health = 0;
	int32_t healthMaximum = 0;
	uint32_t mana = 0;
	uint32_t manaMaximum = 0;
	uint64_t manaSpent = 0;
	uint32_t level = 0;
	uint32_t magicLevel = 0;
	uint16_t potionItemId = 0;
	int32_t potionMaximumHealing = 0;
	uint32_t potionCount = 0;
	uint32_t foodCount = 0;
	uint32_t foodInventoryCount = 0;
	uint32_t pendingFoodCount = 0;
	uint16_t foodItemId = 0;
	uint16_t foodClientId = 0;
	int32_t foodTicks = 0;
	bool canDoAction = false;
	bool buyingPotions = false;
	bool lootMovePending = false;
	bool progressionActive = false;
	bool progressionDeparture = false;
	bool hunting = false;
	bool combatActive = false;
	bool navigationPending = false;
	bool healingExhausted = false;
	bool combatExhausted = false;
	bool hasteActive = false;
	int32_t hasteTicks = 0;
	bool lightActive = false;
	bool regenerationActive = false;
	bool protectionZone = false;
	bool regenerationForecastActive = false;
	uint64_t regenerationManaGain = 0;
	uint32_t regenerationTickInterval = 0;
	uint32_t regenerationTickRemaining = 0;
	uint32_t routeSteps = 0;
	PlayerBotSurvivalTargetObservation target;
	std::vector<PlayerBotSurvivalSpellObservation> spells;
};

enum class PlayerBotSurvivalCommandType : uint8_t {
	None,
	UsePotion,
	UseFood,
	CastSpell,
	InterruptForService,
	Wait,
};

struct PlayerBotSurvivalSpellCommand {
	std::string name;
	std::string words;
	PlayerBotSpellRole role = PlayerBotSpellRole::Healing;
	uint32_t targetId = 0;
	PlayerBotSpellPendingCast pending;
};

struct PlayerBotSurvivalCommand {
	PlayerBotSurvivalCommandType type = PlayerBotSurvivalCommandType::None;
	uint16_t itemId = 0;
	uint16_t itemClientId = 0;
	std::string candidateName;
	std::string need;
	std::optional<PlayerBotPotionVerification> potionVerification;
	std::optional<PlayerBotFoodVerification> foodVerification;
	std::optional<PlayerBotSurvivalSpellCommand> spell;
	std::string reason;
};

struct PlayerBotSurvivalSpellVerification {
	PlayerBotSpellVerification verification;
	PlayerBotSpellProfile calibration;
	double rankingEstimate = 0;
	std::optional<std::string> evictedProfile;
};

struct PlayerBotMagicTrainingCommand {
	std::string name;
	std::string words;
	uint8_t priority = 0;
	uint64_t cost = 0;
	bool refresh = false;
	uint64_t manaBefore = 0;
	uint64_t manaSpentBefore = 0;
	uint32_t magicLevelBefore = 0;
	uint64_t manaGain = 0;
	uint32_t manaTickInterval = 0;
	uint32_t manaTickRemaining = 0;
	uint64_t predictedMana = 0;
	uint64_t wastedMana = 0;
};

class PlayerBotSurvivalRuntime
{
	public:
		bool needsHealing(const PlayerBotSurvivalSnapshot& snapshot) const;
		bool hasPendingDefensiveWork() const;
		uint16_t pendingFoodItemId() const;
		void beginPotion(const PlayerBotSurvivalSnapshot& snapshot);
		PlayerBotSurvivalCommand decideHealing(const PlayerBotSurvivalSnapshot& snapshot,
		                                      std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideFood(const PlayerBotSurvivalSnapshot& snapshot,
		                                   std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideSupportSpell(const PlayerBotSurvivalSnapshot& snapshot,
		                                           std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideOffensiveSpell(const PlayerBotSurvivalSnapshot& snapshot,
		                                             std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotSurvivalSpellVerification> verifySpell(const PlayerBotSpellVerificationInput& input);
		void beginEngineSpellCast();
		void endEngineSpellCast();
		void observeHasteAfterCast(int32_t ticks, int64_t endTime);
		void observeHealthDrain(bool controlledPlayer);
		void observeCombatDamage(uint32_t attackerId, uint32_t targetId, uint32_t playerId, uint32_t damage);
		void observeHealthGain(bool controlledHealer, bool controlledTarget, uint32_t gain);
		bool canRetrySpell(std::chrono::steady_clock::time_point now) const;
		std::optional<PlayerBotSpellPendingCast> pendingSpell() const;
		void deferSpellRetry(std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotSpellProfile> calibrationProfile(const PlayerBotSpellPendingCast& pending) const;
		double calibrationRanking(const PlayerBotSpellPendingCast& pending) const;
		size_t calibrationSize() const;
		const char* magicTrainingReason(const PlayerBotSurvivalSnapshot& snapshot) const;
		std::optional<PlayerBotMagicTrainingCommand> decideMagicTraining(const PlayerBotSurvivalSnapshot& snapshot) const;

	private:
		PlayerBotSurvivalCommand decideSpell(const PlayerBotSurvivalSnapshot& snapshot, const char* spellName,
		                                    const char* need, std::chrono::steady_clock::time_point now);
		PlayerBotRecoverySession recovery;
		PlayerBotSpellRuntime spells;
		PlayerBotSpellCalibration calibration;
};

#endif
