/**
 * Per-bot hunt combat evidence and adaptive challenge policy. World sampling,
 * region selection, and telemetry remain controller responsibilities.
 */
#ifndef FS_PLAYERBOTHUNTPOLICY_H
#define FS_PLAYERBOTHUNTPOLICY_H

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>

#include "playerbothuntregions.h"

struct PlayerBotHuntCombatEvidence {
	double activeSeconds = 0;
	uint32_t kills = 0;
	uint32_t damageTaken = 0;
	uint32_t potionRecoveries = 0;
	uint32_t spellRecoveries = 0;
	uint32_t maximumAttackerOverlap = 0;
	std::array<double, 6> attackerExposureSeconds{};
	double foodActiveSeconds = 0;
	double foodAvailableSeconds = 0;
	uint64_t levelHealthRestored = 0;
	uint64_t levelManaRestored = 0;
	int32_t minimumHealth = std::numeric_limits<int32_t>::max();
	uint32_t minimumMana = std::numeric_limits<uint32_t>::max();
	bool dangerObserved = false;
	bool deathObserved = false;
	std::array<uint32_t, 101> healthPercentSamples{};
	std::array<uint32_t, 101> manaPercentSamples{};
};

struct PlayerBotHuntCombatSample {
	bool active = false;
	double elapsedSeconds = 0;
	int32_t health = 0;
	int32_t maximumHealth = 0;
	uint32_t mana = 0;
	uint32_t maximumMana = 0;
	uint32_t attackers = 0;
	bool foodActive = false;
	bool foodAvailable = false;
};

struct PlayerBotHuntCombatSnapshot {
	bool active = false;
	std::chrono::steady_clock::time_point observedAt;
	int32_t health = 0;
	int32_t maximumHealth = 0;
	uint32_t mana = 0;
	uint32_t maximumMana = 0;
	uint32_t attackers = 0;
	bool foodActive = false;
	bool foodAvailable = false;
};

struct PlayerBotHuntCombatSummary : PlayerBotHuntCombatEvidence {
	uint8_t p10HealthPercent = 0;
	uint8_t p10ManaPercent = 100;
};

enum class PlayerBotHuntChallengeResult : uint8_t {
	InsufficientActiveCombat,
	Backoff,
	Hold,
	Escalated,
	Clamped,
};

struct PlayerBotHuntChallengeSample {
	uint64_t durationSeconds = 0;
	int32_t maximumHealth = 0;
};

struct PlayerBotHuntChallengeUpdate {
	PlayerBotHuntChallengeResult result = PlayerBotHuntChallengeResult::InsufficientActiveCombat;
	double frontierBefore = 0;
	double frontierAfter = 0;
	uint8_t qualifyingHuntsToHold = 0;
	double activeCombatUptime = 0;
	uint32_t verifiedRecoveries = 0;
	double potionRecoveriesPerActiveMinute = 0;
	double minimumActiveCombatSeconds = 0;
	uint32_t minimumKills = 0;
	PlayerBotHuntCombatSummary combat;
};

struct PlayerBotHuntPerformanceSample {
	uint64_t durationSeconds = 0;
	double activeCombatSeconds = 0;
	uint32_t kills = 0;
	uint64_t experienceGained = 0;
	double projectedExperience = 0;
	double observedCorrection = 1;
	uint32_t configuredHuntDurationSeconds = 0;
	bool dangerObserved = false;
	bool deathObserved = false;
	uint64_t coinGoldAcquired = 0;
};

struct PlayerBotHuntPerformanceUpdate {
	double actualExperiencePerMinute = 0;
	double actualCoinGoldPerMinute = 0;
	double updatedCorrection = 1;
	const char* evidenceReason = "insufficient_evidence";
	bool observed = false;
};

const char* playerBotHuntChallengeResultName(PlayerBotHuntChallengeResult result);

class PlayerBotHuntPolicy
{
	public:
		void resetCombatEvidence();
		void observeCombat(const PlayerBotHuntCombatSample& sample);
		void sampleCombat(const PlayerBotHuntCombatSnapshot& snapshot);
		void observeKill();
		void observeDamage(uint32_t damage);
		void observeRecovery(bool potion);
		void observeLevelRestoration(uint32_t health, uint32_t mana);
		void observeDeath();
		bool observeDanger(int32_t maximumHealth, std::chrono::steady_clock::duration huntAge);

		PlayerBotHuntCombatSummary combatSummary() const;
		PlayerBotHuntChallengeUpdate updateChallengeFrontier(const PlayerBotHuntChallengeSample& sample);
		PlayerBotHuntPerformanceUpdate observePerformance(uint64_t variantId, uint64_t atlasRevision,
		                                                  const PlayerBotHuntPerformanceSample& sample);

		PlayerBotSupplyObservation observeSupplies(const PlayerBotHuntRegion& region,
		    const PlayerBotSupplyCapabilitySnapshot& capabilityAfter, uint64_t durationSeconds,
		    int32_t health, int32_t maximumHealth, uint32_t mana, uint32_t potions, bool interrupted);

		double challengeFrontier() const { return frontier; }
		const std::map<uint64_t, PlayerBotHuntRegionPerformance>& regionPerformance() const { return performance; }

	private:
		PlayerBotHuntCombatEvidence evidence;
		std::chrono::steady_clock::time_point lastSample;
		std::map<uint64_t, PlayerBotHuntRegionPerformance> performance;
		double frontier = 0.20;
		uint8_t qualifyingHuntsToHold = 0;
};

#endif
