/**
 * Controller-local spell calibration helpers. Engine legality is deliberately
 * outside this layer; these values are only ranking evidence.
 */
#ifndef FS_PLAYERBOTSPELLCALIBRATION_H
#define FS_PLAYERBOTSPELLCALIBRATION_H

#include <cstdint>
#include <cstddef>
#include <array>
#include <map>
#include <optional>
#include <string>

class Player;

enum class PlayerBotSpellRole : uint8_t {
	Healing,
	Support,
	MeleeOffense,
	RangedOffense,
};

enum class PlayerBotTrainingEffect : uint8_t {
	None,
	Haste,
	Light,
};

struct PlayerBotSpellDescriptor {
	const char* name;
	const char* words;
	PlayerBotSpellRole role;
	// Only explicitly audited, self-contained instant spells may opt in.
	bool magicTrainingSafe = false;
	uint8_t magicTrainingPriority = 0;
	PlayerBotTrainingEffect magicTrainingEffect = PlayerBotTrainingEffect::None;
	bool magicTrainingRefreshSafe = false;
};

struct PlayerBotSpellEnvelope {
	int32_t minimum = 0;
	int32_t maximum = 0;
	uint32_t durationMs = 0;
};

struct PlayerBotSpellObservation {
	bool manaSpent = false;
	bool concurrentDamage = false;
	bool otherRecovery = false;
	bool targetStable = true;
	bool otherAttacker = false;
	bool meleeOrOtherBotDamage = false;
	bool multiTarget = false;
	int32_t value = 0;
};

enum class PlayerBotSpellEvidence : uint8_t {
	Accepted,
	CensoredOverheal,
	ConcurrentDamage,
	OtherRecovery,
	TargetLost,
	TargetClassChanged,
	OtherAttacker,
	MeleeOrOtherBotDamage,
	MultiTarget,
	CastNotVerified,
	Ineffective,
	PreexistingOrReplacedCondition,
};

const PlayerBotSpellDescriptor* playerBotSpellDescriptor(const char* name);
const std::array<PlayerBotSpellDescriptor, 6>& playerBotSpellDescriptors();
std::optional<uint8_t> playerBotSpellLearningPriority(const char* name);
const char* playerBotSpellRoleName(PlayerBotSpellRole role);
PlayerBotSpellEnvelope playerBotSpellEnvelope(const Player& player, const PlayerBotSpellDescriptor& descriptor);
PlayerBotSpellEvidence playerBotClassifySpellObservation(PlayerBotSpellRole role, const PlayerBotSpellObservation& observation,
	                                                      int32_t missingHealth, const PlayerBotSpellEnvelope& envelope);
const char* playerBotSpellEvidenceName(PlayerBotSpellEvidence evidence);

struct PlayerBotSpellProfile {
	uint16_t accepted = 0;
	uint16_t rejected = 0;
	uint16_t ambiguous = 0;
	int32_t minimum = 0;
	int32_t maximum = 0;
	double total = 0;
	double conservative = 0;
	double ranking = 0;
	double confidence = 0;
	uint32_t lastObserved = 0;
};

class PlayerBotSpellCalibration
{
	public:
		const PlayerBotSpellProfile& observe(const std::string& spell, const std::string& targetClass,
		                                     const PlayerBotSpellEnvelope& envelope, PlayerBotSpellEvidence evidence,
		                                     int32_t value);
		const PlayerBotSpellProfile* find(const std::string& spell, const std::string& targetClass) const;
		double ranking(const std::string& spell, const std::string& targetClass, const PlayerBotSpellEnvelope& envelope) const;
		size_t size() const;
		void clear();
		std::optional<std::string> takeEvictedProfile();

	private:
		std::map<std::string, PlayerBotSpellProfile> profiles;
		uint32_t observationSequence = 0;
		std::optional<std::string> evictedProfile;
};

#endif
