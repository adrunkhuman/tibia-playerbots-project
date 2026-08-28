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
#include "playerbothuntregionadapter.h"
#include "playerbotnpccapabilities.h"
#include "playerbottopology.h"

// Playerbot lifecycle, scheduling, navigation execution, and telemetry.
using namespace playerbot;

namespace {
	constexpr uint32_t maximumNpcTravelApproachPortals = 4;
	constexpr uint32_t npcLocalApproachDistance = 8;
	constexpr size_t maximumNpcLocalReplans = 8;
	constexpr uint64_t maximumNpcLocalPathNodes = 5000;
	constexpr uint16_t shovelToolItemId = 2554;
	constexpr std::array<uint16_t, 4> shovelHoleIds = {468, 481, 483, 7932};

	template<typename T, size_t N>
	bool contains(const std::array<T, N>& values, T value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	Item* usableClosedDoor(Tile& tile, Player& player)
	{
		const TileItemVector* items = tile.getItemList();
		if (!items) return nullptr;
		const auto door = std::find_if(items->begin(), items->end(), [&player](Item* item) {
			if (!item) return false;
			const ItemType& type = Item::items[item->getID()];
			return playerBotIsTraversableDoor(*item) &&
			       type.description != "It is locked." &&
			       (!item->getDoor() || item->getDoor()->canUse(&player));
		});
		return door == items->end() ? nullptr : *door;
	}
}

PlayerBotNavigationCostPolicy PlayerBotController::navigationCostPolicy(const Player& player) const
{
	const PlayerBotEquipmentPlayerSnapshot playerFacts = PlayerBotEquipmentAdapter::player(player);
	const PlayerBotEquipmentLoadout loadout = PlayerBotEquipmentAdapter::loadout(player);
	const PlayerBotCombatProfile combat = equipmentPolicy.combatProfile(playerFacts, loadout);
	auto cache = std::make_shared<std::map<Position, double>>();
	PlayerBotNavigationCostPolicy policy;
	policy.topologyExposureMs = static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(player.getStepDuration()) * 16, std::numeric_limits<uint32_t>::max()));
	policy.expectedHealthLossPerSecond = [combat, cache](const Position& position) {
		if (const auto found = cache->find(position); found != cache->end()) return found->second;
		const double danger = PlayerBotHuntRegionAdapter::travelDanger(combat, position);
		cache->emplace(position, danger);
		return danger;
	};
	return policy;
}

bool PlayerBotController::processNpcApproach(Player* player, const Position& currentPosition, Npc* npc,
	const Position& coarseDestination, bool& unavailable)
{
	unavailable = false;
	if (!npc || npc->isRemoved()) {
		unavailable = true;
		return false;
	}

	const Position npcPosition = npc->getPosition();
	if (npcApproach.npcId != npc->getID() || npcApproach.coarseDestination != coarseDestination) {
		npcApproach.npcId = npc->getID();
		npcApproach.coarseDestination = coarseDestination;
		npcApproach.localDestination.reset();
		npcApproach.initialNpcPosition = npcPosition;
		npcApproach.observedNpcPosition = npcPosition;
		npcApproach.replans = 0;
		npcApproach.local = false;
	}
	if (Position::areInRange<3, 3, 0>(currentPosition, npcPosition)) {
		navigationRuntime.reset();
		return true;
	}

	const bool nearLivePosition = currentPosition.z == npcPosition.z &&
		std::max(Position::getDistanceX(currentPosition, npcPosition),
		         Position::getDistanceY(currentPosition, npcPosition)) <= npcLocalApproachDistance + 3;
	const bool withinRecoveryArea = npcApproach.initialNpcPosition &&
		npcApproach.initialNpcPosition->z == npcPosition.z &&
		std::max(Position::getDistanceX(*npcApproach.initialNpcPosition, npcPosition),
		         Position::getDistanceY(*npcApproach.initialNpcPosition, npcPosition)) <= npcLocalApproachDistance;
	if (!npcApproach.local && !nearLivePosition && currentPosition != coarseDestination) {
		PlayerBotNavigationRuntimeOutcome navigation;
		processNavigation(player, currentPosition, coarseDestination, &navigation);
		unavailable = navigation.fixedTargetRouteFailures >= maximumProgressionAttempts;
		return false;
	}
	if (!npcApproach.local) {
		if (!nearLivePosition || !withinRecoveryArea) {
			unavailable = true;
			return false;
		}
		npcApproach.local = true;
		navigationRuntime.reset();
		if (npcApproach.observedNpcPosition && *npcApproach.observedNpcPosition != npcPosition) {
			++npcApproach.replans;
			emit("npc_approach_replanned", currentPosition,
			     "\"npc_id\":" + std::to_string(npc->getID()) + ",\"reason\":\"provider_moved\"");
		}
		npcApproach.observedNpcPosition = npcPosition;
	}

	if (npcApproach.localDestination &&
	    !Position::areInRange<3, 3, 0>(*npcApproach.localDestination, npcPosition)) {
		if (!nearLivePosition || !withinRecoveryArea || npcApproach.replans >= maximumNpcLocalReplans) {
			unavailable = true;
			return false;
		}
		++npcApproach.replans;
		npcApproach.localDestination.reset();
		npcApproach.observedNpcPosition = npcPosition;
		navigationRuntime.reset();
		emit("npc_approach_replanned", currentPosition,
		     "\"npc_id\":" + std::to_string(npc->getID()) + ",\"reason\":\"provider_moved\"");
	}

	if (!npcApproach.localDestination) {
		npcApproach.localDestination = npcPosition;
	}

	PlayerBotNavigationRuntimeOutcome navigation;
	if (processNavigation(player, currentPosition, PlayerBotNavigationGoal::withinRange(npcPosition, 3, 3),
	                      &navigation, maximumNpcLocalPathNodes, true)) {
		if (Position::areInRange<3, 3, 0>(player->getPosition(), npc->getPosition())) return true;
		npcApproach.localDestination.reset();
		return false;
	}
	if (navigation.routeUnavailable) {
		npcApproach.localDestination.reset();
		navigationRuntime.reset();
		unavailable = true;
	}
	return false;
}

PlayerBotController::PlayerBotController(const Player& player,
	                            std::map<uint64_t, std::chrono::steady_clock::time_point>& sharedHuntRegionCooldowns) :
	playerId(player.getID()), playerGuid(player.getGUID()), playerName(player.getName()), fixtureDriver(playerBotTestPolicyFromEnvironment()),
	telemetry(player.getName(), player.getGUID()),
	equipmentPolicy(oracleVocationId),
	inventoryPolicy(economyCatalog.sellValues(), [this](const Player& candidatePlayer, const Item& item) {
		return equipmentPolicy.evaluateUpgrade(PlayerBotEquipmentAdapter::player(candidatePlayer),
		                                      PlayerBotEquipmentAdapter::loadout(candidatePlayer),
		                                      PlayerBotEquipmentAdapter::item(item)).has_value();
	}), huntCoordinator({
		{traversalCombatTimeout, traversalTargetSuppression, lostTargetPursuitTimeout, lostTargetSuppression,
		 maximumLostTargetPursuitDistance, maximumTargetReacquisitionDistance},
		{maxCorpseSearchAttempts, maximumCorpseNavigationFailures, corpseNavigationSuspendThreshold,
		 std::chrono::milliseconds(corpseNavigationRetryInterval), corpseLootTimeout, preferredFoodCount},
		fixtureDriver.huntPatrol(),
		huntCapacityPressureGrace,
	}, sharedHuntRegionCooldowns)
{}

