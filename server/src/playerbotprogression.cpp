/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "playerbotcontroller.h"
#include "playerbotarea.h"

// Goal arbitration and reward discovery, claiming, and equipment.
using namespace playerbot;

void PlayerBotController::emitCombatReadiness(const Player& player, const Position& position, const char* result,
                                              const std::string& recovery, const std::string& terminalReason) const
{
	Item* left = player.getInventoryItem(CONST_SLOT_LEFT);
	Item* right = player.getInventoryItem(CONST_SLOT_RIGHT);
	Item* armor = player.getInventoryItem(CONST_SLOT_ARMOR);
	Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	const PlayerBotFoodInventory food = inventoryPolicy.foodInventory(player);
	const uint32_t usableCapacity = inventoryPolicy.effectiveFreeCapacity(player);
	const bool weaponReady = (left && equipmentPolicy.isKnightMeleeWeapon(player, *left)) ||
	                         (right && equipmentPolicy.isKnightMeleeWeapon(player, *right));
	const bool armorReady = armor && equipmentPolicy.isLegalEquipmentItem(player, *armor) && armor->getArmor() > 0;
	std::ostringstream fields;
	fields << "\"result\":" << jsonString(result)
	       << ",\"vocation_id\":" << player.getVocationId()
	       << ",\"requirements\":[{\"name\":\"legal_melee_weapon\",\"ready\":" << (weaponReady ? "true" : "false")
	       << ",\"left_item_id\":" << (left ? std::to_string(left->getID()) : "null")
	       << ",\"right_item_id\":" << (right ? std::to_string(right->getID()) : "null") << '}'
	       << ",{\"name\":\"armor_loadout\",\"ready\":" << (armorReady && backpack && backpack->getContainer() ? "true" : "false")
	       << ",\"armor_item_id\":" << (armor ? std::to_string(armor->getID()) : "null")
	       << ",\"armor\":" << (armor ? armor->getArmor() : 0) << '}'
	       << ",{\"name\":\"small_health_potions\",\"ready\":" <<
	          (inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold ? "true" : "false")
	       << ",\"count\":" << inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId)
	       << ",\"return_threshold\":" << smallHealthPotionReturnThreshold
	       << ",\"restock_target\":" << smallHealthPotionRestockTarget << '}'
	       << ",{\"name\":\"food\",\"required\":false,\"ready\":true"
	       << ",\"count\":" << food.count << ",\"preferred\":" << preferredFoodCount
	       << ",\"reclaimable_weight\":" << food.weight
	       << ",\"preference_utility\":" <<
	          std::max<int32_t>(0, static_cast<int32_t>(preferredFoodCount -
	              std::min<uint32_t>(preferredFoodCount, food.count))) * foodPreferenceUtility << '}'
	       << ",{\"name\":\"free_capacity\",\"ready\":" << (usableCapacity >= returnCapacityThreshold ? "true" : "false")
	       << ",\"current\":" << player.getFreeCapacity() << ",\"reclaimable_food\":" << food.weight
	       << ",\"effective\":" << usableCapacity << ",\"minimum\":" << returnCapacityThreshold << "}]"
	       << ",\"selected_recovery\":" << (recovery.empty() ? "null" : jsonString(recovery))
	       << ",\"terminal_reason\":" << (terminalReason.empty() ? "null" : jsonString(terminalReason));
	emit("combat_readiness", position, fields.str());
}

PlayerBotEquipmentReadinessInput PlayerBotController::equipmentReadinessInput(const Player& player) const
{
	const Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	return {backpack && backpack->getContainer(),
	        inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId) > smallHealthPotionReturnThreshold,
	        inventoryPolicy.effectiveFreeCapacity(player), returnCapacityThreshold};
}

bool PlayerBotController::beginReadinessEquipment(Player* player, const Position& position, const char* reason)
{
	Item* item = nullptr;
	EquipmentUpgrade upgrade{};
	if (!player || !equipmentPolicy.findCarriedUpgrade(*player, item, upgrade) || !player->canDoAction()) {
		return false;
	}
	Position source;
	uint8_t sourceIndex = 0;
	g_game.internalGetPosition(item, source, sourceIndex);
	Container* sourceContainer = dynamic_cast<Container*>(item->getParent());
	if (sourceContainer && player->getContainerID(sourceContainer) < 0) {
		Container* containerToOpen = sourceContainer;
		while (Container* parent = dynamic_cast<Container*>(containerToOpen->getParent())) {
			if (player->getContainerID(parent) >= 0) {
				break;
			}
			containerToOpen = parent;
		}
		Position containerPosition;
		uint8_t containerIndex = 0;
		Item* containerItem = static_cast<Item*>(containerToOpen);
		g_game.internalGetPosition(containerItem, containerPosition, containerIndex);
		uint8_t containerId = rewardContainerIdBase;
		while (containerId <= maximumContainerId && player->getContainerByID(containerId)) {
			++containerId;
		}
		++pendingReadinessAttempts;
		if (containerId > maximumContainerId || containerPosition.x != 0xFFFF ||
		    pendingReadinessAttempts > maximumProgressionAttempts) {
			return false;
		}
		telemetry.recordActionAttempt();
		g_game.playerUseItem(playerId, containerPosition, containerIndex, containerId, containerItem->getClientID());
		emit("action_result", position, "\"action\":\"open_readiness_container\",\"result\":\"requested\",\"item_id\":" +
		     std::to_string(containerItem->getID()) + ",\"container_id\":" + std::to_string(containerId));
		schedule(navigationDecisionDelay(*player));
		return true;
	}
	pendingReadinessItemId = item->getID();
	pendingReadinessSlot = upgrade.slot;
	readinessEquipmentPending = true;
	pendingReadinessAttempts = 0;
	telemetry.recordActionAttempt();
	g_game.playerMoveItem(player, source, item->getClientID(), sourceIndex,
	                      Position(0xFFFF, upgrade.slot, 0), item->getItemCount(), item, nullptr);
	emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"requested\",\"item_id\":" +
	     std::to_string(item->getID()) + ",\"slot\":" + std::to_string(upgrade.slot) +
	     ",\"reason\":" + jsonString(reason));
	schedule(navigationDecisionDelay(*player));
	return true;
}

