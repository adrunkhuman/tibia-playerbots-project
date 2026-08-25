/** Engine extraction and cache ownership for hunt-region planning. */
#include "otpch.h"

#include "playerbothuntregionadapter.h"

#include "configmanager.h"
#include "game.h"
#include "monsters.h"
#include "player.h"
#include "playerbotinventorypolicy.h"
#include "playerbotspellcalibration.h"
#include "spawn.h"
#include "spells.h"
#include "tile.h"
#include "weapons.h"

#include <deque>

extern Game g_game;
extern ConfigManager g_config;
extern Spells* g_spells;

namespace {
	constexpr int32_t heatRadius = 8;
	constexpr int32_t maximumRegionRadius = 24;
	constexpr uint16_t spawnBucketSize = heatRadius * 2 + 1;
	constexpr size_t maximumModeledAttackers = 5;
	constexpr double challengeBandWidth = 0.05;

	bool heatOverlaps(const Position& left, const Position& right)
	{
		return left.z == right.z && Position::getDistanceX(left, right) <= heatRadius * 2 &&
		       Position::getDistanceY(left, right) <= heatRadius * 2;
	}

	double expectedMonsterDamagePerSecond(const MonsterType& monsterType, const PlayerBotCombatProfile& profile)
	{
		double damagePerSecond = 0;
		const double mitigation = profile.armor * 0.35 + profile.defense * 0.08;
		for (const spellBlock_t& attack : monsterType.info.attackSpells) {
			const double averageDamage = (std::abs(attack.minCombatValue) + std::abs(attack.maxCombatValue)) / 2.0;
			if (averageDamage <= 0 || attack.speed == 0) continue;
			const double mitigatedDamage = attack.isMelee ? std::max(1.0, averageDamage - mitigation) : averageDamage;
			damagePerSecond += mitigatedDamage * (attack.chance / 100.0) * (1000.0 / attack.speed);
			damagePerSecond += attack.conditionDamage * (attack.chance / 100.0) / 10.0;
		}
		return std::max(0.5, damagePerSecond);
	}

	double expectedPlayerDamagePerSecond(const PlayerBotCombatProfile& profile, const MonsterType& monsterType)
	{
		const int32_t maximumDamage = Weapons::getMaxWeaponDamage(profile.level, profile.attackSkill, profile.attack,
		                                                            profile.attackFactor);
		const double averageDamage = std::max(1.0, maximumDamage / 2.0 - monsterType.info.armor * 0.25 -
		                                             monsterType.info.defense * 0.15);
		return averageDamage / 2.0;
	}

	Position nearestApproach(Player& player, const Position& spawnPosition)
	{
		Position best;
		uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
		for (int32_t x = -1; x <= 1; ++x) {
			for (int32_t y = -1; y <= 1; ++y) {
				if (x == 0 && y == 0) continue;
				Position candidate(spawnPosition.x + x, spawnPosition.y + y, spawnPosition.z);
				Tile* tile = g_game.map.getTile(candidate);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				const uint32_t distance = Position::getDistanceX(player.getPosition(), candidate) +
				                          Position::getDistanceY(player.getPosition(), candidate);
				if (distance < bestDistance) {
					best = candidate;
					bestDistance = distance;
				}
			}
		}
		return bestDistance == std::numeric_limits<uint32_t>::max() ? spawnPosition : best;
	}

	double projectedStaminaExperienceMultiplier(const Player& player, double availableHuntSeconds)
	{
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		if (staminaMinutes == 0) return 0;
		if (!g_config.getBoolean(ConfigManager::STAMINA_SYSTEM)) return 1;
		if (staminaMinutes > 2400 && player.isPremium() && availableHuntSeconds > 0) {
			// The award callback can consume two minutes before it checks the premium threshold.
			const double bonusSeconds = std::min(availableHuntSeconds,
			                                     std::max<int32_t>(0, staminaMinutes - 2402) * 60.0);
			return 1 + 0.5 * bonusSeconds / availableHuntSeconds;
		}
		return staminaMinutes <= 840 ? 0.5 : 1;
	}

	struct CachedSpawnBlock {
		Position position;
		uint32_t interval = 0;
		std::vector<std::pair<const MonsterType*, uint16_t>> monsters;
		std::vector<size_t> neighbors;
	};

	struct CachedRegion {
		uint8_t floor = 0;
		Position center;
		std::vector<size_t> members;
	};

	struct HuntRegionCache {
		std::vector<CachedSpawnBlock> spawns;
		std::vector<CachedRegion> regions;
		uint64_t generation = 0;
		bool initialized = false;
	};

	HuntRegionCache huntRegionCache;
	uint64_t huntRegionCacheRevision = 0;

