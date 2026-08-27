/** Pure candidate selection for playerbot progression. */
#include "otpch.h"

#include "playerbotprogressionplanners.h"

#include <algorithm>
#include <limits>
#include <set>

bool PlayerBotDeparturePlanner::hasCompleted(const PlayerBotDeparturePlannerSnapshot& snapshot) const
{
	return snapshot.vocationId != 0 && snapshot.townId != 0 && snapshot.townId != snapshot.rookgaardTownId;
}

bool PlayerBotDeparturePlanner::required(const PlayerBotDeparturePlannerSnapshot& snapshot) const
{
	return snapshot.vocationId == 0 && snapshot.level >= snapshot.minimumLevel;
}

std::optional<PlayerBotOracleDeparturePlan> PlayerBotDeparturePlanner::select(const PlayerBotDeparturePlannerSnapshot& snapshot) const
{
	if (snapshot.vocationId != 0 || snapshot.level < snapshot.minimumLevel || snapshot.level > snapshot.maximumLevel) return std::nullopt;
	for (const auto& provider : snapshot.providers) {
		if (!provider.route.reachable) continue;
		return PlayerBotOracleDeparturePlan{provider.npcId, provider.npcPosition, provider.route.approachPosition,
		                                     provider.route.steps, provider.route.expandedNodes};
	}
	return std::nullopt;
}

PlayerBotSpellTrainingDecision PlayerBotSpellTrainingPlanner::select(const PlayerBotSpellTrainingPlannerSnapshot& snapshot) const
{
	PlayerBotSpellTrainingDecision decision;
	for (size_t offerIndex = 0; offerIndex < snapshot.offers.size(); ++offerIndex) {
		const auto& offer = snapshot.offers[offerIndex];
		const char* rejection = !offer.inScope ? "outside_thais_scope" : !offer.registryMatches ? "spell_registry_mismatch" :
		                        !offer.vocationEligible ? "vocation_ineligible" : !offer.levelEligible ? "level_ineligible" :
		                        !offer.premiumEligible ? "premium_ineligible" : offer.known ? "already_learned" :
		                        !offer.suppliesReady ? "supply_reserve_unmet" : !snapshot.reserveAvailable ? "recovery_reserve_unavailable" :
		                        snapshot.totalMoney < snapshot.reserve + offer.price ? "unaffordable_after_reserves" :
		                        !offer.route.reachable ? "trainer_unreachable" :
		                        offer.route.dangerCost > snapshot.maximumRouteDangerCost ? "route_danger_above_tolerance" :
		                        offer.route.maximumDanger > snapshot.maximumRouteDanger ? "route_peak_danger_above_tolerance" : nullptr;
		if (rejection) {
			decision.rejections.push_back({offerIndex, rejection});
			continue;
		}
		PlayerBotSpellTrainingPlan candidate{offer.npcId, offer.npcPosition, offer.route.approachPosition, offer.spellName,
		                                    offer.keyword, offer.price, offer.level, offer.premium, offer.route.steps, snapshot.reserve};
		if (!decision.selected || candidate.price < decision.selected->price ||
		    (candidate.price == decision.selected->price && (candidate.travelSteps < decision.selected->travelSteps ||
		     (candidate.travelSteps == decision.selected->travelSteps && candidate.spellName < decision.selected->spellName)))) {
			decision.selected = std::move(candidate);
			decision.selectedOfferIndex = offerIndex;
		}
	}
	return decision;
}

