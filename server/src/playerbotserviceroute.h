#ifndef FS_PLAYERBOTSERVICEROUTE_H
#define FS_PLAYERBOTSERVICEROUTE_H

#include "playerbothuntrouteselection.h"
#include "playerbotnavigationruntime.h"
#include "playerbotserviceworkflow.h"

// Service execution uses the shared sliced router even when the quote excludes
// NPC travel. Keep that restriction in the request, not in a planner fallback.
inline PlayerBotHuntRouteRequest playerBotServiceRouteRequest(
    Position from, Position to, uint64_t pass, bool preparationPurchase,
    const PlayerBotServiceLiquidationPlan* quote, const std::set<Position>& blockedPositions)
{
	PlayerBotHuntRouteRequest request;
	request.planningPass = pass;
	request.from = from;
	request.to = to;
	request.walkingOnly = preparationPurchase || (quote && !quote->allowNpcTravel);
	request.preferSafeWalking = playerBotNavigationDistance(from, to) <= 128;
	request.blockedPositions = blockedPositions;
	return request;
}

inline bool playerBotServiceRouteQuoteAllows(const PlayerBotServiceLiquidationPlan* quote,
    const PlayerBotNavigationPlanMetrics& metrics, bool requiresNpcTravel)
{
	return !quote || ((quote->allowNpcTravel || !requiresNpcTravel) && metrics.fare <= quote->maximumFare &&
	    (quote->maximumRouteSteps == 0 || metrics.steps <= quote->maximumRouteSteps));
}

#endif
