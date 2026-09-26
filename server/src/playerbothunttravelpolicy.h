#ifndef FS_PLAYERBOTHUNTTRAVELPOLICY_H
#define FS_PLAYERBOTHUNTTRAVELPOLICY_H

#include "playerbotnavigation.h"

#include <map>

// A volatile world cannot be searched indefinitely under one selector request.
// The retry count survives reconstruction of the request-local walking frontier.
// On exhaustion the caller must report unknown, not unreachable or a stale route.
class PlayerBotHuntRequestRestartBudget {
public:
	bool retry() { return restarts++ < maximumRestarts; }
	static constexpr uint32_t maximumRestarts = 2;
private:
	uint32_t restarts = 0;
};

// Every paid hunt journey must retain at least this recovery reserve at final
// acceptance. Free transport remains legal even below the reserve. Apply the
// necessary condition before doing graph/path work, not after validating all legs.
inline uint64_t playerBotHuntTransportFunds(uint64_t funds, uint64_t minimumRecoveryReserve)
{
	return funds > minimumRecoveryReserve ? funds - minimumRecoveryReserve : 0;
}

// Provider movement matters only after a connection has read its anchor. An
// unvisited provider can move arbitrarily without invalidating any search work.
// Used anchors stay fixed: repeated small moves cannot extend talking range.
class PlayerBotHuntProviderValidation {
public:
	// Rebuild anchors only; the retry budget belongs to the pending request.
	void reset(const std::map<uint32_t, Position>& live) {
		providers.clear();
		for (const auto& [id, position] : live) providers.emplace(id, Provider{position, false});
	}
	bool update(const std::map<uint32_t, Position>& live) {
		for (auto& [id, provider] : providers) {
			const auto found = live.find(id);
			if (found == live.end()) return false;
			if (provider.used) {
				if (!playerBotNpcTravelUsesLocalApproach(provider.anchor, found->second, 2)) return false;
			} else provider.anchor = found->second;
		}
		return true;
	}
	Position use(uint32_t id) {
		auto& provider = providers.at(id);
		provider.used = true;
		return provider.anchor;
	}
	// Rebuild dependent transport labels, not the independent walking search.
	// Preserve the request's spent node allowance across these restarts.
	bool restartAllowed() { return restarts++ < maximumRestarts; }
	static constexpr uint32_t maximumRestarts = 2;
private:
	struct Provider { Position anchor; bool used; };
	std::map<uint32_t, Provider> providers;
	uint32_t restarts = 0;
};

// A reached but unsafe walk does not disprove safe transport. Apply this after
// choosing the best available route, so a safe paid/walking result still wins.
inline PlayerBotNavigationResult playerBotHuntAggregateRouteResult(
    PlayerBotNavigationResult result, uint32_t danger, double peak, bool transportIncomplete)
{
	const bool safe = result == PlayerBotNavigationResult::Reached &&
	    playerBotNavigationRiskAccepts({}, danger, peak);
	return !safe && transportIncomplete ? PlayerBotNavigationResult::NodeLimit : result;
}

#endif