void PlayerBotController::processReadinessEquipment(Player* player, const Position& position)
{
	Item* equipped = pendingReadinessSlot == CONST_SLOT_WHEREEVER ? nullptr : player->getInventoryItem(pendingReadinessSlot);
	if (equipped && equipped->getID() == pendingReadinessItemId) {
		Item* carriedUpgrade = nullptr;
		EquipmentUpgrade upgrade{};
		const bool hasCarriedUpgrade = equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade);
		const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
			*player, hasCarriedUpgrade, equipmentReadinessInput(*player));
		const bool ready = readiness.ready;
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"success\",\"item_id\":" +
		     std::to_string(pendingReadinessItemId) + ",\"slot\":" + std::to_string(pendingReadinessSlot));
		emitCombatReadiness(*player, position, ready ? "ready" : "recovery", readiness.recovery, readiness.terminalReason);
		readinessEquipmentPending = false;
		pendingReadinessAttempts = 0;
		if (readinessResumeService) {
			readinessResumeService = false;
			depotWorkflow.resumeDeposit();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		if (ready) {
			if (huntRuntime.active()) {
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			startHunt(player, position, "readiness_carried_upgrade");
			return;
		}
		ensureCombatReady(player, position, "readiness_upgrade_incomplete");
		return;
	}
	if (++pendingReadinessAttempts >= maximumProgressionAttempts) {
		Item* carriedUpgrade = nullptr;
		EquipmentUpgrade upgrade{};
		const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
			*player, equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade), equipmentReadinessInput(*player));
		readinessEquipmentPending = false;
		readinessResumeService = false;
		emit("action_result", position, "\"action\":\"equip_readiness\",\"result\":\"failed\",\"reason\":\"move_not_verified\"");
		emitCombatReadiness(*player, position, "failed", readiness.recovery, readiness.terminalReason);
		if (!readiness.terminalReason.empty()) {
			stop(("combat_readiness_" + readiness.terminalReason).c_str(), position);
		} else {
			beginService(player, position, "readiness_equipment_move_failed");
			schedule(navigationInterval);
		}
		return;
	}
	readinessEquipmentPending = false;
	if (!beginReadinessEquipment(player, position, "readiness_retry")) {
		if (pendingReadinessAttempts >= maximumProgressionAttempts) {
			emit("action_result", position,
			     "\"action\":\"open_readiness_container\",\"result\":\"failed\",\"reason\":\"access_attempts_exhausted\"");
			pendingReadinessAttempts = 0;
			beginService(player, position, "readiness_container_access_failed");
			schedule(navigationInterval);
		} else {
			schedule(navigationDecisionDelay(*player));
		}
	}
}

bool PlayerBotController::ensureCombatReady(Player* player, const Position& position, const char* reason)
{
	if (!player || !equipmentPolicy.requiresKnightCombatReadiness(*player)) {
		return true;
	}
	Item* carriedUpgrade = nullptr;
	EquipmentUpgrade upgrade{};
	const PlayerBotEquipmentReadiness readiness = equipmentPolicy.combatReadiness(
		*player, equipmentPolicy.findCarriedUpgrade(*player, carriedUpgrade, upgrade), equipmentReadinessInput(*player));
	if (readiness.ready) {
		if (std::strcmp(reason, "readiness_continuous_check") != 0) {
			emitCombatReadiness(*player, position, "ready", {}, {});
		}
		return true;
	}
	emitCombatReadiness(*player, position, "recovery", readiness.recovery, readiness.terminalReason);
	if (readiness.recovery == "equip_carried") {
		if (!beginReadinessEquipment(player, position, reason)) {
			if (pendingReadinessAttempts >= maximumProgressionAttempts) {
				emit("action_result", position,
				     "\"action\":\"open_readiness_container\",\"result\":\"failed\",\"reason\":\"access_attempts_exhausted\"");
				pendingReadinessAttempts = 0;
				beginService(player, position, "readiness_container_access_failed");
				schedule(navigationInterval);
			} else {
				schedule(navigationDecisionDelay(*player));
			}
		}
		return false;
	}
	if (readiness.recovery == "service") {
		beginService(player, position, "combat_readiness_service");
		schedule(navigationInterval);
		return false;
	}
	stop(("combat_readiness_" + readiness.terminalReason).c_str(), position);
	return false;
}

std::string PlayerBotController::rewardInspectionItemsJson(const RewardInspection& inspection) const
{
	std::ostringstream json;
	json << '[';
	bool firstItem = true;
	for (const RewardItemInspection& item : inspection.items) {
		if (!firstItem) {
			json << ',';
		}
		firstItem = false;
		json << "{\"item_id\":" << item.itemId << ",\"name\":" << jsonString(Item::items[item.itemId].name)
		     << ",\"count\":" << item.count << ",\"depth\":" << item.depth
		     << ",\"root_ordinal\":" << item.rootOrdinal << ",\"path\":[";
		for (size_t index = 0; index < item.path.size(); ++index) {
			if (index != 0) {
				json << ',';
			}
			json << item.path[index];
		}
		json << "],\"classes\":[";
		for (size_t index = 0; index < item.classes.size(); ++index) {
			if (index != 0) {
				json << ',';
			}
			json << jsonString(item.classes[index]);
		}
		json << "],\"worth\":" << item.worth << ",\"sell_value\":" << item.sellValue << '}';
	}
	json << ']';
	return json.str();
}

Container* PlayerBotController::playerBackpack(Player& player) const
{
	Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
	return backpackItem ? backpackItem->getContainer() : nullptr;
}

uint32_t PlayerBotController::matchingRewardRootCount(Player& player, const std::string& signature) const
{
	uint32_t count = 0;
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player.getInventoryItem(static_cast<slots_t>(slot));
		if (item && PlayerBotRewardPlanner::itemSignature(*item) == signature) {
			++count;
		}
	}
	if (Container* backpack = playerBackpack(player)) {
		count += static_cast<uint32_t>(std::count_if(backpack->getItemList().begin(), backpack->getItemList().end(),
		                                            [this, &signature](const Item* item) {
			                                            return PlayerBotRewardPlanner::itemSignature(*item) == signature;
		                                            }));
	}
	return count;
}

Item* PlayerBotController::findMatchingRewardRoot(Player& player, const std::string& signature) const
{
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		Item* item = player.getInventoryItem(static_cast<slots_t>(slot));
		if (item && PlayerBotRewardPlanner::itemSignature(*item) == signature) {
			return item;
		}
	}
	Container* backpack = playerBackpack(player);
	if (!backpack) {
		return nullptr;
	}
	for (Item* item : backpack->getItemList()) {
		if (PlayerBotRewardPlanner::itemSignature(*item) == signature) {
			return item;
		}
	}
	return nullptr;
}

Item* PlayerBotController::resolveRewardPath(Item* root, const std::vector<uint16_t>& path, size_t length) const
{
	Item* current = root;
	for (size_t depth = 0; depth < length; ++depth) {
		Container* container = current ? current->getContainer() : nullptr;
		if (!container || path[depth] >= container->size()) {
			return nullptr;
		}
		current = container->getItemByIndex(path[depth]);
	}
	return current;
}

