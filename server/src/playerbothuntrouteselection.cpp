#include "playerbothuntrouteselection.h"

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
		case PlayerBotHuntRouteStage::DiscoverSupply:
			request.from = current->exitDepotDestination;
			for (const auto& budget : current->supplyKindBudgets) {
				if (budget.expected <= budget.routine) continue;
				const auto kind = std::find_if(current->supplyProfile.kinds.begin(), current->supplyProfile.kinds.end(),
				    [&](const auto& profile) { return profile.kind == budget.kind; });
				if (kind != current->supplyProfile.kinds.end() && kind->itemId != 0)
					request.requiredSupplyItems.push_back(kind->itemId);
			}
			break;
		case PlayerBotHuntRouteStage::Supplier:
			request.from = supplySource;
			request.to = suppliers[supplierIndex];
			request.supplyItemId = supplyApproachGroups[supplyGroupIndex].itemId;
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
	supplyApproachGroups.clear();
	supplyGroupIndex = 0;
	searchIncomplete = false;
	stage = PlayerBotHuntRouteStage::Outbound;
}

PlayerBotHuntRouteResult PlayerBotHuntRouteSelection::reject(PlayerBotHuntRegion candidate)
{
	candidate.suitable = false;
	++failureCounts[candidate.rejectionReason.empty() ? "unspecified" : candidate.rejectionReason];
	// An exhausted search can be retried on the next planning pass. Do not
	// turn missing evidence into the ordinary unreachable-region cooldown.
	if (!searchIncomplete) rejectedVariants.push_back(candidate.atlasVariantId);
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
	// Live stock snapshots may omit learned demand. Keep the scored estimate
	// for the same kind/item while refreshing counts and return thresholds.
	auto refreshSupply = [&routed](PlayerBotSupplyProfile profile) {
		for (PlayerBotSupplyKindProfile& kind : profile.kinds) {
			if (kind.demand.samples != 0 || kind.demand.unitsPerCombatSecond != 0) continue;
			const auto prior = std::find_if(routed.supplyProfile.kinds.begin(), routed.supplyProfile.kinds.end(),
			    [&kind](const PlayerBotSupplyKindProfile& previous) {
				    return previous.kind == kind.kind && previous.itemId == kind.itemId;
			    });
			if (prior != routed.supplyProfile.kinds.end()) kind.demand = prior->demand;
		}
		routed.supplyProfile = std::move(profile);
	};
	searchIncomplete = searchIncomplete || observation.searchIncomplete;
	const auto& risk = observation.riskProfile;
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
		if (!observation.reached) routed.rejectionReason = searchIncomplete ? "route_search_incomplete" : "route_unreachable";
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
				routed.rejectionReason = searchIncomplete ? "depot_route_search_incomplete" : "safe_depot_exit_unavailable";
				return reject(std::move(routed));
			}
			return continuation(true);
		}
		routed.routeValidated = true;
		routed.returnRouteDangerCost = observation.dangerCost;
		routed.exitDepotDestination = request.to;
		routed.exitFare = observation.fare;
		routed.exitNpcTravel = observation.npcTravel;
		refreshSupply(observation.supplyProfile);
		routed.reconcileRecovery(observation.potionReserve, observation.funds);
		current = std::move(routed);
		// Retain the depot-time decision across the final live supply refresh.
		const bool typedDeficit = std::any_of(current->supplyKindBudgets.begin(), current->supplyKindBudgets.end(),
		    [](const PlayerBotSupplyKindBudget& budget) { return budget.expected > budget.routine; });
		const bool needsSupplyRoute = !current->supplyRecovery && (typedDeficit || playerBotHuntNeedsSupplyRoute(
		    current->supplyBudget.expectedPotions, current->supplyProfile.potions, observation.potionReserve));
		stage = needsSupplyRoute ? PlayerBotHuntRouteStage::DiscoverSupply : PlayerBotHuntRouteStage::Final;
		return continuation(false);
	}
	case PlayerBotHuntRouteStage::DiscoverSupply:
		supplyApproachGroups = observation.supplyApproachGroups;
		if (supplyApproachGroups.empty()) supplyApproachGroups.push_back({0, observation.approaches});
		supplyGroupIndex = supplierIndex = 0;
		supplySource = current->exitDepotDestination;
		suppliers = supplyApproachGroups.front().approaches;
		if (std::any_of(supplyApproachGroups.begin(), supplyApproachGroups.end(),
		    [](const auto& group) { return group.approaches.empty(); })) {
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
		routed.supplyFare = routed.supplyFare > UINT64_MAX - observation.fare ? UINT64_MAX :
		    routed.supplyFare + observation.fare;
		routed.supplyNpcTravel = routed.supplyNpcTravel || observation.npcTravel;
		current = std::move(routed);
		if (++supplyGroupIndex < supplyApproachGroups.size()) {
			// Validate a continuous service itinerary, not independent depot trips.
			supplySource = request.to;
			suppliers = supplyApproachGroups[supplyGroupIndex].approaches;
			supplierIndex = 0;
			return continuation(true);
		}
		stage = PlayerBotHuntRouteStage::Final;
		return continuation(false);
	case PlayerBotHuntRouteStage::RejectSupply:
		routed.rejectionReason = searchIncomplete ? "supply_route_search_incomplete" : "recovery_supply_route_unavailable";
		return reject(std::move(routed));
	case PlayerBotHuntRouteStage::Final:
		refreshSupply(observation.supplyProfile);
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
