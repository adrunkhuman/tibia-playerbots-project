#ifndef FS_PLAYERBOTHUNTCOARSEPOLICY_H
#define FS_PLAYERBOTHUNTCOARSEPOLICY_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

enum class PlayerBotHuntCoarseVerdict { Reachable, Disconnected, Unknown };

inline bool playerBotHuntCoarseEvidenceStale(
    const std::optional<std::pair<uint64_t, uint64_t>>& evidence, uint64_t revision, uint64_t epoch)
{
	return evidence && *evidence != std::make_pair(revision, epoch);
}

// Only a complete answer from the topology can prove disconnection. A goal
// outside the graph may still be reachable through live map state.
template <typename Position, typename Known, typename Reachable>
PlayerBotHuntCoarseVerdict playerBotHuntCoarseVerdict(
    bool connectivityUnchanged, const Position& source, const std::vector<Position>& goals,
    Known known, Reachable reachable)
{
	if (!known(source) || goals.empty()) return PlayerBotHuntCoarseVerdict::Unknown;
	bool allGoalsKnown = true;
	for (const Position& goal : goals) {
		if (!known(goal)) allGoalsKnown = false;
		else if (reachable(goal)) return PlayerBotHuntCoarseVerdict::Reachable;
	}
	return allGoalsKnown && connectivityUnchanged ? PlayerBotHuntCoarseVerdict::Disconnected :
	       PlayerBotHuntCoarseVerdict::Unknown;
}

#endif
