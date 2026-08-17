/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotspellcalibration.h"

#include "player.h"

#include <algorithm>
#include <array>

namespace {
	constexpr uint16_t maximumCalibrationSamples = 64;
	constexpr size_t maximumCalibrationProfiles = 12;
	constexpr int32_t maximumObservedValue = 60000;
	constexpr uint16_t confidenceSamples = 8;

	constexpr std::array<PlayerBotSpellDescriptor, 6> descriptors = {{
		{"Light Healing", "exura", PlayerBotSpellRole::Healing},
		{"Haste", "utani hur", PlayerBotSpellRole::Support, true, 30, PlayerBotTrainingEffect::Haste, false},
		{"Great Light", "utevo gran lux", PlayerBotSpellRole::Support, true, 20, PlayerBotTrainingEffect::Light, true},
		{"Light", "utevo lux", PlayerBotSpellRole::Support, true, 10, PlayerBotTrainingEffect::Light, true},
		{"Berserk", "exori", PlayerBotSpellRole::MeleeOffense},
		{"Whirlwind Throw", "exori hur", PlayerBotSpellRole::RangedOffense},
	}};

	std::string profileKey(const std::string& spell, const std::string& targetClass)
	{
		return spell + '\n' + targetClass;
	}

	bool ambiguous(PlayerBotSpellEvidence evidence)
	{
		return evidence != PlayerBotSpellEvidence::Accepted && evidence != PlayerBotSpellEvidence::CastNotVerified &&
		       evidence != PlayerBotSpellEvidence::Ineffective;
	}
}

const PlayerBotSpellDescriptor* playerBotSpellDescriptor(const char* name)
{
	auto it = std::find_if(descriptors.begin(), descriptors.end(), [name](const PlayerBotSpellDescriptor& descriptor) {
		return std::strcmp(descriptor.name, name) == 0;
	});
	return it == descriptors.end() ? nullptr : &*it;
}

const std::array<PlayerBotSpellDescriptor, 6>& playerBotSpellDescriptors()
{
	return descriptors;
}

const char* playerBotSpellRoleName(PlayerBotSpellRole role)
{
	switch (role) {
		case PlayerBotSpellRole::Healing: return "healing";
		case PlayerBotSpellRole::Support: return "support";
		case PlayerBotSpellRole::MeleeOffense: return "melee_offense";
		case PlayerBotSpellRole::RangedOffense: return "ranged_offense";
	}
	return "unsupported";
}

PlayerBotSpellEnvelope playerBotSpellEnvelope(const Player& player, const PlayerBotSpellDescriptor& descriptor)
{
	// These mirror the audited Lua callbacks. The engine does not expose callback bounds.
	const double level = player.getLevel();
	if (descriptor.role == PlayerBotSpellRole::Healing) {
		const double magicLevel = player.getMagicLevel();
		return {static_cast<int32_t>(std::floor(level / 5 + magicLevel * 1.4 + 8)),
		        static_cast<int32_t>(std::floor(level / 5 + magicLevel * 1.8 + 11)), 0};
	}
	if (descriptor.role == PlayerBotSpellRole::Support) {
		return {33000, 33000, 33000};
	}
	const Item* weapon = player.getWeapon(true);
	const double skill = weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST);
	const double attack = weapon ? weapon->getAttack() : 7;
	if (descriptor.role == PlayerBotSpellRole::MeleeOffense) {
		return {static_cast<int32_t>(std::floor(level / 5 + skill * attack * 0.03 + 7)),
		        static_cast<int32_t>(std::floor(level / 5 + skill * attack * 0.05 + 11)), 0};
	}
	return {static_cast<int32_t>(std::floor(level / 5 + skill * attack * 0.01 + 1)),
	        static_cast<int32_t>(std::floor(level / 5 + skill * attack * 0.03 + 6)), 0};
}

PlayerBotSpellEvidence playerBotClassifySpellObservation(PlayerBotSpellRole role, const PlayerBotSpellObservation& observation,
	                                                       int32_t missingHealth, const PlayerBotSpellEnvelope& envelope)
{
	if (!observation.manaSpent) return PlayerBotSpellEvidence::CastNotVerified;
	if (role == PlayerBotSpellRole::Healing) {
		if (observation.concurrentDamage) return PlayerBotSpellEvidence::ConcurrentDamage;
		if (observation.otherRecovery) return PlayerBotSpellEvidence::OtherRecovery;
		if (missingHealth < envelope.maximum) return PlayerBotSpellEvidence::CensoredOverheal;
		return observation.value > 0 ? PlayerBotSpellEvidence::Accepted : PlayerBotSpellEvidence::Ineffective;
	}
	if (role == PlayerBotSpellRole::Support) {
		return observation.value > 0 ? PlayerBotSpellEvidence::Accepted : PlayerBotSpellEvidence::PreexistingOrReplacedCondition;
	}
	if (observation.multiTarget) return PlayerBotSpellEvidence::MultiTarget;
	if (!observation.targetStable) return PlayerBotSpellEvidence::TargetLost;
	if (observation.otherAttacker) return PlayerBotSpellEvidence::OtherAttacker;
	if (observation.meleeOrOtherBotDamage) return PlayerBotSpellEvidence::MeleeOrOtherBotDamage;
	return observation.value > 0 ? PlayerBotSpellEvidence::Accepted : PlayerBotSpellEvidence::Ineffective;
}

