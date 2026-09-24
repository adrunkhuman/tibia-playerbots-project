/** Engine extraction and cache ownership for hunt-region planning. */
#include "otpch.h"

#include "playerbothuntregionadapter.h"

#include "playerbottopology.h"

#include "configmanager.h"
#include "condition.h"
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
extern Monsters g_monsters;
extern Spells* g_spells;

namespace {
	constexpr int32_t heatRadius = 8;
	constexpr int32_t maximumRegionRadius = 24;
	constexpr uint16_t spawnBucketSize = heatRadius * 2 + 1;
	constexpr size_t maximumModeledAttackers = 5;
	constexpr uint32_t topologySectorSize = 32;
	constexpr double challengeHeadroom = 0.05;
	constexpr int32_t atlasPortalLinkRadius = 32;
	constexpr size_t maximumNeighborhoodVariantsPerSite = 32;

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

	std::optional<Position> nearestApproach(Player& player, const Position& spawnPosition,
	                                       const PlayerBotTopologyDistances* topologyDistances = nullptr)
	{
		Position best;
		uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
		for (int32_t x = -1; x <= 1; ++x) {
			for (int32_t y = -1; y <= 1; ++y) {
				if (x == 0 && y == 0) continue;
				Position candidate(spawnPosition.x + x, spawnPosition.y + y, spawnPosition.z);
				Tile* tile = g_game.map.getTile(candidate);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				if (topologyDistances && !PlayerBotTopology::instance().distanceTo(*topologyDistances, candidate)) continue;
				const uint32_t geometricDistance = Position::getDistanceX(player.getPosition(), candidate) +
				                                   Position::getDistanceY(player.getPosition(), candidate);
				const auto topologyDistance = topologyDistances ?
				    PlayerBotTopology::instance().distanceTo(*topologyDistances, candidate) : std::nullopt;
				const uint32_t distance = topologyDistance ?
				    std::max(geometricDistance, *topologyDistance * topologySectorSize) : geometricDistance;
				if (distance < bestDistance) {
					best = candidate;
					bestDistance = distance;
				}
			}
		}
		if (bestDistance != std::numeric_limits<uint32_t>::max()) return best;
		if (!topologyDistances || PlayerBotTopology::instance().distanceTo(*topologyDistances, spawnPosition)) {
			return spawnPosition;
		}
		return std::nullopt;
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

	struct CachedPocket {
		uint8_t floor = 0;
		Position center;
		std::vector<size_t> members;
	};

	struct CachedVariant {
		uint64_t siteId = 0;
		uint64_t variantId = 0;
		Position center;
		std::vector<size_t> pockets;
		std::vector<size_t> members;
		uint32_t floorCount = 0;
	};

	struct HuntAtlas {
		std::vector<CachedSpawnBlock> spawns;
		std::vector<CachedPocket> pockets;
		std::vector<CachedVariant> variants;
		std::map<Position, std::vector<size_t>> spawnBuckets;
		uint64_t spawnGeneration = 0;
		uint64_t topologyGeneration = 0;
		uint64_t buildTimeUs = 0;
		size_t siteCount = 0;
		bool initialized = false;
		std::map<const MonsterType*, double> coinGold;
		int32_t lootRate = 0;
	};

	HuntAtlas huntAtlas;
	uint64_t huntAtlasRevision = 0;

	double spawnDamagePerSecond(const CachedSpawnBlock& spawn, const PlayerBotCombatProfile& combat)
	{
		if (spawn.monsters.empty()) return 0;
		if (spawn.monsters.size() == 1) return expectedMonsterDamagePerSecond(*spawn.monsters.front().first, combat);
		double damage = 0;
		double noSelectionProbability = 1;
		for (const auto& [monster, chance] : spawn.monsters) {
			const double selectionProbability = noSelectionProbability * std::min(chance / 100.0, 1.0);
			damage += selectionProbability * expectedMonsterDamagePerSecond(*monster, combat);
			noSelectionProbability -= selectionProbability;
		}
		return damage + noSelectionProbability * expectedMonsterDamagePerSecond(*spawn.monsters.front().first, combat);
	}

	double expectedDamagePerSecondAt(const PlayerBotCombatProfile& combat, const Position& position)
	{
		if (!huntAtlas.initialized || combat.maximumHealth <= 0 ||
		    huntAtlas.spawnGeneration != g_game.map.spawns.getGeneration() ||
		    huntAtlas.topologyGeneration != PlayerBotTopology::instance().generation()) return 0;
		std::array<double, maximumModeledAttackers> strongest{};
		const int32_t bucketX = position.x / spawnBucketSize;
		const int32_t bucketY = position.y / spawnBucketSize;
		for (int32_t x = bucketX - 1; x <= bucketX + 1; ++x) {
			for (int32_t y = bucketY - 1; y <= bucketY + 1; ++y) {
				if (x < 0 || y < 0) continue;
				const auto bucket = huntAtlas.spawnBuckets.find(
				    Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), position.z));
				if (bucket == huntAtlas.spawnBuckets.end()) continue;
				for (size_t spawnIndex : bucket->second) {
					const CachedSpawnBlock& spawn = huntAtlas.spawns[spawnIndex];
					const uint32_t distance = std::max(Position::getDistanceX(position, spawn.position),
					                                   Position::getDistanceY(position, spawn.position));
					if (distance > heatRadius) continue;
					if (!g_game.isSightClear(spawn.position, position, true)) continue;
					const double reach = static_cast<double>(heatRadius + 1 - distance) / (heatRadius + 1);
					double damage = spawnDamagePerSecond(spawn, combat) * reach;
					for (double& current : strongest) {
						if (damage <= current) continue;
						std::swap(current, damage);
					}
				}
			}
		}
		return std::accumulate(strongest.begin(), strongest.end(), 0.0);
	}

	bool topologyRegionReachable(Player& player, const CachedVariant& variant,
	                            const PlayerBotTopologyDistances& distances)
	{
		for (size_t member : variant.members) {
			if (nearestApproach(player, huntAtlas.spawns[member].position, &distances)) return true;
		}
		return false;
	}

	double expectedCoinGold(const std::vector<LootBlock>& loot, double rate, PlayerBotLootCountMemo& memo);

	void buildHuntAtlas(uint64_t& snapshotTimeUs, uint64_t& clusteringTimeUs)
	{
		const auto buildStarted = std::chrono::steady_clock::now();
		huntAtlas = HuntAtlas{};
		huntAtlas.lootRate = g_config.getNumber(ConfigManager::RATE_LOOT);
		PlayerBotLootCountMemo lootCounts;
		const auto snapshotStarted = std::chrono::steady_clock::now();
		for (SpawnBlockSnapshot& snapshot : g_game.map.spawns.getMonsterSpawnSnapshots()) {
			snapshot.monsterTypes.erase(std::remove_if(snapshot.monsterTypes.begin(), snapshot.monsterTypes.end(),
				[](const auto& entry) {
					return !entry.first || !entry.first->info.isHostile || !entry.first->info.isAttackable;
			}), snapshot.monsterTypes.end());
			if (!snapshot.monsterTypes.empty()) {
				for (const auto& entry : snapshot.monsterTypes) {
					if (huntAtlas.coinGold.find(entry.first) == huntAtlas.coinGold.end()) {
						huntAtlas.coinGold.emplace(entry.first,
						    expectedCoinGold(entry.first->info.lootItems, huntAtlas.lootRate, lootCounts));
					}
				}
				huntAtlas.spawns.push_back({snapshot.position, snapshot.interval, std::move(snapshot.monsterTypes)});
			}
		}
		snapshotTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - snapshotStarted).count();

		const auto clusteringStarted = std::chrono::steady_clock::now();
		for (size_t index = 0; index < huntAtlas.spawns.size(); ++index) {
			const Position& position = huntAtlas.spawns[index].position;
			huntAtlas.spawnBuckets[Position(position.x / spawnBucketSize, position.y / spawnBucketSize, position.z)].push_back(index);
		}
		for (CachedSpawnBlock& spawn : huntAtlas.spawns) {
			const int32_t bucketX = spawn.position.x / spawnBucketSize;
			const int32_t bucketY = spawn.position.y / spawnBucketSize;
			for (int32_t x = bucketX - 1; x <= bucketX + 1; ++x) {
				for (int32_t y = bucketY - 1; y <= bucketY + 1; ++y) {
					if (x < 0 || y < 0) continue;
					auto nearby = huntAtlas.spawnBuckets.find(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), spawn.position.z));
					if (nearby == huntAtlas.spawnBuckets.end()) continue;
					for (size_t candidate : nearby->second) {
						if (heatOverlaps(spawn.position, huntAtlas.spawns[candidate].position)) {
							spawn.neighbors.push_back(candidate);
						}
					}
				}
			}
		}
		std::vector<bool> assigned(huntAtlas.spawns.size(), false);
		std::vector<size_t> pocketForSpawn(huntAtlas.spawns.size(), 0);
		for (size_t seed = 0; seed < huntAtlas.spawns.size(); ++seed) {
			if (assigned[seed]) continue;
			CachedPocket pocket;
			pocket.floor = huntAtlas.spawns[seed].position.z;
			std::deque<size_t> frontier = {seed};
			assigned[seed] = true;
			uint64_t xTotal = 0;
			uint64_t yTotal = 0;
			while (!frontier.empty()) {
				const size_t current = frontier.front();
				frontier.pop_front();
				pocket.members.push_back(current);
				xTotal += huntAtlas.spawns[current].position.x;
				yTotal += huntAtlas.spawns[current].position.y;
				for (size_t candidate : huntAtlas.spawns[current].neighbors) {
					if (!assigned[candidate] &&
					    heatOverlaps(huntAtlas.spawns[current].position, huntAtlas.spawns[candidate].position) &&
					    Position::getDistanceX(huntAtlas.spawns[seed].position, huntAtlas.spawns[candidate].position) <= maximumRegionRadius &&
					    Position::getDistanceY(huntAtlas.spawns[seed].position, huntAtlas.spawns[candidate].position) <= maximumRegionRadius) {
						assigned[candidate] = true;
						frontier.push_back(candidate);
					}
				}
			}
			pocket.center = Position(static_cast<uint16_t>(xTotal / pocket.members.size()),
			                         static_cast<uint16_t>(yTotal / pocket.members.size()), pocket.floor);
			const size_t pocketIndex = huntAtlas.pockets.size();
			for (size_t member : pocket.members) pocketForSpawn[member] = pocketIndex;
			huntAtlas.pockets.push_back(std::move(pocket));
		}

		std::vector<std::set<size_t>> directedPocketEdges(huntAtlas.pockets.size());
		auto nearestPocket = [&pocketForSpawn](const Position& position) -> std::optional<size_t> {
			const int32_t bucketX = position.x / spawnBucketSize;
			const int32_t bucketY = position.y / spawnBucketSize;
			uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
			std::optional<size_t> best;
			for (int32_t x = bucketX - 2; x <= bucketX + 2; ++x) {
				for (int32_t y = bucketY - 2; y <= bucketY + 2; ++y) {
					if (x < 0 || y < 0) continue;
					auto bucket = huntAtlas.spawnBuckets.find(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), position.z));
					if (bucket == huntAtlas.spawnBuckets.end()) continue;
					for (size_t spawnIndex : bucket->second) {
						const Position& spawn = huntAtlas.spawns[spawnIndex].position;
						if (Position::getDistanceX(position, spawn) > atlasPortalLinkRadius ||
						    Position::getDistanceY(position, spawn) > atlasPortalLinkRadius) continue;
						if (!PlayerBotTopology::instance().sameWalkNode(position, spawn)) continue;
						const uint32_t distance = Position::getDistanceX(position, spawn) + Position::getDistanceY(position, spawn);
						if (distance < bestDistance) {
							bestDistance = distance;
							best = pocketForSpawn[spawnIndex];
						}
					}
				}
			}
			return best;
		};
		for (const PlayerBotTopologyPortal& portal : PlayerBotTopology::instance().portals()) {
			if (portal.approach.z == portal.destination.z) continue;
			const auto source = nearestPocket(portal.approach);
			const auto destination = nearestPocket(portal.destination);
			if (!source || !destination || *source == *destination) continue;
			directedPocketEdges[*source].insert(*destination);
		}
		std::vector<std::set<size_t>> pocketEdges(huntAtlas.pockets.size());
		for (size_t source = 0; source < directedPocketEdges.size(); ++source) {
			for (size_t destination : directedPocketEdges[source]) {
				if (directedPocketEdges[destination].find(source) == directedPocketEdges[destination].end()) continue;
				pocketEdges[source].insert(destination);
				pocketEdges[destination].insert(source);
			}
		}

		std::vector<bool> pocketAssigned(huntAtlas.pockets.size(), false);
		auto atlasIdentity = [](const std::vector<size_t>& pockets) {
			std::vector<Position> positions;
			for (size_t pocketIndex : pockets) {
				for (size_t member : huntAtlas.pockets[pocketIndex].members) positions.push_back(huntAtlas.spawns[member].position);
			}
			std::sort(positions.begin(), positions.end());
			uint64_t hash = 1469598103934665603ULL;
			for (const Position& position : positions) {
				for (uint64_t value : {static_cast<uint64_t>(position.x), static_cast<uint64_t>(position.y),
				                       static_cast<uint64_t>(position.z)}) {
					hash ^= value;
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		};
		for (size_t seed = 0; seed < huntAtlas.pockets.size(); ++seed) {
			if (pocketAssigned[seed]) continue;
			std::vector<size_t> sitePockets;
			std::deque<size_t> open = {seed};
			pocketAssigned[seed] = true;
			while (!open.empty()) {
				const size_t current = open.front();
				open.pop_front();
				sitePockets.push_back(current);
				for (size_t neighbor : pocketEdges[current]) {
					if (pocketAssigned[neighbor]) continue;
					pocketAssigned[neighbor] = true;
					open.push_back(neighbor);
				}
			}
			++huntAtlas.siteCount;
			const uint64_t siteId = atlasIdentity(sitePockets);

			std::set<std::vector<size_t>> variantKeys;
			auto addVariant = [&](std::vector<size_t> pockets) {
				std::vector<size_t> key = pockets;
				std::sort(key.begin(), key.end());
				key.erase(std::unique(key.begin(), key.end()), key.end());
				if (key.empty() || !variantKeys.insert(key).second) return;
				CachedVariant variant;
				variant.siteId = siteId;
				variant.variantId = atlasIdentity(key);
				variant.pockets = std::move(pockets);
				std::set<uint8_t> floors;
				for (size_t pocketIndex : variant.pockets) {
					floors.insert(huntAtlas.pockets[pocketIndex].floor);
					variant.members.insert(variant.members.end(), huntAtlas.pockets[pocketIndex].members.begin(),
					                       huntAtlas.pockets[pocketIndex].members.end());
				}
				variant.center = huntAtlas.pockets[variant.pockets.front()].center;
				variant.floorCount = static_cast<uint32_t>(floors.size());
				huntAtlas.variants.push_back(std::move(variant));
			};

			for (size_t pocket : sitePockets) addVariant({pocket});
			size_t neighborhoods = 0;
			for (size_t pocket : sitePockets) {
				if (neighborhoods >= maximumNeighborhoodVariantsPerSite) break;
				std::vector<size_t> neighborhood = {pocket};
				neighborhood.insert(neighborhood.end(), pocketEdges[pocket].begin(), pocketEdges[pocket].end());
				if (neighborhood.size() > 1) {
					addVariant(std::move(neighborhood));
					++neighborhoods;
				}
			}
			if (sitePockets.size() > 1) addVariant(sitePockets);
		}
		clusteringTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - clusteringStarted).count();
		huntAtlas.spawnGeneration = g_game.map.spawns.getGeneration();
		huntAtlas.topologyGeneration = PlayerBotTopology::instance().generation();
		huntAtlas.buildTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - buildStarted).count();
		huntAtlas.initialized = true;
		++huntAtlasRevision;
	}

	double expectedCoinGold(const std::vector<LootBlock>& loot, double rate, PlayerBotLootCountMemo& memo)
	{
		double gold = 0;
		for (const LootBlock& block : loot) {
			const ItemType& type = Item::items[block.id];
			if (type.worth != 0) {
				gold += type.worth * memo.expectedCount(block.chance, block.countmax, type.stackable, rate);
			} else if (!block.childLoot.empty()) {
				gold += memo.expectedCount(block.chance, block.countmax, false, rate) * expectedCoinGold(block.childLoot, rate, memo);
			}
		}
		return gold;
	}

	PlayerBotHuntRegion scoreRegion(Player& player, const PlayerBotHuntPlanningProfile& planningProfile,
		size_t candidateIndex, const std::set<uint64_t>& excludedVariants,
		const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance,
		uint32_t huntDurationSeconds, const PlayerBotTopologyDistances* topologyDistances,
		bool& withinPlanningScope)
	{
		const PlayerBotCombatProfile& profile = planningProfile.combat;
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		const CachedVariant& cached = huntAtlas.variants[candidateIndex];
		// Static topology describes walking/tool portals only. A disconnected
		// variant remains a candidate because the bounded route preflight can
		// reach it through a verified NPC travel offer.
		const bool locallyReachable = topologyDistances && topologyRegionReachable(player, cached, *topologyDistances);
		const PlayerBotTopologyDistances* memberDistances = locallyReachable ? topologyDistances : nullptr;
		PlayerBotHuntRegion region;
		region.atlasSiteId = cached.siteId;
		region.atlasVariantId = cached.variantId;
		region.atlasRevision = huntAtlasRevision;
		region.atlasPocketCount = static_cast<uint32_t>(cached.pockets.size());
		region.atlasSpawnCount = static_cast<uint32_t>(cached.members.size());
		region.atlasFloorCount = cached.floorCount;
		region.cashPressure = planningProfile.cashPressure;
		region.supplyRecovery = planningProfile.supplyRecovery;
		region.floor = cached.center.z;
		region.center = cached.center;
		std::map<std::string, PlayerBotHuntMonsterProfile> profiles;
		std::set<size_t> reachableMembers;
		std::vector<PlayerBotReplenishmentPoint> replenishment;
		double expectedCycleExperience = 0;
		double expectedClearSeconds = 0;
		double spawnDamagePerSecond = 0;
		double spawnCombatFraction = 0;
		double crowdDamageInflation = 1;
		for (size_t member : cached.members) {
			const CachedSpawnBlock& spawn = huntAtlas.spawns[member];
			const auto approach = nearestApproach(player, spawn.position, memberDistances);
			if (!approach) continue;
			reachableMembers.insert(member);
			region.patrolPoints.push_back(*approach);
			PlayerBotReplenishmentPoint point{spawn.position, *approach, 0, spawn.interval / 1000.0, true};
			std::vector<std::pair<const MonsterType*, double>> spawnProbabilities;
			if (spawn.monsters.size() == 1) {
				spawnProbabilities.emplace_back(spawn.monsters.front().first, 1.0);
			} else {
				double noSelectionProbability = 1.0;
				for (const auto& [monsterType, chance] : spawn.monsters) {
					const double probability = noSelectionProbability * std::min(chance / 100.0, 1.0);
					spawnProbabilities.emplace_back(monsterType, probability);
					noSelectionProbability -= probability;
				}
				spawnProbabilities.front().second += noSelectionProbability;
			}
			for (const auto& [monsterType, probability] : spawnProbabilities) {
				PlayerBotHuntMonsterProfile& monsterProfile = profiles[monsterType->name];
				monsterProfile.name = monsterType->name;
				monsterProfile.expectedSpawns += probability;
				monsterProfile.experience = monsterType->info.experience;
				monsterProfile.health = monsterType->info.healthMax;
				monsterProfile.expectedDamagePerSecond = expectedMonsterDamagePerSecond(*monsterType, profile);
				const double fightSeconds = monsterType->info.healthMax / expectedPlayerDamagePerSecond(profile, *monsterType);
				monsterProfile.predictedFightDamage = monsterProfile.expectedDamagePerSecond * fightSeconds;
				point.fightSeconds += fightSeconds * probability;
				point.ignoresBlocking = point.ignoresBlocking && monsterType->info.isIgnoringSpawnBlock;
				const double gold = staminaMinutes > 840 ? huntAtlas.coinGold.at(monsterType) * probability : 0;
				point.coinGold += gold;
				point.experience += monsterType->info.experience * probability;
				region.experiencePerMinute += monsterType->info.experience * probability *
				                              (60000.0 / std::max<uint32_t>(spawn.interval, 1));
				expectedCycleExperience += monsterType->info.experience * probability;
				expectedClearSeconds += fightSeconds * probability;
				const double spawnsPerSecond = probability * 1000.0 / std::max<uint32_t>(spawn.interval, 1);
				spawnDamagePerSecond += monsterProfile.predictedFightDamage * spawnsPerSecond;
				spawnCombatFraction += fightSeconds * spawnsPerSecond;
			}
			replenishment.push_back(point);
		}

		if (profiles.empty() || region.patrolPoints.empty()) return region;
		double worstFightDamage = 0;
		double worstFightSeconds = 0;
		for (size_t anchor : reachableMembers) {
			struct LocalAttacker {
				double damagePerSecond;
				double fightSeconds;
			};
			std::vector<LocalAttacker> localAttackers;
			for (size_t neighbor : huntAtlas.spawns[anchor].neighbors) {
				if (reachableMembers.find(neighbor) == reachableMembers.end()) continue;
				for (const auto& [monsterType, chance] : huntAtlas.spawns[neighbor].monsters) {
					(void)chance;
					localAttackers.push_back({expectedMonsterDamagePerSecond(*monsterType, profile),
					                          monsterType->info.healthMax / expectedPlayerDamagePerSecond(profile, *monsterType)});
				}
			}
			std::sort(localAttackers.begin(), localAttackers.end(), [](const LocalAttacker& left, const LocalAttacker& right) {
				return left.damagePerSecond > right.damagePerSecond;
			});
			const size_t attackers = std::min(maximumModeledAttackers, localAttackers.size());
			region.modeledMaximumAttackerOverlap = std::max<uint8_t>(
			    region.modeledMaximumAttackerOverlap, static_cast<uint8_t>(attackers));
			double remainingDamagePerSecond = 0;
			for (size_t index = 0; index < attackers; ++index) remainingDamagePerSecond += localAttackers[index].damagePerSecond;
			double fightDamage = 0;
			double isolatedDamage = 0;
			double fightSeconds = 0;
			for (size_t index = 0; index < attackers; ++index) {
				fightDamage += remainingDamagePerSecond * localAttackers[index].fightSeconds;
				isolatedDamage += localAttackers[index].damagePerSecond * localAttackers[index].fightSeconds;
				fightSeconds += localAttackers[index].fightSeconds;
				remainingDamagePerSecond -= localAttackers[index].damagePerSecond;
			}
			crowdDamageInflation = std::max(crowdDamageInflation,
			    playerBotCrowdDamageInflation(fightDamage, isolatedDamage));
			if (fightDamage > worstFightDamage) {
				worstFightDamage = fightDamage;
				worstFightSeconds = fightSeconds;
			}
		}
		std::set<Position> uniquePatrolPoints;
		region.patrolPoints.erase(std::remove_if(region.patrolPoints.begin(), region.patrolPoints.end(),
			[&uniquePatrolPoints](const Position& point) { return !uniquePatrolPoints.insert(point).second; }), region.patrolPoints.end());
		double patrolSeconds = 0;
		if (region.patrolPoints.size() > 1) {
			for (size_t index = 0; index < region.patrolPoints.size(); ++index) {
				const Position& from = region.patrolPoints[index];
				const Position& to = region.patrolPoints[(index + 1) % region.patrolPoints.size()];
				const uint32_t steps = Position::getDistanceX(from, to) + Position::getDistanceY(from, to) +
				                       Position::getDistanceZ(from, to) * 20;
				patrolSeconds += steps * player.getStepDuration() / 1000.0;
			}
		}
		region.viability = playerBotHuntViability(replenishment, player.getStepDuration() / 1000.0,
		    Map::maxViewportX, Map::maxViewportY);
		region.sustainedEligible = region.viability.eligible;
		region.spawnExperiencePerMinute = region.experiencePerMinute;
		const double cycleSeconds = expectedClearSeconds + patrolSeconds;
		region.clearExperiencePerMinute = cycleSeconds > 0 ? expectedCycleExperience * 60.0 / cycleSeconds : 0;
		if (region.clearExperiencePerMinute > 0) {
			region.experiencePerMinute = std::min(region.spawnExperiencePerMinute, region.clearExperiencePerMinute);
		}
		const double throughput = region.spawnExperiencePerMinute > 0 ?
		    region.experiencePerMinute / region.spawnExperiencePerMinute : 1;
		// Spawn rates already sum isolated fights. Apply only the largest local
		// crowd/isolated ratio, not crowd damage divided by one monster's damage.
		// This is conservative static prediction, not observed supply calibration.
		region.expectedDamagePerSecond = spawnDamagePerSecond * throughput * crowdDamageInflation;
		region.combatFraction = std::min(1.0, spawnCombatFraction * throughput);
		// Blocked members can still be encountered initially: retain their clear
		// time, danger and supply costs, but never count them as recurring yield.
		const auto yield = playerBotSustainedHuntYield(replenishment, region.viability, cycleSeconds);
		region.coinGoldPerMinute = yield.coinGoldPerMinute;
		// Small replenishing spawns can fund recovery without long-hunt throughput.
		if (region.supplyRecovery && region.coinGoldPerMinute > 0 && expectedCycleExperience > 0)
			region.sustainedEligible = true;
		region.spawnExperiencePerMinute = yield.spawnExperiencePerMinute;
		region.clearExperiencePerMinute = yield.clearExperiencePerMinute;
		region.experiencePerMinute = std::min(yield.spawnExperiencePerMinute, yield.clearExperiencePerMinute);
		region.supplyProfile = planningProfile.supply;
		region.supplyGlobalLearning = planningProfile.supplyGlobalLearning;
		region.supplyCapability = playerBotSupplyCapability(planningProfile);
		if (const auto found = performance.find(region.atlasVariantId);
		    found != performance.end() && found->second.atlasRevision == region.atlasRevision) {
			// Retain incompatible local evidence for an honest rejection reason;
			// reconciliation falls back to the independent global multiplier.
			region.supplyCalibration = found->second.supply;
		}
		region.destination = *std::min_element(region.patrolPoints.begin(), region.patrolPoints.end(),
			[&player](const Position& left, const Position& right) {
				const uint32_t leftDistance = Position::getDistanceX(player.getPosition(), left) +
				                              Position::getDistanceY(player.getPosition(), left);
				const uint32_t rightDistance = Position::getDistanceX(player.getPosition(), right) +
				                               Position::getDistanceY(player.getPosition(), right);
				return leftDistance < rightDistance;
			});
		if (topologyDistances) {
			uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
			for (const Position& patrolPoint : region.patrolPoints) {
				const auto topologyDistance = PlayerBotTopology::instance().distanceTo(*topologyDistances, patrolPoint);
				if (!topologyDistance) continue;
				const uint32_t geometricDistance = Position::getDistanceX(player.getPosition(), patrolPoint) +
				                                   Position::getDistanceY(player.getPosition(), patrolPoint) +
				                                   Position::getDistanceZ(player.getPosition(), patrolPoint) * 20;
				const uint32_t distance = std::max(geometricDistance, *topologyDistance * topologySectorSize);
				if (distance < bestDistance) {
					bestDistance = distance;
					region.destination = patrolPoint;
				}
			}
			region.topologyReachable = bestDistance != std::numeric_limits<uint32_t>::max();
			if (region.topologyReachable) region.topologyTravelSteps = bestDistance;
		}
		uint32_t transportTravelSteps = std::numeric_limits<uint32_t>::max();
		if (!region.topologyReachable) {
			region.transportPlausible = false;
			for (const PlayerBotHuntTransportArrival& arrival : planningProfile.transportArrivals) {
				if (!arrival.topologyReachability) continue;
				for (const Position& patrolPoint : region.patrolPoints) {
					if (!PlayerBotTopology::instance().reachable(*arrival.topologyReachability, patrolPoint)) continue;
					const uint32_t localSteps = Position::getDistanceX(arrival.position, patrolPoint) +
					                            Position::getDistanceY(arrival.position, patrolPoint) +
					                            Position::getDistanceZ(arrival.position, patrolPoint) * 20;
					const uint32_t totalSteps = arrival.estimatedSteps > UINT32_MAX - localSteps ?
					    UINT32_MAX : arrival.estimatedSteps + localSteps;
					if (totalSteps >= transportTravelSteps) continue;
					transportTravelSteps = totalSteps;
					region.destination = patrolPoint;
					region.transportPlausible = true;
				}
			}
		}
		for (auto& [name, monsterProfile] : profiles) {
			(void)name;
			region.monsters.push_back(std::move(monsterProfile));
		}
		const uint32_t geometricDistance = Position::getDistanceX(player.getPosition(), region.destination) +
		                                   Position::getDistanceY(player.getPosition(), region.destination) +
		                                   Position::getDistanceZ(player.getPosition(), region.destination) * 20;
		region.predictedFightSeconds = worstFightSeconds;
		region.currentHealth = planningProfile.currentHealth;
		region.maximumHealth = profile.maximumHealth;
		region.recovery = playerBotPredictRecovery(planningProfile, worstFightSeconds);
		region.rawThreatRatio = worstFightDamage / std::max<int32_t>(profile.maximumHealth, 1);
		region.threatRatio = std::max(0.0, worstFightDamage - region.recovery.totalMinimumHealing) /
		                     std::max<int32_t>(profile.maximumHealth, 1);
		region.challengeFrontier = planningProfile.challengeFrontier;
		region.challengeBandMinimum = 0;
		region.challengeBandMaximum = planningProfile.challengeFrontier + challengeHeadroom;
		region.inChallengeBand = region.threatRatio <= region.challengeBandMaximum;
		region.predictedLethal = playerBotPredictedLethal(planningProfile.currentHealth, worstFightDamage);
		withinPlanningScope = true;
		region.suitable = region.sustainedEligible && !region.predictedLethal &&
		    region.threatRatio <= region.challengeBandMaximum;
		if (excludedVariants.find(region.atlasVariantId) != excludedVariants.end()) {
			region.suitable = false;
			region.rejectionReason = "observed_danger_cooldown";
		} else if (region.predictedLethal) {
			region.rejectionReason = "predicted_lethal";
		} else if (!region.sustainedEligible) {
			region.rejectionReason = playerBotHuntViabilityRejection(region.viability);
		} else if (!region.suitable) {
			region.rejectionReason = "challenge_frontier";
		}
		const double experienceStage = g_config.getExperienceStage(player.getLevel());
		region.experiencePerMinute *= experienceStage;
		region.spawnExperiencePerMinute *= experienceStage;
		region.clearExperiencePerMinute *= experienceStage;
		region.staminaMinutes = staminaMinutes;
		const PlayerBotHuntCorrection correction = playerBotHuntCorrectionForVariant(
		    region.atlasVariantId, region.atlasRevision, performance);
		region.observedExperiencePerMinute = correction.observedExperiencePerMinute;
		region.observedCoinGoldPerMinute = correction.observedCoinGoldPerMinute;
		region.observedCorrection = correction.correction;
		region.calibrationSampleCount = correction.sampleCount;
		region.calibrationSource = correction.source;
		if (!region.topologyReachable && !region.transportPlausible) {
			region.suitable = false;
			region.rejectionReason = "transport_requirements_unavailable";
		}
		const uint32_t estimatedTravelSteps = region.topologyReachable ? region.topologyTravelSteps :
		    region.transportPlausible ? transportTravelSteps : geometricDistance;
		region.estimatedTravelSeconds = estimatedTravelSteps * player.getStepDuration() / 1000.0;
		region.availableHuntSeconds = std::max(0.0, huntDurationSeconds - region.estimatedTravelSeconds);
		region.staminaExperienceMultiplier = projectedStaminaExperienceMultiplier(player, region.availableHuntSeconds);
		region.projectedExperience = region.experiencePerMinute * region.observedCorrection *
		                             region.staminaExperienceMultiplier * region.availableHuntSeconds / 60.0;
		region.optimisticProjectedExperience = region.experiencePerMinute * region.observedCorrection *
		                                       1.5 * huntDurationSeconds / 60.0;
		region.score = region.projectedExperience;
		region.reconcileSupplies(planningProfile.supply.reserve);
		if (!region.recoverySustainable()) {
			region.suitable = false;
			region.rejectionReason = "recovery_hunt_not_sustainable";
		}
		region.reachable = withinPlanningScope;
		region.travelSteps = estimatedTravelSteps;
		return region;
	}
}