void PlayerBotController::start(const Position& position, bool recovered, uint32_t recoveryCount)
{
	turnRouter.start();
	lastPosition = position;
	refreshItemValues();
	const bool startInHunt = !recovered && fixtureDriver.startInHunt();
	Player* controlledPlayer = g_game.getPlayerByID(playerId);
	const bool departureComplete = controlledPlayer && departurePlanner.hasCompleted(departureSnapshot(*controlledPlayer));
	const bool departureRequired = controlledPlayer && departurePlanner.required(departureSnapshot(*controlledPlayer));
	const bool useGoalSelector = controlledPlayer && !startInHunt &&
	                             (departureRequired || (!recovered && fixtureDriver.startWithGoalSelection()));
	if (!fixtureDriver.magicTrainingScenario() && !fixtureDriver.deferInitialization() && useGoalSelector &&
	    !selectTopLevelGoal(*controlledPlayer, position, "startup")) {
		return;
	}
	std::ostringstream lifecycle;
	size_t shopProviderCount = 0;
	size_t spellTrainerCount = 0;
	size_t travelOfferCount = 0;
	size_t opaqueTravelOfferCount = 0;
	size_t bankerProviderCount = 0;
	size_t capabilityAuditFindings = 0;
	for (const auto& entry : g_game.getNpcs()) {
		Npc* npc = entry.second;
		if (!npc || npc->isRemoved()) continue;
		const bool shop = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop);
		const bool trainer = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::SpellTrainer);
		const bool travel = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Travel);
		const bool banker = playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Banker);
		shopProviderCount += shop;
		spellTrainerCount += trainer;
		if (travel) travelOfferCount += npc->getTravelOffers().size();
		if (travel) opaqueTravelOfferCount += std::count_if(npc->getTravelOffers().begin(), npc->getTravelOffers().end(),
			[](const NpcTravelOffer& offer) { return offer.hasOpaqueCondition; });
		bankerProviderCount += banker;
		const std::string* metadata = playerBotNpcMetadata(*npc);
		const bool emptyShopDeclaration = metadata && *metadata == "shop" && npc->getShopOffers().empty();
		const bool emptyTrainerDeclaration = metadata && *metadata == "spell_trainer" && npc->getSpellOffers().empty();
		const bool hiddenStructuredCapability = playerBotNpcDisabled(*npc) &&
			(!npc->getShopOffers().empty() || !npc->getSpellOffers().empty() || !npc->getTravelOffers().empty() || npc->isBanker());
		if (emptyShopDeclaration || emptyTrainerDeclaration || hiddenStructuredCapability) {
			++capabilityAuditFindings;
			emit("npc_capability_audit", npc->getPosition(),
			     "\"result\":\"warning\",\"reason\":" + jsonString(hiddenStructuredCapability ? "structured_capability_disabled" : "empty_declared_capability") +
			         ",\"npc_id\":" + std::to_string(npc->getID()) + ",\"npc_name\":" + jsonString(npc->getName()) +
			         ",\"metadata\":" + jsonString(metadata ? *metadata : "") + ",\"shop_offers\":" +
			         std::to_string(npc->getShopOffers().size()) + ",\"spell_offers\":" +
			         std::to_string(npc->getSpellOffers().size()) + ",\"travel_offers\":" +
			         std::to_string(npc->getTravelOffers().size()));
		}
	}
	emit("npc_capability_audit", position,
	     "\"result\":" + jsonString(capabilityAuditFindings == 0 ? "ok" : "warning") +
	         ",\"findings\":" + std::to_string(capabilityAuditFindings) + ",\"shop_providers\":" +
	         std::to_string(shopProviderCount) + ",\"spell_trainers\":" + std::to_string(spellTrainerCount) +
	         ",\"travel_offers\":" + std::to_string(travelOfferCount) + ",\"bankers\":" +
	         std::to_string(bankerProviderCount));
	lifecycle << "\"status\":\"online\",\"message\":\"Playerbot online\""
	          << ",\"recovered\":" << (recovered ? "true" : "false")
	          << ",\"recovery_count\":" << recoveryCount
	          << ",\"objective\":" << jsonString((fixtureDriver.magicTrainingScenario() || fixtureDriver.deferInitialization()) ? "fixture_pending" : useGoalSelector ? PlayerBotGoalArbiter::goalName(progressionRuntime.activeGoal()) :
	                                                    (startInHunt ? "hunt" : "service"))
	          << ",\"step_speed\":" << (g_game.getPlayerByID(playerId) ? g_game.getPlayerByID(playerId)->getSpeed() : 0)
		          << ",\"spell_calibration_profiles\":" << survivalRuntime.calibrationSize()
		          << ",\"shop_providers\":" << shopProviderCount
		          << ",\"spell_trainers\":" << spellTrainerCount
		          << ",\"travel_offers\":" << travelOfferCount
		          << ",\"opaque_travel_offers\":" << opaqueTravelOfferCount
		          << ",\"bankers\":" << bankerProviderCount;
	telemetry.emit("lifecycle", position, lifecycle.str());
	if (controlledPlayer) {
		emitFixtureEvents(fixtureDriver.runSpellCalibration(*controlledPlayer), position);
		emitFixtureEvents(fixtureDriver.runAdaptiveChallenge(*controlledPlayer), position);
	}
	if (fixtureDriver.magicTrainingScenario() || fixtureDriver.deferInitialization()) {
		fixtureDriver.beginDelayedInitialization();
	} else if (useGoalSelector) {
		// The selected goal initialized its own executor state.
	} else if (startInHunt) {
		progressionRuntime.enterHunt();
		startHunt(g_game.getPlayerByID(playerId), position, "focused_fixture");
	} else if (fixtureDriver.depotScenario()) {
		progressionRuntime.enterService();
		turnRouter.setCyclePhase(CyclePhase::ReturnToDepot);
	} else {
		beginService(controlledPlayer, position, "startup");
	}
	setStage(ScenarioStage::Traverse, position);
	// Login fixtures run through Lua after controller creation. Let the real-map
	// depot fixture establish its Naji position before its first discovery pass.
	schedule(fixtureDriver.depotScenario() ? 2000 : fixtureDriver.magicTrainingScenario() ? 1000 : navigationInterval);
}

void PlayerBotController::schedule(uint32_t interval)
{
	if (!turnRouter.running()) return;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval);
	if (scheduledTurnEvent != 0) {
		if (scheduledTurnDeadline <= deadline) return;
		g_scheduler.stopEvent(scheduledTurnEvent);
	}
	const uint64_t generation = ++scheduledTurnGeneration;
	scheduledTurnDeadline = deadline;
	std::weak_ptr<PlayerBotController> weakController = shared_from_this();
	scheduledTurnEvent = g_scheduler.addEvent(createSchedulerTask(interval, ([weakController, generation]() {
		if (std::shared_ptr<PlayerBotController> controller = weakController.lock()) {
			if (controller->scheduledTurnGeneration != generation) return;
			controller->scheduledTurnEvent = 0;
			controller->navigate();
		}
	})));
}

const char* PlayerBotController::stageName(ScenarioStage stage)
{
	return PlayerBotTurnRouter::scenarioStageName(stage);
}

void PlayerBotController::say(Player& player, const std::string& text) const
{
	Player* admin = g_game.getPlayerByName("GOD Admin");
	if (!admin || admin->isRemoved()) {
		return;
	}
	admin->sendTextMessage(MESSAGE_STATUS_CONSOLE_ORANGE, "[Bot One] " + text);
	admin->sendPrivateMessage(&player, TALKTYPE_PRIVATE, text);
}

void PlayerBotController::setStage(ScenarioStage stage, const Position& position)
{
	if (turnRouter.scenarioStage() == stage) {
		return;
	}

	const ScenarioStage previousStage = turnRouter.scenarioStage();
	turnRouter.setScenarioStage(stage);
	const std::string repeatKey = std::string("state:") + stageName(previousStage) + ':' + stageName(stage);
	if (!telemetry.shouldEmitRepeated(repeatKey)) {
		return;
	}
	telemetry.emit("state_transition", position, std::string("\"from\":") + jsonString(stageName(previousStage)) +
	     ",\"to\":" + jsonString(stageName(stage)));
}