	void buildHuntRegionCache(uint64_t& snapshotTimeUs, uint64_t& clusteringTimeUs)
	{
		huntRegionCache = HuntRegionCache{};
		const auto snapshotStarted = std::chrono::steady_clock::now();
		for (SpawnBlockSnapshot& snapshot : g_game.map.spawns.getMonsterSpawnSnapshots()) {
			snapshot.monsterTypes.erase(std::remove_if(snapshot.monsterTypes.begin(), snapshot.monsterTypes.end(),
				[](const auto& entry) {
					return !entry.first || !entry.first->info.isHostile || !entry.first->info.isAttackable;
				}), snapshot.monsterTypes.end());
			if (!snapshot.monsterTypes.empty()) {
				huntRegionCache.spawns.push_back({snapshot.position, snapshot.interval, std::move(snapshot.monsterTypes)});
			}
		}
		snapshotTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - snapshotStarted).count();

		const auto clusteringStarted = std::chrono::steady_clock::now();
		std::map<Position, std::vector<size_t>> spawnBuckets;
		for (size_t index = 0; index < huntRegionCache.spawns.size(); ++index) {
			const Position& position = huntRegionCache.spawns[index].position;
			spawnBuckets[Position(position.x / spawnBucketSize, position.y / spawnBucketSize, position.z)].push_back(index);
		}
		for (CachedSpawnBlock& spawn : huntRegionCache.spawns) {
			const int32_t bucketX = spawn.position.x / spawnBucketSize;
			const int32_t bucketY = spawn.position.y / spawnBucketSize;
			for (int32_t x = bucketX - 1; x <= bucketX + 1; ++x) {
				for (int32_t y = bucketY - 1; y <= bucketY + 1; ++y) {
					if (x < 0 || y < 0) continue;
					auto nearby = spawnBuckets.find(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), spawn.position.z));
					if (nearby == spawnBuckets.end()) continue;
					for (size_t candidate : nearby->second) {
						if (heatOverlaps(spawn.position, huntRegionCache.spawns[candidate].position)) {
							spawn.neighbors.push_back(candidate);
						}
					}
				}
			}
		}
		std::vector<bool> assigned(huntRegionCache.spawns.size(), false);
		for (size_t seed = 0; seed < huntRegionCache.spawns.size(); ++seed) {
			if (assigned[seed]) continue;
			CachedRegion region;
			region.floor = huntRegionCache.spawns[seed].position.z;
			std::deque<size_t> frontier = {seed};
			assigned[seed] = true;
			uint64_t xTotal = 0;
			uint64_t yTotal = 0;
			while (!frontier.empty()) {
				const size_t current = frontier.front();
				frontier.pop_front();
				region.members.push_back(current);
				xTotal += huntRegionCache.spawns[current].position.x;
				yTotal += huntRegionCache.spawns[current].position.y;
				for (size_t candidate : huntRegionCache.spawns[current].neighbors) {
					if (!assigned[candidate] &&
					    heatOverlaps(huntRegionCache.spawns[current].position, huntRegionCache.spawns[candidate].position) &&
					    Position::getDistanceX(huntRegionCache.spawns[seed].position, huntRegionCache.spawns[candidate].position) <= maximumRegionRadius &&
					    Position::getDistanceY(huntRegionCache.spawns[seed].position, huntRegionCache.spawns[candidate].position) <= maximumRegionRadius) {
						assigned[candidate] = true;
						frontier.push_back(candidate);
					}
				}
			}
			region.center = Position(static_cast<uint16_t>(xTotal / region.members.size()),
			                         static_cast<uint16_t>(yTotal / region.members.size()), region.floor);
			huntRegionCache.regions.push_back(std::move(region));
		}
		clusteringTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - clusteringStarted).count();
		huntRegionCache.generation = g_game.map.spawns.getGeneration();
		huntRegionCache.initialized = true;
		++huntRegionCacheRevision;
	}

	PlayerBotHuntRegion scoreRegion(Player& player, const PlayerBotHuntPlanningProfile& planningProfile,
		size_t candidateIndex, const std::set<Position>& excludedRegions,
		const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
		uint32_t huntDurationSeconds, bool& withinPlanningScope)
	{
		const PlayerBotCombatProfile& profile = planningProfile.combat;
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		const CachedRegion& cached = huntRegionCache.regions[candidateIndex];
		PlayerBotHuntRegion region;
		region.floor = cached.floor;
		region.center = cached.center;
		std::map<std::string, PlayerBotHuntMonsterProfile> profiles;
		for (size_t member : cached.members) {
			const CachedSpawnBlock& spawn = huntRegionCache.spawns[member];
			region.patrolPoints.push_back(nearestApproach(player, spawn.position));
			for (const auto& [monsterType, chance] : spawn.monsters) {
				PlayerBotHuntMonsterProfile& monsterProfile = profiles[monsterType->name];
				monsterProfile.name = monsterType->name;
				monsterProfile.expectedSpawns += chance / 100.0;
				monsterProfile.experience = monsterType->info.experience;
				monsterProfile.health = monsterType->info.healthMax;
				monsterProfile.expectedDamagePerSecond = expectedMonsterDamagePerSecond(*monsterType, profile);
				const double fightSeconds = monsterType->info.healthMax / expectedPlayerDamagePerSecond(profile, *monsterType);
				monsterProfile.predictedFightDamage = monsterProfile.expectedDamagePerSecond * fightSeconds;
				region.experiencePerMinute += monsterType->info.experience * (chance / 100.0) *
				                              (60000.0 / std::max<uint32_t>(spawn.interval, 1));
			}
		}

		if (profiles.empty()) return region;
		double worstFightDamage = 0;
		double worstFightSeconds = 0;
		for (size_t anchor : cached.members) {
			struct LocalAttacker {
				double damagePerSecond;
				double fightSeconds;
			};
			std::vector<LocalAttacker> localAttackers;
			for (size_t neighbor : huntRegionCache.spawns[anchor].neighbors) {
				for (const auto& [monsterType, chance] : huntRegionCache.spawns[neighbor].monsters) {
					(void)chance;
					localAttackers.push_back({expectedMonsterDamagePerSecond(*monsterType, profile),
					                          monsterType->info.healthMax / expectedPlayerDamagePerSecond(profile, *monsterType)});
				}
			}
			std::sort(localAttackers.begin(), localAttackers.end(), [](const LocalAttacker& left, const LocalAttacker& right) {
				return left.damagePerSecond > right.damagePerSecond;
			});
			const size_t attackers = std::min(maximumModeledAttackers, localAttackers.size());
			double remainingDamagePerSecond = 0;
			for (size_t index = 0; index < attackers; ++index) remainingDamagePerSecond += localAttackers[index].damagePerSecond;
			double fightDamage = 0;
			double fightSeconds = 0;
			for (size_t index = 0; index < attackers; ++index) {
				fightDamage += remainingDamagePerSecond * localAttackers[index].fightSeconds;
				fightSeconds += localAttackers[index].fightSeconds;
				remainingDamagePerSecond -= localAttackers[index].damagePerSecond;
			}
			if (fightDamage > worstFightDamage) {
				worstFightDamage = fightDamage;
				worstFightSeconds = fightSeconds;
			}
		}
		std::sort(region.patrolPoints.begin(), region.patrolPoints.end());
		region.patrolPoints.erase(std::unique(region.patrolPoints.begin(), region.patrolPoints.end()), region.patrolPoints.end());
		region.destination = *std::min_element(region.patrolPoints.begin(), region.patrolPoints.end(),
			[&player](const Position& left, const Position& right) {
				const uint32_t leftDistance = Position::getDistanceX(player.getPosition(), left) +
				                              Position::getDistanceY(player.getPosition(), left);
				const uint32_t rightDistance = Position::getDistanceX(player.getPosition(), right) +
				                               Position::getDistanceY(player.getPosition(), right);
				return leftDistance < rightDistance;
			});
		for (auto& [name, monsterProfile] : profiles) {
			(void)name;
			region.monsters.push_back(std::move(monsterProfile));
		}
		const uint32_t geometricDistance = Position::getDistanceX(player.getPosition(), region.destination) +
		                                   Position::getDistanceY(player.getPosition(), region.destination) +
		                                   Position::getDistanceZ(player.getPosition(), region.destination) * 20;
		region.predictedFightSeconds = worstFightSeconds;
		region.currentHealth = planningProfile.currentHealth;
		region.recovery = playerBotPredictRecovery(planningProfile, worstFightSeconds);
		region.rawThreatRatio = worstFightDamage / std::max<int32_t>(profile.maximumHealth, 1);
		region.threatRatio = std::max(0.0, worstFightDamage - region.recovery.totalMinimumHealing) /
		                     std::max<int32_t>(profile.maximumHealth, 1);
		region.challengeFrontier = planningProfile.challengeFrontier;
		region.challengeBandMinimum = std::max(0.0, planningProfile.challengeFrontier - challengeBandWidth);
		region.challengeBandMaximum = planningProfile.challengeFrontier + challengeBandWidth;
		region.inChallengeBand = region.threatRatio >= region.challengeBandMinimum &&
		                         region.threatRatio <= region.challengeBandMaximum;
		region.predictedLethal = playerBotPredictedLethal(planningProfile.currentHealth, worstFightDamage);
		withinPlanningScope = true;
		region.suitable = !region.predictedLethal && region.threatRatio <= region.challengeBandMaximum && withinPlanningScope;
		if (excludedRegions.find(region.center) != excludedRegions.end()) {
			region.suitable = false;
			region.rejectionReason = "observed_danger_cooldown";
		} else if (!withinPlanningScope) {
			region.rejectionReason = "travel_distance";
		} else if (region.predictedLethal) {
			region.rejectionReason = "predicted_lethal";
		} else if (!region.suitable) {
			region.rejectionReason = "challenge_frontier";
		}
		region.experiencePerMinute *= g_config.getExperienceStage(player.getLevel());
		region.staminaMinutes = staminaMinutes;
		if (auto observed = performance.find(region.center); observed != performance.end()) {
			region.observedExperiencePerMinute = observed->second.observedExperiencePerMinute;
			region.observedCorrection = observed->second.correction;
		}
		region.estimatedTravelSeconds = geometricDistance * player.getStepDuration() / 1000.0;
		region.availableHuntSeconds = std::max(0.0, huntDurationSeconds - region.estimatedTravelSeconds);
		region.staminaExperienceMultiplier = projectedStaminaExperienceMultiplier(player, region.availableHuntSeconds);
		region.projectedExperience = region.experiencePerMinute * region.observedCorrection *
		                             region.staminaExperienceMultiplier * region.availableHuntSeconds / 60.0;
		region.optimisticProjectedExperience = region.experiencePerMinute * region.observedCorrection *
		                                       1.5 * huntDurationSeconds / 60.0;
		region.score = region.projectedExperience;
		return region;
	}
}