void PlayerBotHuntRegionAdapter::invalidateCache()
{
	huntAtlas = HuntAtlas{};
	++huntAtlasRevision;
}

PlayerBotHuntAtlasSummary PlayerBotHuntRegionAdapter::rebuildAtlas()
{
	uint64_t snapshotTimeUs = 0;
	uint64_t clusteringTimeUs = 0;
	buildHuntAtlas(snapshotTimeUs, clusteringTimeUs);
	return atlasSummary();
}

PlayerBotHuntAtlasSummary PlayerBotHuntRegionAdapter::atlasSummary()
{
	return {huntAtlasRevision, huntAtlas.topologyGeneration, huntAtlas.spawnGeneration, huntAtlas.buildTimeUs,
	        huntAtlas.spawns.size(), huntAtlas.pockets.size(), huntAtlas.siteCount, huntAtlas.variants.size()};
}

uint64_t PlayerBotHuntRegionAdapter::getCacheRevision()
{
	if (huntAtlas.initialized &&
	    (huntAtlas.spawnGeneration != g_game.map.spawns.getGeneration() ||
	     huntAtlas.lootRate != g_config.getNumber(ConfigManager::RATE_LOOT) ||
	     huntAtlas.topologyGeneration != PlayerBotTopology::instance().generation())) invalidateCache();
	return huntAtlasRevision;
}