std::optional<PlayerBotTraversalTarget> PlayerBotController::clearTraversalTarget(const Position& position, const char* reason)
{
	const auto target = huntCoordinator.clearTraversalTarget();
	if (!target) {
		return std::nullopt;
	}

	if (!telemetry.shouldEmitRepeated(std::string("target:clear:") + reason)) {
		return target;
	}
	telemetry.emit("target_changed", position, "\"previous_target_id\":" + std::to_string(target->id) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	return target;
}

Item* PlayerBotController::findActionableSlottedItem(const Player& player, uint16_t itemId, slots_t& slot) const
{
	for (int32_t slotIndex = CONST_SLOT_FIRST; slotIndex <= CONST_SLOT_LAST; ++slotIndex) {
		const slots_t candidateSlot = static_cast<slots_t>(slotIndex);
		Item* item = player.getInventoryItem(candidateSlot);
		if (!item || !inventoryPolicy.isActionableSlottedItem(player, *item, candidateSlot, itemId)) {
			continue;
		}
		slot = candidateSlot;
		return item;
	}
	return nullptr;
}

uint32_t PlayerBotController::getSaleItemCount(const Player& player, uint16_t itemId) const
{
	uint32_t count = inventoryPolicy.backpackSaleItemCount(player, itemId);
	slots_t slot = CONST_SLOT_WHEREEVER;
	if (Item* slotted = findActionableSlottedItem(player, itemId, slot)) {
		count += slotted->getItemCount();
	}
	return count;
}

playerbot::PlayerBotTelemetrySummary PlayerBotController::telemetrySummary() const
{
	playerbot::PlayerBotTelemetrySummary summary{turnRouter.stateName(), std::nullopt};
	if (const auto activeTarget = huntCoordinator.activeTarget()) {
		summary.target = playerbot::PlayerBotTelemetryTarget{activeTarget->id, activeTarget->position};
	}
	return summary;
}

void PlayerBotController::emitFixtureEvents(const std::vector<playerbot::PlayerBotFixtureEvent>& events, const Position& position) const
{
	for (const auto& event : events) emit(event.name, position, event.fields);
}

void PlayerBotController::stop(const char* reason, const Position& position)
{
	if (telemetry.terminalLogged()) {
		return;
	}

	const bool wasRunning = turnRouter.running();
	const char* previous = turnRouter.stateName();
	turnRouter.stop();
	if (scheduledTurnEvent != 0) {
		g_scheduler.stopEvent(scheduledTurnEvent);
		scheduledTurnEvent = 0;
		++scheduledTurnGeneration;
	}
	huntCoordinator.cancelPlanning();
	resetNavigation();
	if (wasRunning) {
		const std::string repeatKey = std::string("state:") + previous + ':' + turnRouter.stateName();
		if (telemetry.shouldEmitRepeated(repeatKey)) {
			telemetry.emit("state_transition", position, std::string("\"from\":") + jsonString(previous) +
			     ",\"to\":" + jsonString(turnRouter.stateName()));
		}
	}
	telemetry.emitTerminal(reason, position, telemetrySummary());
}

void PlayerBotController::pause(const Position& position)
{
	if (!turnRouter.running()) return;
	const char* previous = turnRouter.stateName();
	turnRouter.pause();
	const std::string repeatKey = std::string("state:") + previous + ':' + turnRouter.stateName();
	if (telemetry.shouldEmitRepeated(repeatKey)) {
		telemetry.emit("state_transition", position, std::string("\"from\":") + jsonString(previous) +
		     ",\"to\":" + jsonString(turnRouter.stateName()));
	}
}

bool PlayerBotController::findPath(Player* player, const Position& target, std::vector<Direction>& result, const FindPathParams& pathParams)
{
	const auto startedAt = std::chrono::steady_clock::now();
	const bool found = player->getPathTo(target, result, pathParams);
	telemetry.recordPathfinding(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - startedAt), found);
	return found;
}

void PlayerBotController::resetNavigation()
{
	navigationRuntime.reset();
}

void PlayerBotController::observeNavigationPlan(const Position& destination, std::deque<PlayerBotNavigationStep> steps)
{
	PlayerBotNavigationRoutePlan plan;
	plan.metrics.result = PlayerBotNavigationResult::Reached;
	plan.metrics.steps = steps.size();
	plan.steps = std::move(steps);
	navigationRuntime.observePlan({PlayerBotNavigationGoal::exact(destination), std::move(plan), true, true, std::chrono::steady_clock::now()});
}

void PlayerBotController::onDeath(const Player& player, const Creature* killer, const Creature* mostDamageKiller)
{
	if (deathObserved || player.getID() != playerId) {
		return;
	}
	deathObserved = true;
	lastPosition = player.getPosition();
	if (huntCoordinator.huntActive() && turnRouter.cyclePhase() == CyclePhase::Hunt &&
	    (turnRouter.scenarioStage() == ScenarioStage::TraversalCombat ||
	     turnRouter.scenarioStage() == ScenarioStage::TargetPursuit)) {
		const auto now = std::chrono::steady_clock::now();
		huntCoordinator.observeHuntDeath(true, now, huntRegionCooldown);
	} else {
		const auto now = std::chrono::steady_clock::now();
		huntCoordinator.observeHuntDeath(false, now, huntRegionCooldown);
	}
	finishHuntRegion(player, lastPosition, "death");
	std::ostringstream fields;
	fields << "\"status\":\"dead\",\"level\":" << player.getLevel()
	       << ",\"health\":" << player.getHealth() << ",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(turnRouter.stateName())
	       << ",\"target_id\":";
	const auto activeTarget = huntCoordinator.activeTarget();
	fields << (activeTarget ? std::to_string(activeTarget->id) : "null");
	fields << ",\"killer_id\":" << (killer ? std::to_string(killer->getID()) : "null")
	       << ",\"killer_name\":" << (killer ? jsonString(killer->getName()) : "null")
	       << ",\"killer_type\":" << (killer ? jsonString(killer->getPlayer() ? "player" : killer->getMonster() ? "monster" : "other") : "null")
	       << ",\"most_damage_id\":" << (mostDamageKiller ? std::to_string(mostDamageKiller->getID()) : "null")
	       << ",\"most_damage_name\":" << (mostDamageKiller ? jsonString(mostDamageKiller->getName()) : "null");
	telemetry.emit("lifecycle", lastPosition, fields.str());
}

Item* PlayerBotController::findNavigationItem(const PlayerBotNavigationStep& step) const
{
	Tile* tile = g_game.map.getTile(step.target);
	if (!tile) {
		return nullptr;
	}
	if (Item* ground = tile->getGround(); ground && ground->getID() == step.itemId) {
		return ground;
	}
	if (TileItemVector* items = tile->getItemList()) {
		for (Item* item : *items) {
			if (item->getID() == step.itemId) {
				return item;
			}
		}
	}
	return nullptr;
}

PlayerBotNavigationStep PlayerBotController::resolveTopologyPortal(
	Player& player, const PlayerBotNavigationStep& portal,
	const std::set<Position>& blockedPositions) const
{
	PlayerBotNavigationStep step = portal;
	Tile* tile = g_game.map.getTile(portal.target);
	if (!tile) return step;
	if (Item* door = usableClosedDoor(*tile, player)) {
		step.action = PlayerBotNavigationAction::UseDoor;
		step.itemId = door->getID();
		return step;
	}
	if (portal.action == PlayerBotNavigationAction::UseShovel) {
		Item* ground = tile->getGround();
		if (ground && contains(shovelHoleIds, ground->getID())) {
			step.itemId = ground->getID();
			return step;
		}
	}
	if (portal.action == PlayerBotNavigationAction::Move ||
	    portal.action == PlayerBotNavigationAction::UseDoor ||
	    portal.action == PlayerBotNavigationAction::UseShovel) {
		const Position currentPosition = player.getPosition();
		const Direction direction = getDirectionTo(currentPosition, portal.target);
		PlayerBotNavigationStep move;
		if (getNextPosition(direction, currentPosition) == portal.target &&
		    PlayerBotNavigator().resolveMove(player, currentPosition, direction, blockedPositions, move) &&
		    (portal.action != PlayerBotNavigationAction::UseShovel ||
		     move.expectedPosition == portal.expectedPosition)) {
			move.topologyPortal = true;
			return move;
		}
	}
	return step;
}

bool PlayerBotController::executeNavigationStep(Player* player, const PlayerBotNavigationStep& step)
{
	telemetry.recordActionAttempt();
	if (step.action == PlayerBotNavigationAction::Move) {
		if (!fixtureDriver.navigationStepCommand().dispatch) {
			return true;
		}
		g_game.playerMove(playerId, step.direction);
		return true;
	}
	if (step.action == PlayerBotNavigationAction::NpcTravel) {
		if (player->isPzLocked()) return false;
		Npc* npc = g_game.getNpcByID(step.npcId);
		const bool offerAvailable = npc && playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Travel) &&
			std::any_of(npc->getTravelOffers().begin(), npc->getTravelOffers().end(), [&step](const NpcTravelOffer& offer) {
				return offer.destination == step.expectedPosition && offer.dialogue == step.dialogue && !offer.hasOpaqueCondition &&
				       !offer.hasOpaqueAction && offer.price == step.price && offer.level == step.minimumLevel &&
				       offer.premium == step.premium;
			});
		if (!offerAvailable || player->getLevel() < step.minimumLevel || (step.premium && !player->isPremium()) ||
		    player->getMoney() + player->getBankBalance() < step.price || npc->getPosition().z != player->getPosition().z ||
		    std::max(Position::getDistanceX(npc->getPosition(), player->getPosition()),
		             Position::getDistanceY(npc->getPosition(), player->getPosition())) > 3) return false;
		npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, "hi");
		for (const std::string& phrase : step.dialogue) {
			npc->receiveSpeech(player, TALKTYPE_PRIVATE_PN, phrase);
		}
		const bool travelled = player->getPosition() == step.expectedPosition;
		if (!travelled) {
			unavailableTravelOffers[{step.npcId, step.expectedPosition}] =
				std::chrono::steady_clock::now() + std::chrono::minutes(5);
		}
		telemetry.emit("npc_travel", player->getPosition(),
		     std::string("\"result\":") + jsonString(travelled ? "success" : "failed") +
		         ",\"npc_id\":" + std::to_string(step.npcId) +
		         ",\"destination\":{\"x\":" + std::to_string(step.expectedPosition.x) +
		         ",\"y\":" + std::to_string(step.expectedPosition.y) + ",\"z\":" +
		         std::to_string(static_cast<uint16_t>(step.expectedPosition.z)) + "}");
		return travelled;
	}

	Item* target = findNavigationItem(step);
	Tile* tile = g_game.map.getTile(step.target);
	const int32_t stackPosition = target && tile ? tile->getThingIndex(target) : -1;
	if (!target || stackPosition < 0 || stackPosition > UINT8_MAX) {
		return false;
	}

	if (step.action == PlayerBotNavigationAction::UseRope ||
	    step.action == PlayerBotNavigationAction::UseShovel) {
		const uint16_t toolId = step.action == PlayerBotNavigationAction::UseRope ? ropeItemId : 2554;
		Item* tool = g_game.findItemOfType(player, toolId, true);
		if (!tool) {
			return false;
		}
		g_game.playerUseItemEx(playerId, Position(0xFFFF, 0, 0), 0, tool->getClientID(), step.target,
		                         static_cast<uint8_t>(stackPosition), target->getClientID());
		return true;
	}

	g_game.playerUseItem(playerId, step.target, static_cast<uint8_t>(stackPosition), 0, target->getClientID());
	return true;
}

