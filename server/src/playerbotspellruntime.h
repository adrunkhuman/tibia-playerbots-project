/**
 * Per-bot pending spell transaction state. Spell selection, engine dispatch,
 * calibration, and event formatting remain with the controller.
 */
#ifndef FS_PLAYERBOTSPELLRUNTIME_H
#define FS_PLAYERBOTSPELLRUNTIME_H

#include "playerbotspellcalibration.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

struct PlayerBotSpellPendingCast {
	std::string name;
	PlayerBotSpellRole role = PlayerBotSpellRole::Healing;
	std::string need;
	uint32_t manaBefore = 0;
	uint32_t manaReserve = 0;
	int32_t healthBefore = 0;
	uint32_t targetId = 0;
	int32_t targetHealthBefore = 0;
	int32_t missingHealth = 0;
	int32_t hasteTicksBefore = 0;
	int32_t hasteTicksAfterCast = 0;
	int32_t hasteTicksObserved = 0;
	int32_t hasteDurationMeasured = 0;
	int64_t hasteEndTimeAfterCast = 0;
	PlayerBotSpellEnvelope envelope;
	std::string targetClass;
	uint32_t observedSpellHealing = 0;
	uint32_t observedSpellDamage = 0;
	bool concurrentDamage = false;
	bool otherRecovery = false;
	bool otherAttacker = false;
	bool meleeOrOtherBotDamage = false;
	std::array<uint32_t, 4> spellVictimIds{};
	uint8_t spellVictimCount = 0;
	bool spellVictimOverflow = false;
	std::chrono::steady_clock::time_point observedAt;
};

struct PlayerBotSpellVerificationInput {
	uint32_t mana = 0;
	int32_t health = 0;
	int32_t hasteTicks = 0;
	int64_t hasteEndTime = 0;
	bool targetStable = false;
	std::chrono::steady_clock::time_point observedAt;
};

struct PlayerBotSpellVerification {
	PlayerBotSpellPendingCast pending;
	PlayerBotSpellObservation observation;
	PlayerBotSpellEvidence evidence = PlayerBotSpellEvidence::CastNotVerified;
	bool manaSpent = false;
	bool observed = false;
	bool success = false;
};

class PlayerBotSpellRuntime
{
	public:
		bool hasPending() const;
		const PlayerBotSpellPendingCast* pending() const;
		void begin(PlayerBotSpellPendingCast cast);
		void beginEngineCast();
		void endEngineCast();
		void observeHasteAfterCast(int32_t ticks, int64_t endTime);
		void observeHealthDrain(bool controlledPlayer);
		void observeCombatDamage(uint32_t attackerId, uint32_t targetId, uint32_t playerId, uint32_t damage);
		void observeHealthGain(bool controlledHealer, bool controlledTarget, uint32_t gain);
		std::optional<PlayerBotSpellVerification> verify(const PlayerBotSpellVerificationInput& input);
		void deferRetry(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration delay);
		bool canRetry(std::chrono::steady_clock::time_point now) const;

	private:
		PlayerBotSpellPendingCast pendingCast;
		bool engineCastExecuting = false;
		std::chrono::steady_clock::time_point retryAfter;
};

#endif