PlayerBotRewardObservation::ItemAccess PlayerBotController::observeRewardItemAccess(Player& player, size_t& containerDepth) const
{
	Container* backpack = playerBackpack(player);
	if (!backpack || !player.getInventoryItem(CONST_SLOT_BACKPACK)) return PlayerBotRewardObservation::ItemAccess::BackpackUnavailable;
	if (player.getContainerByID(backpackContainerId) != backpack) {
		return player.canDoAction() ? PlayerBotRewardObservation::ItemAccess::BackpackClosed :
		                              PlayerBotRewardObservation::ItemAccess::ActionUnavailable;
	}

	const auto& reward = rewardSession.plan();
	Item* root = findMatchingRewardRoot(player, reward.rootSignature);
	if (!root) {
		Item* extracted = g_game.findItemOfType(&player, reward.itemId, true);
		return extracted && isRewardClaimed(player, reward.uniqueId) ? PlayerBotRewardObservation::ItemAccess::Ready :
		                                                     PlayerBotRewardObservation::ItemAccess::RootUnavailable;
	}
	if (reward.selectedItemPath.size() > maximumContainerId - rewardContainerIdBase + 1) {
		return PlayerBotRewardObservation::ItemAccess::DepthUnsupported;
	}
	for (size_t depth = 0; depth < reward.selectedItemPath.size(); ++depth) {
		Item* ancestor = resolveRewardPath(root, reward.selectedItemPath, depth);
		Container* container = ancestor ? ancestor->getContainer() : nullptr;
		if (!container) {
			return PlayerBotRewardObservation::ItemAccess::PathInvalid;
		}
		const uint8_t containerId = static_cast<uint8_t>(rewardContainerIdBase + depth);
		if (player.getContainerByID(containerId) == container) {
			continue;
		}
		Position sourcePosition;
		uint8_t sourceIndex = 0;
		g_game.internalGetPosition(ancestor, sourcePosition, sourceIndex);
		if (sourcePosition.x != 0xFFFF) {
			return PlayerBotRewardObservation::ItemAccess::ContainerPositionUnavailable;
		}
		containerDepth = depth;
		return player.canDoAction() ? PlayerBotRewardObservation::ItemAccess::ContainerOpenRequired :
		                              PlayerBotRewardObservation::ItemAccess::ActionUnavailable;
	}
	Item* selectedItem = resolveRewardPath(root, reward.selectedItemPath, reward.selectedItemPath.size());
	return selectedItem && selectedItem->getID() == reward.itemId ? PlayerBotRewardObservation::ItemAccess::Ready :
	                                                               PlayerBotRewardObservation::ItemAccess::ItemPathInvalid;
}

Item* PlayerBotController::rewardItemForAccess(Player& player) const
{
	const auto& reward = rewardSession.plan();
	Item* root = findMatchingRewardRoot(player, reward.rootSignature);
	if (!root) {
		Item* extracted = g_game.findItemOfType(&player, reward.itemId, true);
		return extracted && isRewardClaimed(player, reward.uniqueId) ? extracted : nullptr;
	}
	return resolveRewardPath(root, reward.selectedItemPath, reward.selectedItemPath.size());
}

void PlayerBotController::openRewardBackpack(Player& player)
{
	Item* backpack = player.getInventoryItem(CONST_SLOT_BACKPACK);
	if (!backpack) return;
	player.closeContainer(backpackContainerId);
	g_game.playerUseItem(playerId, Position(0xFFFF, CONST_SLOT_BACKPACK, 0), 0, backpackContainerId, backpack->getClientID());
}

void PlayerBotController::openRewardContainer(Player& player, const Position& position, size_t depth)
{
	Item* root = findMatchingRewardRoot(player, rewardSession.plan().rootSignature);
	Item* ancestor = root ? resolveRewardPath(root, rewardSession.plan().selectedItemPath, depth) : nullptr;
	if (!ancestor) return;
	Position sourcePosition;
	uint8_t sourceIndex = 0;
	g_game.internalGetPosition(ancestor, sourcePosition, sourceIndex);
	if (sourcePosition.x != 0xFFFF) return;
	const uint8_t containerId = static_cast<uint8_t>(rewardContainerIdBase + depth);
	player.closeContainer(containerId);
	g_game.playerUseItem(playerId, sourcePosition, sourceIndex, containerId, ancestor->getClientID());
	emit("action_result", position,
	     "\"action\":\"open_reward_container\",\"result\":\"requested\",\"container_id\":" +
	         std::to_string(containerId) + ",\"depth\":" + std::to_string(depth) +
	         ",\"item_id\":" + std::to_string(ancestor->getID()));
}

bool PlayerBotController::isRewardPosition(const Player& player, const Position& position) const
{
	if (position.z >= MAP_MAX_LAYERS || !player.getTown()) {
		return false;
	}
	if (player.getTown()->getID() == rookgaardTownId) {
		return Position::getDistanceX(position, rookgaardTemplePosition) <= rookgaardRewardRadius &&
		       Position::getDistanceY(position, rookgaardTemplePosition) <= rookgaardRewardRadius;
	}
	return player.getTown()->getID() == oracleTownId &&
	       playerbot::isInsideLocalPlanningArea(player.getTemplePosition(), position);
}

bool PlayerBotController::isRewardClaimed(const Player& player, uint16_t uniqueId) const
{
	int32_t storageValue = -1;
	return player.getStorageValue(uniqueId, storageValue) && storageValue != -1;
}

bool PlayerBotController::planSimpleRewardApproach(Player& player, const Position& rewardPosition, Position& approachPosition,
                              std::deque<PlayerBotNavigationStep>& approachSteps, uint64_t& expandedNodes)
{
	const Position currentPosition = player.getPosition();
	std::vector<Position> candidates;
	candidates.reserve(8);
	for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) {
		for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
			if (xOffset == 0 && yOffset == 0) {
				continue;
			}
			const Position candidate(rewardPosition.x + xOffset, rewardPosition.y + yOffset, rewardPosition.z);
			Tile* tile = g_game.map.getTile(candidate);
			if (!tile || tile->queryAdd(0, player, 1, 0) != RETURNVALUE_NOERROR) {
				continue;
			}
			candidates.push_back(candidate);
		}
	}
	std::sort(candidates.begin(), candidates.end(), [&currentPosition](const Position& left, const Position& right) {
		return Position::getDistanceX(currentPosition, left) + Position::getDistanceY(currentPosition, left) <
		       Position::getDistanceX(currentPosition, right) + Position::getDistanceY(currentPosition, right);
	});
	for (const Position& candidate : candidates) {
			std::deque<PlayerBotNavigationStep> steps;
			uint64_t candidateExpandedNodes = 0;
			const auto startedAt = std::chrono::steady_clock::now();
			const PlayerBotNavigationRoutePlan routePlan = candidate == currentPosition ? PlayerBotNavigationRoutePlan{} :
				navigationRuntime.plan(player, candidate);
			const PlayerBotNavigationResult planResult = candidate == currentPosition ? PlayerBotNavigationResult::Reached : routePlan.metrics.result;
			if (candidate != currentPosition) {
				steps = routePlan.steps;
				candidateExpandedNodes = routePlan.metrics.expandedNodes;
			}
			const bool planned = planResult == PlayerBotNavigationResult::Reached;
			telemetry.recordPathfindingAttempt(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - startedAt));
			if (!planned || (candidate != currentPosition && steps.empty())) {
				continue;
			}
			const bool simple = std::none_of(steps.begin(), steps.end(), [](const PlayerBotNavigationStep& step) {
				return step.action == PlayerBotNavigationAction::UseDoor ||
				       step.action == PlayerBotNavigationAction::UseRope ||
				       step.action == PlayerBotNavigationAction::UseShovel;
			});
			if (!simple || steps.size() > 120) {
				continue;
			}
			approachPosition = candidate;
			approachSteps = std::move(steps);
			expandedNodes = candidateExpandedNodes;
			return true;
	}
	return false;
}