uint32_t PlayerBotController::navigationDecisionDelay(const Player& player) const
{
	const uint32_t walkDelay = static_cast<uint32_t>(std::max<int32_t>(0, player.getWalkDelay()));
	return std::max<uint32_t>(SCHEDULER_MINTICKS, std::max(walkDelay, player.getNextActionTime()));
}

void PlayerBotController::onHealthDrain(const Player& player, uint32_t damage)
{
	survivalRuntime.observeHealthDrain(player.getID() == playerId);
	if (player.getID() == playerId && isActiveHuntCombat(player)) {
		huntCoordinator.observeHuntDamage(damage);
	}
}

void PlayerBotController::onCombatDamage(Creature* attacker, const Creature& target, uint32_t damage)
{
	survivalRuntime.observeCombatDamage(attacker ? attacker->getID() : 0, target.getID(), playerId, damage);
}

PlayerBotNavigationRoutePlan PlayerBotController::planNavigationRoute(Player& player, const Position& destination,
	                                                                    const std::set<Position>& blockedPositions,
	                                                                    uint64_t maximumExpandedNodes) const

{
	return planNavigationRoute(player, PlayerBotNavigationGoal::exact(destination), blockedPositions, maximumExpandedNodes);
}

PlayerBotNavigationRoutePlan PlayerBotController::planCompleteNavigationRoute(
	Player& player, const Position& destination, const std::set<Position>& blockedPositions,
	uint64_t maximumExpandedNodes) const

{
	return planCompleteNavigationRoute(player, player.getPosition(), destination, blockedPositions, maximumExpandedNodes);
}

PlayerBotNavigationRoutePlan PlayerBotController::planCompleteNavigationRoute(
	Player& player, const Position& start, const Position& destination, const std::set<Position>& blockedPositions,
	uint64_t maximumExpandedNodes) const
{
	PlayerBotNavigationRoutePlan plan;
	plan.metrics.attempted = true;
	plan.metrics.result = PlayerBotNavigationResult::Reached;
	plan.metrics.waypoint = destination;
	const auto startedAt = std::chrono::steady_clock::now();
	const PlayerBotNavigationCostPolicy costPolicy = navigationCostPolicy(player);
	plan.metrics.dangerAware = costPolicy.enabled();
	const PlayerBotNavigator navigator;
	const PlayerBotTopology& topology = PlayerBotTopology::instance();
	const bool canUseRope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
	const bool canUseShovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	Position segmentStart = start;
	std::set<Position> portalDestinations;
	uint64_t travelMs = 0;
	auto stepDuration = [&player](const PlayerBotNavigationStep& step) -> uint32_t {
		return step.action == PlayerBotNavigationAction::Move ? player.getStepDuration(step.direction) : 1000;
	};
	while (segmentStart != destination) {
		const auto topologyRoute = topology.route(segmentStart, destination, blockedPositions, canUseRope,
		                                          canUseShovel, player.getLevel(), &costPolicy);
		if (!topologyRoute) {
			plan.metrics.result = PlayerBotNavigationResult::Unreachable;
			break;
		}
		std::deque<PlayerBotNavigationStep> segment;
		uint64_t segmentNodes = 0;
		PlayerBotNavigationCostSummary segmentCost;
		const PlayerBotNavigationResult result = navigator.planFrom(
		    player, segmentStart, topologyRoute->waypoint, blockedPositions, segment, segmentNodes,
		    maximumExpandedNodes > plan.metrics.expandedNodes ? maximumExpandedNodes - plan.metrics.expandedNodes : 0,
		    &plan.metrics.closestPosition, &costPolicy, &segmentCost);
		plan.metrics.expandedNodes += segmentNodes;
		if (result != PlayerBotNavigationResult::Reached) {
			plan.metrics.result = result;
			break;
		}
		plan.metrics.movementCost = static_cast<uint32_t>(std::min<uint64_t>(
		    static_cast<uint64_t>(plan.metrics.movementCost) + segmentCost.movementCost,
		    std::numeric_limits<uint32_t>::max()));
		plan.metrics.dangerCost = static_cast<uint32_t>(std::min<uint64_t>(
		    static_cast<uint64_t>(plan.metrics.dangerCost) + segmentCost.dangerCost,
		    std::numeric_limits<uint32_t>::max()));
		plan.metrics.maximumHealthLossPerSecond = std::max(
		    plan.metrics.maximumHealthLossPerSecond, segmentCost.maximumHealthLossPerSecond);
		for (const PlayerBotNavigationStep& step : segment) travelMs += stepDuration(step);
		plan.steps.insert(plan.steps.end(), segment.begin(), segment.end());
		if (!topologyRoute->portal) {
			segmentStart = topologyRoute->waypoint;
			continue;
		}
		const PlayerBotTopologyPortal& portal = *topologyRoute->portal;
		if (!portalDestinations.insert(portal.destination).second) {
			plan.metrics.result = PlayerBotNavigationResult::Unreachable;
			break;
		}
		PlayerBotNavigationStep step;
		switch (portal.action) {
			case PlayerBotTopologyPortalAction::Move: step.action = PlayerBotNavigationAction::Move; break;
			case PlayerBotTopologyPortalAction::Use: step.action = PlayerBotNavigationAction::Use; break;
			case PlayerBotTopologyPortalAction::UseDoor: step.action = PlayerBotNavigationAction::UseDoor; break;
			case PlayerBotTopologyPortalAction::UseRope: step.action = PlayerBotNavigationAction::UseRope; break;
			case PlayerBotTopologyPortalAction::UseShovel: step.action = PlayerBotNavigationAction::UseShovel; break;
		}
		step.direction = portal.direction;
		step.target = portal.target;
		step.expectedPosition = portal.destination;
		step.itemId = portal.itemId;
		step.topologyPortal = true;
		plan.steps.push_back(step);
		const uint32_t portalExposureMs = stepDuration(step);
		travelMs += portalExposureMs;
		plan.metrics.dangerCost = static_cast<uint32_t>(std::min<uint64_t>(
		    static_cast<uint64_t>(plan.metrics.dangerCost) +
		        costPolicy.dangerCost(portal.destination, portalExposureMs),
		    std::numeric_limits<uint32_t>::max()));
		plan.metrics.maximumHealthLossPerSecond = std::max(
		    plan.metrics.maximumHealthLossPerSecond, costPolicy.dangerAt(portal.destination));
		segmentStart = portal.destination;
		if (plan.metrics.expandedNodes >= maximumExpandedNodes) {
			plan.metrics.result = PlayerBotNavigationResult::NodeLimit;
			break;
		}
	}
	plan.metrics.steps = plan.steps.size();
	plan.metrics.estimatedTravelSeconds = travelMs / 1000.0;
	plan.metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - startedAt);
	return plan;
}

