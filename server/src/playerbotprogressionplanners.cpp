/** Pure candidate selection for playerbot progression. */
#include "otpch.h"

#include "playerbotprogressionplanners.h"

#include "container.h"
#include "item.h"
#include "player.h"

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
	for (const auto& offer : snapshot.offers) {
		const char* rejection = !offer.inScope ? "outside_thais_scope" : !offer.registryMatches ? "spell_registry_mismatch" :
		                        !offer.vocationEligible ? "vocation_ineligible" : !offer.levelEligible ? "level_ineligible" :
		                        !offer.premiumEligible ? "premium_ineligible" : offer.known ? "already_learned" :
		                        !offer.suppliesReady ? "supply_reserve_unmet" : !snapshot.reserveAvailable ? "recovery_reserve_unavailable" :
		                        snapshot.totalMoney < snapshot.reserve + offer.price ? "unaffordable_after_reserves" :
		                        !offer.route.reachable ? "trainer_unreachable" : nullptr;
		if (rejection) {
			decision.rejections.push_back(rejection);
			continue;
		}
		PlayerBotSpellTrainingPlan candidate{offer.npcId, offer.npcPosition, offer.route.approachPosition, offer.spellName,
		                                    offer.keyword, offer.price, offer.level, offer.route.steps, snapshot.reserve};
		if (!decision.selected || candidate.price < decision.selected->price ||
		    (candidate.price == decision.selected->price && (candidate.travelSteps < decision.selected->travelSteps ||
		     (candidate.travelSteps == decision.selected->travelSteps && candidate.spellName < decision.selected->spellName)))) {
			decision.selected = std::move(candidate);
		}
	}
	return decision;
}

PlayerBotEquipmentProviderDecision PlayerBotEquipmentProviderPlanner::select(const PlayerBotEquipmentProviderPlannerSnapshot& snapshot) const
{
	PlayerBotEquipmentProviderDecision decision;
	if (!snapshot.enabled) return decision;
	for (const auto& offer : snapshot.offers) {
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
		if (!rejection && !evaluation.carried && !snapshot.reserveAvailable) rejection = "recovery_reserve_unavailable";
		if (!rejection && !evaluation.carried && snapshot.totalMoney < snapshot.reserve + evaluation.price) rejection = "unaffordable_after_reserves";
		if (!rejection && !evaluation.carried && !offer.route.reachable) rejection = offer.routeBudgetExhausted ? "provider_evaluation_budget_exhausted" :
		                                                               offer.route.nodeLimitReached ? "provider_route_node_budget_exhausted" : "provider_unreachable";
		if (rejection) {
			decision.rejections.push_back(rejection);
			continue;
		}
		if (!evaluation.carried) {
			evaluation.approachPosition = offer.route.approachPosition;
			evaluation.travelSteps = offer.route.steps;
		} else evaluation.travelSteps = 0;
		if (!decision.selected || PlayerBotEquipmentPolicy::prefers(evaluation, *decision.selected)) decision.selected = std::move(evaluation);
	}
	return decision;
}

std::string PlayerBotRewardPlanner::itemSignature(const Item& item)
{
	std::ostringstream signature;
	signature << item.getID() << ':' << item.getSubType();
	if (const Container* container = item.getContainer()) {
		signature << '[';
		bool first = true;
		for (const Item* child : container->getItemList()) {
			if (!first) signature << ',';
			first = false;
			signature << itemSignature(*child);
		}
		signature << ']';
	}
	return signature.str();
}