void PlayerBotController::emitRewardCandidate(const PlayerBotRewardPlan& candidate, const Position& position, const char* result,
                         const char* reason) const
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << candidate.uniqueId
	       << ",\"acquisition_source\":\"map_reward\""
	       << ",\"result\":" << jsonString(result)
	       << ",\"item_id\":" << candidate.itemId
	       << ",\"root_item_id\":" << candidate.rootItemId
	       << ",\"slot\":" << static_cast<int32_t>(candidate.slot)
	       << ",\"metric\":" << jsonString(candidate.metric)
	       << ",\"current_value\":" << candidate.currentValue
	       << ",\"candidate_value\":" << candidate.candidateValue
	       << ",\"benefit\":" << candidate.benefit
	       << ",\"known_utility\":" << candidate.knownUtility
	       << ",\"item_count\":" << candidate.itemCount
	       << ",\"container_count\":" << candidate.containerCount
	       << ",\"unknown_count\":" << candidate.unknownCount
	       << ",\"currency_value\":" << candidate.currencyValue
	       << ",\"sell_value\":" << candidate.sellValue
	       << ",\"equipment_upgrade_count\":" << candidate.equipmentUpgradeCount
	       << ",\"travel_steps\":" << candidate.travelSteps
	       << ",\"destination\":{\"x\":" << candidate.itemPosition.x
	       << ",\"y\":" << candidate.itemPosition.y << ",\"z\":"
	       << static_cast<uint16_t>(candidate.itemPosition.z) << '}';
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("strategy_candidate", position, fields.str());
}

void PlayerBotController::emitRewardInspection(uint16_t uniqueId, const Position& rewardPosition,
                          const RewardInspection& inspection, const Position& position)
{
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"candidate_id\":" << uniqueId
	       << ",\"acquisition_source\":\"map_reward\""
	       << ",\"result\":\"inspected\",\"recursive\":true"
	       << ",\"known_utility\":" << inspection.knownUtility
	       << ",\"item_count\":" << inspection.itemCount
	       << ",\"container_count\":" << inspection.containerCount
	       << ",\"unknown_count\":" << inspection.unknownCount
	       << ",\"currency_value\":" << inspection.currencyValue
	       << ",\"sell_value\":" << inspection.sellValue
	       << ",\"equipment_upgrade_count\":" << inspection.equipmentUpgradeCount
	       << ",\"equipment_rule\":" << (inspection.bestEquipment ?
	              jsonString(PlayerBotEquipmentPolicy::decisionRuleName(inspection.bestEquipment->rule)) : "null")
	       << ",\"equipment_rejection\":" << (inspection.equipmentRejection.empty() ?
	              "null" : jsonString(inspection.equipmentRejection))
	       << ",\"destination\":{\"x\":" << rewardPosition.x << ",\"y\":" << rewardPosition.y
	       << ",\"z\":" << static_cast<uint16_t>(rewardPosition.z) << '}'
	       << ",\"items\":" << rewardInspectionItemsJson(inspection);
	const std::string fingerprint = fields.str();
	auto previous = rewardInspectionFingerprints.find(uniqueId);
	if (previous != rewardInspectionFingerprints.end() && previous->second == fingerprint) {
		return;
	}
	rewardInspectionFingerprints[uniqueId] = fingerprint;
	emit("reward_inspection", position, fingerprint);
}

bool PlayerBotController::findPickupReward(Player& player, const Position& position, PlayerBotRewardPlan& reward,
                       std::deque<PlayerBotNavigationStep>& rewardSteps)
{
	const EquipmentLoadout currentLoadout = equipmentPolicy.loadout(player);
	const PlayerBotCombatProfile currentProfile = equipmentPolicy.combatProfile(player, currentLoadout);
	const EquipmentHuntSummary currentHunts = equipmentHuntSummary(player, currentProfile);
	const bool currentReady = equipmentPolicy.loadoutReady(player, currentLoadout, equipmentReadinessInput(player));
	std::map<std::pair<uint16_t, uint32_t>, EquipmentOfferEvaluation> equipmentEvaluations;
	size_t simulatedItems = 0;
	std::vector<PlayerBotRewardCandidateSnapshot> candidates;
	for (const auto& entry : g_game.getUniqueItems()) {
		Item* rewardObject = entry.second;
		if (!rewardObject) {
			continue;
		}
		const bool containerReward = rewardObject->getActionId() == genericQuestChestActionId;
		const bool doubletReward = rewardObject->getActionId() == nonContainerQuestActionId &&
		                           rewardObject->getUniqueId() == doubletQuestUniqueId;
		if (!containerReward && !doubletReward) {
			continue;
		}
		if (!rewardObject->isLoadedFromMap() && !doubletReward) {
			continue;
		}
		Tile* tile = rewardObject->getTile();
		Container* contents = rewardObject->getContainer();
		if (!tile || !isRewardPosition(player, tile->getPosition())) {
			continue;
		}
		PlayerBotRewardPlan rejected;
		rejected.uniqueId = rewardObject->getUniqueId();
		rejected.rootItemId = rewardObject->getID();
		rejected.itemPosition = tile->getPosition();
		if (containerReward && !contents) {
			emitRewardCandidate(rejected, position, "rejected", "malformed_reward_container");
			continue;
		}
		if (containerReward && contents->empty()) {
			emitRewardCandidate(rejected, position, "rejected", "empty_reward_bundle");
			continue;
		}

		std::unique_ptr<Item> knownReward;
		if (doubletReward) {
			knownReward.reset(Item::CreateItem(doubletItemId));
			if (!knownReward) {
				continue;
			}
		}
		uint32_t totalWeight = 0;
		if (containerReward) {
			for (Item* reward : contents->getItemList()) {
				totalWeight += reward->getWeight();
			}
		} else {
			totalWeight = knownReward->getWeight();
		}
		const bool claimed = isRewardClaimed(player, rewardObject->getUniqueId());
		const uint32_t acquisitionWeight = claimed ? 0 : totalWeight;
		const PlayerBotRewardInspectionContext inspectionContext{
			player, equipmentPolicy, inventoryPolicy, economyCatalog, currentLoadout, currentProfile, currentHunts,
			equipmentReadinessInput(player), currentReady, acquisitionWeight, maximumEquipmentCandidateSimulations,
			smallHealthPotionItemId, smallHealthPotionRestockTarget, preferredFoodCount, ropeItemId, 2554,
			missingPotionUtility, foodPreferenceUtility,
			[this](Player& candidate, const PlayerBotCombatProfile& profile) { return equipmentHuntSummary(candidate, profile); },
			[&player](uint16_t itemId) { return g_game.findItemOfType(&player, itemId, true) != nullptr; },
			equipmentEvaluations, simulatedItems,
		};
		RewardInspection inspection = containerReward ?
			rewardPlanner.inspectBundle(*contents, inspectionContext) :
			rewardPlanner.inspectKnownReward(*knownReward, inspectionContext);
		if (inspection.itemCount != 0) {
			emitRewardInspection(rewardObject->getUniqueId(), tile->getPosition(), inspection, position);
		}
		std::optional<PlayerBotRewardPlan> candidate = rewardPlanner.plan(
			rewardObject->getUniqueId(), tile->getPosition(),
			playerBotNavigationDistance(position, tile->getPosition()), inspection);
		if (!candidate) {
			rejected.itemCount = inspection.itemCount;
			rejected.containerCount = inspection.containerCount;
			rejected.unknownCount = inspection.unknownCount;
			rejected.equipmentUpgradeCount = inspection.equipmentUpgradeCount;
			emitRewardCandidate(rejected, position, "rejected",
			                    inspection.equipmentRejection.empty() ? "unsupported_reward_bundle" :
			                                                            inspection.equipmentRejection.c_str());
			continue;
		}
		const bool ownedUpgrade = claimed && inspection.bestUpgrade &&
		                          (candidate->selectedItemPath.empty() ?
		                               g_game.findItemOfType(&player, candidate->itemId, true) != nullptr :
		                               findMatchingRewardRoot(player, candidate->rootSignature) != nullptr);
		Item* backpackItem = player.getInventoryItem(CONST_SLOT_BACKPACK);
		Container* backpack = backpackItem ? backpackItem->getContainer() : nullptr;
		const uint32_t freeBackpackSlots = backpack ? backpack->capacity() -
		                                  std::min<uint32_t>(backpack->capacity(), backpack->size()) : 0;
		candidates.push_back({std::move(*candidate), claimed, ownedUpgrade, totalWeight, backpack != nullptr,
		                      freeBackpackSlots, {}});
	}

	std::map<uint16_t, std::deque<PlayerBotNavigationStep>> plannedRoutes;
	const PlayerBotRewardPlannerSnapshot snapshot{player.getFreeCapacity(), pickupRewardBaseUtility,
	                                              economicPickupBaseUtility, huntGoalUtility, std::move(candidates)};
	const PlayerBotRewardDecision decision = rewardPlanner.select(snapshot,
		[this, &player, &plannedRoutes](const PlayerBotRewardPlan& candidate) {
			PlayerBotRouteEstimate estimate;
			std::deque<PlayerBotNavigationStep> steps;
			if (!planSimpleRewardApproach(player, candidate.itemPosition, estimate.approachPosition, steps,
			                               estimate.expandedNodes)) return estimate;
			estimate.reachable = true;
			estimate.steps = static_cast<uint32_t>(steps.size());
			plannedRoutes.emplace(candidate.uniqueId, std::move(steps));
			return estimate;
		});
	for (const auto& outcome : decision.outcomes) {
		emitRewardCandidate(outcome.plan, position, outcome.result, outcome.reason);
	}
	if (!decision.selected) return false;
	reward = *decision.selected;
	if (!reward.resumeEquipment) rewardSteps = std::move(plannedRoutes[reward.uniqueId]);
	return true;
}