PlayerBotNavigationRoutePlan PlayerBotController::planNavigationRoute(Player& player, const PlayerBotNavigationGoal& goal,
	                                                                    const std::set<Position>& blockedPositions,
	                                                                    uint64_t maximumExpandedNodes) const
{
	PlayerBotNavigationRoutePlan routePlan;
	routePlan.metrics.attempted = true;
	const auto startedAt = std::chrono::steady_clock::now();
	const PlayerBotNavigator navigator;
	const PlayerBotNavigationCostPolicy costPolicy = navigationCostPolicy(player);
	PlayerBotNavigationCostSummary costSummary;
	if (goal.type != PlayerBotNavigationGoalType::Exact) {
		routePlan.metrics.waypoint = goal.representative();
		routePlan.metrics.result = navigator.plan(player, goal, blockedPositions, routePlan.steps,
		                                          routePlan.metrics.expandedNodes, maximumExpandedNodes,
		                                          &routePlan.metrics.closestPosition, &costPolicy, &costSummary);
		routePlan.metrics.movementCost = costSummary.movementCost;
		routePlan.metrics.dangerCost = costSummary.dangerCost;
		routePlan.metrics.maximumHealthLossPerSecond = costSummary.maximumHealthLossPerSecond;
		routePlan.metrics.dangerAware = costPolicy.enabled();
		routePlan.metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - startedAt);
		routePlan.metrics.steps = routePlan.steps.size();
		return routePlan;
	}
	const Position destination = goal.position;
	const bool travelEligible = maximumExpandedNodes == playerBotNavigationMaximumExpandedNodes && !player.isPzLocked();
	const PlayerBotTopology& topology = PlayerBotTopology::instance();
	const bool canUseRope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
	const bool canUseShovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	const std::optional<PlayerBotTopologyRoute> topologyRoute =
	    topology.route(player.getPosition(), destination, blockedPositions, canUseRope, canUseShovel, player.getLevel(), &costPolicy);
	const PlayerBotTopologyDistances coarseDistances = topology.distancesFrom(
	    player.getPosition(), canUseRope, canUseShovel, player.getLevel());
	const std::optional<uint32_t> coarseDistance = topology.distanceTo(coarseDistances, destination);
	if (travelEligible && (!topologyRoute || (coarseDistance && *coarseDistance > maximumNpcTravelApproachPortals))) {
		if (auto travelRoute = planNpcTravelRoute(player, destination, blockedPositions, maximumExpandedNodes)) {
			const size_t directSteps = coarseDistance ?
			    std::max<size_t>(playerBotNavigationDistance(player.getPosition(), destination),
			                     static_cast<size_t>(*coarseDistance) * 32) :
			    std::numeric_limits<size_t>::max();
			const double directTravelSeconds = directSteps == std::numeric_limits<size_t>::max() ?
			    std::numeric_limits<double>::infinity() : directSteps * player.getStepDuration() / 1000.0;
			if (travelRoute->metrics.estimatedTravelSeconds < directTravelSeconds) {
				travelRoute->metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				    std::chrono::steady_clock::now() - startedAt);
				travelRoute->metrics.attempted = true;
				if (travelRoute->metrics.steps == 0) travelRoute->metrics.steps = travelRoute->steps.size();
				return *travelRoute;
			}
		}
	}
	const Position localDestination = topologyRoute ? topologyRoute->waypoint : destination;
	routePlan.metrics.waypoint = localDestination;
	routePlan.metrics.result = navigator.plan(player, localDestination, blockedPositions, routePlan.steps,
	                                          routePlan.metrics.expandedNodes, maximumExpandedNodes,
	                                          &routePlan.metrics.closestPosition, &costPolicy, &costSummary);
	routePlan.metrics.movementCost = costSummary.movementCost;
	routePlan.metrics.dangerCost = topologyRoute ? static_cast<uint32_t>(std::min<uint64_t>(
	    static_cast<uint64_t>(costSummary.dangerCost) + topologyRoute->dangerCost,
	    std::numeric_limits<uint32_t>::max())) : costSummary.dangerCost;
	routePlan.metrics.maximumHealthLossPerSecond = topologyRoute ?
	    std::max(costSummary.maximumHealthLossPerSecond, topologyRoute->maximumHealthLossPerSecond) :
	    costSummary.maximumHealthLossPerSecond;
	routePlan.metrics.dangerAware = costPolicy.enabled();
	if (routePlan.metrics.result == PlayerBotNavigationResult::Reached && topologyRoute && topologyRoute->portal) {
		const PlayerBotTopologyPortal& portal = *topologyRoute->portal;
		PlayerBotNavigationStep step;
		switch (portal.action) {
			case PlayerBotTopologyPortalAction::Move: step.action = PlayerBotNavigationAction::Move; break;
			case PlayerBotTopologyPortalAction::Use: step.action = PlayerBotNavigationAction::Use; break;
			case PlayerBotTopologyPortalAction::UseDoor: step.action = PlayerBotNavigationAction::UseDoor; break;
			case PlayerBotTopologyPortalAction::UseRope: step.action = PlayerBotNavigationAction::UseRope; break;
			case PlayerBotTopologyPortalAction::UseShovel: step.action = PlayerBotNavigationAction::UseShovel; break;
		}
		step.direction = portal.direction;
		step.target = portal.target;
		step.expectedPosition = portal.destination;
		step.itemId = portal.itemId;
		step.topologyPortal = true;
		routePlan.steps.push_back(step);
	}
	if (routePlan.metrics.result != PlayerBotNavigationResult::Reached && travelEligible && topologyRoute) {
		if (auto travelRoute = planNpcTravelRoute(player, destination, blockedPositions, maximumExpandedNodes)) {
			travelRoute->metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - startedAt);
			travelRoute->metrics.attempted = true;
			travelRoute->metrics.expandedNodes += routePlan.metrics.expandedNodes;
			if (travelRoute->metrics.steps == 0) travelRoute->metrics.steps = travelRoute->steps.size();
			return *travelRoute;
		}
	}
	routePlan.metrics.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - startedAt);
	if (routePlan.metrics.result == PlayerBotNavigationResult::Reached && topologyRoute && topologyRoute->portal && coarseDistance) {
		routePlan.metrics.steps = std::max<size_t>(playerBotNavigationDistance(player.getPosition(), destination),
		                                         static_cast<size_t>(*coarseDistance) * 32);
		routePlan.metrics.estimatedTravelSeconds = routePlan.metrics.steps * player.getStepDuration() / 1000.0;
	} else {
		routePlan.metrics.steps = routePlan.steps.size();
	}
	return routePlan;
}