void PlayerBotHuntRegionAdapter::invalidateCache()
{
	huntRegionCache = HuntRegionCache{};
	++huntRegionCacheRevision;
}

uint64_t PlayerBotHuntRegionAdapter::getCacheRevision()
{
	if (huntRegionCache.initialized && huntRegionCache.generation != g_game.map.spawns.getGeneration()) invalidateCache();
	return huntRegionCacheRevision;
}

PlayerBotHuntRegionScan PlayerBotHuntRegionAdapter::beginScan(const Player& player)
{
	PlayerBotHuntRegionScan scan;
	scan.cacheHit = huntRegionCache.initialized && huntRegionCache.generation == g_game.map.spawns.getGeneration();
	if (!scan.cacheHit) buildHuntRegionCache(scan.snapshotTimeUs, scan.clusteringTimeUs);
	scan.revision = huntRegionCacheRevision;
	for (size_t index = 0; index < huntRegionCache.regions.size(); ++index) {
		scan.candidateIndices.push_back(index);
	}
	scan.candidateCount = scan.candidateIndices.size();
	return scan;
}

PlayerBotHuntRegionScore PlayerBotHuntRegionAdapter::score(Player& player, const PlayerBotHuntPlanningProfile& profile,
	uint64_t revision, size_t candidateIndex, const std::set<Position>& excludedRegions,
	const std::map<Position, PlayerBotHuntRegionPerformance>& performance, uint32_t huntDurationSeconds)
{
	PlayerBotHuntRegionScore observation;
	if (revision != getCacheRevision() || candidateIndex >= huntRegionCache.regions.size()) return observation;
	observation.region = scoreRegion(player, profile, candidateIndex, excludedRegions, performance,
	                               huntDurationSeconds, observation.withinPlanningScope);
	observation.valid = true;
	observation.candidateFactsAvailable = !observation.region.monsters.empty();
	return observation;
}

