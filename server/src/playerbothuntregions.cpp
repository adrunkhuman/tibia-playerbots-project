/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbothuntregions.h"

#include "configmanager.h"
#include "game.h"
#include "item.h"
#include "monsters.h"
#include "player.h"
#include "playerbotnavigation.h"
#include "spawn.h"
#include "tile.h"
#include "weapons.h"

#include <deque>
#include <numeric>
#include <set>

extern Game g_game;
extern ConfigManager g_config;

namespace {
	constexpr Position rookTemple(32097, 32219, 7);
	constexpr int32_t rookRadius = 180;
	constexpr int32_t heatRadius = 8;
	constexpr int32_t maximumRegionRadius = 24;
	constexpr double maximumThreatRatio = 0.35;
	constexpr size_t maximumReachabilityChecks = 12;
	constexpr uint64_t maximumReachabilityExpandedNodes = 300000;

	bool isRookgaardSpawn(const Position& position)
	{
		return position.z >= 6 && position.z <= 15 &&
		       Position::getDistanceX(position, rookTemple) <= rookRadius &&
		       Position::getDistanceY(position, rookTemple) <= rookRadius;
	}

	bool heatOverlaps(const Position& left, const Position& right)
	{
		return left.z == right.z &&
		       Position::getDistanceX(left, right) <= heatRadius * 2 &&
		       Position::getDistanceY(left, right) <= heatRadius * 2;
	}

	double expectedMonsterDamagePerSecond(const MonsterType& monsterType, const Player& player)
	{
		double damagePerSecond = 0;
		const double mitigation = player.getArmor() * 0.35 + player.getDefense() * 0.08;
		for (const spellBlock_t& attack : monsterType.info.attackSpells) {
			const double averageDamage = (std::abs(attack.minCombatValue) + std::abs(attack.maxCombatValue)) / 2.0;
			if (averageDamage <= 0 || attack.speed == 0) {
				continue;
			}
			const double mitigatedDamage = attack.isMelee ? std::max(1.0, averageDamage - mitigation) : averageDamage;
			damagePerSecond += mitigatedDamage * (attack.chance / 100.0) * (1000.0 / attack.speed);
			damagePerSecond += attack.conditionDamage * (attack.chance / 100.0) / 10.0;
		}
		return std::max(0.5, damagePerSecond);
	}

