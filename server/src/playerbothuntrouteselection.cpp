#include "playerbothuntrouteselection.h"
#include "playerbotnavigation.h"

#include <algorithm>
#include <utility>

PlayerBotHuntRouteSelection::PlayerBotHuntRouteSelection(std::vector<PlayerBotHuntRegion> candidates) :
	candidates(std::move(candidates)) {}

PlayerBotHuntRouteRequest PlayerBotHuntRouteSelection::next()
{
	if (pending) return *pending;
	if (finished) return {};
	if (!current && stage == PlayerBotHuntRouteStage::Outbound) {
		index = playerBotNextHuntCandidateToValidate(candidates, index, best ? &*best : nullptr);
		if (index == candidates.size()) stage = PlayerBotHuntRouteStage::Done;
		else current = candidates[index];
	}
	PlayerBotHuntRouteRequest request;
	request.stage = stage;
	request.sequence = ++sequence;
	if (current) {
		request.outboundDangerCost = current->routeDangerCost;
		request.returnDangerCost = current->returnRouteDangerCost;
		switch (stage) {
		case PlayerBotHuntRouteStage::Outbound: request.to = current->destination; break;
		case PlayerBotHuntRouteStage::Depot:
			request.from = current->destination;
			request.routeAvailable = depotIndex < depots.size();
			if (request.routeAvailable) request.to = depots[depotIndex];
			break;
		case PlayerBotHuntRouteStage::DiscoverSupply: request.from = current->exitDepotDestination; break;
		case PlayerBotHuntRouteStage::Supplier:
			request.from = current->exitDepotDestination;
			request.to = suppliers[supplierIndex];
			break;
		default: break;
		}
	}
	pending = request;
	return request;
}

void PlayerBotHuntRouteSelection::advanceCandidate()
{
	++index;
	current.reset();
	depots.clear();
	depotIndex = 0;
	suppliers.clear();
	supplierIndex = 0;
	stage = PlayerBotHuntRouteStage::Outbound;
}

PlayerBotHuntRouteResult PlayerBotHuntRouteSelection::reject(PlayerBotHuntRegion candidate)
{
	candidate.suitable = false;
	++failureCounts[candidate.rejectionReason.empty() ? "unspecified" : candidate.rejectionReason];
	rejectedVariants.push_back(candidate.atlasVariantId);
	PlayerBotHuntRouteResult result;
	result.accepted = result.yield = true;
	result.completedCandidate = std::move(candidate);
	advanceCandidate();
	return result;
}

PlayerBotHuntRouteResult PlayerBotHuntRouteSelection::finish(PlayerBotHuntRegion candidate)
{
	PlayerBotHuntRouteResult result;
	result.accepted = true;
	result.completedCandidate = candidate;
	if (!best || playerBotPreferHuntRegion(candidate, *best)) best = std::move(candidate);
	advanceCandidate();
	index = playerBotNextHuntCandidateToValidate(candidates, index, &*best);
	if (index == candidates.size()) {
		stage = PlayerBotHuntRouteStage::Done;
		result.terminal = true;
		result.selectedRouteRegion = best;
		result.failureCounts = failureCounts;
		result.rejectedVariants = rejectedVariants;
		finished = true;
	}
	result.yield = !result.terminal;
	return result;
}