const char* playerBotSpellEvidenceName(PlayerBotSpellEvidence evidence)
{
	switch (evidence) {
		case PlayerBotSpellEvidence::Accepted: return "accepted";
		case PlayerBotSpellEvidence::CensoredOverheal: return "censored_overheal";
		case PlayerBotSpellEvidence::ConcurrentDamage: return "concurrent_damage";
		case PlayerBotSpellEvidence::OtherRecovery: return "other_recovery";
		case PlayerBotSpellEvidence::TargetLost: return "target_lost";
		case PlayerBotSpellEvidence::TargetClassChanged: return "target_class_changed";
		case PlayerBotSpellEvidence::OtherAttacker: return "other_attacker";
		case PlayerBotSpellEvidence::MeleeOrOtherBotDamage: return "melee_or_other_bot_damage";
		case PlayerBotSpellEvidence::MultiTarget: return "multi_target";
		case PlayerBotSpellEvidence::CastNotVerified: return "cast_not_verified";
		case PlayerBotSpellEvidence::Ineffective: return "ineffective";
		case PlayerBotSpellEvidence::PreexistingOrReplacedCondition: return "preexisting_or_replaced_condition";
	}
	return "unknown";
}

const PlayerBotSpellProfile& PlayerBotSpellCalibration::observe(const std::string& spell, const std::string& targetClass,
	                                                              const PlayerBotSpellEnvelope& envelope,
	                                                              PlayerBotSpellEvidence evidence, int32_t value)
{
	const std::string key = profileKey(spell, targetClass);
	auto it = profiles.find(key);
	if (it == profiles.end()) {
		if (profiles.size() >= maximumCalibrationProfiles) {
			auto evicted = std::min_element(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
				return left.second.lastObserved == right.second.lastObserved ? left.first < right.first :
				       left.second.lastObserved < right.second.lastObserved;
			});
			evictedProfile = evicted->first;
			profiles.erase(evicted);
		}
		it = profiles.emplace(key, PlayerBotSpellProfile{}).first;
	}
	PlayerBotSpellProfile& profile = it->second;
	if (observationSequence == std::numeric_limits<uint32_t>::max()) {
		for (auto& entry : profiles) entry.second.lastObserved = 0;
		observationSequence = 0;
	}
	profile.lastObserved = ++observationSequence;
	if (evidence == PlayerBotSpellEvidence::Accepted) {
		if (profile.accepted >= maximumCalibrationSamples) return profile;
		++profile.accepted;
		const int32_t bounded = std::clamp(value, 0, maximumObservedValue);
		if (profile.accepted == 1) profile.minimum = profile.maximum = bounded;
		else {
			profile.minimum = std::min(profile.minimum, bounded);
			profile.maximum = std::max(profile.maximum, bounded);
		}
		profile.conservative = profile.minimum;
		profile.total = std::min<double>(maximumCalibrationSamples * maximumObservedValue, profile.total + bounded);
		const double sampleMean = profile.total / profile.accepted;
		profile.confidence = std::min(1.0, static_cast<double>(profile.accepted) / confidenceSamples);
		const double baseline = (envelope.minimum + envelope.maximum) / 2.0;
		profile.ranking = baseline + (sampleMean - baseline) * profile.confidence;
	} else if (ambiguous(evidence)) {
		if (profile.ambiguous < maximumCalibrationSamples) ++profile.ambiguous;
	} else if (profile.rejected < maximumCalibrationSamples) {
		++profile.rejected;
	}
	return profile;
}

const PlayerBotSpellProfile* PlayerBotSpellCalibration::find(const std::string& spell, const std::string& targetClass) const
{
	auto it = profiles.find(profileKey(spell, targetClass));
	return it == profiles.end() ? nullptr : &it->second;
}

double PlayerBotSpellCalibration::ranking(const std::string& spell, const std::string& targetClass,
	                                        const PlayerBotSpellEnvelope& envelope) const
{
	const PlayerBotSpellProfile* profile = find(spell, targetClass);
	return profile && profile->confidence > 0 ? profile->ranking : (envelope.minimum + envelope.maximum) / 2.0;
}

size_t PlayerBotSpellCalibration::size() const
{
	return profiles.size();
}

void PlayerBotSpellCalibration::clear()
{
	profiles.clear();
	observationSequence = 0;
	evictedProfile.reset();
}

std::optional<std::string> PlayerBotSpellCalibration::takeEvictedProfile()
{
	std::optional<std::string> result = std::move(evictedProfile);
	evictedProfile.reset();
	return result;
}