std::optional<PlayerBotNavigationRoutePlan> PlayerBotController::planNpcTravelRoute(
	Player& player, const Position& destination, const std::set<Position>& blockedPositions,
	uint64_t maximumExpandedNodes) const
{
	if (player.isPzLocked()) return std::nullopt;

	struct Edge {
		Npc* npc = nullptr;
		const NpcTravelOffer* offer = nullptr;
	};
	std::vector<Edge> edges;
	std::vector<Position> states{player.getPosition()};
	std::map<Position, size_t> stateIndices{{player.getPosition(), 0}};
	const Position currentPosition = player.getPosition();
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Travel, currentPosition)) {
		for (const NpcTravelOffer& offer : npc->getTravelOffers()) {
			if ((offer.level != 0 && player.getLevel() < offer.level) || (offer.premium && !player.isPremium()) ||
			    offer.hasOpaqueCondition || offer.hasOpaqueAction || offer.destination == currentPosition ||
			    blockedPositions.find(npc->getPosition()) != blockedPositions.end()) continue;
			const auto unavailable = unavailableTravelOffers.find({npc->getID(), offer.destination});
			if (unavailable != unavailableTravelOffers.end() && unavailable->second > std::chrono::steady_clock::now()) continue;
			edges.push_back({npc, &offer});
			if (stateIndices.find(offer.destination) == stateIndices.end()) {
				stateIndices[offer.destination] = states.size();
				states.push_back(offer.destination);
			}
		}
	}
	if (edges.empty()) return std::nullopt;

	struct QueueEntry {
		uint64_t cost = 0;
		uint64_t fare = 0;
		uint32_t dangerCost = 0;
		double maximumDanger = 0;
		uint32_t routeSteps = 0;
		double travelSeconds = 0;
		size_t state = 0;
		size_t firstEdge = std::numeric_limits<size_t>::max();
	};
	struct SegmentEstimate {
		uint32_t stepCount = 0;
		uint32_t movementCost = 0;
		uint32_t dangerCost = 0;
		double maximumDanger = 0;
		double travelSeconds = 0;
		std::deque<PlayerBotNavigationStep> steps;
		Position destination;
		bool coarse = false;
	};
	auto worse = [](const QueueEntry& left, const QueueEntry& right) { return left.cost > right.cost; };
	std::priority_queue<QueueEntry, std::vector<QueueEntry>, decltype(worse)> queue(worse);
	std::vector<std::vector<std::pair<uint64_t, uint64_t>>> labels(states.size());
	labels[0].push_back({0, 0});
	queue.push({0, 0, 0, 0, 0, 0, 0, std::numeric_limits<size_t>::max()});
	const uint64_t availableMoney = player.getMoney() + player.getBankBalance();
	const PlayerBotNavigator navigator;
	const PlayerBotNavigationCostPolicy costPolicy = navigationCostPolicy(player);
	const uint64_t graphNodeBudget = maximumExpandedNodes;
	constexpr uint64_t segmentNodeBudget = 30000;
	uint64_t graphExpandedNodes = 0;
	std::map<std::pair<size_t, uint32_t>, std::optional<SegmentEstimate>> connections;
	std::vector<std::optional<PlayerBotTopologyDistances>> coarseDistanceCache;
	auto travelSeconds = [&player](const std::deque<PlayerBotNavigationStep>& steps) {
		double seconds = 0;
		for (const PlayerBotNavigationStep& step : steps) {
			seconds += step.action == PlayerBotNavigationAction::Move ? player.getStepDuration(step.direction) / 1000.0 : 1.0;
		}
		return seconds;
	};
	auto movementCost = [](const std::deque<PlayerBotNavigationStep>& steps) {
		uint32_t cost = 0;
		for (const PlayerBotNavigationStep& step : steps) {
			cost += step.action == PlayerBotNavigationAction::Move &&
			        (step.direction & DIRECTION_DIAGONAL_MASK) != 0 ? 3 : 1;
		}
		return cost;
	};
	auto walkSegment = [&](size_t stateIndex, size_t edgeIndex) -> std::optional<SegmentEstimate> {
		const Edge& edge = edges[edgeIndex];
		const auto key = std::make_pair(stateIndex, edge.npc->getID());
		if (auto cached = connections.find(key); cached != connections.end()) return cached->second;
		if (coarseDistanceCache.size() <= stateIndex) coarseDistanceCache.resize(stateIndex + 1);
		if (!coarseDistanceCache[stateIndex]) {
			coarseDistanceCache[stateIndex] = PlayerBotTopology::instance().distancesFrom(
			    states[stateIndex], g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr,
			    g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr, player.getLevel());
		}
		for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) {
			for (int32_t yOffset = -1; yOffset <= 1; ++yOffset) {
				if (xOffset == 0 && yOffset == 0) continue;
				const Position approach(edge.npc->getPosition().x + xOffset, edge.npc->getPosition().y + yOffset,
				                        edge.npc->getPosition().z);
				Tile* tile = g_game.map.getTile(approach);
				if (!tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR) continue;
				const uint32_t approachDistance = playerBotNavigationDistance(states[stateIndex], approach);
				const std::optional<uint32_t> portalDistance = PlayerBotTopology::instance().distanceTo(
				    *coarseDistanceCache[stateIndex], approach);
				if (portalDistance && *portalDistance <= maximumNpcTravelApproachPortals) {
					const auto topologyRoute = PlayerBotTopology::instance().route(
					    states[stateIndex], approach, blockedPositions,
					    g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr,
					    g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr,
					    player.getLevel(), &costPolicy);
					if (!topologyRoute) continue;
					const uint32_t estimatedSteps = std::max(playerBotNavigationDistance(states[stateIndex], approach),
					                                         *portalDistance * 32);
					SegmentEstimate estimate{estimatedSteps, estimatedSteps, topologyRoute->dangerCost,
					                         topologyRoute->maximumHealthLossPerSecond,
					                         estimatedSteps * player.getStepDuration() / 1000.0,
					                         {}, approach, true};
					connections.emplace(key, estimate);
					return estimate;
				}
				if (approachDistance > 512) continue;
				if (graphExpandedNodes >= graphNodeBudget) return std::nullopt;
				std::deque<PlayerBotNavigationStep> steps;
				uint64_t expandedNodes = 0;
				PlayerBotNavigationCostSummary segmentCost;
				const PlayerBotNavigationResult result = navigator.planFrom(
					player, states[stateIndex], approach, blockedPositions, steps, expandedNodes,
					std::min(segmentNodeBudget, graphNodeBudget - graphExpandedNodes), nullptr, &costPolicy, &segmentCost);
				graphExpandedNodes += expandedNodes;
				if (result == PlayerBotNavigationResult::Reached) {
					SegmentEstimate estimate{static_cast<uint32_t>(steps.size()), movementCost(steps), segmentCost.dangerCost,
					                         segmentCost.maximumHealthLossPerSecond, travelSeconds(steps), std::move(steps),
					                         approach, false};
					connections.emplace(key, estimate);
					return estimate;
				}
			}
		}
		connections.emplace(key, std::nullopt);
		return std::nullopt;
	};
	size_t selectedEdge = std::numeric_limits<size_t>::max();
	uint64_t selectedCost = std::numeric_limits<uint64_t>::max();
	uint32_t selectedDangerCost = 0;
	double selectedMaximumDanger = 0;
	uint32_t selectedRouteSteps = 0;
	double selectedTravelSeconds = 0;
	while (!queue.empty()) {
		const QueueEntry current = queue.top();
		queue.pop();
		if (current.cost >= selectedCost) break;
		const auto& currentLabels = labels[current.state];
		if (std::find(currentLabels.begin(), currentLabels.end(), std::pair{current.cost, current.fare}) == currentLabels.end()) continue;
		if (current.state != 0) {
			if (coarseDistanceCache.size() <= current.state) coarseDistanceCache.resize(current.state + 1);
			if (!coarseDistanceCache[current.state]) {
				coarseDistanceCache[current.state] = PlayerBotTopology::instance().distancesFrom(
				    states[current.state], g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr,
				    g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr, player.getLevel());
			}
			const std::optional<uint32_t> destinationPortals = PlayerBotTopology::instance().distanceTo(
			    *coarseDistanceCache[current.state], destination);
			if (destinationPortals && *destinationPortals <= maximumNpcTravelApproachPortals) {
				const auto topologyRoute = PlayerBotTopology::instance().route(
				    states[current.state], destination, blockedPositions,
				    g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr,
				    g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr,
				    player.getLevel(), &costPolicy);
				if (!topologyRoute) continue;
				const uint32_t finalSteps = std::max(playerBotNavigationDistance(states[current.state], destination),
				                                     *destinationPortals * 32);
				const uint64_t totalCost = current.cost + finalSteps + topologyRoute->dangerCost / 10;
				if (totalCost < selectedCost) {
					selectedCost = totalCost;
					selectedEdge = current.firstEdge;
					selectedDangerCost = static_cast<uint32_t>(std::min<uint64_t>(
					    static_cast<uint64_t>(current.dangerCost) + topologyRoute->dangerCost,
					    std::numeric_limits<uint32_t>::max()));
					selectedMaximumDanger = std::max(current.maximumDanger, topologyRoute->maximumHealthLossPerSecond);
					selectedRouteSteps = current.routeSteps + finalSteps;
					selectedTravelSeconds = current.travelSeconds + finalSteps * player.getStepDuration() / 1000.0;
				}
			}
		}
		if (current.state != 0 && graphExpandedNodes < graphNodeBudget &&
		    playerBotNavigationDistance(states[current.state], destination) <= 512) {
			std::deque<PlayerBotNavigationStep> finalSteps;
			uint64_t expandedNodes = 0;
			PlayerBotNavigationCostSummary finalCost;
			const PlayerBotNavigationResult result = navigator.planFrom(
				player, states[current.state], destination, blockedPositions, finalSteps, expandedNodes,
				std::min(segmentNodeBudget, graphNodeBudget - graphExpandedNodes), nullptr, &costPolicy, &finalCost);
			graphExpandedNodes += expandedNodes;
			if (result == PlayerBotNavigationResult::Reached) {
				const uint64_t totalCost = current.cost + movementCost(finalSteps) + finalCost.dangerCost / 10;
				if (totalCost < selectedCost) {
					selectedCost = totalCost;
					selectedEdge = current.firstEdge;
					selectedDangerCost = static_cast<uint32_t>(std::min<uint64_t>(
					    static_cast<uint64_t>(current.dangerCost) + finalCost.dangerCost,
					    std::numeric_limits<uint32_t>::max()));
					selectedMaximumDanger = std::max(current.maximumDanger, finalCost.maximumHealthLossPerSecond);
					selectedRouteSteps = current.routeSteps + static_cast<uint32_t>(finalSteps.size());
					selectedTravelSeconds = current.travelSeconds + travelSeconds(finalSteps);
				}
			}
		}
		std::vector<size_t> edgeIndices(edges.size());
		std::iota(edgeIndices.begin(), edgeIndices.end(), 0);
		std::sort(edgeIndices.begin(), edgeIndices.end(), [&edges, &states, &current](size_t left, size_t right) {
			return playerBotNavigationDistance(states[current.state], edges[left].npc->getPosition()) <
			       playerBotNavigationDistance(states[current.state], edges[right].npc->getPosition());
		});
		for (size_t edgeIndex : edgeIndices) {
			const Edge& edge = edges[edgeIndex];
			const std::optional<SegmentEstimate> approach = walkSegment(current.state, edgeIndex);
			if (!approach) continue;
			const size_t nextState = stateIndices.at(edge.offer->destination);
			const uint64_t nextFare = current.fare + edge.offer->price;
			if (nextFare > availableMoney) continue;
			const uint64_t nextCost = current.cost + approach->movementCost + approach->dangerCost / 10 +
			                          50 + edge.offer->price / 10;
			const uint32_t nextDangerCost = static_cast<uint32_t>(std::min<uint64_t>(
			    static_cast<uint64_t>(current.dangerCost) + approach->dangerCost,
			    std::numeric_limits<uint32_t>::max()));
			const double nextMaximumDanger = std::max(current.maximumDanger, approach->maximumDanger);
			const uint32_t nextRouteSteps = current.routeSteps + approach->stepCount + 1;
			const double nextTravelSeconds = current.travelSeconds + approach->travelSeconds + 1.0;
			const size_t firstEdge = current.firstEdge == std::numeric_limits<size_t>::max() ? edgeIndex : current.firstEdge;
			auto& nextLabels = labels[nextState];
			const bool dominated = std::any_of(nextLabels.begin(), nextLabels.end(), [nextCost, nextFare](const auto& label) {
				return label.first <= nextCost && label.second <= nextFare;
			});
			if (dominated) continue;
			nextLabels.erase(std::remove_if(nextLabels.begin(), nextLabels.end(), [nextCost, nextFare](const auto& label) {
				return nextCost <= label.first && nextFare <= label.second;
			}), nextLabels.end());
			nextLabels.push_back({nextCost, nextFare});
			queue.push({nextCost, nextFare, nextDangerCost, nextMaximumDanger, nextRouteSteps,
			            nextTravelSeconds, nextState, firstEdge});
		}
	}
	if (selectedEdge == std::numeric_limits<size_t>::max()) return std::nullopt;

	const Edge& selected = edges[selectedEdge];
	auto firstConnection = connections.find({0, selected.npc->getID()});
	if (firstConnection == connections.end() || !firstConnection->second) return std::nullopt;
	SegmentEstimate firstSegment = *firstConnection->second;
	if (firstSegment.coarse && firstSegment.destination != player.getPosition()) {
		uint64_t expandedNodes = 0;
		std::deque<PlayerBotNavigationStep> steps;
		PlayerBotNavigationCostSummary firstCost;
		const PlayerBotNavigationResult result = navigator.planFrom(
		    player, player.getPosition(), firstSegment.destination, blockedPositions, steps, expandedNodes,
		    std::min<uint64_t>(segmentNodeBudget, maximumExpandedNodes), nullptr, &costPolicy, &firstCost);
		graphExpandedNodes += expandedNodes;
		if (result != PlayerBotNavigationResult::Reached) return std::nullopt;
		firstSegment.stepCount = static_cast<uint32_t>(steps.size());
		firstSegment.movementCost = movementCost(steps);
		firstSegment.dangerCost = firstCost.dangerCost;
		firstSegment.maximumDanger = firstCost.maximumHealthLossPerSecond;
		firstSegment.travelSeconds = travelSeconds(steps);
		firstSegment.steps = std::move(steps);
		selectedDangerCost = selectedDangerCost >= firstConnection->second->dangerCost ?
		    selectedDangerCost - firstConnection->second->dangerCost + firstSegment.dangerCost : selectedDangerCost;
		selectedMaximumDanger = std::max(selectedMaximumDanger, firstSegment.maximumDanger);
		selectedRouteSteps = selectedRouteSteps >= firstConnection->second->stepCount ?
		    selectedRouteSteps - firstConnection->second->stepCount + firstSegment.stepCount : selectedRouteSteps;
		selectedTravelSeconds = std::max(0.0, selectedTravelSeconds - firstConnection->second->travelSeconds +
		                                            firstSegment.travelSeconds);
	}
	PlayerBotNavigationRoutePlan route;
	route.metrics.result = PlayerBotNavigationResult::Reached;
	route.metrics.expandedNodes = graphExpandedNodes;
	route.metrics.steps = selectedRouteSteps;
	route.metrics.estimatedTravelSeconds = selectedTravelSeconds;
	route.metrics.movementCost = firstSegment.movementCost;
	route.metrics.dangerCost = selectedDangerCost;
	route.metrics.maximumHealthLossPerSecond = selectedMaximumDanger;
	route.metrics.dangerAware = costPolicy.enabled();
	route.steps = std::move(firstSegment.steps);
	PlayerBotNavigationStep travel;
	travel.action = PlayerBotNavigationAction::NpcTravel;
	travel.target = selected.npc->getPosition();
	travel.expectedPosition = selected.offer->destination;
	travel.npcId = selected.npc->getID();
	travel.price = selected.offer->price;
	travel.minimumLevel = selected.offer->level;
	travel.premium = selected.offer->premium;
	travel.dialogue = selected.offer->dialogue;
	route.steps.push_back(std::move(travel));
	return route;
}