uint32_t PlayerBotController::saleableItemCount(const Player& player) const
{
	uint32_t count = 0;
	for (const auto& [itemId, value] : economyCatalog.sellValues()) {
		if (value != 0) {
			count += getSaleItemCount(player, itemId);
		}
	}
	return count;
}

PlayerBotController::GoalCandidate PlayerBotController::serviceGoalCandidate(const Player& player) const
{
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t missingPotions = potionCount <= smallHealthPotionReturnThreshold ?
	                                  smallHealthPotionRestockTarget - potionCount : 0;
	const uint32_t sellable = saleableItemCount(player);
	const bool lowCapacity = inventoryPolicy.effectiveFreeCapacity(player) < returnCapacityThreshold;
	const bool criticalHealing = survivalRuntime.needsHealing(survivalSnapshot(player)) && missingPotions != 0;
	const bool cashAdjustment = player.getMoney() != inventoryPolicy.desiredCarriedGold(player);
	const bool feasible = lowCapacity || missingPotions != 0 || sellable != 0 || cashAdjustment;
	int32_t utility = feasible ? serviceGoalBaseUtility : 0;
	utility += static_cast<int32_t>(missingPotions * missingPotionUtility +
	                                std::min<uint32_t>(sellable, 20) * sellableItemUtility);
	utility += cashAdjustment ? 10 : 0;
	if (lowCapacity) {
		utility = std::max<int32_t>(utility, capacityServiceUtility);
	}
	if (criticalHealing) {
		utility = std::max<int32_t>(utility, criticalHealingServiceUtility);
	}
	const char* reason = criticalHealing ? "critical_healing" : lowCapacity ? "capacity" :
	                     missingPotions != 0 ? "healing_reserve" :
	                     sellable != 0 ? "sellable_inventory" : cashAdjustment ? "cash_reserve" : "no_service_need";
	return GoalCandidate{TopLevelGoal::Service, feasible, utility, reason};
}

void PlayerBotController::emitGoalCandidate(const Player& player, const GoalCandidate& candidate, uint64_t decisionId, const Position& position, const char* decisionReason,
	                       const PlayerBotRewardPlan* reward, const PlayerBotOracleDeparturePlan* departure,
	                       const EquipmentOfferEvaluation* equipment) const
{
	std::ostringstream fields;
	const bool evaluated = candidate.reason != "deferred_lower_utility";
	fields << "\"decision_id\":" << decisionId << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"goal\":" << jsonString(PlayerBotGoalArbiter::goalName(candidate.goal))
	       << ",\"evaluated\":" << (evaluated ? "true" : "false")
	       << ",\"feasible\":" << (candidate.feasible ? "true" : "false")
	       << ",\"utility\":" << candidate.utility << ",\"reason\":" << jsonString(candidate.reason);
	if (candidate.goal == TopLevelGoal::Service) {
		const PlayerBotFoodInventory food = inventoryPolicy.foodInventory(player);
		const uint32_t foodGap = food.count < preferredFoodCount ? preferredFoodCount - food.count : 0;
		fields << ",\"potion_count\":" << inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId)
		       << ",\"potion_return_threshold\":" << smallHealthPotionReturnThreshold
		       << ",\"potion_restock_target\":" << smallHealthPotionRestockTarget
		       << ",\"food_count\":" << food.count << ",\"food_weight\":" << food.weight
		       << ",\"food_preferred\":" << preferredFoodCount << ",\"food_gap\":" << foodGap
		       << ",\"food_utility\":" << foodGap * foodPreferenceUtility
		       << ",\"sellable_count\":" << saleableItemCount(player);
	}
	if (reward) {
		fields << ",\"candidate_id\":" << reward->uniqueId << ",\"item_id\":" << reward->itemId
		       << ",\"benefit\":" << reward->benefit << ",\"travel_steps\":" << reward->travelSteps;
	}
	if (departure) {
		fields << ",\"npc_id\":" << departure->npcId << ",\"town\":\"thais\",\"town_id\":" << oracleTownId
		       << ",\"vocation\":\"knight\",\"vocation_id\":" << oracleVocationId
		       << ",\"travel_steps\":" << departure->travelSteps;
	}
	if (equipment) {
		fields << ",\"npc_id\":" << equipment->npcId << ",\"item_id\":" << equipment->itemId
		       << ",\"price\":" << equipment->price << ",\"rule\":"
	       << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule))
		       << ",\"travel_steps\":" << equipment->travelSteps;
	}
	if (candidate.goal == TopLevelGoal::MagicTraining) {
		const std::optional<ManaRegenerationForecast> forecast = player.getManaRegenerationForecast();
		fields << ",\"mana\":" << player.getMana() << ",\"mana_max\":" << player.getMaxMana();
		if (forecast) {
			fields << ",\"mana_gain\":" << forecast->gain << ",\"mana_tick_interval\":" << forecast->interval
			       << ",\"mana_tick_remaining\":" << forecast->remaining;
		}
	}
	emit("goal_candidate", position, fields.str());
}

