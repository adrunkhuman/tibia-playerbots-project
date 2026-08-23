/** Engine adapter for hunt-region observations and cache access. */
#ifndef FS_PLAYERBOTHUNTREGIONADAPTER_H
#define FS_PLAYERBOTHUNTREGIONADAPTER_H

#include "playerbothuntregions.h"

class PlayerBotHuntRegionAdapter
{
	public:
		static void invalidateCache();
		static uint64_t getCacheRevision();
		static PlayerBotHuntRegionScan beginScan(const Player& player);
		static PlayerBotHuntRegionScore score(Player& player, const PlayerBotHuntPlanningProfile& profile,
		                                      uint64_t revision, size_t candidateIndex,
		                                      const std::set<Position>& excludedRegions,
		                                      const std::map<Position, PlayerBotHuntRegionPerformance>& performance,
		                                      uint32_t huntDurationSeconds);
		static PlayerBotHuntPlanningProfile planningProfile(const Player& player, const PlayerBotCombatProfile& combat,
		                                                     double challengeFrontier);
};

#endif