void PlayerBotController::onHealthGain(Creature* healer, const Creature& target, uint32_t gain)
{
	survivalRuntime.observeHealthGain(healer && healer->getID() == playerId, target.getID() == playerId, gain);
}

bool PlayerBotController::processNavigation(Player* player, const Position& currentPosition, const Position& destination,
	                                            PlayerBotNavigationRuntimeOutcome* navigationOutcome,
	                                            uint64_t maximumExpandedNodes)

{
	return processNavigation(player, currentPosition, PlayerBotNavigationGoal::exact(destination), navigationOutcome,
	                         maximumExpandedNodes);
}

bool PlayerBotController::processNavigation(Player* player, const Position& currentPosition, const PlayerBotNavigationGoal& goal,
	                                            PlayerBotNavigationRuntimeOutcome* navigationOutcome,
	                                            uint64_t maximumExpandedNodes, bool npcApproach)
{
	const Position destination = goal.representative();
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotNavigationRuntimeTiming timing = {
		now, navigationStepTimeout, navigationBlockSuppression, navigationOscillationSuppression,
	};
	PlayerBotNavigationRuntimeOutcome outcome = navigationRuntime.process({
		currentPosition, goal, player->getWalkDelay() > 0 || !player->canDoAction(), player->canDoAction(), timing,
	});
	if (outcome.routeRequest) {
		const uint64_t routeNodeBudget = std::min(outcome.routeRequest->maximumExpandedNodes, maximumExpandedNodes);
		const PlayerBotFixtureRoutePlan fixturePlan = fixtureDriver.navigationPlan(routeNodeBudget, npcApproach);
		PlayerBotNavigationRoutePlan routePlan;
		if (fixturePlan.forceFailure) {
			routePlan.metrics.attempted = true;
			routePlan.metrics.result = PlayerBotNavigationResult::Unreachable;
			routePlan.metrics.expandedNodes = outcome.routeRequest->maximumExpandedNodes;
		} else {
			routePlan = planNavigationRoute(*player, outcome.routeRequest->goal, outcome.routeRequest->blockedPositions,
			                                std::min(fixturePlan.maximumExpandedNodes, routeNodeBudget));
		}
		const PlayerBotPendingMovementResult movementResult = outcome.movementResult;
		outcome = navigationRuntime.observePlan({goal, std::move(routePlan), player->canDoAction(), false, now});
		outcome.movementResult = movementResult;
	}
	if (navigationOutcome) *navigationOutcome = outcome;
	fixtureDriver.observeNavigationPlan(outcome.plan.attempted);
	if (outcome.destinationReached) {
		resetNavigation();
		return true;
	}
	if (outcome.oscillation) {
		const PlayerBotNavigationOscillation& oscillation = *outcome.oscillation;
		std::ostringstream fields;
		fields << "\"result\":\"suppressed\",\"reason\":\"position_oscillation\""
		       << ",\"destination\":{\"x\":" << destination.x << ",\"y\":" << destination.y
		       << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}'
		       << ",\"blocked_target\":{\"x\":" << oscillation.blockedTarget.x
		       << ",\"y\":" << oscillation.blockedTarget.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation.blockedTarget.z) << '}'
		       << ",\"position_a\":{\"x\":" << currentPosition.x << ",\"y\":" << currentPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(currentPosition.z) << '}'
		       << ",\"position_b\":{\"x\":" << oscillation.previousPosition.x
		       << ",\"y\":" << oscillation.previousPosition.y
		       << ",\"z\":" << static_cast<uint16_t>(oscillation.previousPosition.z) << '}';
		telemetry.emit("navigation_progress", currentPosition, fields.str());
		telemetry.recordStuckEvent();
		schedule(blockedRouteRetryInterval);
		return false;
	}

	if (outcome.movementResult == PlayerBotPendingMovementResult::Waiting) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	if (outcome.movementResult == PlayerBotPendingMovementResult::Mismatch) {
		telemetry.logActionFailure("navigate", "step_result_mismatch", currentPosition);
		if (outcome.stepFailureCount >= maximumRepeatedNavigationStepFailures) {
			schedule(blockedRouteRetryInterval);
			return false;
		}
	}
	if (outcome.pendingWorldChange) {
		const bool unchanged = findNavigationItem(*outcome.pendingWorldChange) != nullptr;
		Tile* targetTile = g_game.map.getTile(outcome.pendingWorldChange->target);
		const bool blockedDoor = outcome.pendingWorldChange->action == PlayerBotNavigationAction::UseDoor &&
		                         (!targetTile || targetTile->queryAdd(0, *player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR);
		if (unchanged || blockedDoor) {
			telemetry.logActionFailure("navigate", "transition_state_unchanged", currentPosition);
		}
		navigationRuntime.observeWorldChange({*outcome.pendingWorldChange, unchanged || blockedDoor, now, navigationBlockSuppression});
	}
	if (outcome.plan.attempted) {
		telemetry.recordPathfinding(outcome.plan.elapsed, !outcome.routeUnavailable);
		if (outcome.routeUnavailable) {
			telemetry.emit("navigation_progress", currentPosition,
			     "\"result\":\"failed\",\"reason\":\"route_unavailable\",\"cycle_phase\":" +
			         jsonString(cyclePhaseName()) + ",\"destination\":{\"x\":" + std::to_string(destination.x) +
			         ",\"y\":" + std::to_string(destination.y) + ",\"z\":" +
			         std::to_string(static_cast<uint16_t>(destination.z)) + "},\"plan_result\":" +
			         std::to_string(static_cast<uint16_t>(outcome.plan.result)) + ",\"expanded_nodes\":" +
			         std::to_string(outcome.plan.expandedNodes) + ",\"closest_position\":{\"x\":" +
			         std::to_string(outcome.plan.closestPosition.x) + ",\"y\":" +
			         std::to_string(outcome.plan.closestPosition.y) + ",\"z\":" +
			         std::to_string(static_cast<uint16_t>(outcome.plan.closestPosition.z)) + "},\"waypoint\":{\"x\":" +
			         std::to_string(outcome.plan.waypoint.x) + ",\"y\":" + std::to_string(outcome.plan.waypoint.y) +
			         ",\"z\":" + std::to_string(static_cast<uint16_t>(outcome.plan.waypoint.z)) + "}");
			telemetry.logActionFailure("navigate", "route_unavailable", currentPosition);
			if (outcome.fixedTargetRouteExhausted) {
				stop("navigation_route_unavailable", currentPosition);
			}
			schedule(blockedRouteRetryInterval);
			return false;
		}
		std::ostringstream fields;
		fields << "\"action\":\"plan\",\"result\":\"success\",\"steps\":" << outcome.plan.steps
		       << ",\"expanded_nodes\":" << outcome.plan.expandedNodes
		       << ",\"danger_aware\":" << (outcome.plan.dangerAware ? "true" : "false")
		       << ",\"movement_cost\":" << outcome.plan.movementCost
		       << ",\"danger_cost\":" << outcome.plan.dangerCost
		       << ",\"maximum_health_loss_per_second\":" << outcome.plan.maximumHealthLossPerSecond
		       << ",\"destination\":{\"x\":" << destination.x
		       << ",\"y\":" << destination.y << ",\"z\":" << static_cast<uint16_t>(destination.z) << '}';
		telemetry.emit("action_result", currentPosition, fields.str());
	}

	if (outcome.command != PlayerBotNavigationRuntimeCommand::Move &&
	    outcome.command != PlayerBotNavigationRuntimeCommand::Use) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}

	if (!outcome.nextStep) {
		schedule(navigationDecisionDelay(*player));
		return false;
	}
	PlayerBotNavigationStep step = *outcome.nextStep;
	if (step.topologyPortal) {
		step = resolveTopologyPortal(*player, step, navigationRuntime.activeBlockedPositions(now));
	}
	if (!executeNavigationStep(player, step)) {
		navigationRuntime.observeStep({step, PlayerBotNavigationStepResult::Rejected,
		                               std::chrono::steady_clock::now(), navigationBlockSuppression});
		telemetry.logActionFailure("navigate", "transition_unavailable", currentPosition);
		schedule(blockedRouteRetryInterval);
		return false;
	}

	navigationRuntime.observeStep({step, PlayerBotNavigationStepResult::Dispatched,
	                               std::chrono::steady_clock::now(), navigationBlockSuppression});
	schedule(navigationDecisionDelay(*player));
	return false;
}

