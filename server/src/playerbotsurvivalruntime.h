/**
 * Per-bot survival policy. The controller supplies authoritative snapshots and
 * executes the returned engine command; this class owns recovery and spell policy.
 */
#ifndef FS_PLAYERBOTSURVIVALRUNTIME_H
#define FS_PLAYERBOTSURVIVALRUNTIME_H

#include "playerbotrecoverysession.h"
#include "playerbotspellruntime.h"

#include <chrono>
#include <optional>
#include <string>

class Player;
class Creature;
class InstantSpell;
struct Position;

struct PlayerBotSurvivalSnapshot {
	int32_t health = 0;
	int32_t healthMaximum = 0;
	uint32_t mana = 0;
	uint32_t manaMaximum = 0;
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
	bool combatOrPursuit = false;
	bool navigationPending = false;
	bool healingExhausted = false;
	bool hasteActive = false;
	uint32_t routeSteps = 0;
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
	const PlayerBotSpellDescriptor* descriptor = nullptr;
	InstantSpell* spell = nullptr;
	Creature* target = nullptr;
	PlayerBotSpellPendingCast pending;
};

struct PlayerBotSurvivalCommand {
	PlayerBotSurvivalCommandType type = PlayerBotSurvivalCommandType::None;
	uint16_t itemClientId = 0;
	const PlayerBotSpellDescriptor* candidate = nullptr;
	const char* need = nullptr;
	std::optional<PlayerBotPotionVerification> potionVerification;
	std::optional<PlayerBotFoodVerification> foodVerification;
	std::optional<PlayerBotSurvivalSpellCommand> spell;
	const char* reason = nullptr;
};

struct PlayerBotSurvivalSpellVerification {
	PlayerBotSpellVerification verification;
	PlayerBotSpellProfile calibration;
	double rankingEstimate = 0;
	std::optional<std::string> evictedProfile;
};

struct PlayerBotMagicTrainingCommand {
	const PlayerBotSpellDescriptor* descriptor = nullptr;
	InstantSpell* spell = nullptr;
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
		PlayerBotSurvivalCommand decideHealing(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
		                                      const Position& position, std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideFood(const PlayerBotSurvivalSnapshot& snapshot,
		                                   std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
		                                    const Position& position, const char* spellName, const char* need,
		                                    Creature* target, std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideSupportSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
		                                           const Position& position, std::chrono::steady_clock::time_point now);
		PlayerBotSurvivalCommand decideOffensiveSpell(Player& player, const PlayerBotSurvivalSnapshot& snapshot,
		                                             const Position& position, Creature* target,
		                                             std::chrono::steady_clock::time_point now);
		std::optional<PlayerBotSurvivalSpellVerification> verifySpell(const PlayerBotSpellVerificationInput& input);
		void beginEngineSpellCast();
		void endEngineSpellCast();
		void observeHasteAfterCast(int32_t ticks, int64_t endTime);
		void observeHealthDrain(bool controlledPlayer);
		void observeCombatDamage(uint32_t attackerId, uint32_t targetId, uint32_t playerId, uint32_t damage);
		void observeHealthGain(bool controlledHealer, bool controlledTarget, uint32_t gain);
		bool canRetrySpell(std::chrono::steady_clock::time_point now) const;
		const PlayerBotSpellPendingCast* pendingSpell() const;
		void deferSpellRetry(std::chrono::steady_clock::time_point now);
		const PlayerBotSpellProfile* calibrationProfile(const PlayerBotSpellPendingCast& pending) const;
		double calibrationRanking(const PlayerBotSpellPendingCast& pending) const;
		size_t calibrationSize() const;
		const PlayerBotSpellProfile& observeCalibrationFixture(const std::string& spell, const std::string& targetClass,
		                                                      const PlayerBotSpellEnvelope& envelope,
		                                                      PlayerBotSpellEvidence evidence, int32_t value);
		std::optional<std::string> takeCalibrationEviction();
		void clearCalibration();
		const char* magicTrainingReason(const Player& player, const PlayerBotSurvivalSnapshot& snapshot) const;
		std::optional<PlayerBotMagicTrainingCommand> decideMagicTraining(const Player& player,
		                                                                 const PlayerBotSurvivalSnapshot& snapshot) const;

	private:
		PlayerBotRecoverySession recovery;
		PlayerBotSpellRuntime spells;
		PlayerBotSpellCalibration calibration;
};

#endif