void PlayerBotController::beginPickupReward(Player& player, const Position& position, PlayerBotRewardPlan reward,
                       std::deque<PlayerBotNavigationStep> rewardSteps)
{
	std::map<uint16_t, uint32_t> displaced;
	for (uint16_t itemId : {reward.replacedItemId, reward.displacedLeftItemId, reward.displacedRightItemId}) {
		if (itemId != 0) displaced[itemId] = inventoryPolicy.inventoryItemCount(player, itemId);
	}
	progressionRuntime.beginReward(std::move(reward), std::move(displaced));
	const auto& selected = rewardSession.plan();
	if (!selected.resumeEquipment) {
		navigationRuntime.adopt(selected.approachPosition, std::move(rewardSteps));
	}
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"candidate_id\":" << selected.uniqueId
	       << ",\"reason\":" << jsonString(selected.resumeEquipment ? "resume_claimed_upgrade" :
	                                                                    "highest_known_utility_reachable_reward")
	       << ",\"item_id\":" << selected.itemId << ",\"root_item_id\":" << selected.rootItemId
	       << ",\"benefit\":" << selected.benefit << ",\"known_utility\":" << selected.knownUtility
	       << ",\"travel_steps\":" << selected.travelSteps;
	emit("strategy_selection", position, fields.str());
	say(player, selected.resumeEquipment ? "Equipping a previously claimed reward." :
	                                         "Going to claim a useful equipment reward.");
}

bool PlayerBotController::selectTopLevelGoal(Player& player, const Position& position, const char* decisionReason)
{
	if (departurePlanner.required(departureSnapshot(player))) {
		return forceOracleDeparture(player, position, decisionReason);
	}
	PlayerBotOracleDeparturePlan departure;
	std::deque<PlayerBotNavigationStep> departureRoute;
	const bool departureEligible = player.getLevel() >= oracleMinimumLevel && player.getLevel() <= oracleMaximumLevel &&
	                               player.getVocation()->getId() == 0;
	const bool departureFound = departureEligible && findOracleDeparture(player, position, departure, departureRoute);
	PlayerBotRewardPlan reward;
	std::deque<PlayerBotNavigationStep> rewardSteps;
	const auto now = std::chrono::steady_clock::now();
	const bool pickupCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::PickupReward, now);
	const bool pickupFound = !pickupCoolingDown && findPickupReward(player, position, reward, rewardSteps);
	const PlayerBotRewardPlannerSnapshot rewardSnapshot{
		0, pickupRewardBaseUtility, economicPickupBaseUtility, huntGoalUtility, {},
	};
	const int32_t pickupUtility = pickupFound ? rewardPlanner.utility(reward, rewardSnapshot) : 0;
	PlayerBotSpellTrainingPlan spellTraining;
	std::deque<PlayerBotNavigationStep> spellTrainingSteps;
	const bool spellTrainingCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::LearnSpell, now);
	const bool spellTrainingFound = !spellTrainingCoolingDown && findSpellTraining(player, position, spellTraining, spellTrainingSteps);
	const bool equipmentPurchaseCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::BuyEquipment, now);
	const std::optional<EquipmentOfferEvaluation> equipment = equipmentPurchaseCoolingDown ? std::nullopt :
	                                                        evaluateEquipmentOffers(player, position);
	const bool equipmentFound = fixtureDriver.equipmentPurchasesEnabled() && equipment.has_value();
	const bool magicTrainingCoolingDown = goalArbiter.isCoolingDown(TopLevelGoal::MagicTraining, now);
	const char* magicTrainingReason = magicTrainingCoolingDown ? "cooldown" :
	                                  survivalRuntime.magicTrainingReason(player, survivalSnapshot(player));
	const uint32_t potionCount = inventoryPolicy.inventoryItemCount(player, smallHealthPotionItemId);
	const uint32_t missingPotions = potionCount <= smallHealthPotionReturnThreshold ?
	                                  smallHealthPotionRestockTarget - potionCount : 0;
	const uint32_t sellable = saleableItemCount(player);
	const bool lowCapacity = inventoryPolicy.effectiveFreeCapacity(player) < returnCapacityThreshold;
	const bool criticalHealing = survivalRuntime.needsHealing(survivalSnapshot(player)) && missingPotions != 0;
	const PlayerBotGoalPlannerSnapshot snapshot{
		departurePlanner.required(departureSnapshot(player)), departureEligible, departureFound,
		player.getVocation()->getId() != 0, player.getLevel() < oracleMinimumLevel,
		player.getLevel() > oracleMaximumLevel,
		lowCapacity, criticalHealing, missingPotions, sellable,
		player.getMoney() != inventoryPolicy.desiredCarriedGold(player),
		pickupCoolingDown, pickupFound, pickupUtility,
		spellTrainingCoolingDown, spellTrainingFound,
		equipmentPurchaseCoolingDown, fixtureDriver.equipmentPurchasesEnabled(), equipmentFound,
		equipmentFound ? PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule) : "",
		magicTrainingCoolingDown, magicTrainingReason ? magicTrainingReason : "",
	};
	const PlayerBotGoalArbiter::GoalDecision decision = progressionRuntime.decide(snapshot);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Departure), decision.id, position, decisionReason, nullptr,
	                  departureFound ? &departure : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Service), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::PickupReward), decision.id, position, decisionReason, pickupFound ? &reward : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::LearnSpell), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::BuyEquipment), decision.id, position, decisionReason, nullptr, nullptr,
	                  equipmentFound ? &*equipment : nullptr);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::MagicTraining), decision.id, position, decisionReason);
	emitGoalCandidate(player, decision.candidate(TopLevelGoal::Hunt), decision.id, position, decisionReason);
	if (fixtureDriver.pauseAfterEquipmentStorageRejection()) {
		return false;
	}

	if (!decision.selected) {
		emit("goal_selection", position,
		     "\"decision_id\":" + std::to_string(decision.id) + ",\"decision_reason\":" +
		         jsonString(decisionReason) + ",\"result\":\"failed\",\"reason\":\"no_feasible_goal\"");
		stop("no_feasible_goal", position);
		return false;
	}
	const GoalCandidate& selected = *decision.selected;
	progressionRuntime.apply(decision);
	std::ostringstream fields;
	fields << "\"decision_id\":" << decision.id << ",\"decision_reason\":" << jsonString(decisionReason)
	       << ",\"from_goal\":" << jsonString(PlayerBotGoalArbiter::goalName(decision.previousGoal))
	       << ",\"to_goal\":" << jsonString(PlayerBotGoalArbiter::goalName(selected.goal))
	       << ",\"utility\":" << selected.utility << ",\"reason\":" << jsonString(selected.reason);
	if (selected.goal == TopLevelGoal::Departure) {
		fields << ",\"npc_id\":" << departure.npcId << ",\"town_id\":" << oracleTownId
		       << ",\"vocation_id\":" << oracleVocationId;
	} else if (selected.goal == TopLevelGoal::PickupReward) {
		fields << ",\"candidate_id\":" << reward.uniqueId << ",\"item_id\":" << reward.itemId;
	} else if (selected.goal == TopLevelGoal::LearnSpell) {
		fields << ",\"npc_id\":" << spellTraining.npcId << ",\"spell\":" << jsonString(spellTraining.spellName)
		       << ",\"price\":" << spellTraining.price;
	} else if (selected.goal == TopLevelGoal::BuyEquipment) {
		fields << ",\"npc_id\":" << equipment->npcId << ",\"item_id\":" << equipment->itemId
		       << ",\"price\":" << equipment->price << ",\"rule\":"
	       << jsonString(PlayerBotEquipmentPolicy::decisionRuleName(equipment->rule));
	}
	emit("goal_selection", position, fields.str());
	if (selected.goal == TopLevelGoal::Departure) {
		beginOracleDeparture(player, position, std::move(departure), std::move(departureRoute));
	} else if (selected.goal == TopLevelGoal::PickupReward) {
		beginPickupReward(player, position, std::move(reward), std::move(rewardSteps));
	} else if (selected.goal == TopLevelGoal::LearnSpell) {
		beginSpellTraining(player, position, std::move(spellTraining), std::move(spellTrainingSteps));
	} else if (selected.goal == TopLevelGoal::BuyEquipment) {
		beginEquipmentPurchase(player, position, *equipment);
	} else if (selected.goal == TopLevelGoal::Service) {
		beginService(&player, position, "goal_selected");
	} else if (selected.goal == TopLevelGoal::MagicTraining) {
		schedule(SCHEDULER_MINTICKS);
	} else {
		startHunt(&player, position, "goal_selected");
	}
	return true;
}