void PlayerBotController::navigate()
{
	auto decisionTimer = telemetry.recordDecision();
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		return;
	}
	if (!turnRouter.running()) return;
	if (deathObserved) {
		stop("controlled_player_dead", lastPosition);
		return;
	}
	Player* player = g_game.getPlayerByID(playerId);
	if (!player) {
		stop("controlled_player_not_found", lastPosition);
		return;
	}
	if (!player->isPlayerBot()) {
		stop("controlled_player_ownership_lost", lastPosition);
		return;
	}
	if (player->isRemoved()) {
		telemetry.emit("lifecycle", lastPosition, "\"status\":\"removed\"");
		stop("controlled_player_removed", lastPosition);
		return;
	}
	if (player->isDead()) {
		onDeath(*player, nullptr, nullptr);
		stop("controlled_player_dead", player->getPosition());
		return;
	}

	const Position currentPosition = player->getPosition();
	lastPosition = currentPosition;
	if (const PlayerBotFixtureInitialization initialization = fixtureDriver.delayedInitializationStatus(*player);
	    initialization != PlayerBotFixtureInitialization::NotPending) {
		if (initialization == PlayerBotFixtureInitialization::Cancelled) {
			return;
		}
		if (initialization == PlayerBotFixtureInitialization::Waiting) {
			schedule(navigationInterval);
			return;
		}
		emitFixtureEvents(fixtureDriver.runMagicTraining(*player), currentPosition);
		const bool useGoalSelector = !fixtureDriver.startInHunt() &&
		                             (departurePlanner.required(departureSnapshot(*player)) || fixtureDriver.startWithGoalSelection());
		if (useGoalSelector) {
			if (!selectTopLevelGoal(*player, currentPosition, "startup")) {
				return;
			}
		} else if (fixtureDriver.startInHunt()) {
			progressionRuntime.enterHunt();
			startHunt(player, currentPosition, "focused_fixture");
		} else {
			beginService(player, currentPosition, "startup");
		}
		schedule(navigationInterval);
		return;
	}
	telemetry.maybeEmitSummary(currentPosition, telemetrySummary());
	recordActiveHuntCombat(*player);
	verifySpellCast(*player, currentPosition);
	if (huntCoordinator.huntActive() && turnRouter.cyclePhase() == CyclePhase::Hunt) {
		const auto now = std::chrono::steady_clock::now();
		if (huntCoordinator.observeHuntDanger(player->getMaxHealth(), now, huntRegionCooldown)) {
			beginService(player, currentPosition, "hunt_region_observed_danger");
			schedule(navigationInterval);
			return;
		}
	}
	const bool accessingReward = progressionRuntime.session().active(PlayerBotProgressionProcedure::PickupReward) &&
	                             (progressionRuntime.reward().stage() == PlayerBotRewardStage::VerifyReward ||
	                              progressionRuntime.reward().stage() == PlayerBotRewardStage::EquipReward ||
	                              progressionRuntime.reward().stage() == PlayerBotRewardStage::VerifyEquipment);
	const bool verifyingDeparture = progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture) &&
	                                progressionRuntime.departure().stage() == PlayerBotOracleDepartureStage::Verify;
	if (!accessingReward && !verifyingDeparture && handleHealing(player, currentPosition)) {
		schedule(blockedRouteRetryInterval);
		return;
	}
	const bool waitingForRecovery = turnRouter.cyclePhase() == CyclePhase::Service &&
	                                survivalRuntime.needsHealing(survivalSnapshot(*player));
	if (!accessingReward && !progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture) &&
	    departurePlanner.required(departureSnapshot(*player)) && !waitingForRecovery) {
		if (selectTopLevelGoal(*player, currentPosition, "level_eight_interrupt")) {
			schedule(SCHEDULER_MINTICKS);
		}
		return;
	}
	if (!accessingReward && !verifyingDeparture && handleFood(player, currentPosition)) {
		schedule(blockedRouteRetryInterval);
		return;
	}
	processTraversal(player, currentPosition);
}