	double expectedPlayerDamagePerSecond(const Player& player, const MonsterType& monsterType)
	{
		const Item* weapon = player.getWeapon(true);
		const int32_t attackValue = weapon ? weapon->getAttack() : 7;
		const int32_t attackSkill = weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST);
		const int32_t maximumDamage = Weapons::getMaxWeaponDamage(player.getLevel(), attackSkill, attackValue,
		                                                            player.getAttackFactor());
		const double averageDamage = std::max(1.0, maximumDamage / 2.0 -
		                                             monsterType.info.armor * 0.25 - monsterType.info.defense * 0.15);
		return averageDamage / 2.0;
	}

	Position nearestApproach(Player& player, const Position& spawnPosition)
	{
		Position best;
		uint32_t bestDistance = std::numeric_limits<uint32_t>::max();
		for (int32_t x = -1; x <= 1; ++x) {
			for (int32_t y = -1; y <= 1; ++y) {
				if (x == 0 && y == 0) {
					continue;
				}
				Position candidate(spawnPosition.x + x, spawnPosition.y + y, spawnPosition.z);
				Tile* tile = g_game.map.getTile(candidate);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) {
					continue;
				}
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

	double estimateTravelSeconds(const Player& player, const std::deque<PlayerBotNavigationStep>& route)
	{
		double seconds = 0;
		for (const PlayerBotNavigationStep& step : route) {
			seconds += step.action == PlayerBotNavigationAction::Move ?
			           player.getStepDuration(step.direction) / 1000.0 : 1.0;
		}
		return seconds;
	}

	double projectedStaminaExperienceMultiplier(const Player& player, double availableHuntSeconds)
	{
		const uint16_t staminaMinutes = player.getStaminaMinutes();
		if (staminaMinutes == 0) {
			return 0;
		}
		if (!g_config.getBoolean(ConfigManager::STAMINA_SYSTEM)) {
			return 1;
		}
		if (staminaMinutes > 2400 && player.isPremium() && availableHuntSeconds > 0) {
			// The award callback can consume two minutes before it checks the premium threshold.
			const double bonusSeconds = std::min(availableHuntSeconds,
			                                     std::max<int32_t>(0, staminaMinutes - 2402) * 60.0);
			return 1 + 0.5 * bonusSeconds / availableHuntSeconds;
		}
		return staminaMinutes <= 840 ? 0.5 : 1;
	}
}

std::vector<PlayerBotHuntRegion> PlayerBotHuntRegionPlanner::evaluate(Player& player,
	                                                                  const PlayerBotNavigator& navigator,
	                                                                  const std::set<Position>& excludedRegions,
	                                                                  const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
	                                                                  uint32_t huntDurationSeconds) const
{
	const uint16_t staminaMinutes = player.getStaminaMinutes();
	std::vector<SpawnBlockSnapshot> spawns;
	for (SpawnBlockSnapshot& spawn : g_game.map.spawns.getMonsterSpawnSnapshots()) {
		spawn.monsterTypes.erase(std::remove_if(spawn.monsterTypes.begin(), spawn.monsterTypes.end(),
			[](const auto& entry) {
				return !entry.first || !entry.first->info.isHostile || !entry.first->info.isAttackable;
			}), spawn.monsterTypes.end());
		if (isRookgaardSpawn(spawn.position) && !spawn.monsterTypes.empty()) {
			spawns.push_back(std::move(spawn));
		}
	}

	std::vector<bool> assigned(spawns.size(), false);
	std::vector<PlayerBotHuntRegion> regions;
	for (size_t seed = 0; seed < spawns.size(); ++seed) {
		if (assigned[seed]) {
			continue;
		}
		std::vector<size_t> members;
		std::deque<size_t> frontier = {seed};
		assigned[seed] = true;
		while (!frontier.empty()) {
			const size_t current = frontier.front();
			frontier.pop_front();
			members.push_back(current);
			for (size_t candidate = 0; candidate < spawns.size(); ++candidate) {
				if (!assigned[candidate] && heatOverlaps(spawns[current].position, spawns[candidate].position) &&
				    Position::getDistanceX(spawns[seed].position, spawns[candidate].position) <= maximumRegionRadius &&
				    Position::getDistanceY(spawns[seed].position, spawns[candidate].position) <= maximumRegionRadius) {
					assigned[candidate] = true;
					frontier.push_back(candidate);
				}
			}
		}

		PlayerBotHuntRegion region;
		region.floor = spawns[seed].position.z;
		std::map<std::string, PlayerBotHuntMonsterProfile> profiles;
		uint64_t xTotal = 0;
		uint64_t yTotal = 0;
		for (size_t member : members) {
			const SpawnBlockSnapshot& spawn = spawns[member];
			xTotal += spawn.position.x;
			yTotal += spawn.position.y;
			region.patrolPoints.push_back(nearestApproach(player, spawn.position));
			for (const auto& [monsterType, chance] : spawn.monsterTypes) {
				if (!monsterType || !monsterType->info.isHostile || !monsterType->info.isAttackable) {
					continue;
				}
				PlayerBotHuntMonsterProfile& profile = profiles[monsterType->name];
				profile.name = monsterType->name;
				profile.expectedSpawns += chance / 100.0;
				profile.experience = monsterType->info.experience;
				profile.health = monsterType->info.healthMax;
				profile.expectedDamagePerSecond = expectedMonsterDamagePerSecond(*monsterType, player);
				const double fightSeconds = monsterType->info.healthMax /
				                            expectedPlayerDamagePerSecond(player, *monsterType);
				profile.predictedFightDamage = profile.expectedDamagePerSecond * fightSeconds;
				region.experiencePerMinute += monsterType->info.experience * (chance / 100.0) *
				                              (60000.0 / std::max<uint32_t>(spawn.interval, 1));
			}
		}

		if (profiles.empty()) {
			continue;
		}
		double worstFightDamage = 0;
		for (size_t anchor : members) {
			struct LocalAttacker {
				double damagePerSecond;
				double fightSeconds;
			};
			std::vector<LocalAttacker> localAttackers;
			for (size_t neighbor = 0; neighbor < spawns.size(); ++neighbor) {
				if (!heatOverlaps(spawns[anchor].position, spawns[neighbor].position)) {
					continue;
				}
				for (const auto& [monsterType, chance] : spawns[neighbor].monsterTypes) {
					(void)chance;
					localAttackers.push_back({expectedMonsterDamagePerSecond(*monsterType, player),
					                          monsterType->info.healthMax /
					                              expectedPlayerDamagePerSecond(player, *monsterType)});
				}
			}
			std::sort(localAttackers.begin(), localAttackers.end(), [](const LocalAttacker& left, const LocalAttacker& right) {
				return left.damagePerSecond > right.damagePerSecond;
			});
			const size_t attackers = std::min<size_t>(3, localAttackers.size());
			double remainingDamagePerSecond = 0;
			for (size_t index = 0; index < attackers; ++index) {
				remainingDamagePerSecond += localAttackers[index].damagePerSecond;
			}
			double fightDamage = 0;
			for (size_t index = 0; index < attackers; ++index) {
				fightDamage += remainingDamagePerSecond * localAttackers[index].fightSeconds;
				remainingDamagePerSecond -= localAttackers[index].damagePerSecond;
			}
			worstFightDamage = std::max(worstFightDamage, fightDamage);
		}
		region.center = Position(static_cast<uint16_t>(xTotal / members.size()),
		                         static_cast<uint16_t>(yTotal / members.size()), region.floor);
		std::sort(region.patrolPoints.begin(), region.patrolPoints.end());
		region.patrolPoints.erase(std::unique(region.patrolPoints.begin(), region.patrolPoints.end()),
		                          region.patrolPoints.end());
		region.destination = *std::min_element(region.patrolPoints.begin(), region.patrolPoints.end(),
			[&player](const Position& left, const Position& right) {
				const uint32_t leftDistance = Position::getDistanceX(player.getPosition(), left) +
				                              Position::getDistanceY(player.getPosition(), left);
				const uint32_t rightDistance = Position::getDistanceX(player.getPosition(), right) +
				                               Position::getDistanceY(player.getPosition(), right);
				return leftDistance < rightDistance;
			});
		for (auto& [name, profile] : profiles) {
			(void)name;
			region.monsters.push_back(std::move(profile));
		}
		region.threatRatio = worstFightDamage / std::max<int32_t>(player.getMaxHealth(), 1);
		region.suitable = region.threatRatio <= maximumThreatRatio;
		if (excludedRegions.find(region.center) != excludedRegions.end()) {
			region.suitable = false;
			region.rejectionReason = "observed_danger_cooldown";
		} else if (!region.suitable) {
			region.rejectionReason = "predicted_damage";
		}
		region.experiencePerMinute *= g_config.getExperienceStage(player.getLevel());
		region.staminaMinutes = staminaMinutes;
		if (auto observed = performance.find(region.center); observed != performance.end()) {
			region.observedExperiencePerMinute = observed->second.observedExperiencePerMinute;
			region.observedCorrection = observed->second.correction;
		}
		const uint32_t geometricDistance = Position::getDistanceX(player.getPosition(), region.destination) +
		                                   Position::getDistanceY(player.getPosition(), region.destination) +
		                                   Position::getDistanceZ(player.getPosition(), region.destination) * 20;
		region.estimatedTravelSeconds = geometricDistance * player.getStepDuration() / 1000.0;
		region.availableHuntSeconds = std::max(0.0, huntDurationSeconds - region.estimatedTravelSeconds);
		region.staminaExperienceMultiplier = projectedStaminaExperienceMultiplier(player, region.availableHuntSeconds);
		region.projectedExperience = region.experiencePerMinute * region.observedCorrection *
		                             region.staminaExperienceMultiplier *
		                             region.availableHuntSeconds / 60.0;
		region.score = region.projectedExperience;
		regions.push_back(std::move(region));
	}

	std::sort(regions.begin(), regions.end(), [](const PlayerBotHuntRegion& left, const PlayerBotHuntRegion& right) {
		return left.score > right.score;
	});
	uint32_t regionId = 1;
	size_t reachabilityChecks = 0;
	uint64_t reachabilityExpandedNodes = 0;
	for (PlayerBotHuntRegion& region : regions) {
		region.id = regionId++;
		if (!region.suitable || reachabilityChecks >= maximumReachabilityChecks ||
		    reachabilityExpandedNodes >= maximumReachabilityExpandedNodes) {
			if (region.suitable) {
				region.rejectionReason = "reachability_budget";
			}
			continue;
		}
		++reachabilityChecks;
		std::deque<PlayerBotNavigationStep> route;
		std::set<Position> blocked;
		region.reachable = navigator.plan(player, region.destination, blocked, route, region.expandedNodes);
		reachabilityExpandedNodes += region.expandedNodes;
		if (!region.reachable) {
			region.rejectionReason = "unreachable";
			continue;
		}
		region.travelSteps = static_cast<uint32_t>(route.size());
		region.estimatedTravelSeconds = estimateTravelSeconds(player, route);
		region.availableHuntSeconds = std::max(0.0, huntDurationSeconds - region.estimatedTravelSeconds);
		region.staminaExperienceMultiplier = projectedStaminaExperienceMultiplier(player, region.availableHuntSeconds);
		region.projectedExperience = region.experiencePerMinute * region.observedCorrection *
		                             region.staminaExperienceMultiplier *
		                             region.availableHuntSeconds / 60.0;
		region.score = region.projectedExperience;
	}
	return regions;
}