PlayerBotEquipmentProviderDecision PlayerBotEquipmentProviderPlanner::select(const PlayerBotEquipmentProviderPlannerSnapshot& snapshot) const
{
	PlayerBotEquipmentProviderDecision decision;
	if (!snapshot.enabled) return decision;
	decision.evaluated = true;
	for (size_t offerIndex = 0; offerIndex < snapshot.offers.size(); ++offerIndex) {
		const auto& offer = snapshot.offers[offerIndex];
		PlayerBotEquipmentOfferEvaluation evaluation = offer.evaluation;
		const char* rejection = !evaluation.rejection.empty() ? evaluation.rejection.c_str() :
		                        !evaluation.carried && offer.itemWeight > snapshot.freeCapacity ? "insufficient_capacity" : nullptr;
		uint32_t displacedSlots = 0;
		std::set<slots_t> countedSlots;
		for (const auto& displaced : {std::pair<slots_t, uint16_t>{evaluation.slot, evaluation.replacedItemId},
		                              {CONST_SLOT_LEFT, evaluation.displacedLeftItemId}, {CONST_SLOT_RIGHT, evaluation.displacedRightItemId}}) {
			if (displaced.second != 0 && countedSlots.insert(displaced.first).second) ++displacedSlots;
		}
		const uint32_t requiredSlots = displacedSlots + (evaluation.carried ? 0 : 1);
		if (!rejection && (!offer.backpackAvailable || offer.freeBackpackSlots < requiredSlots)) rejection = "insufficient_displaced_item_space";
		if (!rejection && !evaluation.carried && !offer.purchaseAvailable) rejection = "offer_not_for_sale";
		if (!rejection && !evaluation.carried && !snapshot.reserveAvailable) rejection = "recovery_reserve_unavailable";
		if (!rejection && !evaluation.carried && snapshot.totalMoney < snapshot.reserve + evaluation.price) rejection = "unaffordable_after_reserves";
		if (!rejection && !evaluation.carried && !offer.route.reachable) rejection = offer.routeBudgetExhausted ? "provider_evaluation_budget_exhausted" :
		                                                               offer.route.nodeLimitReached ? "provider_route_node_budget_exhausted" : "provider_unreachable";
		if (!rejection && !evaluation.carried && offer.route.dangerCost > snapshot.maximumRouteDangerCost) rejection = "route_danger_above_tolerance";
		if (!rejection && !evaluation.carried && offer.route.maximumDanger > snapshot.maximumRouteDanger) rejection = "route_peak_danger_above_tolerance";
		if (rejection) {
			decision.rejections.push_back({offerIndex, rejection});
			continue;
		}
		if (!evaluation.carried) {
			evaluation.approachPosition = offer.route.approachPosition;
			evaluation.travelSteps = offer.route.steps;
		} else evaluation.travelSteps = 0;
		if (!decision.selected || PlayerBotEquipmentPolicy::prefers(evaluation, *decision.selected)) {
			decision.selected = std::move(evaluation);
			decision.selectedOfferIndex = offerIndex;
		}
	}
	return decision;
}