void PlayerBotRewardPlanner::inspectItem(const Item& item, uint16_t rootOrdinal, std::vector<uint16_t>& path,
	                                        const std::string& rootSignature,
	                                        const PlayerBotRewardInspectionContext& context,
	                                        PlayerBotRewardInspection& inspection) const
{
	PlayerBotRewardItemInspection inspected{item.getID(), item.getItemCount(), static_cast<uint32_t>(path.size()), rootOrdinal, path};
	++inspection.itemCount;
	if (item.getContainer()) {
		inspected.classes.emplace_back("container");
		++inspection.containerCount;
	}

	PlayerBotEquipmentOfferEvaluation equipment;
	bool evaluatedEquipment = false;
	if (context.equipmentPolicy.isCombatEquipment(item)) {
		evaluatedEquipment = true;
		const auto cacheKey = std::make_pair(item.getID(), context.additionalWeight);
		if (const auto cached = context.equipmentEvaluations.find(cacheKey); cached != context.equipmentEvaluations.end()) {
			equipment = cached->second;
		} else {
			equipment = context.equipmentPolicy.evaluateCandidate(
				context.player, item.getID(), context.currentLoadout, context.currentProfile, context.currentHunts,
				context.currentReady, context.readiness, context.additionalWeight,
				context.simulatedItems < context.maximumEquipmentCandidateSimulations, context.huntSummary);
			if (equipment.simulated) ++context.simulatedItems;
			context.equipmentEvaluations.emplace(cacheKey, equipment);
		}
	}
	if (evaluatedEquipment && equipment.rejection.empty()) {
		const ItemType& candidateType = Item::items[item.getID()];
		const uint16_t currentItemId = context.currentLoadout.itemIds[equipment.slot];
		const ItemType* currentType = currentItemId == 0 ? nullptr : &Item::items[currentItemId];
		const bool armorSlot = equipment.slot == CONST_SLOT_HEAD || equipment.slot == CONST_SLOT_ARMOR ||
		                       equipment.slot == CONST_SLOT_LEGS || equipment.slot == CONST_SLOT_FEET;
		const bool shield = candidateType.weaponType == WEAPON_SHIELD;
		const char* metric = armorSlot ? "armor" : shield ? "defense" : "attack";
		const int32_t candidateValue = armorSlot ? candidateType.armor : shield ? candidateType.defense : candidateType.attack;
		const int32_t currentValue = !currentType ? 0 : armorSlot ? currentType->armor :
		                             shield ? currentType->defense : currentType->attack;
		const PlayerBotEquipmentUpgrade upgrade{equipment.slot, std::max(1, candidateValue - currentValue), metric,
		                                        currentValue, candidateValue};
		inspected.classes.emplace_back("equipment_upgrade");
		++inspection.equipmentUpgradeCount;
		if (!inspection.bestEquipment || equipment.rule > inspection.bestEquipment->rule ||
		    (equipment.rule == inspection.bestEquipment->rule && upgrade.benefit > inspection.bestUpgrade->benefit)) {
			inspection.bestUpgrade = upgrade;
			inspection.bestEquipment = equipment;
			inspection.bestItemId = item.getID();
			inspection.bestRootOrdinal = rootOrdinal;
			inspection.bestItemPath = path;
			inspection.bestRootSignature = rootSignature;
		}
	} else if (evaluatedEquipment && inspection.equipmentRejection.empty()) {
		inspection.equipmentRejection = equipment.rejection;
	}

	inspected.worth = item.getWorth();
	if (inspected.worth != 0) {
		inspected.classes.emplace_back("currency");
		inspection.currencyValue += inspected.worth;
	}
	if (item.getID() == context.potionItemId) {
		inspected.classes.emplace_back("required_supply");
		inspection.potionCount += item.getItemCount();
	} else if (playerbot::PlayerBotInventoryPolicy::isFoodItem(item.getID())) {
		inspected.classes.emplace_back("food");
		inspection.foodCount += item.getItemCount();
	} else if (item.getID() == context.ropeItemId) {
		inspected.classes.emplace_back("tool");
		inspection.ropeCount += item.getItemCount();
	} else if (item.getID() == context.shovelItemId) {
		inspected.classes.emplace_back("tool");
		inspection.shovelCount += item.getItemCount();
	}
	const uint32_t learnedSellValue = context.economyCatalog.sellValue(item.getID());
	const ItemType& itemType = Item::items[item.getID()];
	const bool unsupportedTwoHandedWeapon = (itemType.slotPosition & SLOTP_TWO_HAND) != 0 && itemType.weaponType != WEAPON_NONE;
	const uint32_t sellPrice = inspected.worth == 0 && !unsupportedTwoHandedWeapon ? learnedSellValue : 0;
	if (sellPrice != 0) {
		inspected.classes.emplace_back("sellable");
		inspected.sellValue = sellPrice * item.getItemCount();
		inspection.sellValue += inspected.sellValue;
	}
	uint32_t itemUtility = inspected.worth + inspected.sellValue;
	if (item.getID() == context.potionItemId) itemUtility += context.missingPotionUtility * item.getItemCount();
	else if (playerbot::PlayerBotInventoryPolicy::isFoodItem(item.getID())) itemUtility += context.foodPreferenceUtility * item.getItemCount();
	else if (item.getID() == context.ropeItemId || item.getID() == context.shovelItemId) itemUtility += 100;
	if (itemUtility > inspection.primaryKnownItemUtility) {
		inspection.primaryKnownItemId = item.getID();
		inspection.primaryKnownRootOrdinal = rootOrdinal;
		inspection.primaryKnownRootSignature = rootSignature;
		inspection.primaryKnownItemUtility = itemUtility;
	}
	if (inspected.classes.empty()) {
		inspected.classes.emplace_back("unknown_keep");
		++inspection.unknownCount;
	}
	inspection.items.push_back(std::move(inspected));

	if (const Container* container = item.getContainer()) {
		uint16_t childOrdinal = 0;
		for (const Item* child : container->getItemList()) {
			path.push_back(childOrdinal++);
			inspectItem(*child, rootOrdinal, path, rootSignature, context, inspection);
			path.pop_back();
		}
	}
}

