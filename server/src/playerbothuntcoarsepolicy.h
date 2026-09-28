#ifndef FS_PLAYERBOTHUNTCOARSEPOLICY_H
#define FS_PLAYERBOTHUNTCOARSEPOLICY_H

#include <vector>

enum class PlayerBotHuntCoarseVerdict { Reachable, Disconnected, Unknown };

// Only a complete answer from the topology can prove disconnection. A goal
// outside the graph may still be reachable through live map state.
template <typename Position, typename Known, typename Reachable>
PlayerBotHuntCoarseVerdict playerBotHuntCoarseVerdict(
    const Position& source, const std::vector<Position>& goals, Known known, Reachable reachable)
{
	if (!known(source) || goals.empty()) return PlayerBotHuntCoarseVerdict::Unknown;
	bool allGoalsKnown = true;
	for (const Position& goal : goals) {
		if (!known(goal)) allGoalsKnown = false;
		else if (reachable(goal)) return PlayerBotHuntCoarseVerdict::Reachable;
	}
	return allGoalsKnown ? PlayerBotHuntCoarseVerdict::Disconnected : PlayerBotHuntCoarseVerdict::Unknown;
}

#endif