PlayerBotRewardInspection PlayerBotRewardPlanner::inspect(const PlayerBotRewardInspectionSnapshot& snapshot,
	                                                        const PlayerBotRewardInspectionContext& context) const
{
	PlayerBotRewardInspection inspection;
	inspection.rootItemIds = snapshot.rootItemIds;
	inspection.rootSignatures = snapshot.rootSignatures;
	inspection.nonStackableRootSignatures = snapshot.nonStackableRootSignatures;
	inspection.stackableRootCounts = snapshot.stackableRootCounts;
	for (const PlayerBotRewardItemObservation& item : snapshot.items) {
		PlayerBotRewardItemInspection inspected{item.itemId, item.count, item.depth, item.rootOrdinal, item.path};
		++inspection.itemCount;
		if (item.container) {
			inspected.classes.emplace_back("container");
			++inspection.containerCount;
		}
		const PlayerBotEquipmentOfferEvaluation equipment = item.equipment.value_or(PlayerBotEquipmentOfferEvaluation{});
		if (item.equipmentCandidate && item.equipment && equipment.rejection.empty()) {
		const PlayerBotEquipmentUpgrade upgrade{equipment.slot, std::max(1, item.candidateValue - item.currentValue), item.metric,
		                                        item.currentValue, item.candidateValue};
		inspected.classes.emplace_back("equipment_upgrade");
		++inspection.equipmentUpgradeCount;
		if (!inspection.bestEquipment || equipment.rule > inspection.bestEquipment->rule ||
		    (equipment.rule == inspection.bestEquipment->rule && upgrade.benefit > inspection.bestUpgrade->benefit)) {
			inspection.bestUpgrade = upgrade;
			inspection.bestEquipment = equipment;
			inspection.bestItemId = item.itemId;
			inspection.bestRootOrdinal = item.rootOrdinal;
			inspection.bestItemPath = item.path;
			inspection.bestRootSignature = item.rootSignature;
		}
		} else if (item.equipmentCandidate && item.equipment && inspection.equipmentRejection.empty()) {
		inspection.equipmentRejection = equipment.rejection;
		}

		inspected.worth = item.worth;
	if (inspected.worth != 0) {
		inspected.classes.emplace_back("currency");
		inspection.currencyValue += inspected.worth;
	}
	if (item.potion) {
		inspected.classes.emplace_back("required_supply");
		inspection.potionCount += item.count;
	} else if (item.food) {
		inspected.classes.emplace_back("food");
		inspection.foodCount += item.count;
	} else if (item.rope) {
		inspected.classes.emplace_back("tool");
		inspection.ropeCount += item.count;
	} else if (item.shovel) {
		inspected.classes.emplace_back("tool");
		inspection.shovelCount += item.count;
	}
	if (item.sellValue != 0) {
		inspected.classes.emplace_back("sellable");
		inspected.sellValue = item.sellValue;
		inspection.sellValue += inspected.sellValue;
	}
	uint32_t itemUtility = inspected.worth + inspected.sellValue;
	if (item.potion) itemUtility += context.missingPotionUtility * item.count;
	else if (item.food) itemUtility += context.foodPreferenceUtility * item.count;
	else if (item.rope || item.shovel) itemUtility += 100;
	if (itemUtility > inspection.primaryKnownItemUtility) {
		inspection.primaryKnownItemId = item.itemId;
		inspection.primaryKnownRootOrdinal = item.rootOrdinal;
		inspection.primaryKnownRootSignature = item.rootSignature;
		inspection.primaryKnownItemUtility = itemUtility;
	}
	if (inspected.classes.empty()) {
		inspected.classes.emplace_back("unknown_keep");
		++inspection.unknownCount;
	}
	inspection.items.push_back(std::move(inspected));
	}
	finalizeInspection(context, inspection);
	return inspection;
}

void PlayerBotRewardPlanner::finalizeInspection(const PlayerBotRewardInspectionContext& context,
	                                                PlayerBotRewardInspection& inspection) const
{
	if (inspection.bestUpgrade) inspection.knownUtility += inspection.bestUpgrade->benefit * 20;
	inspection.knownUtility += static_cast<int32_t>(inspection.currencyValue + inspection.sellValue);
	const uint32_t potionNeed = context.heldPotions < context.potionRestockTarget ? context.potionRestockTarget - context.heldPotions : 0;
	const uint32_t foodNeed = context.heldFood < context.preferredFoodCount ? context.preferredFoodCount - context.heldFood : 0;
	inspection.knownUtility += static_cast<int32_t>(std::min(inspection.potionCount, potionNeed) * context.missingPotionUtility +
	                                                  std::min(inspection.foodCount, foodNeed) * context.foodPreferenceUtility);
	if (inspection.ropeCount != 0 && !context.ownsRope) inspection.knownUtility += 100;
	if (inspection.shovelCount != 0 && !context.ownsShovel) inspection.knownUtility += 100;
}