void PlayerBotRewardPlanner::finalizeInspection(const PlayerBotRewardInspectionContext& context,
	                                                PlayerBotRewardInspection& inspection) const
{
	if (inspection.bestUpgrade) inspection.knownUtility += inspection.bestUpgrade->benefit * 20;
	inspection.knownUtility += static_cast<int32_t>(inspection.currencyValue + inspection.sellValue);
	const uint32_t heldPotions = context.inventoryPolicy.inventoryItemCount(context.player, context.potionItemId);
	const uint32_t heldFood = context.inventoryPolicy.foodInventory(context.player).count;
	const uint32_t potionNeed = heldPotions < context.potionRestockTarget ? context.potionRestockTarget - heldPotions : 0;
	const uint32_t foodNeed = heldFood < context.preferredFoodCount ? context.preferredFoodCount - heldFood : 0;
	inspection.knownUtility += static_cast<int32_t>(std::min(inspection.potionCount, potionNeed) * context.missingPotionUtility +
	                                                  std::min(inspection.foodCount, foodNeed) * context.foodPreferenceUtility);
	if (inspection.ropeCount != 0 && !context.ownsItem(context.ropeItemId)) inspection.knownUtility += 100;
	if (inspection.shovelCount != 0 && !context.ownsItem(context.shovelItemId)) inspection.knownUtility += 100;
}

PlayerBotRewardInspection PlayerBotRewardPlanner::inspectBundle(const Container& contents,
	                                                               const PlayerBotRewardInspectionContext& context) const
{
	PlayerBotRewardInspection inspection;
	uint16_t rootOrdinal = 0;
	for (const Item* root : contents.getItemList()) {
		const std::string signature = itemSignature(*root);
		inspection.rootItemIds.push_back(root->getID());
		inspection.rootSignatures.push_back(signature);
		if (root->isStackable()) inspection.stackableRootCounts[root->getID()] += root->getItemCount();
		else inspection.nonStackableRootSignatures.push_back(signature);
		std::vector<uint16_t> path;
		inspectItem(*root, rootOrdinal++, path, signature, context, inspection);
	}
	finalizeInspection(context, inspection);
	return inspection;
}

PlayerBotRewardInspection PlayerBotRewardPlanner::inspectKnownReward(const Item& item,
	                                                                    const PlayerBotRewardInspectionContext& context) const
{
	PlayerBotRewardInspection inspection;
	const std::string signature = itemSignature(item);
	inspection.rootItemIds.push_back(item.getID());
	inspection.rootSignatures.push_back(signature);
	if (item.isStackable()) inspection.stackableRootCounts[item.getID()] += item.getItemCount();
	else inspection.nonStackableRootSignatures.push_back(signature);
	std::vector<uint16_t> path;
	inspectItem(item, 0, path, signature, context, inspection);
	finalizeInspection(context, inspection);
	return inspection;
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
	return std::max<int32_t>(0, base + static_cast<int32_t>(plan.knownUtility) - static_cast<int32_t>(plan.travelSteps));
}

int32_t PlayerBotRewardPlanner::estimatedUtility(const PlayerBotRewardPlan& plan,
	                                                const PlayerBotRewardPlannerSnapshot& snapshot) const
{
	const int32_t base = plan.equipmentUpgradeCount != 0 ? snapshot.pickupBaseUtility : snapshot.economicBaseUtility;
	return std::max<int32_t>(0, base + static_cast<int32_t>(plan.knownUtility) - static_cast<int32_t>(plan.estimatedDistance));
}

PlayerBotRewardDecision PlayerBotRewardPlanner::select(const PlayerBotRewardPlannerSnapshot& snapshot,
	                                                      const RouteEstimator& route) const
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
		const PlayerBotRouteEstimate routeEstimate = route(candidate);
		if (!routeEstimate.reachable) { decision.outcomes.push_back({candidate, "rejected", "simple_route_unavailable"}); continue; }
		candidate.approachPosition = routeEstimate.approachPosition;
		candidate.travelSteps = routeEstimate.steps;
		candidate.expandedNodes = routeEstimate.expandedNodes;
		if (utility(candidate, snapshot) <= snapshot.huntUtility) { decision.outcomes.push_back({candidate, "rejected", "actual_utility_below_hunt"}); continue; }
		decision.outcomes.push_back({candidate, "feasible", nullptr});
		decision.selected = std::move(candidate);
		return decision;
	}
	return decision;
}