const char* PlayerBotController::objectiveName() const
{
	return progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture) ? "oracle_departure" :
	       progressionSession.active(PlayerBotProgressionProcedure::PickupReward) ? "pickup_reward" :
	       progressionSession.active(PlayerBotProgressionProcedure::LearnSpell) ? "learn_spell" :
	       progressionSession.active(PlayerBotProgressionProcedure::BuyEquipment) ? "buy_equipment" : cyclePhaseName();
}

void PlayerBotController::finishProgressionObjective(Player* player, const Position& position, const char* result, const char* reason,
                                 bool scheduleNext)
{
	const auto& reward = rewardSession.plan();
	std::ostringstream fields;
	fields << "\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"candidate_id\":" << reward.uniqueId
	       << ",\"item_id\":" << reward.itemId << ",\"result\":" << jsonString(result)
	       << ",\"reason\":" << jsonString(reason);
	emit("strategy_objective_result", position, fields.str());
	emit("goal_result", position,
	     "\"decision_id\":" + std::to_string(goalArbiter.decisionId()) +
	         ",\"goal\":\"pickup_reward\",\"acquisition_source\":\"map_reward\",\"result\":" +
	         jsonString(result) + ",\"reason\":" + jsonString(reason));
	if (player) {
		say(*player, std::string("Equipment reward objective ") + result + ": " + reason + '.');
	}
	progressionRuntime.finish();
	clearNavigation();
	serviceWorkflow.reset();
	serviceWorkflow.resetNpc();
	cyclePhase = CyclePhase::Service;
	progressionRuntime.setCooldown(TopLevelGoal::PickupReward, std::strcmp(result, "success") == 0 ? pickupRewardSuccessCooldown :
	                                                                                              pickupRewardFailureCooldown);
	if (fixtureDriver.progressionEnabled() && player) {
		const char* decisionReason = std::strcmp(result, "success") == 0 ? "pickup_complete" :
		                             std::strcmp(result, "interrupted") == 0 ? "pickup_interrupted" : "pickup_failed";
		selectTopLevelGoal(*player, position, decisionReason);
	} else {
		progressionRuntime.setActiveGoal(TopLevelGoal::Service);
		emit("objective_transition", position,
		     "\"from\":\"pickup_reward\",\"to\":\"service\",\"reason\":" + jsonString(reason));
	}
	if (scheduleNext) {
		schedule(SCHEDULER_MINTICKS);
	}
}