PlayerBotHuntPlanningProfile PlayerBotHuntRegionAdapter::planningProfile(const Player& player,
	const PlayerBotCombatProfile& combat, double challengeFrontier)
{
	PlayerBotHuntPlanningProfile profile;
	profile.combat = combat;
	profile.currentHealth = player.getHealth();
	profile.mana = player.getMana();
	profile.magicLevel = player.getMagicLevel();
	profile.potionCount = static_cast<const Cylinder&>(player).getItemTypeCount(
	    playerbot::recoveryPotionItemId(player.getVocationId()));
	profile.potionMinimumHealing = playerbot::recoveryPotionMinimumHealing(player.getVocationId());
	profile.challengeFrontier = challengeFrontier;
	InstantSpell* spell = g_spells ? g_spells->getInstantSpellByName("Light Healing") : nullptr;
	if (!spell || spell->getWords() != "exura" || !spell->isLearnable() || !spell->isEnabled() ||
	    !spell->canCast(&player) || player.getLevel() < spell->getLevel() ||
	    player.getMagicLevel() < spell->getMagicLevel() || (spell->isPremium() && !player.isPremium())) {
		return profile;
	}
	profile.lightHealingLegal = true;
	profile.lightHealingManaCost = spell->getManaCost(&player);
	profile.lightHealingCooldown = std::max(spell->getCooldown(), spell->getGroupCooldown());
	if (const PlayerBotSpellDescriptor* descriptor = playerBotSpellDescriptor("Light Healing")) {
		profile.lightHealingMinimum = playerBotSpellEnvelope(player, *descriptor).minimum;
	}
	return profile;
}