PlayerBotHuntRouteResult PlayerBotHuntRouteSelection::observe(const PlayerBotHuntRouteRequest& request,
    const PlayerBotHuntRouteObservation& observation)
{
	if (!pending || request.sequence != pending->sequence || request.stage != pending->stage) return {};
	pending.reset();
	if (stage == PlayerBotHuntRouteStage::Done) {
		PlayerBotHuntRouteResult result;
		result.accepted = true;
		result.terminal = true;
		result.selectedRouteRegion = best;
		result.failureCounts = failureCounts;
		result.rejectedVariants = rejectedVariants;
		finished = true;
		return result;
	}
	auto continuation = [](bool yield) {
		PlayerBotHuntRouteResult result;
		result.accepted = true;
		result.yield = yield;
		return result;
	};
	PlayerBotHuntRegion routed = *current;
	const PlayerBotNavigationRiskProfile risk;
	const bool safe = observation.reached &&
	    playerBotNavigationRiskAccepts(risk, observation.dangerCost, observation.peakDanger);
	switch (stage) {
	case PlayerBotHuntRouteStage::Outbound:
		routed.travelSteps = observation.steps;
		routed.outboundFare = observation.fare;
		routed.outboundNpcTravel = observation.npcTravel;
		routed.routeDangerCost = observation.dangerCost;
		routed.maximumRouteDanger = observation.peakDanger;
		if (observation.travelSeconds > 0)
			routed.reconcileTravel(observation.huntDurationSeconds, observation.travelSeconds,
		        observation.staminaMultiplier);
		if (!observation.reached) routed.rejectionReason = "route_unreachable";
		else if (!safe) routed.rejectionReason = observation.dangerCost >
		    static_cast<uint32_t>(risk.maximumRouteHealthLoss * risk.healthLossCost) ?
		    "route_danger_above_tolerance" : "route_peak_danger_above_tolerance";
		if (!routed.suitable || !safe) return reject(std::move(routed));
		current = std::move(routed);
		depots = observation.approaches;
		stage = PlayerBotHuntRouteStage::Depot;
		return continuation(true);
	case PlayerBotHuntRouteStage::Depot: {
		if (depots.empty()) {
			routed.rejectionReason = "safe_depot_exit_unavailable";
			return reject(std::move(routed));
		}
		if (!safe) {
			if (++depotIndex == depots.size()) {
				routed.rejectionReason = "safe_depot_exit_unavailable";
				return reject(std::move(routed));
			}
			return continuation(true);
		}
		routed.routeValidated = true;
		routed.returnRouteDangerCost = observation.dangerCost;
		routed.exitDepotDestination = request.to;
		routed.exitFare = observation.fare;
		routed.exitNpcTravel = observation.npcTravel;
		routed.supplyProfile = observation.supplyProfile;
		routed.reconcileRecovery(observation.potionReserve, observation.funds);
		current = std::move(routed);
		// Retain the depot-time decision across the final live supply refresh.
		const bool needsSupplyRoute = !current->supplyRecovery && playerBotHuntNeedsSupplyRoute(
		    current->supplyBudget.expectedPotions, current->supplyProfile.potions, observation.potionReserve);
		stage = needsSupplyRoute ? PlayerBotHuntRouteStage::DiscoverSupply : PlayerBotHuntRouteStage::Final;
		return continuation(false);
	}
	case PlayerBotHuntRouteStage::DiscoverSupply:
		suppliers = observation.approaches;
		if (suppliers.empty()) {
			routed.rejectionReason = "recovery_supply_route_unavailable";
			return reject(std::move(routed));
		}
		stage = PlayerBotHuntRouteStage::Supplier;
		return continuation(true);
	case PlayerBotHuntRouteStage::Supplier:
		if (!safe) {
			++supplierIndex;
			if (supplierIndex == suppliers.size()) {
				stage = PlayerBotHuntRouteStage::RejectSupply;
			}
			return continuation(true);
		}
		routed.supplyDestination = request.to;
		routed.supplyFare = observation.fare;
		routed.supplyNpcTravel = observation.npcTravel;
		current = std::move(routed);
		stage = PlayerBotHuntRouteStage::Final;
		return continuation(false);
	case PlayerBotHuntRouteStage::RejectSupply:
		routed.rejectionReason = "recovery_supply_route_unavailable";
		return reject(std::move(routed));
	case PlayerBotHuntRouteStage::Final:
		routed.supplyProfile = observation.supplyProfile;
		routed.recoveryPotionReserve = observation.potionReserve;
		routed.recoveryRouteHealthLoss = observation.recoveryRouteHealthLoss;
		routed.reconcileRecovery(observation.potionReserve, observation.funds);
		if (!routed.recoverySustainable()) routed.rejectionReason = "recovery_hunt_not_sustainable";
		else if (!playerBotHuntTravelAffordable(observation.funds, observation.recoverySpendingReserve,
		    routed.outboundFare, routed.exitFare, routed.supplyFare))
			routed.rejectionReason = "travel_fare_breaks_recovery_reserve";
		if (!routed.rejectionReason.empty()) return reject(std::move(routed));
		return finish(std::move(routed));
	default: return {};
	}
}