std::optional<PlayerBotRewardPlan> PlayerBotRewardPlanner::plan(uint16_t uniqueId, const Position& itemPosition,
	                                                                uint32_t estimatedDistance,
	                                                                const PlayerBotRewardInspection& inspection) const
{
	if (inspection.knownUtility <= 0 || inspection.rootSignatures.empty()) return std::nullopt;
	PlayerBotRewardPlan candidate;
	candidate.uniqueId = uniqueId;
	candidate.rootOrdinal = inspection.bestUpgrade ? inspection.bestRootOrdinal : inspection.primaryKnownRootOrdinal;
	if (candidate.rootOrdinal >= inspection.rootItemIds.size()) return std::nullopt;
	candidate.rootItemId = inspection.rootItemIds[candidate.rootOrdinal];
	candidate.itemId = inspection.bestUpgrade ? inspection.bestItemId : inspection.primaryKnownItemId;
	candidate.itemPosition = itemPosition;
	if (inspection.bestUpgrade) {
		candidate.slot = inspection.bestUpgrade->slot;
		candidate.benefit = inspection.bestUpgrade->benefit;
		candidate.metric = inspection.bestUpgrade->metric;
		candidate.currentValue = inspection.bestUpgrade->currentValue;
		candidate.candidateValue = inspection.bestUpgrade->candidateValue;
		candidate.replacedItemId = inspection.bestEquipment->replacedItemId;
		candidate.displacedLeftItemId = inspection.bestEquipment->displacedLeftItemId;
		candidate.displacedRightItemId = inspection.bestEquipment->displacedRightItemId;
	}
	candidate.knownUtility = inspection.knownUtility;
	candidate.itemCount = inspection.itemCount;
	candidate.containerCount = inspection.containerCount;
	candidate.unknownCount = inspection.unknownCount;
	candidate.currencyValue = inspection.currencyValue;
	candidate.sellValue = inspection.sellValue;
	candidate.equipmentUpgradeCount = inspection.equipmentUpgradeCount;
	candidate.selectedItemPath = inspection.bestItemPath;
	candidate.rootSignature = inspection.bestUpgrade ? inspection.bestRootSignature : inspection.primaryKnownRootSignature;
	candidate.rootSignatures = inspection.rootSignatures;
	candidate.nonStackableRootSignatures = inspection.nonStackableRootSignatures;
	candidate.stackableRootCounts = inspection.stackableRootCounts;
	candidate.estimatedDistance = estimatedDistance;
	std::set<slots_t> displacedSlots;
	if (inspection.bestEquipment) {
		for (const auto& displaced : {std::pair<slots_t, uint16_t>{candidate.slot, candidate.replacedItemId},
		                              {CONST_SLOT_LEFT, candidate.displacedLeftItemId}, {CONST_SLOT_RIGHT, candidate.displacedRightItemId}}) {
			if (displaced.second != 0) displacedSlots.insert(displaced.first);
		}
	}
	candidate.requiredBackpackSlots = static_cast<uint32_t>(inspection.rootItemIds.size() + displacedSlots.size());
	return candidate;
}

int32_t PlayerBotRewardPlanner::utility(const PlayerBotRewardPlan& plan, const PlayerBotRewardPlannerSnapshot& snapshot) const
{
	const int32_t base = plan.equipmentUpgradeCount != 0 ? snapshot.pickupBaseUtility : snapshot.economicBaseUtility;
	return std::max<int32_t>(0, base + static_cast<int32_t>(plan.knownUtility) -
	    static_cast<int32_t>(plan.travelSteps) - static_cast<int32_t>(plan.travelDangerCost / 10));
}

int32_t PlayerBotRewardPlanner::estimatedUtility(const PlayerBotRewardPlan& plan,
	                                                const PlayerBotRewardPlannerSnapshot& snapshot) const
{
	const int32_t base = plan.equipmentUpgradeCount != 0 ? snapshot.pickupBaseUtility : snapshot.economicBaseUtility;
	return std::max<int32_t>(0, base + static_cast<int32_t>(plan.knownUtility) - static_cast<int32_t>(plan.estimatedDistance));
}

std::vector<size_t> PlayerBotRewardPlanner::routeCandidates(const PlayerBotRewardPlannerSnapshot& snapshot) const
{
	std::vector<size_t> candidates;
	for (size_t index = 0; index < snapshot.candidates.size(); ++index) {
		if (!snapshot.candidates[index].claimed) candidates.push_back(index);
	}
	std::sort(candidates.begin(), candidates.end(), [this, &snapshot](size_t left, size_t right) {
		const PlayerBotRewardPlan& leftPlan = snapshot.candidates[left].plan;
		const PlayerBotRewardPlan& rightPlan = snapshot.candidates[right].plan;
		const int32_t leftUtility = estimatedUtility(leftPlan, snapshot);
		const int32_t rightUtility = estimatedUtility(rightPlan, snapshot);
		if (leftUtility != rightUtility) return leftUtility > rightUtility;
		if (leftPlan.estimatedDistance != rightPlan.estimatedDistance) return leftPlan.estimatedDistance < rightPlan.estimatedDistance;
		return leftPlan.uniqueId < rightPlan.uniqueId;
	});
	candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&snapshot, this](size_t index) {
		const PlayerBotRewardCandidateSnapshot& source = snapshot.candidates[index];
		return source.totalWeight > snapshot.freeCapacity || !source.backpackAvailable ||
		       source.freeBackpackSlots < source.plan.requiredBackpackSlots ||
		       estimatedUtility(source.plan, snapshot) <= snapshot.huntUtility;
	}), candidates.end());
	return candidates;
}

