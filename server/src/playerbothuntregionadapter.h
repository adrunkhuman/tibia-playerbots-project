/** Engine adapter for hunt-region observations and cache access. */
#ifndef FS_PLAYERBOTHUNTREGIONADAPTER_H
#define FS_PLAYERBOTHUNTREGIONADAPTER_H

#include "playerbothuntregions.h"

// The cache is shared across bots and is not synchronized; use this adapter
// only from the server's serialized game execution context. Candidate indices
// returned by beginScan() are valid only for that scan's revision. score()
// returns valid=false when the revision changed or the index is no longer
// valid, and the caller must discard and restart that plan. Spawn-generation
// changes invalidate the cache lazily when its revision is queried or a score
// is requested; beginScan() rebuilds a stale cache before returning candidates.
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