PlayerBotHuntRegionScan PlayerBotHuntRegionAdapter::beginScan(
	Player& player, const PlayerBotTopologyDistances* topologyDistances)
{
	(void)player;
	(void)topologyDistances;
	PlayerBotHuntRegionScan scan;
	scan.cacheHit = huntAtlas.initialized && huntAtlas.lootRate == g_config.getNumber(ConfigManager::RATE_LOOT) &&
	                huntAtlas.spawnGeneration == g_game.map.spawns.getGeneration() &&
	                huntAtlas.topologyGeneration == PlayerBotTopology::instance().generation();
	if (!scan.cacheHit) buildHuntAtlas(scan.snapshotTimeUs, scan.clusteringTimeUs);
	scan.revision = huntAtlasRevision;
	for (size_t index = 0; index < huntAtlas.variants.size(); ++index) {
		scan.candidateIndices.push_back(index);
	}
	scan.candidateCount = scan.candidateIndices.size();
	return scan;
}

PlayerBotHuntRegionScore PlayerBotHuntRegionAdapter::score(Player& player, const PlayerBotHuntPlanningProfile& profile,
	uint64_t revision, size_t candidateIndex, const std::set<uint64_t>& excludedVariants,
	const std::map<uint64_t, PlayerBotHuntRegionPerformance>& performance, uint32_t huntDurationSeconds,
	const PlayerBotTopologyDistances* topologyDistances)
{
	PlayerBotHuntRegionScore observation;
	if (revision != getCacheRevision() || candidateIndex >= huntAtlas.variants.size()) return observation;
	observation.region = scoreRegion(player, profile, candidateIndex, excludedVariants, performance,
	                               huntDurationSeconds, topologyDistances, observation.withinPlanningScope);
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
	profile.supply.potions = profile.potionCount;
	profile.supply.potionHealing = profile.potionMinimumHealing;
	profile.supply.mana = profile.mana;
	for (uint8_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		if (slot == CONST_SLOT_BACKPACK) continue;
		if (const Item* item = player.getInventoryItem(static_cast<slots_t>(slot))) {
			profile.equipmentItemIds[slot] = item->getID();
		}
	}
	profile.supply.maximumMana = player.getMaxMana();
	auto interval = [](int64_t ticks) {
		// Conditions execute on creature ticks, resetting their counter on gain.
		return ticks > 0 ? std::ceil(static_cast<double>(ticks) / EVENT_CREATURE_THINK_INTERVAL) *
		    EVENT_CREATURE_THINK_INTERVAL / 1000.0 : 0;
	};
	Condition* food = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT);
	profile.foodAvailable = food || playerbot::PlayerBotInventoryPolicy::foodInventory(player).count > 0;
	if (food) {
		profile.supply.regenerationSeconds = food->getTicks() == -1 ?
		    std::numeric_limits<double>::max() : std::max(0, food->getTicks()) / 1000.0;
		profile.supply.healthGain = std::max(0, food->getParam(CONDITION_PARAM_HEALTHGAIN));
		profile.supply.healthInterval = interval(food->getParam(CONDITION_PARAM_HEALTHTICKS));
		profile.supply.manaGain = std::max(0, food->getParam(CONDITION_PARAM_MANAGAIN));
		profile.supply.manaInterval = interval(food->getParam(CONDITION_PARAM_MANATICKS));
	} else {
		const Vocation* vocation = player.getVocation();
		profile.supply.healthGain = vocation->getHealthGainAmount();
		profile.supply.healthInterval = interval(static_cast<int64_t>(vocation->getHealthGainTicks()) * 1000);
		profile.supply.manaGain = vocation->getManaGainAmount();
		profile.supply.manaInterval = interval(static_cast<int64_t>(vocation->getManaGainTicks()) * 1000);
	}
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
	profile.supply.spellLegal = profile.lightHealingLegal;
	profile.supply.spellMana = profile.lightHealingManaCost;
	profile.supply.spellHealing = std::max(0, profile.lightHealingMinimum);
	profile.supply.spellInterval = std::max<uint32_t>(profile.lightHealingCooldown, 1) / 1000.0;
	return profile;
}