PlayerBotRewardDecision PlayerBotRewardPlanner::select(const PlayerBotRewardPlannerSnapshot& snapshot) const
{
	PlayerBotRewardDecision decision;
	for (const auto& source : snapshot.candidates) {
		if (!source.claimed) continue;
		if (!source.ownedUpgrade) {
			decision.outcomes.push_back({source.plan, "rejected", "claimed_reward_missing"});
			continue;
		}
		PlayerBotRewardPlan candidate = source.plan;
		candidate.travelSteps = 0;
		candidate.resumeEquipment = true;
		decision.outcomes.push_back({candidate, "feasible", "claimed_reward_owned"});
		if (!decision.selected || candidate.benefit > decision.selected->benefit ||
		    (candidate.benefit == decision.selected->benefit && candidate.uniqueId < decision.selected->uniqueId)) {
			decision.selected = std::move(candidate);
		}
	}
	if (decision.selected) return decision;

	std::vector<PlayerBotRewardCandidateSnapshot> candidates;
	for (const auto& source : snapshot.candidates) {
		if (!source.claimed) candidates.push_back(source);
	}
	std::sort(candidates.begin(), candidates.end(), [this, &snapshot](const auto& left, const auto& right) {
		const int32_t leftUtility = estimatedUtility(left.plan, snapshot);
		const int32_t rightUtility = estimatedUtility(right.plan, snapshot);
		if (leftUtility != rightUtility) return leftUtility > rightUtility;
		if (left.plan.estimatedDistance != right.plan.estimatedDistance) return left.plan.estimatedDistance < right.plan.estimatedDistance;
		return left.plan.uniqueId < right.plan.uniqueId;
	});
	for (const auto& source : candidates) {
		PlayerBotRewardPlan candidate = source.plan;
		if (source.totalWeight > snapshot.freeCapacity) { decision.outcomes.push_back({candidate, "rejected", "insufficient_capacity"}); continue; }
		if (!source.backpackAvailable || source.freeBackpackSlots < candidate.requiredBackpackSlots) { decision.outcomes.push_back({candidate, "rejected", "insufficient_inventory_space"}); continue; }
		if (estimatedUtility(candidate, snapshot) <= snapshot.huntUtility) { decision.outcomes.push_back({candidate, "rejected", "utility_below_hunt"}); continue; }
		const PlayerBotRouteEstimate& routeEstimate = source.route;
		if (!routeEstimate.reachable) { decision.outcomes.push_back({candidate, "rejected", "simple_route_unavailable"}); continue; }
		candidate.approachPosition = routeEstimate.approachPosition;
		candidate.travelSteps = routeEstimate.steps;
		candidate.travelDangerCost = routeEstimate.dangerCost;
		candidate.maximumTravelDanger = routeEstimate.maximumDanger;
		candidate.expandedNodes = routeEstimate.expandedNodes;
		if (candidate.maximumTravelDanger > snapshot.maximumTravelDanger ||
		    candidate.travelDangerCost > snapshot.maximumTravelDangerCost) {
			decision.outcomes.push_back({candidate, "rejected", "route_danger_above_tolerance"});
			continue;
		}
		if (utility(candidate, snapshot) <= snapshot.huntUtility) { decision.outcomes.push_back({candidate, "rejected", "actual_utility_below_hunt"}); continue; }
		decision.outcomes.push_back({candidate, "feasible", nullptr});
		decision.selected = std::move(candidate);
		return decision;
	}
	return decision;
}