void PlayerBotController::processPickupReward(Player* player, const Position& currentPosition)
{
	const auto& pickupReward = rewardSession.plan();
	PlayerBotRewardObservation observation;
	observation.actionAvailable = player->canDoAction();
	if (rewardSession.stage() == PlayerBotRewardStage::Travel) {
		observation.navigationReached = processNavigation(player, currentPosition, pickupReward.approachPosition);
		observation.navigationFailed = navigationRuntime.fixedTargetRouteFailureCount() >= maximumProgressionAttempts;
	} else {
		Item* rewardObject = g_game.getUniqueItem(pickupReward.uniqueId);
		Tile* tile = rewardObject ? rewardObject->getTile() : nullptr;
		observation.rewardObjectAvailable = rewardObject && tile && tile->getPosition() == pickupReward.itemPosition &&
		                                    tile->getThingIndex(rewardObject) >= 0 && tile->getThingIndex(rewardObject) <= UINT8_MAX;
		observation.inRange = Position::areInRange<1, 1, 0>(currentPosition, pickupReward.itemPosition);
		observation.claimed = isRewardClaimed(*player, pickupReward.uniqueId);
		observation.currentClaim.itemCount = inventoryPolicy.inventoryItemCount(*player, pickupReward.itemId);
		observation.currentClaim.rootCount = matchingRewardRootCount(*player, pickupReward.rootSignature);
		for (const std::string& signature : pickupReward.nonStackableRootSignatures) {
			observation.currentClaim.roots.emplace(signature, matchingRewardRootCount(*player, signature));
		}
		for (const auto& [itemId, count] : pickupReward.stackableRootCounts) {
			observation.currentClaim.stackables.emplace(itemId, inventoryPolicy.inventoryItemCount(*player, itemId));
		}
		Item* equipped = player->getInventoryItem(pickupReward.slot);
		observation.equipmentVerified = equipped && equipped->getID() == pickupReward.itemId;
		observation.rootRelocationRequired = equipped && equipped->getID() == pickupReward.rootItemId &&
		                                    !pickupReward.selectedItemPath.empty() &&
		                                    resolveRewardPath(equipped, pickupReward.selectedItemPath, pickupReward.selectedItemPath.size());
		Container* backpack = playerBackpack(*player);
		observation.rootRelocationSpaceAvailable = backpack && backpack->size() < backpack->capacity();
		observation.itemAccess = observeRewardItemAccess(*player, observation.containerDepth);
		Item* reward = observation.itemAccess == PlayerBotRewardObservation::ItemAccess::Ready ? rewardItemForAccess(*player) : nullptr;
		if (reward) {
			for (const auto& [slot, itemId] : {std::pair<slots_t, uint16_t>{pickupReward.slot, pickupReward.replacedItemId},
			                                  {CONST_SLOT_LEFT, pickupReward.displacedLeftItemId},
			                                  {CONST_SLOT_RIGHT, pickupReward.displacedRightItemId}}) {
				Item* item = itemId == 0 ? nullptr : player->getInventoryItem(slot);
				if (item && item->getID() == itemId && item != reward) {
					observation.displacedMoveRequired = true;
					break;
				}
			}
		}
		observation.displacedMoveSpaceAvailable = backpack && backpack->size() < backpack->capacity();
		for (uint16_t itemId : {pickupReward.replacedItemId, pickupReward.displacedLeftItemId, pickupReward.displacedRightItemId}) {
			if (itemId != 0) observation.displacedCounts[itemId] = inventoryPolicy.inventoryItemCount(*player, itemId);
		}
	}
	const PlayerBotProgressionOutcome result = progressionRuntime.advanceReward(observation);
	if (result.type == PlayerBotProgressionOutcomeType::Succeeded || result.type == PlayerBotProgressionOutcomeType::Failed) {
		if (result.type == PlayerBotProgressionOutcomeType::Succeeded && result.reason && std::strcmp(result.reason, "reward_equipped") == 0) {
			std::ostringstream fields;
			fields << "\"action\":\"equip\",\"result\":\"success\",\"item_id\":" << pickupReward.itemId
			       << ",\"slot\":" << static_cast<int32_t>(pickupReward.slot) << ",\"displaced_item_id\":" << pickupReward.replacedItemId
			       << ",\"metric\":" << jsonString(pickupReward.metric) << ",\"value_before\":" << pickupReward.currentValue
			       << ",\"value_after\":" << pickupReward.candidateValue;
			emit("action_result", currentPosition, fields.str());
		}
		if (result.type == PlayerBotProgressionOutcomeType::Succeeded && result.reason && std::strcmp(result.reason, "reward_bundle_claimed") == 0) {
			emit("action_result", currentPosition, "\"action\":\"claim_reward\",\"result\":\"success\",\"candidate_id\":" +
			     std::to_string(pickupReward.uniqueId) + ",\"item_id\":" + std::to_string(pickupReward.itemId) +
			     ",\"root_item_id\":" + std::to_string(pickupReward.rootItemId) + ",\"inventory_before\":" +
			     std::to_string(rewardSession.claimSnapshot().itemCount) + ",\"inventory_after\":" +
			     std::to_string(observation.currentClaim.itemCount) + ",\"root_count_before\":" +
			     std::to_string(rewardSession.claimSnapshot().rootCount) + ",\"root_count_after\":" +
			     std::to_string(observation.currentClaim.rootCount) + ",\"top_level_root_count\":" +
			     std::to_string(pickupReward.rootSignatures.size()) + ",\"all_roots_verified\":true");
		}
		finishProgressionObjective(player, currentPosition, result.type == PlayerBotProgressionOutcomeType::Succeeded ? "success" : "failed", result.reason);
		return;
	}
	if (result.command.type == PlayerBotProgressionCommandType::Use) {
		Item* rewardObject = g_game.getUniqueItem(pickupReward.uniqueId);
		Tile* tile = rewardObject ? rewardObject->getTile() : nullptr;
		if (rewardObject && tile) {
			telemetry.recordActionAttempt();
			g_game.playerUseItem(playerId, pickupReward.itemPosition, static_cast<uint8_t>(tile->getThingIndex(rewardObject)), 0, rewardObject->getClientID());
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Open) {
		if (std::strcmp(result.command.reason, "open_reward_backpack") == 0) openRewardBackpack(*player);
		else openRewardContainer(*player, currentPosition, observation.containerDepth);
	}
	if (result.command.type == PlayerBotProgressionCommandType::Equip) {
		Item* item = nullptr;
		slots_t slot = CONST_SLOT_WHEREEVER;
		if (std::strcmp(result.command.reason, "relocate_reward_root") == 0) {
			item = player->getInventoryItem(pickupReward.slot);
			slot = pickupReward.slot;
		} else if (std::strcmp(result.command.reason, "preserve_displaced_equipment") == 0) {
			Item* reward = rewardItemForAccess(*player);
			for (const auto& [candidateSlot, itemId] : {std::pair<slots_t, uint16_t>{pickupReward.slot, pickupReward.replacedItemId},
			                                            {CONST_SLOT_LEFT, pickupReward.displacedLeftItemId}, {CONST_SLOT_RIGHT, pickupReward.displacedRightItemId}}) {
				Item* candidate = itemId == 0 ? nullptr : player->getInventoryItem(candidateSlot);
				if (candidate && candidate->getID() == itemId && candidate != reward) { item = candidate; slot = candidateSlot; break; }
			}
		} else item = rewardItemForAccess(*player);
		if (item) {
			Position source;
			uint8_t index = 0;
			g_game.internalGetPosition(item, source, index);
			telemetry.recordActionAttempt();
			if (std::strcmp(result.command.reason, "equip_reward") == 0) {
				emit("action_result", currentPosition, "\"action\":\"equip_reward\",\"result\":\"requested\",\"item_id\":" +
				     std::to_string(item->getID()) + ",\"slot\":" + std::to_string(pickupReward.slot) + ",\"source_y\":" +
				     std::to_string(source.y) + ",\"source_z\":" + std::to_string(source.z) + ",\"source_index\":" + std::to_string(index));
				g_game.playerMoveItem(player, source, item->getClientID(), index, Position(0xFFFF, pickupReward.slot, 0), item->getItemCount(), item, nullptr);
			} else {
				Container* backpack = playerBackpack(*player);
				emit("action_result", currentPosition, "\"action\":" + jsonString(result.command.reason) + ",\"result\":\"requested\",\"item_id\":" +
				     std::to_string(item->getID()) + ",\"slot\":" + std::to_string(slot));
				g_game.playerMoveItem(player, source, item->getClientID(), index,
				                      Position(0xFFFF, 0x40 | backpackContainerId, static_cast<uint8_t>(backpack->size())), item->getItemCount(), item,
				                      std::strcmp(result.command.reason, "relocate_reward_root") == 0 ? backpack : nullptr);
			}
		}
	}
	if (result.command.type == PlayerBotProgressionCommandType::Navigate) return;
	const bool actionIssued = result.command.type == PlayerBotProgressionCommandType::Use ||
	                          result.command.type == PlayerBotProgressionCommandType::Open ||
	                          result.command.type == PlayerBotProgressionCommandType::Equip;
	schedule(actionIssued || (result.command.type == PlayerBotProgressionCommandType::None && result.reason &&
	                          std::strcmp(result.reason, "action_unavailable") == 0) ? navigationDecisionDelay(*player) : SCHEDULER_MINTICKS);
}

void PlayerBotController::processProgression(Player* player, const Position& currentPosition)
{
	if (progressionSession.active(PlayerBotProgressionProcedure::OracleDeparture)) {
		processOracleDeparture(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::PickupReward)) {
		processPickupReward(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::LearnSpell)) {
		processSpellTraining(player, currentPosition);
	} else if (progressionSession.active(PlayerBotProgressionProcedure::BuyEquipment)) {
		processEquipmentPurchase(player, currentPosition);
	}
}