double PlayerBotHuntRegionAdapter::travelDanger(const PlayerBotCombatProfile& combat, const Position& position)
{
	return expectedDamagePerSecondAt(combat, position) / std::max<int32_t>(combat.maximumHealth, 1);
}

PlayerBotFightEstimate PlayerBotHuntRegionAdapter::fightEstimate(
	const PlayerBotCombatProfile& combat, const std::string& monsterName, int32_t remainingHealth)
{
	const MonsterType* monsterType = g_monsters.getMonsterType(monsterName, false);
	if (!monsterType) return {};
	return {expectedMonsterDamagePerSecond(*monsterType, combat),
	        std::max<int32_t>(remainingHealth, 1) / expectedPlayerDamagePerSecond(combat, *monsterType)};
}

PlayerBotHuntCorridorDanger PlayerBotHuntRegionAdapter::corridorDanger(const PlayerBotCombatProfile& combat,
	const std::deque<PlayerBotNavigationStep>& steps, const Position& destinationCenter, uint32_t stepDurationMs)
{
	PlayerBotHuntCorridorDanger result;
	if (!huntAtlas.initialized || steps.empty() || combat.maximumHealth <= 0) return result;
	result.available = true;
	double expectedDamage = 0;
	std::set<size_t> corridorSpawns;
	for (const PlayerBotNavigationStep& step : steps) {
		const Position& position = step.expectedPosition;
		if (position.z == destinationCenter.z &&
		    Position::getDistanceX(position, destinationCenter) <= maximumRegionRadius &&
		    Position::getDistanceY(position, destinationCenter) <= maximumRegionRadius) continue;
		++result.sampledPositions;
		std::set<size_t> nearbySpawns;
		const int32_t bucketX = position.x / spawnBucketSize;
		const int32_t bucketY = position.y / spawnBucketSize;
		for (int32_t x = bucketX - 1; x <= bucketX + 1; ++x) {
			for (int32_t y = bucketY - 1; y <= bucketY + 1; ++y) {
				if (x < 0 || y < 0) continue;
				auto bucket = huntAtlas.spawnBuckets.find(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), position.z));
				if (bucket == huntAtlas.spawnBuckets.end()) continue;
				for (size_t spawnIndex : bucket->second) {
					const Position& spawnPosition = huntAtlas.spawns[spawnIndex].position;
					if (Position::getDistanceX(position, spawnPosition) <= heatRadius &&
					    Position::getDistanceY(position, spawnPosition) <= heatRadius) nearbySpawns.insert(spawnIndex);
				}
			}
		}
		std::vector<double> attackers;
		for (size_t spawnIndex : nearbySpawns) {
			corridorSpawns.insert(spawnIndex);
			const auto& monsters = huntAtlas.spawns[spawnIndex].monsters;
			if (monsters.empty()) continue;
			double spawnDamagePerSecond = 0;
			if (monsters.size() == 1) {
				spawnDamagePerSecond = expectedMonsterDamagePerSecond(*monsters.front().first, combat);
			} else {
				double noSelectionProbability = 1;
				for (const auto& [monster, chance] : monsters) {
					const double selectionProbability = noSelectionProbability * std::min(chance / 100.0, 1.0);
					spawnDamagePerSecond += selectionProbability * expectedMonsterDamagePerSecond(*monster, combat);
					noSelectionProbability -= selectionProbability;
				}
				// Spawn::spawnMonster falls back to the first entry when every chance roll fails.
				spawnDamagePerSecond += noSelectionProbability * expectedMonsterDamagePerSecond(*monsters.front().first, combat);
			}
			attackers.push_back(spawnDamagePerSecond);
		}
		std::sort(attackers.begin(), attackers.end(), std::greater<double>());
		if (attackers.size() > maximumModeledAttackers) attackers.resize(maximumModeledAttackers);
		expectedDamage += std::accumulate(attackers.begin(), attackers.end(), 0.0) * stepDurationMs / 1000.0;
	}
	result.nearbySpawnBlocks = static_cast<uint32_t>(corridorSpawns.size());
	result.dangerRatio = expectedDamage / combat.maximumHealth;
	return result;
}
