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
#include "playerbotplanningbudget.h"
#include "playerbothuntregionadapter.h"
#include "playerbothunttriptiming.h"
#include "weapons.h"
#include "playerbotnpccapabilities.h"
#include "playerbottopology.h"
#include "spells.h"
#include "groups.h"
#include "guild.h"

#include <map>
#include <utility>

// Playerbot survival, combat targeting, and hunt orchestration.
using namespace playerbot;

extern Spells* g_spells;
extern Weapons* g_weapons;

namespace {
	constexpr uint64_t maximumTargetApproachExpandedNodes = 10000;
	constexpr size_t maximumDetailedHuntCandidates = 256;

	std::string positionJson(const Position& position)
	{
		return "{\"x\":" + std::to_string(position.x) + ",\"y\":" + std::to_string(position.y) +
		       ",\"z\":" + std::to_string(position.z) + '}';
	}

	// At most eight positions; blocked sets are small, but keep records bounded.
	std::string positionListJson(const std::set<Position>& positions)
	{
		std::string result = "[";
		size_t count = 0;
		for (const Position& position : positions) {
			if (count++ == 8) break;
			if (count > 1) result += ',';
			result += positionJson(position);
		}
		return result + ']';
	}

	std::string supplyKindBudgetsJson(const std::vector<PlayerBotSupplyKindBudget>& budgets)
	{
		std::ostringstream json;
		json << '[';
		for (size_t index = 0; index < budgets.size(); ++index) {
			const PlayerBotSupplyKindBudget& budget = budgets[index];
			json << (index == 0 ? "" : ",") << "{\"kind\":" << jsonString(playerBotSupplyKindName(budget.kind))
			     << ",\"count\":" << budget.count << ",\"routine\":" << budget.routine
			     << ",\"expected\":" << budget.expected << ",\"fits\":" << (budget.fits ? "true" : "false") << '}';
		}
		json << ']';
		return json.str();
	}

	std::string supplyDemandJson(const std::vector<PlayerBotSupplyDemandUpdate>& updates)
	{
		std::ostringstream json;
		json << '[';
		for (size_t index = 0; index < updates.size(); ++index) {
			const PlayerBotSupplyDemandUpdate& update = updates[index];
			json << (index == 0 ? "" : ",") << "{\"kind\":" << jsonString(playerBotSupplyKindName(update.kind))
			     << ",\"consumed\":" << update.consumed << ",\"debt\":" << update.debt
			     << ",\"observed_per_combat_minute\":" << update.observedUnitsPerCombatSecond * 60
			     << ",\"per_combat_minute\":" << update.demand.unitsPerCombatSecond * 60
			     << ",\"samples\":" << update.demand.samples << ",\"updated\":" << (update.updated ? "true" : "false")
			     << ",\"reason\":" << jsonString(update.reason) << '}';
		}
		json << ']';
		return json.str();
	}

	std::vector<PlayerBotNavigationStep> npcTravelSteps(const std::deque<PlayerBotNavigationStep>& steps)
	{
		std::vector<PlayerBotNavigationStep> result;
		std::copy_if(steps.begin(), steps.end(), std::back_inserter(result),
		             [](const auto& step) { return step.action == PlayerBotNavigationAction::NpcTravel; });
		return result;
	}

	const char* playerBotPendingMovementResultName(PlayerBotPendingMovementResult result)
	{
		switch (result) {
			case PlayerBotPendingMovementResult::Completed: return "completed";
			case PlayerBotPendingMovementResult::Waiting: return "waiting";
			case PlayerBotPendingMovementResult::Mismatch: return "mismatch";
			case PlayerBotPendingMovementResult::LandingOffset: return "landing_offset";
			default: return "none";
		}
	}

	std::string supplyCapabilityChangedFields(uint64_t fields)
	{
		std::ostringstream result;
		result << '[';
		bool first = true;
		auto append = [&](uint64_t field, const char* name) {
			if ((fields & field) == 0) return;
			if (!first) result << ',';
			first = false;
			result << jsonString(name);
		};
		append(PlayerBotSupplyCapabilityLevel, "level");
		append(PlayerBotSupplyCapabilityMaximumHealth, "maximum_health");
		append(PlayerBotSupplyCapabilityArmor, "armor");
		append(PlayerBotSupplyCapabilityDefense, "defense");
		append(PlayerBotSupplyCapabilityAttack, "attack");
		append(PlayerBotSupplyCapabilityAttackSkill, "attack_skill");
		append(PlayerBotSupplyCapabilityAttackFactor, "attack_factor");
		append(PlayerBotSupplyCapabilityMagicLevel, "magic_level");
		append(PlayerBotSupplyCapabilityMaximumMana, "maximum_mana");
		append(PlayerBotSupplyCapabilitySpellLegal, "spell_legal");
		append(PlayerBotSupplyCapabilitySpellHealing, "spell_healing");
		append(PlayerBotSupplyCapabilitySpellMana, "spell_mana");
		append(PlayerBotSupplyCapabilitySpellInterval, "spell_interval");
		append(PlayerBotSupplyCapabilityPotionHealing, "potion_healing");
		append(PlayerBotSupplyCapabilityEquipment, "equipment_identity");
		result << ']';
		return result.str();
	}

	double projectedHuntStaminaMultiplier(const Player& player, double availableHuntSeconds)
	{
		return playerBotHuntStaminaExperienceMultiplier(player.getStaminaMinutes(),
		    g_config.getBoolean(ConfigManager::STAMINA_SYSTEM), player.isPremium(), availableHuntSeconds);
	}

	PlayerBotHuntRuntimePlayerObservation huntPlayerObservation(Player& player)
	{
		PlayerBotHuntRuntimePlayerObservation observation;
		observation.position = player.getPosition();
		observation.level = player.getLevel();
		observation.health = player.getHealth();
		observation.maximumHealth = player.getMaxHealth();
		observation.staminaMinutes = player.getStaminaMinutes();
		observation.experience = player.getExperience();
		observation.topologyGeneration = PlayerBotTopology::instance().generation();
		observation.npcGeneration = g_game.getNpcGeneration();
		observation.canUseRope = g_game.findItemOfType(&player, ropeItemId, true) != nullptr;
		observation.canUseShovel = g_game.findItemOfType(&player, 2554, true) != nullptr;
		observation.premium = player.isPremium();
		observation.potions = static_cast<const Cylinder&>(player).getItemTypeCount(recoveryPotionItemId(player.getVocationId()));
		observation.mana = player.getMana();
		observation.funds = player.getMoney() + player.getBankBalance();
		observation.supplies = PlayerBotInventoryPolicy::additionalSupplyStocks(player);
		return observation;
	}

	PlayerBotHuntPlanningProfile huntPlanningFacts(Player& player, const PlayerBotCombatProfile& combat)
	{
		PlayerBotHuntPlanningProfile profile = playerBotHuntPlanningProfile(player, combat, 0);
		PlayerBotEquipmentPlayerSnapshot equipmentPlayer = PlayerBotEquipmentAdapter::player(player);
		// getDefenseFactor() changes briefly after an attack. Supply compatibility
		// uses the same equipment formula without that transient; attackFactor
		// separately preserves the selected fight mode.
		equipmentPlayer.defenseFactor = 1.0f;
		profile.supplyCapabilityDefense = PlayerBotEquipmentPolicy().combatProfile(
		    equipmentPlayer, PlayerBotEquipmentAdapter::loadout(player)).defense;
		for (auto& kind : profile.supply.kinds) {
			if (kind.kind != PlayerBotSupplyKind::ThrowingWeapon) continue;
			if (const Weapon* weapon = g_weapons->getWeapon(kind.itemId)) {
				const Item* wielded = player.getWeapon(true);
				const uint32_t interval = wielded && wielded->getAttackSpeed() ? wielded->getAttackSpeed() :
				    player.getVocation()->getAttackSpeed();
				kind.staticUnitsPerCombatSecond = playerBotThrowingBreakDemand(weapon->getBreakChance(), interval);
			}
		}
		return profile;
	}

	PlayerBotCombatProfile huntCombatProfile(Player& player)
	{
		const Item* weapon = player.getWeapon(true);
		return {player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(),
		        weapon ? weapon->getAttack() : 7,
		        weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor()};
	}

	std::optional<std::pair<double, double>> manageablePassageFight(
		Player& player, const std::vector<Creature*>& attackers, const Creature& target)
	{
		const PlayerBotCombatProfile combat = huntCombatProfile(player);
		double incomingDamagePerSecond = 0;
		double targetFightSeconds = 0;
		for (Creature* attacker : attackers) {
			const PlayerBotFightEstimate estimate = PlayerBotHuntRegionAdapter::fightEstimate(
			    combat, attacker->getName(), attacker->getHealth());
			if (estimate.incomingDamagePerSecond <= 0 || estimate.fightSeconds <= 0) return std::nullopt;
			incomingDamagePerSecond += estimate.incomingDamagePerSecond;
			if (attacker->getID() == target.getID()) targetFightSeconds = estimate.fightSeconds;
		}
		if (targetFightSeconds <= 0) return std::nullopt;
		const double predictedDamage = incomingDamagePerSecond * targetFightSeconds;
		const PlayerBotRecoveryPrediction recovery = playerBotPredictRecovery(
		    huntPlanningFacts(player, combat), targetFightSeconds);
		if (!playerBotPassageFightManageable(player.getHealth(), recovery.totalMinimumHealing,
		                                     incomingDamagePerSecond, targetFightSeconds)) {
			return std::nullopt;
		}
		return std::pair{predictedDamage, targetFightSeconds};
	}

	std::shared_ptr<const std::vector<PlayerBotHuntTransportOffer>> huntTransportCatalog()
	{
		// Capture providers once per planning session. NPC movement must not
		// invalidate an active plan, while the next plan sees current positions
		// and offers and owns an immutable view of them.
		auto catalog = std::make_shared<std::vector<PlayerBotHuntTransportOffer>>();
		for (const auto& entry : g_game.getNpcs()) {
			Npc* npc = entry.second;
			if (!npc || !playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Travel)) continue;
			for (const NpcTravelOffer& offer : npc->getTravelOffers()) {
				catalog->push_back({npc->getPosition(), offer.destination, offer.price, offer.level,
				                    offer.premium, offer.hasOpaqueCondition, offer.hasOpaqueAction});
			}
		}
		return catalog;
	}
}

PlayerBotExpectedCorpse PlayerBotController::expectedCorpseFor(const Creature& target) const
{
	PlayerBotExpectedCorpse expectation;
	const Monster* monster = target.getMonster();
	expectation.itemId = monster ? monster->getCorpseItemId() : 0;
	if (expectation.itemId != 0) {
		const ItemType& corpseType = Item::items[expectation.itemId];
		expectation.lootable = corpseType.corpseType != RACE_NONE && corpseType.isContainer();
	}
	return expectation;
}

PlayerBotSurvivalSnapshot PlayerBotController::survivalSnapshot(const Player& player, const Creature* target) const
{
	PlayerBotSurvivalSnapshot snapshot;
	snapshot.health = player.getHealth();
	snapshot.healthMaximum = player.getMaxHealth();
	snapshot.mana = player.getMana();
	snapshot.manaMaximum = player.getMaxMana();
	snapshot.manaSpent = player.getSpentMana();
	snapshot.level = player.getLevel();
	snapshot.magicLevel = player.getBaseMagicLevel();
	snapshot.potionItemId = recoveryPotionItemId(player.getVocationId());
	snapshot.potionCount = inventoryPolicy.inventoryItemCount(player, snapshot.potionItemId);
	snapshot.manaPotionItemId = playerBotManaPotionItemId;
	snapshot.manaPotionCount = inventoryPolicy.inventoryItemCount(player, snapshot.manaPotionItemId);
	snapshot.foodInventoryCount = inventoryPolicy.foodInventory(player).count;
	if (const uint16_t pendingFoodId = survivalRuntime.pendingFoodItemId()) {
		snapshot.pendingFoodCount = inventoryPolicy.inventoryItemCount(player, pendingFoodId);
	}
	if (Condition* condition = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT, 0)) {
		snapshot.foodTicks = condition->getTicks();
	}
	std::function<Item*(Item*)> findFood = [&](Item* item) -> Item* {
		if (!item) return nullptr;
		if (PlayerBotInventoryPolicy::isFoodItem(item->getID())) return item;
		if (Container* container = item->getContainer()) {
			for (Item* child : container->getItemList()) {
				if (Item* food = findFood(child)) return food;
			}
		}
		return nullptr;
	};
	for (int32_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST && snapshot.foodClientId == 0; ++slot) {
		if (Item* food = findFood(player.getInventoryItem(static_cast<slots_t>(slot)))) {
			snapshot.foodItemId = food->getID();
			snapshot.foodClientId = food->getClientID();
			snapshot.foodCount = inventoryPolicy.inventoryItemCount(player, food->getID());
		}
	}
	snapshot.canDoAction = player.canDoAction();
	snapshot.buyingPotions = serviceWorkflow.buyingPotions();
	snapshot.lootMovePending = huntCoordinator.hasPendingLootMove() ||
	    preparationOption == PlayerBotPreparationOption::BuyFood; // Do not consume a receipt before trade verification.
	snapshot.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	snapshot.progressionDeparture = progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture);
	snapshot.hunting = turnRouter.cyclePhase() == CyclePhase::Hunt;
	snapshot.combatActive = turnRouter.scenarioStage() != ScenarioStage::Traverse || huntCoordinator.hasActiveCombat() ||
	                          const_cast<Player&>(player).getAttackedCreature() != nullptr;
	snapshot.departureHealthTarget = preparationOption == PlayerBotPreparationOption::RestoreHealth && preparationRequirement &&
	    std::chrono::steady_clock::now() < preparationDeadline ?
	    preparationRequirement->health : huntDepartureHealth.active() && supplyRecovery.active() &&
	    !huntCoordinator.huntArrived() ? huntDepartureHealth.target() : 0;
	snapshot.navigationPending = navigationRuntime.hasPendingWork();
	snapshot.healingExhausted = player.hasCondition(CONDITION_EXHAUST_HEAL);
	snapshot.combatExhausted = player.hasCondition(CONDITION_EXHAUST_COMBAT);
	snapshot.hasteActive = player.hasCondition(CONDITION_HASTE);
	if (snapshot.hasteActive) snapshot.hasteTicks = player.getCondition(CONDITION_HASTE)->getTicks();
	snapshot.lightActive = player.hasCondition(CONDITION_LIGHT);
	snapshot.regenerationActive = player.hasCondition(CONDITION_REGENERATION);
	snapshot.protectionZone = player.getZone() == ZONE_PROTECTION;
	if (const auto forecast = player.getManaRegenerationForecast()) {
		snapshot.regenerationForecastActive = true;
		snapshot.regenerationManaGain = forecast->gain;
		snapshot.regenerationTickInterval = forecast->interval;
		snapshot.regenerationTickRemaining = forecast->remaining;
	}
	snapshot.routeSteps = navigationRuntime.routeSize();
	if (target) {
		snapshot.target.id = target->getID();
		snapshot.target.health = target->getHealth();
		snapshot.target.targetClass = target->getMonster() ? "monster:" + target->getName() : "creature";
		snapshot.target.targetClass.resize(std::min<size_t>(snapshot.target.targetClass.size(), 56));
		snapshot.target.valid = !target->isRemoved() && !target->isDead() &&
		                        const_cast<Player&>(player).getAttackedCreature() == target && player.canSeeCreature(target) &&
		                        player.canSee(target->getPosition()) &&
		                        Position::areInRange<1, 1, 0>(player.getPosition(), target->getPosition());
	}
	for (const PlayerBotSpellDescriptor& descriptor : playerBotSpellDescriptors()) {
		PlayerBotSurvivalSpellObservation spell;
		spell.name = descriptor.name;
		InstantSpell* engineSpell = g_spells ? g_spells->getInstantSpellByName(descriptor.name) : nullptr;
		spell.metadataMatches = engineSpell && engineSpell->getWords() == descriptor.words && engineSpell->isLearnable();
		spell.learned = player.hasLearnedInstantSpell(descriptor.name);
		if (engineSpell) {
			spell.targetReachable = !engineSpell->getNeedTarget() ||
			                        (target && !target->isRemoved() && engineSpell->canThrowSpell(&player, target));
			spell.manaCost = engineSpell->getManaCost(&player);
			spell.level = engineSpell->getLevel();
			const VocSpellMap& vocations = engineSpell->getVocMap();
			spell.vocationAllowed = vocations.empty() || vocations.count(player.getVocationId()) != 0;
			spell.requirementsMet = engineSpell->isEnabled() && player.getLevel() >= engineSpell->getLevel() &&
			                        player.getMagicLevel() >= engineSpell->getMagicLevel() &&
			                        player.getSoul() >= engineSpell->getSoulCost() &&
			                        (!engineSpell->isPremium() || player.isPremium()) &&
			                        (!engineSpell->getNeedWeapon() || player.getWeapon(true));
			spell.envelope = playerBotSpellEnvelope(player, descriptor);
			spell.magicTrainingEligible = descriptor.magicTrainingSafe && descriptor.magicTrainingPriority != 0 &&
			                              descriptor.magicTrainingEffect != PlayerBotTrainingEffect::None && spell.learned &&
			                              spell.metadataMatches && engineSpell->isEnabled() && player.getLevel() >= engineSpell->getLevel() &&
			                              player.getMagicLevel() >= engineSpell->getMagicLevel() && player.getSoul() >= engineSpell->getSoulCost() &&
			                              (!engineSpell->isPremium() || player.isPremium()) &&
			                              (!engineSpell->getNeedWeapon() || player.getWeapon(true)) && !snapshot.healingExhausted &&
			                              !engineSpell->getAggressive() && engineSpell->getSelfTarget() && !engineSpell->getNeedTarget() &&
			                              !engineSpell->getHasParam() && !engineSpell->getHasPlayerNameParam() &&
			                              !engineSpell->getNeedDirection() && !engineSpell->getNeedCasterTargetOrDirection();
		}
		snapshot.spells.push_back(std::move(spell));
	}
	return snapshot;
}

void PlayerBotController::logHealResult(uint16_t itemId, const char* result, const char* reason, const PlayerBotPotionAttempt& before,
					const PlayerBotPotionAttempt& after, const Position& position)
{
	std::ostringstream fields;
	if (before.restoresMana) {
		fields << "\"action\":\"restore_mana\",\"result\":" << jsonString(result)
		       << ",\"method\":\"mana_potion\",\"item_id\":" << itemId
		       << ",\"trigger\":" << jsonString(before.trigger) << ",\"objective\":" << jsonString(objectiveName())
		       << ",\"state\":" << jsonString(turnRouter.stateName())
		       << ",\"mana_before\":" << before.mana << ",\"mana_after\":" << after.mana
		       << ",\"health_before\":" << before.health << ",\"health_after\":" << after.health
		       << ",\"resource_before\":" << before.potionCount << ",\"resource_after\":" << after.potionCount;
		if (reason) fields << ",\"reason\":" << jsonString(reason);
		emit("action_result", position, fields.str());
		return;
	}
	fields << "\"action\":\"heal\",\"result\":" << jsonString(result)
	       << ",\"method\":" << jsonString(itemId == smallHealthPotionItemId ? "small_health_potion" : "health_potion")
	       << ",\"item_id\":" << itemId
	       << ",\"trigger\":" << jsonString(before.trigger.empty() ? "health_threshold" : before.trigger)
	       << ",\"objective\":" << jsonString(objectiveName())
	       << ",\"state\":" << jsonString(turnRouter.stateName())
	       << ",\"health_before\":" << before.health
	       << ",\"health_after\":" << after.health
	       << ",\"health_max\":" << before.healthMaximum
	       << ",\"resource_before\":" << before.potionCount
	       << ",\"resource_after\":" << after.potionCount;
	if (reason) {
		fields << ",\"reason\":" << jsonString(reason);
	}
	emit("action_result", position, fields.str());
}

bool PlayerBotController::safePreparationRest(Player& player, const Position& position) const
{
	Tile* tile = g_game.map.getTile(position);
	return tile && !tile->hasFlag(TILESTATE_PROTECTIONZONE) && playerBotStableApproachTile(tile, player) &&
	       navigationCostPolicy(player).dangerAt(position) == 0;
}

std::vector<PlayerBotPreparationOption> PlayerBotController::evaluatePreparation(Player& player, const Position& position)
{
	std::vector<PlayerBotPreparationOption> options;
	preparationEvaluationPending = false;
	preparationRest.reset();
	preparationFoodItem = 0;
	preparationFoodProvider = 0;
	const auto& readiness = huntSupplyHandoff.readiness();
	if (readiness.state != PlayerBotReadiness::Blocked || !readiness.requirement) return options;
	const auto& requirement = *readiness.requirement;
	if (requirement.kind == PlayerBotRequirementKind::Stock) {
		const auto facts = huntSupplyFacts(player);
		// The existing service executor validates providers/routes before trade.
		// An unaffordable floor instead requests the existing recovery evaluator.
		const uint64_t spending = supplyFloorSpendingReserve(player);
		const uint64_t floorCost = spending == UINT64_MAX ? spending : spending - carriedGoldReserve;
		options.push_back(!supplyRecovery.restockBlocked(facts.funds, facts.stockKey) &&
		       floorCost <= facts.funds ? PlayerBotPreparationOption::ServiceStock :
		       PlayerBotPreparationOption::RecoveryIncome);
		return options;
	}
	const auto snapshot = survivalSnapshot(player);
	options = survivalRuntime.healthPreparations(snapshot, requirement, false, false);
	// A hunt callback may already own this controller's slot. Outside it,
	// defer arbitration rather than treating budget denial as infeasibility.
	auto& budget = playerBotHuntPlanningBudget();
	std::optional<PlayerBotPlanningBudget::Charge> charge;
	if (!budget.isActive(playerId)) {
		const auto admission = budget.request(playerId, PlayerBotPlanningBudget::Clock::now());
		if (!admission.admitted) {
			preparationEvaluationPending = true;
			schedule(static_cast<uint32_t>(std::max<int64_t>(SCHEDULER_MINTICKS,
			    (admission.wait.count() + 999) / 1000)));
			return options;
		}
		charge.emplace(budget, playerId);
	}
	PlayerBotPreparationRouteBudget proofs;
	PlayerBotNavigationCostPolicy cost = navigationCostPolicy(player);
	cost.risk.maximumRouteHealthLoss = cost.risk.maximumHealthLossPerSecond = 0;
	const PlayerBotNavigator navigator;
	auto prove = [&](const Position& from, const PlayerBotNavigationGoal& goal) -> std::optional<Position> {
		const uint64_t allowance = proofs.allowance();
		if (allowance == 0) return std::nullopt;
		std::deque<PlayerBotNavigationStep> steps;
		uint64_t nodes = 0;
		PlayerBotNavigationCostSummary summary;
		const auto result = navigator.planFrom(player, from, goal, {}, steps, nodes, allowance,
		    nullptr, &cost, &summary, false); // Normal stairs, doors, and item-use transitions.
		proofs.observed(nodes);
		const Position reached = steps.empty() ? from : steps.back().expectedPosition;
		if (!playerBotPreparationRouteAccepted(result, summary.dangerCost, summary.maximumHealthLossPerSecond,
		    0, goal.reached(reached))) return std::nullopt;
		return reached;
	};
	const auto restPositions = playerBotPreparationRestPositions(position, [&](const Position& candidate) {
		Tile* tile = g_game.map.getTile(candidate);
		return tile && !tile->hasFlag(TILESTATE_PROTECTIONZONE) && playerBotStableApproachTile(tile, player) &&
		    cost.dangerAt(candidate) == 0;
	});
	if (restPositions.empty()) return options;
	preparationRest = prove(position, PlayerBotNavigationGoal::anyOf(restPositions));
	if (!preparationRest) return options;
	if ((snapshot.regenerationActive && snapshot.foodTicks > 0) || snapshot.foodClientId != 0) {
		return survivalRuntime.healthPreparations(snapshot, requirement, true, false);
	}
	// The inventory food predicate is the supported set. Lua food.lua remains
	// authoritative for use and duration; purchase only one, then observe feed.
	for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Shop, position)) {
		if (!playerBotPreparationProviderInRange(position, npc->getPosition())) continue;
		for (const auto& offer : npc->getShopOffers()) {
			if (!PlayerBotInventoryPolicy::isFoodItem(offer.itemId) || offer.buyPrice == 0 ||
			    offer.buyPrice > player.getMoney() + player.getBankBalance() ||
			    Item::items[offer.itemId].weight > player.getFreeCapacity()) continue;
			std::vector<Position> approaches;
			for (int dx = -3; dx <= 3; ++dx) for (int dy = -3; dy <= 3; ++dy) {
				Position approach(npc->getPosition().x + dx, npc->getPosition().y + dy, npc->getPosition().z);
				if (playerBotStableApproachTile(g_game.map.getTile(approach), player)) approaches.push_back(approach);
			}
			if (approaches.empty()) continue;
			if (proofs.allowance() == 0) return options;
			const auto approach = prove(position, PlayerBotNavigationGoal::anyOf(approaches));
			if (!approach || !prove(*approach, PlayerBotNavigationGoal::exact(*preparationRest))) continue;
			preparationFoodItem = offer.itemId;
			preparationFoodProvider = npc->getID();
			return survivalRuntime.healthPreparations(snapshot, requirement, true, true);
		}
	}
	return options;
}

void PlayerBotController::retirePreparationService(Player* player, const Position& position, const char* reason)
{
	if (serviceWorkflow.intent() != PlayerBotServiceIntent::PreparationPurchase) return;
	PlayerBotServiceObservation receipt;
	if (const auto* pending = serviceWorkflow.pendingPreparationPurchase(); pending && player) {
		receipt.inventoryCounts[pending->itemId] = inventoryPolicy.inventoryItemCount(*player, pending->itemId);
		receipt.money = player->getMoney();
		receipt.bankBalance = player->getBankBalance();
	}
	const auto result = serviceWorkflow.retirePreparationPurchase(receipt);
	if (result.verification) {
		const auto& before = result.verification->before;
		emit("action_result", position, "\"action\":\"buy_food\",\"result\":" +
		    jsonString(result.verification->result == PlayerBotServiceVerificationResult::Success ? "success" : "failed") +
		    ",\"reason\":" + jsonString(reason) + ",\"item_id\":" + std::to_string(before.itemId) +
		    ",\"count\":" + std::to_string(before.amount) + ",\"carried_before\":" + std::to_string(before.money) +
		    ",\"carried_after\":" + std::to_string(receipt.money) + ",\"bank_before\":" +
		    std::to_string(before.balance) + ",\"bank_after\":" + std::to_string(receipt.bankBalance));
	}
	serviceRouteSearch.reset();
	auto& budget = playerBotHuntPlanningBudget();
	if (!budget.isActive(playerId)) budget.cancel(playerId, PlayerBotPlanningBudget::Clock::now());
}

void PlayerBotController::retirePreparation(Player* player, const Position& position, const char* reason)
{
	if (!preparationEvaluationPending && preparationOption == PlayerBotPreparationOption::None &&
	    serviceWorkflow.intent() != PlayerBotServiceIntent::PreparationPurchase) return;
	retirePreparationService(player, position, reason);
	serviceRouteSearch.reset();
	auto& budget = playerBotHuntPlanningBudget();
	if (!budget.isActive(playerId)) budget.cancel(playerId, PlayerBotPlanningBudget::Clock::now());
	preparationEvaluationPending = false;
	preparationOption = PlayerBotPreparationOption::None;
	preparationRequirement.reset();
	preparationRest.reset();
	preparationFoodItem = 0;
	preparationFoodProvider = 0;
	resetNavigation();
}

void PlayerBotController::finishPreparation(Player& player, const Position& position, const char* reason)
{
	if (!turnRouter.running() || telemetry.terminalLogged()) return;
	const auto snapshot = survivalSnapshot(player);
	const auto requirement = preparationRequirement;
	uint64_t gap = requirement ? requirement->health - std::min<uint32_t>(requirement->health, std::max(0, player.getHealth())) : 0;
	gap += requirement && requirement->observeRegeneration && (!snapshot.regenerationActive || snapshot.foodTicks <= 0) ? 1 : 0;
	// Track actual health improvement, not a moving request target. Reaching a
	// higher successive target must count even when both completed gaps are zero.
	const uint64_t healthGap = std::max(0, player.getMaxHealth() - player.getHealth());
	if (requirement && !requirement->observeRegeneration && player.getHealth() >= static_cast<int64_t>(requirement->health)) {
		emit("action_result", position, "\"action\":\"hunt_departure_health\",\"result\":\"ready\",\"health\":" +
		    std::to_string(player.getHealth()) + ",\"required_health\":" + std::to_string(requirement->health));
	}
	const bool exhausted = preparationBudget.completed(healthGap,
	    snapshot.regenerationActive && snapshot.foodTicks > 0 && !snapshot.protectionZone &&
	    safePreparationRest(player, position));
	emit("goal_result", position, std::string("\"goal\":\"prepare\",\"reason\":") + jsonString(reason) +
	    ",\"remaining_gap\":" + std::to_string(gap));
	retirePreparation(&player, position, reason);
	if (exhausted) { stop("preparation_no_progress", position); return; }
	huntSupplyHandoff.replan();
	// Only a fresh authoritative hunt evaluation can authorize departure.
	startHunt(&player, position, "preparation_observed");
}

void PlayerBotController::processPreparation(Player* player, const Position& position)
{
	if (preparationEvaluationPending) {
		if (selectTopLevelGoal(*player, position, "preparation_budget_retry") &&
		    !trainerDiscovery && !preparationEvaluationPending) schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (std::chrono::steady_clock::now() >= preparationDeadline) {
		finishPreparation(*player, position, "preparation_timeout");
		return;
	}
	if (preparationOption == PlayerBotPreparationOption::BuyFood) {
		processService(player, position);
		return;
	}
	const auto snapshot = survivalSnapshot(*player);
	if (!preparationRequirement || survivalRuntime.healthPreparationSatisfied(snapshot, *preparationRequirement,
	    safePreparationRest(*player, position))) {
		finishPreparation(*player, position, "requirement_observed");
		return;
	}
	if (preparationOption == PlayerBotPreparationOption::RestoreHealth) {
		// Peaceful target is supplied to the existing survival evaluator. It owns
		// spell/potion legality, preference, action delay, and effect verification.
		if (!handleHealing(player, position) && preparationOption != PlayerBotPreparationOption::None)
			finishPreparation(*player, position, "recovery_unavailable");
		else schedule(1000);
		return;
	}
	if (!preparationRest || !safePreparationRest(*player, *preparationRest)) {
		finishPreparation(*player, position, "rest_invalidated");
		return;
	}
	if (position != *preparationRest) {
		PlayerBotNavigationRuntimeOutcome result;
		PlayerBotNavigationRiskProfile safe = riskProfile;
		safe.maximumRouteHealthLoss = safe.maximumHealthLossPerSecond = 0;
		auto& budget = playerBotHuntPlanningBudget();
		const auto admission = budget.request(playerId, PlayerBotPlanningBudget::Clock::now());
		if (!admission.admitted) {
			schedule(static_cast<uint32_t>(std::max<int64_t>(SCHEDULER_MINTICKS,
			    (admission.wait.count() + 999) / 1000)));
			return;
		}
		PlayerBotPlanningBudget::Charge charge(budget, playerId);
		processNavigation(player, position, *preparationRest, &result, 4096, false, &safe);
		if (telemetry.terminalLogged()) return;
		if (result.fixedTargetRouteFailures >= maximumProgressionAttempts)
			finishPreparation(*player, position, "rest_unreachable");
		return;
	}
	if (!snapshot.regenerationActive && snapshot.foodClientId == 0) {
		finishPreparation(*player, position, "regeneration_unavailable");
		return;
	}
	schedule(1000);
}

bool PlayerBotController::prepareHuntDeparture(Player* player, const Position& position)
{
	const uint32_t floor = supplyRecovery.active() ?
	    playerBotSupplyRecoveryRequiredHealth(std::max(0, player->getMaxHealth())) : 0;
	if (player->getHealth() < static_cast<int64_t>(floor) && preparationOption == PlayerBotPreparationOption::None) {
		PlayerBotPreparationRequirement requirement;
		requirement.kind = PlayerBotRequirementKind::HealthRegeneration;
		requirement.health = floor;
		huntSupplyHandoff.blocked({PlayerBotReadiness::Blocked, requirement}, huntSupplyFacts(*player));
		selectTopLevelGoal(*player, position, "departure_health_requirement");
		return false;
	}
	const bool wasActive = huntDepartureHealth.active();
	const uint32_t target = supplyRecovery.active() ?
	    playerBotSupplyRecoveryRequiredHealth(std::max(0, player->getMaxHealth())) : 0;
	const auto result = huntDepartureHealth.advance(player->getHealth(), player->getMaxHealth(), target,
	    std::chrono::steady_clock::now());
	if (result == PlayerBotDepartureHealthResult::TimedOut) {
		stop("hunt_departure_healing_timeout", position);
		return false;
	}
	if (wasActive != huntDepartureHealth.active()) {
		emit("action_result", position, std::string("\"action\":\"hunt_departure_health\",\"result\":\"") +
		    (huntDepartureHealth.active() ? "started" : "ready") + "\",\"health\":" +
		    std::to_string(player->getHealth()) + ",\"required_health\":" + std::to_string(target));
	}
	// Callers own scheduling. The next turn executes the existing healing path
	// before any planning admission; prerequisite waits do not spend scan attempts.
	return result == PlayerBotDepartureHealthResult::Ready;
}

bool PlayerBotController::handleHealing(Player* player, const Position& currentPosition)
{
	if (huntDepartureHealth.active() && !prepareHuntDeparture(player, currentPosition) && telemetry.terminalLogged())
		return true;
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotSurvivalSnapshot snapshot = survivalSnapshot(*player);
	const PlayerBotSurvivalCommand command = survivalRuntime.decideHealing(snapshot, now);
	if (command.potionVerification) {
		const auto& verification = *command.potionVerification;
		if (verification.result == PlayerBotPotionVerificationResult::Success) {
			logHealResult(verification.before.itemId, "success", nullptr, verification.before, verification.after, currentPosition);
			// Hunt recovery evidence counts health potions only; mana potions have typed demand.
			if (!verification.before.restoresMana) recordHuntRecovery(true);
		} else {
			telemetry.recordActionFailure();
			logHealResult(verification.before.itemId, "failed", verification.result == PlayerBotPotionVerificationResult::IneffectiveRecovery ?
			              "ineffective_recovery" : "use_not_verified", verification.before, verification.after, currentPosition);
		}
	}
	if (!command.reason.empty() && !command.candidateName.empty()) {
		dispatchSpellCommand(*player, currentPosition, command);
	}
	if (command.type == PlayerBotSurvivalCommandType::None) return false;
	if (command.type == PlayerBotSurvivalCommandType::CastSpell) return dispatchSpellCommand(*player, currentPosition, command);
	if (command.type == PlayerBotSurvivalCommandType::Wait) return true;
	if (command.type == PlayerBotSurvivalCommandType::InterruptForService) {
		if (preparationOption != PlayerBotPreparationOption::None && !snapshot.combatActive &&
		    static_cast<int64_t>(snapshot.health) * 100 > static_cast<int64_t>(snapshot.healthMaximum) * 30) {
			if (preparationOption == PlayerBotPreparationOption::RestoreHealth) {
				finishPreparation(*player, currentPosition, "recovery_unavailable");
				return true;
			}
			return false;
		}
		if (huntDepartureHealth.active()) {
			if (snapshot.regenerationActive) return true; // Bounded by the preparation deadline.
			stop("hunt_departure_healing_unavailable", currentPosition);
			return true;
		}
		if (progressionRuntime.equipmentBackpackRecoveryActive()) return false;
		std::ostringstream fields;
		fields << "\"action\":\"heal\",\"result\":\"skipped\",\"reason\":\"missing_supply\""
		       << ",\"method\":" << jsonString(snapshot.potionItemId == smallHealthPotionItemId ? "small_health_potion" : "health_potion")
		       << ",\"item_id\":" << snapshot.potionItemId
		       << ",\"trigger\":\"health_threshold\",\"objective\":" << jsonString(objectiveName())
		       << ",\"state\":" << jsonString(turnRouter.stateName())
		       << ",\"health_before\":" << player->getHealth()
		       << ",\"health_after\":" << player->getHealth()
		       << ",\"health_max\":" << player->getMaxHealth()
		       << ",\"resource_before\":0,\"resource_after\":0";
		emit("action_result", currentPosition, fields.str());
		if (progressionRuntime.session().active() != PlayerBotProgressionProcedure::None) {
			if (interruptBackpackUpgradeForService(*player, currentPosition, "healing_supply_missing")) return true;
			if (progressionRuntime.session().active(PlayerBotProgressionProcedure::OracleDeparture)) {
				finishOracleDeparture(player, currentPosition, "interrupted", "healing_supply_missing");
			} else {
				finishProgressionObjective(player, currentPosition, "interrupted", "healing_supply_missing", false);
			}
			return true;
		}
		if (progressionRuntime.activeGoal() != TopLevelGoal::Service) {
			beginService(player, currentPosition, "healing_supply_missing");
		}
		return false;
	}
	// A mana top-up is routine combat upkeep, not a recovery interruption.
	if (command.reason != "healing_reserve") cancelHuntPlanning("survival_action", currentPosition);
	Item* potion = g_game.findItemOfType(player, command.itemId, true);
	if (!potion) {
		return true;
	}

	if (command.need == "mana") {
		std::ostringstream fields;
		fields << "\"action\":\"restore_mana\",\"result\":\"requested\",\"method\":\"mana_potion\",\"item_id\":" << command.itemId
		       << ",\"reason\":" << jsonString(command.reason) << ",\"mana_before\":" << player->getMana()
		       << ",\"mana_max\":" << player->getMaxMana() << ",\"mana_reserve\":" << command.manaReserve.value_or(0)
		       << ",\"health_before\":" << player->getHealth() << ",\"health_max\":" << player->getMaxHealth()
		       << ",\"resource_before\":" << snapshot.manaPotionCount;
		emit("action_result", currentPosition, fields.str());
	}
	survivalRuntime.beginPotion(survivalSnapshot(*player), command.itemId,
	                            command.need == "mana" ? command.reason : snapshot.departureHealthTarget ? "hunt_departure" : "health_threshold");
	telemetry.recordActionAttempt();
	g_game.playerUseWithCreature(playerId, Position(0xFFFF, 0, 0), 0, playerId, potion->getClientID());
	return true;
}

void PlayerBotController::logEatSuccess(uint16_t itemId, uint32_t inventoryCount, int32_t foodTicks, const Position& position)
{
	std::ostringstream fields;
	fields << "\"action\":\"eat\",\"result\":\"success\",\"item_id\":" << itemId
	       << ",\"count\":1,\"inventory_count\":" << inventoryCount << ",\"food_ticks\":" << foodTicks;
	emit("action_result", position, fields.str());
}

bool PlayerBotController::handleFood(Player* player, const Position& currentPosition)
{
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotSurvivalCommand command = survivalRuntime.decideFood(survivalSnapshot(*player), now);
	if (command.foodVerification) {
		const auto& verification = *command.foodVerification;
		if (verification.result == PlayerBotFoodVerificationResult::Success) {
			logEatSuccess(verification.before.itemId, verification.inventoryCount, verification.foodTicks, currentPosition);
		} else if (verification.result == PlayerBotFoodVerificationResult::Failed ||
		           verification.result == PlayerBotFoodVerificationResult::Cooldown) {
			logActionFailure("eat", "consumption_not_verified", currentPosition);
			if (verification.result == PlayerBotFoodVerificationResult::Cooldown) {
				emit("action_result", currentPosition,
				     "\"action\":\"eat\",\"result\":\"cooldown\",\"reason\":\"retry_exhausted\",\"retry_after_ms\":" +
				         std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(eatFailureCooldown).count()));
			}
		}
	}
	if (command.type == PlayerBotSurvivalCommandType::None) return false;
	if (command.type == PlayerBotSurvivalCommandType::Wait) return true;
	telemetry.recordActionAttempt();
	g_game.playerUseItem(playerId, Position(0xFFFF, 0, 0), 0, 0, command.itemClientId);
	return true;
}

bool PlayerBotController::attackVisibleMonster(Player* player, const Position& currentPosition, uint32_t preferredTargetId,
	                                            bool routeValidated)
{
	const bool travellingToHunt = huntCoordinator.huntActive() && !huntRegionReached;
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	std::vector<PlayerBotTraversalCandidate> candidates;
	for (Creature* creature : spectators) {
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || !player->canSee(creature->getPosition())) {
			continue;
		}
		if (preferredTargetId != 0 && creature->getID() != preferredTargetId) continue;
		const Monster* monster = creature->getMonster();
		if (travellingToHunt && preferredTargetId == 0 &&
		    (creature->getAttackedCreature() != player ||
		     Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition()) ||
		     monster->getTargetDistance() <= 1)) continue;
		if (huntCoordinator.huntActive() && creature->getAttackedCreature() != player && !huntCoordinator.matchesHuntMonster(creature->getName())) {
			continue;
		}
		candidates.push_back({{creature->getID(), creature->getPosition(), creature->getName()}, expectedCorpseFor(*creature),
		                      creature->getAttackedCreature() == player});
	}
	while (!candidates.empty()) {
		const auto command = huntCoordinator.selectTraversalAttack(candidates, currentPosition, std::chrono::steady_clock::now());
		if (!command) {
			return false;
		}
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [command](const PlayerBotTraversalCandidate& candidate) {
			return candidate.id == command->target.id;
		}), candidates.end());
		Creature* target = g_game.getCreatureByID(command->target.id);
		if (!target) {
			continue;
		}
		const uint8_t engagementRange = rangedAttackRange(*player);
		if (!routeValidated && (currentPosition.z != target->getPosition().z ||
		                        playerBotChebyshev(currentPosition, target->getPosition()) > engagementRange)) {
			const PlayerBotNavigationRoutePlan route = planNavigationRoute(
			    *player, PlayerBotNavigationGoal::withinRange(target->getPosition(), engagementRange, engagementRange), {},
			    maximumTargetApproachExpandedNodes, true);
			telemetry.recordPathfinding(route.metrics.elapsed, route.metrics.result == PlayerBotNavigationResult::Reached);
			if (route.metrics.result != PlayerBotNavigationResult::Reached) {
				huntCoordinator.suppressTraversalTarget(target->getID(), std::chrono::steady_clock::now(),
				                                              navigationBlockSuppression);
				emit("action_result", currentPosition,
				     "\"action\":\"target_approach\",\"result\":\"skipped\",\"reason\":\"route_unavailable\",\"target_id\":" +
				         std::to_string(target->getID()) + ",\"same_floor\":true,\"expanded_nodes\":" +
				         std::to_string(route.metrics.expandedNodes));
				return false;
			}
		}
		telemetry.recordActionAttempt();
		g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
		g_game.playerSetAttackedCreature(playerId, target->getID());
		const PlayerBotCombatDecision started = huntCoordinator.confirmCombatAttack(*command, player->getAttackedCreature() == target,
		                                                                  std::chrono::steady_clock::now());
		if (!started.result || std::strcmp(started.result, "started") != 0) {
			continue;
		}
		resetNavigation();
		setStage(ScenarioStage::TraversalCombat, currentPosition);
		std::ostringstream fields;
		fields << "\"previous_target_id\":null,\"target_id\":" << started.target.id
		       << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(started.target.name)
		       << ",\"target_position\":{\"x\":" << started.target.position.x << ",\"y\":" << started.target.position.y
		       << ",\"z\":" << static_cast<uint16_t>(started.target.position.z) << "},\"reason\":\"visible_monster\"";
		emit("target_changed", currentPosition, fields.str());
		return true;
	}
	return false;
}

bool PlayerBotController::attackDefensiveThreat(Player* player, const Position& currentPosition)
{
	if (huntCoordinator.traversalTarget()) return false;
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, currentPosition);
	const bool movementFallback = huntCoordinator.transitMovementFallbackRequired();
	const bool planningDefense = turnRouter.cyclePhase() == CyclePhase::Hunt &&
	                             fixtureDriver.huntObservation().selectRegion && !huntCoordinator.huntActive();
	const std::optional<Position> intendedStep = huntCoordinator.transitIntendedStep();
	std::vector<Creature*> adjacentAttackers;
	for (Creature* creature : spectators) {
		if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
		    creature->getAttackedCreature() != player || !player->canSeeCreature(creature) ||
		    !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) continue;
		adjacentAttackers.push_back(creature);
	}

	std::vector<PlayerBotDefensiveTarget> candidates;
	if (movementFallback) {
		for (Creature* creature : adjacentAttackers) {
			const auto risk = manageablePassageFight(*player, adjacentAttackers, *creature);
			if (!risk) continue;
			PlayerBotDefensiveTarget candidate;
			candidate.id = creature->getID();
			candidate.position = creature->getPosition();
			candidate.name = creature->getName();
			candidate.routeCritical = true;
			candidate.intendedStep = intendedStep && creature->getPosition() == *intendedStep;
			candidate.predictedFightDamage = risk->first;
			candidate.predictedFightSeconds = risk->second;
			candidates.push_back(std::move(candidate));
		}
	} else if (!huntCoordinator.inTransit() || planningDefense) {
		for (Creature* creature : adjacentAttackers) {
			candidates.push_back({creature->getID(), creature->getPosition(), creature->getName()});
		}
	}
	const auto command = huntCoordinator.selectDefensiveAttack(std::move(candidates), currentPosition, planningDefense);
	if (!command) return false;
	Creature* target = g_game.getCreatureByID(command->target.id);
	if (!target) return false;
	telemetry.recordActionAttempt();
	g_game.playerSetFightModes(playerId, FIGHTMODE_ATTACK, false, false);
	g_game.playerSetAttackedCreature(playerId, target->getID());
	const PlayerBotCombatDecision started = huntCoordinator.confirmCombatAttack(
	    *command, player->getAttackedCreature() == target, std::chrono::steady_clock::now());
	if (!started.result || std::strcmp(started.result, "started") != 0) {
		logActionFailure("defensive_combat", "target_rejected", currentPosition);
		return false;
	}
	if (planningDefense) cancelHuntPlanning("defensive_attacker", currentPosition);
	resetNavigation();
	std::ostringstream targetFields;
	targetFields << "\"previous_target_id\":null,\"target_id\":" << started.target.id
	             << ",\"target_type\":\"monster\",\"target_name\":" << jsonString(started.target.name)
	             << ",\"target_position\":{\"x\":" << started.target.position.x
	             << ",\"y\":" << started.target.position.y << ",\"z\":"
	             << static_cast<uint16_t>(started.target.position.z) << "},\"reason\":"
	             << jsonString(started.routeCritical ? "defensive_path_blocker" : "defensive_attacker")
	             << ",\"route_critical\":" << (started.routeCritical ? "true" : "false");
	if (started.routeCritical) {
		targetFields << ",\"fallback_cause\":\"movement_stalled\",\"selected_intended_step\":"
		             << (started.intendedStep ? "true" : "false")
		             << ",\"predicted_fight_damage\":" << started.predictedFightDamage
		             << ",\"predicted_fight_seconds\":" << started.predictedFightSeconds
		             << ",\"intended_step\":";
		if (intendedStep) {
			targetFields << "{\"x\":" << intendedStep->x << ",\"y\":" << intendedStep->y
			             << ",\"z\":" << static_cast<uint16_t>(intendedStep->z) << '}';
		} else {
			targetFields << "null";
		}
	}
	emit("target_changed", currentPosition, targetFields.str());
	emit("action_result", currentPosition,
	     "\"action\":\"defensive_combat\",\"result\":\"started\",\"target_id\":" +
	         std::to_string(started.target.id) + ",\"chase\":false,\"route_critical\":" +
	         (started.routeCritical ? "true" : "false"));
	return true;
}

void PlayerBotController::finishDefensiveCombat(Player* player, const Position& currentPosition, const char* result, const char* reason)
{
	const auto previous = huntCoordinator.defensiveTarget();
	const uint32_t previousTarget = previous ? previous->id : 0;
	const std::optional<Position> intendedStep = previous && previous->routeCritical ?
	    huntCoordinator.transitIntendedStep() : std::nullopt;
	huntCoordinator.clearDefensiveTarget();
	if (previous && previous->routeCritical) huntCoordinator.clearTransitMovementFallback();
	if (player->getAttackedCreature() && player->getAttackedCreature()->getID() == previousTarget) {
		g_game.playerSetAttackedCreature(playerId, 0);
	}
	resetNavigation();
	emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTarget) +
	     ",\"target_id\":null,\"reason\":" + jsonString(reason));
	std::ostringstream resultFields;
	resultFields << "\"action\":\"defensive_combat\",\"result\":" << jsonString(result)
	             << ",\"target_id\":" << previousTarget << ",\"reason\":" << jsonString(reason);
	if (previous && previous->routeCritical) {
		resultFields << ",\"fallback_cause\":\"movement_stalled\",\"intended_step\":";
		if (intendedStep) {
			resultFields << "{\"x\":" << intendedStep->x << ",\"y\":" << intendedStep->y
			             << ",\"z\":" << static_cast<uint16_t>(intendedStep->z) << '}';
		} else {
			resultFields << "null";
		}
	}
	emit("action_result", currentPosition, resultFields.str());
}

void PlayerBotController::processDefensiveCombat(Player* player, const Position& currentPosition)
{
	const auto defensive = huntCoordinator.defensiveTarget();
	Creature* target = defensive ? g_game.getCreatureByID(defensive->id) : nullptr;
	PlayerBotCombatTargetSnapshot observed;
	if (target) {
		observed.present = true;
		observed.removed = target->isRemoved();
		observed.dead = target->isDead();
		if (!observed.removed && !observed.dead) {
			observed.visible = player->canSee(target->getPosition());
			observed.visibleCreature = player->canSeeCreature(target);
			observed.adjacent = Position::areInRange<1, 1, 0>(currentPosition, target->getPosition());
			observed.attacksPlayer = target->getAttackedCreature() == player;
			observed.attackedByPlayer = player->getAttackedCreature() == target;
			observed.target = {target->getID(), target->getPosition(), target->getName()};
		}
	}
	const bool targetLifetimeComplete = defensive &&
	    playerBotDefensiveLifetimeCompletion(*defensive, observed).has_value();
	if (defensive && defensive->routeCritical && target && !targetLifetimeComplete) {
		const std::optional<Position> intendedStep = huntCoordinator.transitIntendedStep();
		if (intendedStep) {
			Tile* intendedTile = g_game.map.getTile(*intendedStep);
			if (intendedTile && intendedTile->queryAdd(0, *player, 1, 0) == RETURNVALUE_NOERROR) {
				finishDefensiveCombat(player, currentPosition, "skipped", "transit_passage_open");
				schedule(navigationInterval);
				return;
			}
		}
		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, currentPosition);
		std::vector<Creature*> adjacentAttackers;
		for (Creature* creature : spectators) {
			if (!creature->getMonster() || creature->isRemoved() || creature->isDead() ||
			    !player->canSee(creature->getPosition()) ||
			    !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) continue;
			if (creature->getAttackedCreature() == player || creature == target) {
				adjacentAttackers.push_back(creature);
			}
		}
		const bool targetAdjacent = Position::areInRange<1, 1, 0>(currentPosition, target->getPosition());
		const bool safe = targetAdjacent && manageablePassageFight(*player, adjacentAttackers, *target).has_value();
		if (!huntCoordinator.retainTransitDefense(defensive->id, currentPosition, target->getPosition(), safe)) {
			const char* reason = !targetAdjacent ? "transit_blocker_moved" :
			                     !safe ? "transit_passage_unsafe" : "transit_passage_open";
			finishDefensiveCombat(player, currentPosition, "skipped", reason);
			schedule(navigationInterval);
			return;
		}
	}
	const PlayerBotCombatDecision decision = huntCoordinator.advanceCombat({currentPosition, std::chrono::steady_clock::now(), {}, observed});
	if (decision.command == PlayerBotCombatCommand::CompleteDefensiveCombat) {
		finishDefensiveCombat(player, currentPosition, decision.result, decision.reason);
	}
	schedule(navigationInterval);
}

void PlayerBotController::finishTraversalCombat(Player* player, const Position& currentPosition, const char* reason)
{
	g_game.playerSetAttackedCreature(playerId, 0);
	clearTraversalTarget(currentPosition, reason);
	resetNavigation();
	setStage(ScenarioStage::Traverse, currentPosition);
}

void PlayerBotController::processTraversalCombat(Player* player, const Position& currentPosition)
{
	const auto traversal = huntCoordinator.traversalTarget();
	Creature* target = traversal ? g_game.getCreatureByID(traversal->id) : nullptr;
	const bool travellingToHunt = huntCoordinator.huntActive() && !huntRegionReached;
	if (target && target->getAttackedCreature() != player) {
		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, currentPosition);
		const auto now = std::chrono::steady_clock::now();
		const uint32_t currentTargetDistance = std::max(Position::getDistanceX(currentPosition, target->getPosition()),
		                                                Position::getDistanceY(currentPosition, target->getPosition()));
		uint64_t remainingExpandedNodes = maximumTargetApproachExpandedNodes;
		uint32_t reachableAttackerId = 0;
		for (Creature* creature : spectators) {
			if (!creature->getMonster() || creature->isRemoved() || creature->isDead() || creature == target ||
			    creature->getAttackedCreature() != player || !player->canSee(creature->getPosition())) {
				continue;
			}
			if (travellingToHunt && !Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition()) &&
			    creature->getMonster()->getTargetDistance() <= 1) continue;
			const uint32_t candidateDistance = std::max(Position::getDistanceX(currentPosition, creature->getPosition()),
			                                            Position::getDistanceY(currentPosition, creature->getPosition()));
			if (candidateDistance >= currentTargetDistance) continue;
			const std::vector<PlayerBotTraversalCandidate> candidate{{
			    {creature->getID(), creature->getPosition(), creature->getName()}, expectedCorpseFor(*creature), true}};
			if (!huntCoordinator.selectTraversalAttack(candidate, currentPosition, now)) continue;
			if (Position::areInRange<1, 1, 0>(currentPosition, creature->getPosition())) {
				reachableAttackerId = creature->getID();
				break;
			}
			if (remainingExpandedNodes == 0) break;
			const PlayerBotNavigationRoutePlan route = planNavigationRoute(
			    *player, PlayerBotNavigationGoal::withinRange(creature->getPosition(), 1, 1), {},
			    remainingExpandedNodes, true);
			telemetry.recordPathfinding(route.metrics.elapsed, route.metrics.result == PlayerBotNavigationResult::Reached);
			remainingExpandedNodes -= std::min(remainingExpandedNodes, route.metrics.expandedNodes);
			if (route.metrics.result == PlayerBotNavigationResult::Reached) {
				reachableAttackerId = creature->getID();
				break;
			}
		}
		if (reachableAttackerId != 0) {
			const uint32_t previousTargetId = target->getID();
			g_game.playerSetAttackedCreature(playerId, 0);
			huntCoordinator.suppressTraversalTarget(previousTargetId, now,
			                                              traversalTargetSuppression);
			huntCoordinator.clearTraversalTarget();
			resetNavigation();
			setStage(ScenarioStage::Traverse, currentPosition);
			emit("target_changed", currentPosition, "\"previous_target_id\":" + std::to_string(previousTargetId) +
			     ",\"target_id\":null,\"reason\":\"active_attacker_preempted\"");
			attackVisibleMonster(player, currentPosition, reachableAttackerId, true);
			schedule(navigationInterval);
			return;
		}
	}
	PlayerBotCombatTargetSnapshot observed;
	if (target) observed = {true, target->isRemoved(), target->isDead(), player->canSee(target->getPosition()), player->canSeeCreature(target),
	                        Position::areInRange<1, 1, 0>(currentPosition, target->getPosition()), target->getAttackedCreature() == player,
	                        player->getAttackedCreature() == target, {target->getID(), target->getPosition(), target->getName()}};
	// Lifetime and last-known target position must advance even on movement turns.
	const PlayerBotCombatDecision combat = huntCoordinator.advanceCombat(
	    {currentPosition, std::chrono::steady_clock::now(), observed, {}});
	if (combat.command == PlayerBotCombatCommand::BeginLoot) {
		resetNavigation();
		beginLoot(player, currentPosition, combat);
		schedule(navigationInterval);
		return;
	}
	if (combat.command == PlayerBotCombatCommand::Abandon) {
		if (combat.reason && std::strcmp(combat.reason, "combat_timeout") == 0) {
			logActionFailure("attack", combat.reason, currentPosition);
		}
		finishTraversalCombat(player, currentPosition, combat.reason ? combat.reason : "target_lost");
		schedule(navigationInterval);
		return;
	}
	const bool targetEngageable = target && observed.present && !observed.removed && !observed.dead &&
	                              observed.visible && observed.visibleCreature && observed.attackedByPlayer;
	const uint8_t weaponRange = targetEngageable ? rangedAttackRange(*player) : 1;
	bool approachTarget = targetEngageable && !observed.adjacent;
	uint8_t approachRange = 1;
	if (targetEngageable && weaponRange > 1) {
		const RangedTurn turn = positionRangedFight(player, currentPosition, *target, weaponRange);
		if (turn.waiting || turn.action == PlayerBotRangedAction::Retreat) {
			schedule(navigationDecisionDelay(*player));
			return;
		}
		approachTarget = turn.action == PlayerBotRangedAction::Close;
		approachRange = turn.approachRange;
	}
	if (approachTarget) {
		PlayerBotNavigationRuntimeOutcome navigation;
		if (!processNavigation(player, currentPosition,
		                       PlayerBotNavigationGoal::withinRange(target->getPosition(), approachRange, approachRange),
		                       &navigation, maximumTargetApproachExpandedNodes, false, true)) {
			if (navigation.routeUnavailable || navigation.oscillation) {
				const uint32_t previousTargetId = target->getID();
				huntCoordinator.suppressTraversalTarget(previousTargetId, std::chrono::steady_clock::now(),
				                                              navigationBlockSuppression);
				emit("action_result", currentPosition,
				     "\"action\":\"target_approach\",\"result\":\"skipped\",\"reason\":\"route_unavailable\",\"target_id\":" +
				         std::to_string(previousTargetId) + ",\"same_floor\":true");
				finishTraversalCombat(player, currentPosition, "target_route_unavailable");
			}
			return;
		}
	}
	if (targetEngageable && tryOffensiveSpell(player, currentPosition)) {
		schedule(navigationDecisionDelay(*player));
		return;
	}
	// A closing monster reaches a ranged bot within a step or two, so check sooner.
	schedule(weaponRange > 1 && targetEngageable ? rangedPositionInterval : navigationInterval);
}

uint8_t PlayerBotController::rangedAttackRange(const Player& player) const
{
	if (fixtureDriver.rangedPositionDisabled()) return 1;
	const Item* launcher = player.getWeapon(true);
	if (!launcher || launcher->getWeaponType() != WEAPON_DISTANCE || !player.getWeapon()) return 1;
	return std::max<uint8_t>(1, launcher->getShootRange());
}

PlayerBotController::RangedTurn PlayerBotController::positionRangedFight(Player* player, const Position& currentPosition,
	                                                                     Creature& target, uint8_t range)
{
	const auto now = std::chrono::steady_clock::now();
	const bool actionPending = player->getWalkDelay() > 0 || !player->canDoAction();
	const auto pendingTile = rangedMovement.pendingMoveTarget();
	const auto movement = rangedMovement.observeMovement(currentPosition, actionPending, now,
	                                                     navigationStepTimeout, navigationBlockSuppression);
	if (movement == PlayerBotPendingMovementResult::Waiting) return {PlayerBotRangedAction::Hold, 1, true};
	if (pendingTile) {
		std::ostringstream fields;
		fields << "\"result\":\"" << (movement == PlayerBotPendingMovementResult::Completed ? "completed" : "failed")
		       << "\",\"target_id\":" << target.getID() << ",\"tile\":{\"x\":" << pendingTile->x
		       << ",\"y\":" << pendingTile->y << ",\"z\":" << static_cast<uint16_t>(pendingTile->z) << '}';
		emit("ranged_position_step", currentPosition, fields.str());
	}
	PlayerBotRangedPositionInput input;
	input.self = currentPosition;
	input.target = target.getPosition();
	input.range = range;
	// Engine step durations include terrain and the near-target monster slowdown.
	input.selfStepMs = player->getStepDuration();
	input.targetStepMs = target.getStepDuration();
	input.sightClear = g_game.canThrowObjectTo(currentPosition, input.target, true, true, range, range);
	const uint32_t distance = playerBotChebyshev(currentPosition, input.target);
	// Only a closing monster needs the neighbouring tiles; skip the work otherwise.
	if (currentPosition.z == input.target.z && distance < range && input.targetStepMs >= input.selfStepMs &&
	    movement != PlayerBotPendingMovementResult::Mismatch) {
		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, currentPosition);
		std::vector<Position> hostiles;
		for (Creature* creature : spectators) {
			const Monster* monster = creature->getMonster();
			if (!monster || creature == &target || creature->isRemoved() || creature->isDead() || creature->getMaster() ||
			    !monster->isHostile()) continue;
			hostiles.push_back(creature->getPosition());
		}
		const PlayerBotNavigationCostPolicy costPolicy = navigationCostPolicy(*player);
		const double currentDanger = costPolicy.dangerAt(currentPosition);
		const bool confined = huntCoordinator.huntActive() && huntRegionReached;
		// Cardinal tiles first: diagonal steps take longer.
		static constexpr Direction directions[] = {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
		                                           DIRECTION_NORTHEAST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST,
		                                           DIRECTION_NORTHWEST};
		const PlayerBotNavigator navigator;
		const auto blocked = rangedMovement.activeBlockedPositions(now);
		for (Direction direction : directions) {
			PlayerBotNavigationStep step;
			PlayerBotRangedTile tile;
			tile.position = getNextPosition(direction, currentPosition);
			const Tile* destination = g_game.map.getTile(tile.position);
			tile.open = destination && !destination->getTopCreature() &&
			            !destination->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT | TILESTATE_PROTECTIONZONE) &&
			            navigator.resolveMove(*player, currentPosition, direction, blocked, step) &&
			            step.expectedPosition == tile.position;
			if (tile.open) {
				tile.insideArea = !confined || huntCoordinator.insideHuntArea(tile.position, Map::maxClientViewportX,
				                                                              Map::maxClientViewportX + 1,
				                                                              Map::maxClientViewportY,
				                                                              Map::maxClientViewportY + 1);
				tile.hazardous = costPolicy.dangerAt(tile.position) > currentDanger;
				tile.sightClear = g_game.canThrowObjectTo(tile.position, input.target, true, true, range, range);
				tile.hostileNearby = std::any_of(hostiles.begin(), hostiles.end(), [&tile](const Position& hostile) {
					return tile.position.z == hostile.z && playerBotChebyshev(tile.position, hostile) <= 2;
				});
			}
			input.neighbours.push_back(tile);
		}
	}
	PlayerBotRangedPositionDecision decision = playerBotRangedPosition(input);
	if (movement == PlayerBotPendingMovementResult::Mismatch) {
		decision.action = PlayerBotRangedAction::FightInPlace;
		decision.reason = "retreat_failed";
	}
	// Local positioning owns movement until closing resumes. An old approach
	// destination must not later be interpreted as a failed retreat landing.
	if (decision.action != PlayerBotRangedAction::Close) navigationRuntime.reset();
	RangedTurn turn;
	turn.action = decision.action;
	if (decision.action == PlayerBotRangedAction::Close) {
		// With a blocked shot inside range, `range` is already "reached": step closer instead.
		turn.approachRange = decision.distance > range ? range : static_cast<uint8_t>(std::max<uint32_t>(1, decision.distance - 1));
	}
	if (decision.action == PlayerBotRangedAction::Retreat) {
		if (actionPending) return {PlayerBotRangedAction::Hold, 1, true};
		PlayerBotNavigationStep step;
		step.action = PlayerBotNavigationAction::Move;
		step.direction = getDirectionTo(currentPosition, decision.tile);
		step.target = step.expectedPosition = decision.tile;
		if (executeNavigationStep(player, step)) {
			rangedMovement.beginMovement(step, now);
		} else {
			rangedMovement.suppress(step.target, now + navigationBlockSuppression);
			turn.action = PlayerBotRangedAction::FightInPlace;
		}
	}
	const char* reason = turn.action == decision.action ? decision.reason : "retreat_failed";
	if (decision.action == PlayerBotRangedAction::Retreat || lastRangedPosition.targetId != target.getID() ||
	    lastRangedPosition.action != turn.action || lastRangedPosition.reason != reason) {
		lastRangedPosition = {target.getID(), turn.action, reason};
		static constexpr const char* names[] = {"hold", "retreat", "close", "fight_in_place"};
		std::ostringstream fields;
		fields << "\"action\":\"" << names[static_cast<uint8_t>(turn.action)] << "\",\"reason\":" << jsonString(reason)
		       << ",\"target_id\":" << target.getID() << ",\"distance\":" << decision.distance
		       << ",\"range\":" << static_cast<uint32_t>(range) << ",\"self_speed\":" << player->getSpeed()
		       << ",\"target_speed\":" << target.getSpeed() << ",\"self_step_ms\":" << input.selfStepMs
		       << ",\"target_step_ms\":" << input.targetStepMs << ",\"sight_clear\":"
		       << (input.sightClear ? "true" : "false");
		if (decision.action == PlayerBotRangedAction::Retreat) {
			fields << ",\"tile\":{\"x\":" << decision.tile.x << ",\"y\":" << decision.tile.y << ",\"z\":"
			       << static_cast<uint16_t>(decision.tile.z) << '}';
		}
		emit("ranged_position", currentPosition, fields.str());
	}
	return turn;
}

bool PlayerBotController::isActiveHuntCombat(const Player& player) const
{
	return huntCoordinator.huntActive() && huntRegionReached && turnRouter.cyclePhase() == CyclePhase::Hunt &&
	       turnRouter.scenarioStage() == ScenarioStage::TraversalCombat &&
	       const_cast<Player&>(player).getAttackedCreature() != nullptr;
}

void PlayerBotController::recordActiveHuntCombat(const Player& player)
{
	const auto now = std::chrono::steady_clock::now();
	const bool active = isActiveHuntCombat(player);
	SpectatorVec spectators;
	uint32_t attackers = 0;
	if (active) {
		g_game.map.getSpectators(spectators, player.getPosition());
		for (Creature* creature : spectators) {
			if (creature->getMonster() && creature->getAttackedCreature() == &player) {
				++attackers;
			}
		}
	}
	bool foodActive = false;
	bool foodAvailable = false;
	if (active) {
		foodActive = player.getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT, 0) != nullptr;
		foodAvailable = foodActive || PlayerBotInventoryPolicy::foodInventory(player).count > 0;
	}
	huntCoordinator.sampleHuntCombat({active, now, player.getHealth(), player.getMaxHealth(),
	                                player.getMana(), player.getMaxMana(), attackers,
	                                foodActive, foodAvailable});
}

void PlayerBotController::recordHuntRecovery(bool potion)
{
	Player* player = g_game.getPlayerByID(playerId);
	if (!player || !huntCoordinator.huntActive() || !huntRegionReached || turnRouter.cyclePhase() != CyclePhase::Hunt) {
		return;
	}
	if (potion) {
		huntCoordinator.observeHuntRecovery(true);
	} else {
		huntCoordinator.observeHuntRecovery(false);
	}
}

void PlayerBotController::emitChallengeFrontier(const PlayerBotHuntChallengeUpdate& update, const Position& position,
	                                                const char* reason) const
{
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(3)
	       << "\"result\":" << jsonString(playerBotHuntChallengeResultName(update.result))
	       << ",\"reason\":" << jsonString(reason)
	       << ",\"frontier_before\":" << update.frontierBefore
	       << ",\"frontier_after\":" << update.frontierAfter
	       << ",\"hold_qualifying_hunts\":" << static_cast<uint16_t>(update.qualifyingHuntsToHold)
	       << ",\"active_combat_seconds\":" << update.combat.activeSeconds
	       << ",\"active_combat_uptime\":" << update.activeCombatUptime
	       << ",\"kills\":" << update.combat.kills
	       << ",\"minimum_active_combat_seconds\":" << update.minimumActiveCombatSeconds
	       << ",\"minimum_kills\":" << update.minimumKills
	       << ",\"minimum_health\":" << (update.combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : update.combat.minimumHealth)
	       << ",\"p10_health_percent\":" << static_cast<uint16_t>(update.combat.p10HealthPercent)
	       << ",\"minimum_mana\":" << (update.combat.minimumMana == std::numeric_limits<uint32_t>::max() ? 0 : update.combat.minimumMana)
	       << ",\"p10_mana_percent\":" << static_cast<uint16_t>(update.combat.p10ManaPercent)
	       << ",\"verified_recoveries\":" << update.verifiedRecoveries
	       << ",\"potion_recoveries_per_active_minute\":" << update.potionRecoveriesPerActiveMinute
	       << ",\"retreat\":" << (std::strcmp(reason, "hunt_region_observed_danger") == 0 ? "true" : "false")
	       << ",\"danger\":" << (update.combat.dangerObserved ? "true" : "false")
	       << ",\"death\":" << (update.combat.deathObserved ? "true" : "false");
	emit("hunt_challenge_frontier", position, fields.str());
}

void PlayerBotController::emitHuntRegionCandidate(const PlayerBotHuntRegion& region, const Position& position,
                                                   uint64_t planningPass, uint64_t scoringRevision,
                                                   const char* candidatePhase) const
{
	uint32_t unstablePatrolPoints = 0;
	for (const Position& point : region.patrolPoints) {
		const Tile* tile = g_game.map.getTile(point);
		if (!tile || tile->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) ++unstablePatrolPoints;
	}
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2)
	       << "\"planning_pass\":" << planningPass
	       << ",\"scoring_revision\":" << scoringRevision
	       << ",\"candidate_phase\":" << jsonString(candidatePhase)
	       << ",\"region_id\":" << region.id
	       << ",\"atlas_site_id\":" << region.atlasSiteId
	       << ",\"atlas_variant_id\":" << region.atlasVariantId
	       << ",\"atlas_revision\":" << region.atlasRevision
	       << ",\"atlas_pockets\":" << region.atlasPocketCount
	       << ",\"atlas_spawns\":" << region.atlasSpawnCount
	       << ",\"atlas_floors\":" << region.atlasFloorCount
	       << ",\"floor\":" << static_cast<uint16_t>(region.floor)
	       << ",\"center\":{\"x\":" << region.center.x << ",\"y\":" << region.center.y
	       << ",\"z\":" << static_cast<uint16_t>(region.center.z) << '}'
	       << ",\"destination\":{\"x\":" << region.destination.x << ",\"y\":" << region.destination.y
	       << ",\"z\":" << static_cast<uint16_t>(region.destination.z) << '}'
	       << ",\"patrol_points\":" << region.patrolPoints.size()
	       << ",\"unstable_patrol_points\":" << unstablePatrolPoints
	       << ",\"experience_per_minute\":" << region.experiencePerMinute
	       << ",\"coin_estimate_source\":\"static_loaded_loot_gross\""
	       << ",\"expected_coin_gold_per_minute\":" << region.coinGoldPerMinute
	       << ",\"cash_pressure\":" << (region.cashPressure ? "true" : "false")
	       << ",\"supply_recovery\":" << (region.supplyRecovery ? "true" : "false")
	       << ",\"recovery_route_health_loss\":" << region.recoveryRouteHealthLoss
	       << ",\"estimated_return_seconds\":" << region.estimatedReturnSeconds
	       << ",\"estimated_supply_seconds\":" << region.estimatedSupplySeconds
	       << ",\"profitability_hint\":\"" << playerBotHuntProfitabilityHintName(region.economicKey().profitabilityHint) << '\"'
	       << ",\"economic_coin_gold_per_minute\":" << region.economicKey().coinGoldPerMinute
	       << ",\"sustained_eligible\":" << (region.sustainedEligible ? "true" : "false")
	       << ",\"reachable_spawns\":" << region.viability.reachableSpawns
	       << ",\"replenishing_spawns\":" << region.viability.replenishingSpawns
	       << ",\"replenishment_estimate_source\":\"patrol_duty_share_heuristic\""
	       << ",\"minimum_away_interval_ratio\":" << region.viability.minimumAwayRatio
	       << ",\"minimum_unblocked_patrol_ratio\":" << region.viability.minimumUnblockedPatrolRatio
	       << ",\"minimum_empty_patrol_unblocked_ratio\":" << region.viability.minimumEmptyPatrolRatio
	       << ",\"spawn_experience_per_minute\":" << region.spawnExperiencePerMinute
	       << ",\"clear_experience_per_minute\":" << region.clearExperiencePerMinute
	       << ",\"estimated_travel_seconds\":" << region.estimatedTravelSeconds
	       << ",\"available_hunt_seconds\":" << region.availableHuntSeconds
	       << ",\"observed_experience_per_minute\":" << region.observedExperiencePerMinute
	       << ",\"observed_correction\":" << region.observedCorrection
	       << ",\"calibration_source\":" << jsonString(region.calibrationSource)
	       << ",\"calibration_sample_count\":" << region.calibrationSampleCount
	       << ",\"stamina_minutes\":" << region.staminaMinutes
	       << ",\"stamina_experience_multiplier\":" << region.staminaExperienceMultiplier
	       << ",\"projected_experience\":" << region.projectedExperience
	       << ",\"optimistic_projected_experience\":" << region.optimisticProjectedExperience
	       << ",\"route_validated\":" << (region.routeValidated ? "true" : "false")
	       << ",\"supply_estimate_source\":" << jsonString(region.supplyEstimateSource)
	       << ",\"supply_estimate_reason\":" << jsonString(region.supplyEstimateReason)
	       << ",\"supply_local_rejection_reason\":"
	       << (region.supplyLocalRejectionReason ? jsonString(region.supplyLocalRejectionReason) : "null")
	       << ",\"supply_global_multiplier\":" << region.supplyGlobalLearning.multiplier
	       << ",\"supply_global_samples\":" << region.supplyGlobalLearning.samples
	       << ",\"supply_static_potions_per_combat_minute\":"
	       << region.supplyStaticPotionsPerCombatSecond * 60
	       << ",\"supply_budget_fits\":" << (region.supplyBudget.fits ? "true" : "false")
	       << ",\"supply_expected_damage\":" << region.supplyBudget.expectedDamage
	       << ",\"supply_regeneration_healing\":" << region.supplyBudget.regenerationHealing
	       << ",\"supply_spell_healing\":" << region.supplyBudget.spellHealing
	       << ",\"supply_expected_potions\":" << region.supplyBudget.expectedPotions
	       << ",\"supply_calibration_samples\":" << region.supplyCalibration.samples
	       << ",\"supply_potions_per_combat_minute\":"
	       << region.supplyAppliedPotionsPerCombatSecond * 60
	       << ",\"supply_reserved_potions\":" << region.supplyBudget.reservedPotions
	       << ",\"supply_routine_potions\":" << region.supplyBudget.routinePotions
	       << ",\"supply_kinds\":" << supplyKindBudgetsJson(region.supplyKindBudgets)
	       << ",\"threat_ratio\":" << region.threatRatio
	       << ",\"raw_threat_ratio\":" << region.rawThreatRatio
	       << ",\"corridor_danger_available\":" << (region.corridorDangerAvailable ? "true" : "false")
	       << ",\"corridor_danger_ratio\":" << region.corridorDangerRatio
	       << ",\"corridor_samples\":" << region.corridorSampleCount
	       << ",\"corridor_spawn_blocks\":" << region.corridorSpawnBlocks
	       << ",\"current_health\":" << region.currentHealth
	       << ",\"predicted_fight_seconds\":" << region.predictedFightSeconds
	       << ",\"challenge_frontier\":" << region.challengeFrontier
	       << ",\"challenge_band_minimum\":" << region.challengeBandMinimum
	       << ",\"challenge_band_maximum\":" << region.challengeBandMaximum
	       << ",\"in_challenge_band\":" << (region.inChallengeBand ? "true" : "false")
	       << ",\"predicted_lethal\":" << (region.predictedLethal ? "true" : "false")
	       << ",\"recovery\":{\"light_healing_legal\":" << (region.recovery.lightHealingLegal ? "true" : "false")
	       << ",\"spell_minimum_healing\":" << region.recovery.spellMinimumHealing
	       << ",\"potion_minimum_healing\":" << region.recovery.potionMinimumHealing
	       << ",\"total_minimum_healing\":" << region.recovery.totalMinimumHealing
	       << ",\"available_before_lethal\":" << region.recovery.availableBeforeLethal
	       << ",\"spell_mana_cost\":" << region.recovery.spellManaCost
	       << ",\"spell_cooldown\":" << region.recovery.spellCooldown
	       << ",\"spell_casts\":" << region.recovery.spellCasts
	       << ",\"potion_uses\":" << region.recovery.potionUses
	       << ",\"mana_reserve\":" << region.recovery.manaReserve << '}'
	       << ",\"score\":" << region.score
	       << ",\"travel_steps\":" << region.travelSteps
	       << ",\"topology_reachable\":" << (region.topologyReachable ? "true" : "false")
	       << ",\"transport_plausible\":" << (region.transportPlausible ? "true" : "false")
	       << ",\"topology_travel_steps\":" << region.topologyTravelSteps
	       << ",\"route_danger_cost\":" << region.routeDangerCost
	       << ",\"maximum_route_danger\":" << region.maximumRouteDanger
	       << ",\"outbound_npc_travel\":" << (region.outboundNpcTravel ? "true" : "false")
	       << ",\"outbound_fare\":" << region.outboundFare
	       << ",\"exit_npc_travel\":" << (region.exitNpcTravel ? "true" : "false")
	       << ",\"exit_fare\":" << region.exitFare
	       << ",\"supply_npc_travel\":" << (region.supplyNpcTravel ? "true" : "false")
	       << ",\"supply_fare\":" << region.supplyFare
	       << ",\"exit_depot_destination\":{\"x\":" << region.exitDepotDestination.x
	       << ",\"y\":" << region.exitDepotDestination.y << ",\"z\":"
	       << static_cast<uint16_t>(region.exitDepotDestination.z) << '}'
	       << ",\"suitable\":" << (region.suitable ? "true" : "false")
	       << ",\"reachable\":" << (region.reachable ? "true" : "false")
	       << ",\"rejection_reason\":" << (region.rejectionReason.empty() ? "null" : jsonString(region.rejectionReason))
	       << ",\"monsters\":[";
	for (size_t index = 0; index < region.monsters.size(); ++index) {
		const PlayerBotHuntMonsterProfile& monster = region.monsters[index];
		if (index != 0) {
			fields << ',';
		}
		fields << "{\"name\":" << jsonString(monster.name)
		       << ",\"expected_spawns\":" << monster.expectedSpawns
		       << ",\"experience\":" << monster.experience
		       << ",\"health\":" << monster.health
		       << ",\"expected_dps\":" << monster.expectedDamagePerSecond
		       << ",\"predicted_fight_damage\":" << monster.predictedFightDamage << '}';
	}
	fields << ']';
	emit("hunt_region_candidate", position, fields.str());
}


void PlayerBotController::emitHuntRegionPlanning(const PlayerBotHuntPlanningSession& planning, const Position& position,
                                                  const char* phase, uint64_t planningPass,
                                                  uint64_t scoringRevision) const
{
	const auto latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - planning.started()).count();
	std::ostringstream fields;
	fields << "\"phase\":" << jsonString(phase) << ",\"planning_pass\":" << planningPass
	       << ",\"scoring_revision\":" << scoringRevision
	       << ",\"cache\":" << jsonString(planning.cacheHit() ? "hit" : "build")
	       << ",\"snapshot_time_us\":" << planning.snapshotTimeUs() << ",\"clustering_time_us\":" << planning.clusteringTimeUs()
	       << ",\"topology_time_us\":" << planning.topologyTimeUs()
	       << ",\"transport_offer_count\":" << planning.transportOfferCount()
	       << ",\"transport_arrival_count\":" << planning.transportArrivalCount()
	       << ",\"scoring_time_us\":" << planning.scoringTimeUs() << ",\"candidate_count\":" << planning.totalCandidates()
	       << ",\"scored_candidate_count\":" << planning.scoredCandidates() << ",\"suitable_candidate_count\":" << planning.suitableCandidates()
	       << ",\"route_candidate_policy\":\"all_cheap_viable_ranked\""
	       << ",\"route_candidate_count\":" << planning.routeCandidates().size()
	       << ",\"yields\":" << planning.yields() << ",\"selection_strategy\":"
	       << jsonString(planning.topologySelection() ? "atlas_topology_selection" : "atlas_geometric_selection")
	       << ",\"challenge_frontier\":" << planning.profile().challengeFrontier << ",\"decision_latency_us\":" << latencyUs;
	emit("hunt_region_scan", position, fields.str());
}

void PlayerBotController::recordHuntPlanningSlice(HuntPlanningSliceRecord record)
{
	// Scoring, transport, and route searches yield once per turn for up to
	// minutes. Merge those quiet slices into at most one record per interval
	// and phase; milestones and failures still close the record.
	if (huntPlanningSliceBacklog &&
	    (huntPlanningSliceBacklog->attribution.planningPass != record.attribution.planningPass ||
	     huntPlanningSliceBacklog->attribution.scoringRevision != record.attribution.scoringRevision ||
	     std::strcmp(huntPlanningSliceBacklog->phase, record.phase) != 0)) {
		flushHuntPlanningSlice();
	}
	if (huntPlanningSliceBacklog) {
		huntPlanningSliceBacklog->counters.add(record.counters);
		record.counters = huntPlanningSliceBacklog->counters;
		record.started = huntPlanningSliceBacklog->started;
		huntPlanningSliceBacklog.reset();
	}
	const bool quiet = std::strcmp(record.result, "route_pending") == 0 || std::strcmp(record.result, "yield") == 0 ||
	    (std::strcmp(record.result, "pending") == 0 &&
	     (std::strcmp(record.phase, "transport_yield") == 0 || std::strcmp(record.phase, "scoring_yield") == 0));
	if (quiet && std::chrono::steady_clock::now() - record.started < progressRecordInterval) {
		huntPlanningSliceBacklog = std::move(record);
		return;
	}
	emitHuntPlanningSlice(record);
}

void PlayerBotController::flushHuntPlanningSlice()
{
	if (!huntPlanningSliceBacklog) return;
	if (!telemetry.terminalLogged()) emitHuntPlanningSlice(*huntPlanningSliceBacklog);
	huntPlanningSliceBacklog.reset();
}

void PlayerBotController::emitHuntPlanningSlice(const HuntPlanningSliceRecord& record) const
{
	const PlayerBotHuntSliceCounters& counters = record.counters;
	std::ostringstream fields;
	fields << "\"phase\":" << jsonString(record.phase) << ",\"result\":" << jsonString(record.result)
	       << ",\"reason\":" << jsonString(record.reason) << ",\"planning_pass\":" << record.attribution.planningPass
	       << ",\"scoring_revision\":" << record.attribution.scoringRevision
	       << ",\"invalidated_planning_pass\":" << record.attribution.invalidatedPass
	       << ",\"invalidated_scoring_revision\":" << record.attribution.invalidatedRevision
	       << ",\"slices\":" << counters.slices << ",\"elapsed_us\":" << counters.elapsedUs
	       << ",\"max_elapsed_us\":" << counters.maxElapsedUs
	       << ",\"over_10ms\":" << (counters.maxElapsedUs >= PlayerBotHuntSliceTiming::overrunThresholdUs ? "true" : "false")
	       << ",\"schedule_delay_ms\":";
	if (record.scheduleDelayMs) fields << *record.scheduleDelayMs;
	else fields << "null";
	fields << ",\"schedule_late_us\":";
	if (record.scheduleDelayMs) fields << counters.maxScheduleLateUs;
	else fields << "null";
	fields << ",\"setup_us\":" << counters.us(PlayerBotHuntSlicePart::Setup)
	       << ",\"transport_us\":" << counters.us(PlayerBotHuntSlicePart::Transport)
	       << ",\"score_us\":" << counters.us(PlayerBotHuntSlicePart::Score)
	       << ",\"score_completion_us\":" << counters.us(PlayerBotHuntSlicePart::ScoreCompletion)
	       << ",\"evidence_us\":" << counters.us(PlayerBotHuntSlicePart::Evidence)
	       << ",\"route_bookkeeping_us\":" << counters.us(PlayerBotHuntSlicePart::RouteBookkeeping)
	       << ",\"route_checks_us\":" << counters.us(PlayerBotHuntSlicePart::RouteChecks)
	       << ",\"transport_work_count\":" << counters.transportWork
	       << ",\"score_work_count\":" << counters.scoreWork
	       << ",\"candidate_emissions\":" << counters.candidateEmissions
	       << ",\"outbound_checks\":" << counters.outboundChecks << ",\"depot_checks\":" << counters.depotChecks
	       << ",\"depot_unavailable_checks\":" << counters.depotUnavailableChecks
	       << ",\"supplier_checks\":" << counters.supplierChecks << ",\"discovery_checks\":" << counters.discoveryChecks
	       << ",\"depot_discovery_calls\":" << counters.depotDiscoveryCalls
	       << ",\"supplier_discovery_calls\":" << counters.supplierDiscoveryCalls
	       << ",\"route_attempts\":" << counters.routes.attempts << ",\"walking_reached\":" << counters.routes.walkingReached
	       << ",\"npc_plan_returned\":" << counters.routes.npcReturned << ",\"npc_reached\":" << counters.routes.npcReached
	       << ",\"walking_expanded_nodes\":" << counters.routes.walkingExpandedNodes
	       << ",\"npc_returned_expanded_nodes\":" << counters.routes.npcReturnedExpandedNodes
	       << ",\"route_request_sequence\":" << counters.routes.requestSequence
	       << ",\"route_transport_spendable_gold\":" << counters.routes.transportSpendableFunds
	       << ",\"route_local_searches\":" << counters.routes.localSearches
	       << ",\"route_local_expanded_nodes\":" << counters.routes.localExpandedNodes
	       << ",\"route_node_limits\":" << counters.routes.nodeLimits
	       << ",\"route_local_bound_stops\":" << counters.routes.localBoundStops
	       << ",\"route_topology_queries\":" << counters.routes.topologyQueries
	       << ",\"route_topology_expanded_nodes\":" << counters.routes.topologyExpandedNodes
	       << ",\"route_topology_cache_hits\":" << counters.routes.topologyCacheHits
	       << ",\"route_coarse_rejects\":" << counters.routes.coarseRejects
	       << ",\"route_coarse_connections\":" << counters.routes.coarseConnections
	       << ",\"route_local_connections\":" << counters.routes.localConnections
	       << ",\"route_connection_cache_hits\":" << counters.routes.connectionCacheHits
	       << ",\"route_unknown_connections\":" << counters.routes.unknownConnections
	       << ",\"route_bound_rejects\":" << counters.routes.boundRejects
	       << ",\"route_risk_rejects\":" << counters.routes.riskRejects
	       << ",\"route_graph_labels\":" << counters.routes.graphLabels
	       << ",\"route_shared_cache_hits\":" << counters.routes.sharedCacheHits
	       << ",\"route_shared_cache_misses\":" << counters.routes.sharedCacheMisses
	       << ",\"route_incomplete_cache_hits\":" << counters.routes.incompleteCacheHits
	       << ",\"route_source_tree_hits\":" << counters.routes.sourceTreeHits
	       << ",\"route_hierarchy_fallbacks\":" << counters.routes.hierarchyFallbacks
	       << ",\"route_alternate_itineraries\":" << counters.routes.alternateItineraries
	       << ",\"route_corridor_searches\":" << counters.routes.corridorSearches
	       << ",\"route_corridor_widenings\":" << counters.routes.corridorWidenings
	       << ",\"route_guided_searches\":" << counters.routes.guidedSearches
	       << ",\"route_heuristic_builds\":" << counters.routes.heuristicBuilds
	       << ",\"route_heuristic_restarts\":" << counters.routes.heuristicRestarts
	       << ",\"route_ordinary_searches\":" << counters.routes.ordinarySearches
	       << ",\"route_yields\":" << counters.routes.yields
	       << ",\"route_invalidations\":" << counters.routes.invalidations
	       << ",\"route_invalidation_reason\":\"" << counters.routes.invalidationReason << "\""
	       << ",\"route_invalidation_cause\":\"" << counters.routes.invalidationCause << "\""
	       << ",\"route_invalidation_tile_key\":" << counters.routes.invalidationChangedTile
	       << ",\"route_invalidation_journal_delta\":" << counters.routes.invalidationJournalDelta
	       << ",\"route_invalidation_watched_tiles\":" << counters.routes.invalidationWatchedTiles
	       << ",\"route_request_restart_limits\":" << counters.routes.requestRestartLimits
	       << ",\"route_transport_restarts\":" << counters.routes.transportRestarts
	       << ",\"route_transport_restart_limits\":" << counters.routes.transportRestartLimits
	       << ",\"outbound_route_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.outboundRoute).count()
	       << ",\"depot_route_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.depotRoute).count()
	       << ",\"supplier_route_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.supplierRoute).count()
	       << ",\"depot_discovery_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.depotDiscovery).count()
	       << ",\"supplier_discovery_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.supplierDiscovery).count()
	       << ",\"walking_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.routes.walkingTime).count()
	       << ",\"npc_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(counters.routes.npcTime).count();
	emit("hunt_planning_slice", record.position, fields.str());
}

void PlayerBotController::finishHuntRegion(const Player& player, const Position& position, const char* reason)
{
	Player& mutablePlayer = const_cast<Player&>(player);
	auto observation = huntPlayerObservation(mutablePlayer);
	observation.supplyCapability = playerBotSupplyCapability(huntPlanningFacts(mutablePlayer, huntCombatProfile(mutablePlayer)));
	observation.supplyInterrupted = std::strcmp(reason, "hunt_deadline") != 0;
	const auto completion = huntCoordinator.finishHunt(observation, std::chrono::steady_clock::now(),
		static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS))));
	if (!completion) return;
	const auto& combat = completion->combat;
	const int32_t p10Health = player.getMaxHealth() * combat.p10HealthPercent / 100;
	const double activeDamagePerMinute = combat.activeSeconds == 0 ? 0 : combat.damageTaken * 60.0 / combat.activeSeconds;
	emitChallengeFrontier(completion->challenge, position, reason);
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2) << "\"region_id\":" << completion->region.id
	       << ",\"reason\":" << jsonString(reason) << ",\"duration_seconds\":" << completion->durationSeconds
	       << ",\"level_before\":" << completion->levelBefore << ",\"level_after\":" << player.getLevel()
	       << ",\"coin_gold_acquired\":" << completion->coinGoldAcquired
	       << ",\"actual_coin_gold_per_minute\":" << (completion->durationSeconds > 0 ?
	           completion->coinGoldAcquired * 60.0 / completion->durationSeconds : 0)
	       << ",\"expected_coin_gold_per_minute\":" << completion->region.coinGoldPerMinute
	       << ",\"experience_gained\":" << completion->experienceGained
	       << ",\"actual_experience_per_minute\":" << completion->performance.actualExperiencePerMinute
	       << ",\"predicted_experience\":" << completion->region.projectedExperience
	       << ",\"updated_observed_correction\":" << completion->performance.updatedCorrection
	       << ",\"performance_observed\":" << (completion->performance.observed ? "true" : "false")
	       << ",\"performance_evidence_reason\":" << jsonString(completion->performance.evidenceReason)
	       << ",\"supply_estimate_source\":" << jsonString(completion->region.supplyEstimateSource)
	       << ",\"supply_estimate_reason\":" << jsonString(completion->region.supplyEstimateReason)
	       << ",\"supply_local_rejection_reason\":"
	       << (completion->region.supplyLocalRejectionReason ?
	               jsonString(completion->region.supplyLocalRejectionReason) : "null")
	       << ",\"supply_static_potions_per_combat_minute\":"
	       << completion->supplyObservation.staticPotionsPerCombatSecond * 60
	       << ",\"supply_global_multiplier_before\":"
	       << completion->supplyObservation.globalMultiplierBefore
	       << ",\"supply_global_multiplier_after\":"
	       << completion->supplyObservation.globalMultiplierAfter
	       << ",\"supply_global_samples_before\":"
	       << completion->supplyObservation.globalSamplesBefore
	       << ",\"supply_global_samples_after\":"
	       << completion->supplyObservation.globalSamplesAfter
	       << ",\"supply_global_updated\":"
	       << (completion->supplyObservation.globalUpdated ? "true" : "false")
	       << ",\"supply_global_update_direction\":"
	       << jsonString(playerBotSupplyEstimateDirectionName(
	              completion->supplyObservation.globalEstimateDirection))
	       << ",\"supply_global_update_reason\":"
	       << jsonString(completion->supplyObservation.globalReason)
	       << ",\"supply_observation_accepted\":" << (completion->supplyObservation.accepted ? "true" : "false")
	       << ",\"supply_observation_reason\":" << jsonString(completion->supplyObservation.reason)
	       << ",\"supply_capability_changed_fields\":"
	       << supplyCapabilityChangedFields(completion->supplyObservation.changedFields)
	       << ",\"supply_capability_direction\":"
	       << jsonString(playerBotSupplyCapabilityDirectionName(completion->supplyObservation.direction))
	       << ",\"supply_local_updated\":"
	       << (completion->supplyObservation.localUpdated ? "true" : "false")
	       << ",\"supply_local_update_direction\":"
	       << jsonString(playerBotSupplyEstimateDirectionName(
	              completion->supplyObservation.localEstimateDirection))
	       << ",\"supply_observation_samples\":" << completion->supplyObservation.calibration.samples
	       << ",\"supply_calibration_samples\":" << completion->region.supplyCalibration.samples
	       << ",\"supply_potions_per_combat_minute\":"
	       << completion->region.supplyAppliedPotionsPerCombatSecond * 60
	       << ",\"supply_observation_updated_potions_per_combat_minute\":"
	       << completion->supplyObservation.calibration.potionsPerCombatSecond * 60
	       << ",\"supply_arrival_baseline_observed\":"
	       << (completion->supplyObservation.arrivalBaselineObserved ? "true" : "false")
	       << ",\"supply_starting_health\":" << completion->supplyObservation.startingHealth
	       << ",\"supply_starting_maximum_health\":"
	       << completion->supplyObservation.startingMaximumHealth
	       << ",\"supply_starting_mana\":" << completion->supplyObservation.startingMana
	       << ",\"supply_starting_maximum_mana\":"
	       << completion->supplyObservation.startingMaximumMana
	       << ",\"supply_ending_health\":" << completion->supplyObservation.endingHealth
	       << ",\"supply_ending_maximum_health\":"
	       << completion->supplyObservation.endingMaximumHealth
	       << ",\"supply_ending_mana\":" << completion->supplyObservation.endingMana
	       << ",\"supply_ending_maximum_mana\":"
	       << completion->supplyObservation.endingMaximumMana
	       << ",\"supply_guard_duration_seconds\":" << completion->supplyObservation.durationSeconds
	       << ",\"supply_guard_minimum_duration_seconds\":"
	       << completion->supplyObservation.minimumDurationSeconds
	       << ",\"supply_guard_active_combat_seconds\":"
	       << completion->supplyObservation.activeCombatSeconds
	       << ",\"supply_guard_minimum_active_combat_seconds\":"
	       << completion->supplyObservation.minimumActiveCombatSeconds
	       << ",\"supply_guard_kills\":" << completion->supplyObservation.kills
	       << ",\"supply_guard_minimum_kills\":" << completion->supplyObservation.minimumKills
	       << ",\"supply_guard_p10_health_percent\":"
	       << static_cast<uint16_t>(completion->supplyObservation.p10HealthPercent)
	       << ",\"supply_guard_p10_mana_percent\":"
	       << static_cast<uint16_t>(completion->supplyObservation.p10ManaPercent)
	       << ",\"supply_guard_minimum_health_percent\":"
	       << static_cast<uint16_t>(completion->supplyObservation.minimumHealthPercent)
	       << ",\"supply_guard_interrupted\":"
	       << (completion->supplyObservation.interrupted ? "true" : "false")
	       << ",\"supply_guard_potions_depleted\":"
	       << (completion->supplyObservation.potionsDepleted ? "true" : "false")
	       << ",\"supply_guard_danger_observed\":"
	       << (completion->supplyObservation.dangerObserved ? "true" : "false")
	       << ",\"supply_guard_death_observed\":"
	       << (completion->supplyObservation.deathObserved ? "true" : "false")
	       << ",\"supply_food_active_seconds\":" << completion->supplyObservation.foodActiveSeconds
	       << ",\"supply_food_available_seconds\":" << completion->supplyObservation.foodAvailableSeconds
	       << ",\"supply_level_health_restored\":" << completion->supplyObservation.levelHealthRestored
	       << ",\"supply_level_mana_restored\":" << completion->supplyObservation.levelManaRestored
	       << ",\"supply_level_adjusted_health_debt\":"
	       << completion->supplyObservation.levelAdjustedHealthDebt
	       << ",\"supply_level_adjusted_mana_debt\":"
	       << completion->supplyObservation.levelAdjustedManaDebt
	       << ",\"supply_potion_equivalent_demand\":"
	       << completion->supplyObservation.potionEquivalentDemand
	       << ",\"supply_mana_potion_debt\":" << completion->supplyObservation.manaPotionDebt
	       << ",\"supply_demand\":" << supplyDemandJson(completion->supplyDemand)
	       << ",\"kills\":" << combat.kills << ",\"damage_taken\":" << combat.damageTaken
	       << ",\"active_combat_seconds\":" << combat.activeSeconds
	       << ",\"active_combat_uptime\":" << (completion->durationSeconds == 0 ? 0 : combat.activeSeconds / completion->durationSeconds)
	       << ",\"active_combat_damage_per_minute\":" << activeDamagePerMinute
	       << ",\"minimum_health\":" << (combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : combat.minimumHealth)
	       << ",\"p10_health\":" << p10Health << ",\"p10_health_percent\":" << static_cast<uint16_t>(combat.p10HealthPercent)
	       << ",\"verified_potion_recoveries\":" << combat.potionRecoveries << ",\"verified_spell_recoveries\":" << combat.spellRecoveries
	       << ",\"maximum_attacker_overlap\":" << combat.maximumAttackerOverlap
	       << ",\"retreat_observed\":" << (std::strcmp(reason, "hunt_region_observed_danger") == 0 ? "true" : "false")
	       << ",\"danger_observed\":" << (combat.dangerObserved ? "true" : "false") << ",\"death_observed\":" << (combat.deathObserved ? "true" : "false")
	       << ",\"frontier_before\":" << completion->challenge.frontierBefore << ",\"frontier_after\":" << completion->challenge.frontierAfter;
	emit("hunt_region_outcome", position, fields.str());
	if (Player* speakingPlayer = g_game.getPlayerByID(playerId)) say(*speakingPlayer, "Leaving hunt: " + std::string(reason) + ". " + std::to_string(combat.kills) + " kills, " + std::to_string(completion->experienceGained) + " experience.");
}

PlayerBotHuntSupplyFacts PlayerBotController::huntSupplyFacts(Player& player) const
{
	auto dependencies = huntReturnCoverageContext(player, Position{});
	// Shopping changes the return destination/fare, not these dependencies.
	dependencies.depotDestination = Position{};
	dependencies.fare = 0;
	return {playerBotSupplyCapability(huntPlanningFacts(player, huntCombatProfile(player))), dependencies,
	        PlayerBotHuntRegionPlanner::getCacheRevision(), player.getMoney() + player.getBankBalance(),
	        playerBotSupplyStockKey(supplyStocks(player)), player.getHealth()};
}

bool PlayerBotController::selectHuntRegion(Player& player, const Position& position, const char* reason,
                                           std::chrono::steady_clock::duration* retryAfter)
{
	if (!huntCoordinator.planningActive() && !prepareHuntDeparture(&player, position)) {
		if (retryAfter) *retryAfter = std::chrono::milliseconds(1000);
		return false;
	}
	PlayerBotPlanningBudget& budget = playerBotHuntPlanningBudget();
	const uint64_t budgetId = playerId;
	const PlayerBotPlanningBudget::Result admission = budget.request(budgetId, PlayerBotPlanningBudget::Clock::now());
	if (!admission.admitted) {
		// Callers schedule in whole milliseconds; round up rather than retrying
		// before the head has earned enough credit.
		const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
		    admission.wait + std::chrono::microseconds(999));
		if (retryAfter) *retryAfter = delay;
		const auto suppressed = huntPlanningBudgetRecords.admit(PlayerBotPlanningBudget::Clock::now());
		if (suppressed && !telemetry.terminalLogged()) {
			emit("hunt_planning_budget", position,
			     "\"suppressed_denials\":" + std::to_string(*suppressed) +
			     ",\"requested_delay_ms\":" + std::to_string(delay.count()) +
			     ",\"debt_us\":" + std::to_string(admission.debtUs) +
			     ",\"consumed_us\":" + std::to_string(admission.consumedUs) +
			     ",\"credit_us\":" + std::to_string(admission.creditUs) +
			     ",\"queued\":" + (admission.queued ? "true" : "false") +
			     ",\"queue_depth\":" + std::to_string(admission.waiting));
		}
		return false;
	}
	PlayerBotPlanningBudget::Charge budgetCharge(budget, budgetId);
	PlayerBotHuntSliceTiming timing;
	PlayerBotHuntRouteTiming routes;
	PlayerBotHuntSliceAttribution attribution;
	const char* slicePhase = "setup";
	const char* sliceResult = "pending";
	bool stopForScopeExhaustion = false;
	uint32_t transportWorkCount = 0, scoreWorkCount = 0, candidateEmissions = 0;
	uint32_t outboundChecks = 0, depotChecks = 0, supplierChecks = 0, discoveryChecks = 0;
	uint32_t depotUnavailableChecks = 0, depotDiscoveryCalls = 0, supplierDiscoveryCalls = 0;
	std::chrono::steady_clock::duration outboundRouteTime{}, depotRouteTime{}, supplierRouteTime{};
	std::chrono::steady_clock::duration depotDiscoveryTime{}, supplierDiscoveryTime{};
	const auto scheduleDelay = executingTurnDelayMs;
	const int64_t scheduleLateUs = executingTurnLateUs;
	auto run = [&]() -> bool {
	updateSupplyRecovery(player, position);
	const auto now = std::chrono::steady_clock::now();
	const PlayerBotHuntPlanningObservation fixtureObservation = fixtureDriver.huntPlanningObservation();
	const uint32_t configuredDuration = static_cast<uint32_t>(
	    std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	const uint32_t duration = fixtureDriver.huntPlanningDuration(configuredDuration);
	auto planningInput = [&]() {
		PlayerBotHuntRuntimePlanningInput input;
		input.player = huntPlayerObservation(player);
		input.player.excludedVariants = huntCoordinator.activeHuntCooldowns(now);
		input.cacheRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
		input.huntDurationSeconds = duration;
		input.reason = reason;
		if (huntCoordinator.planningStartRequired(now)) {
			PlayerBotHuntRegionPlanner planner;
			const auto topologyStarted = std::chrono::steady_clock::now();
			std::shared_ptr<PlayerBotTopologyDistances> topologyDistances;
			std::shared_ptr<const PlayerBotTopologyReachability> topologyReachability;
			if (PlayerBotTopology::instance().walkComponent(player.getPosition())) {
				topologyDistances = std::make_shared<PlayerBotTopologyDistances>(PlayerBotTopology::instance().distancesFrom(
				    player.getPosition(), input.player.canUseRope, input.player.canUseShovel, player.getLevel()));
				topologyReachability = PlayerBotTopology::instance().reachabilityFrom(
				    player.getPosition(), input.player.canUseRope, input.player.canUseShovel, player.getLevel());
			}
			const uint64_t topologyTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
			    std::chrono::steady_clock::now() - topologyStarted).count();
			PlayerBotHuntRegionScan scan = planner.beginScan(player, topologyDistances.get());
			auto profile = huntPlanningFacts(player, huntCombatProfile(player));
			profile.cashPressure = playerBotHuntCashPressure(input.player.funds);
			const uint32_t healthPrice = cheapestShopPrice(player, recoveryPotionItemId(player.getVocationId()));
			profile.healthPotionUnitPrice = healthPrice == UINT32_MAX ? 0 : healthPrice;
			for (auto& kind : profile.supply.kinds) {
				const uint32_t price = cheapestShopPrice(player, kind.itemId);
				kind.unitPrice = price == UINT32_MAX ? 0 : price;
			}

			input.start = {{std::move(scan), std::move(profile), std::move(topologyDistances),
			                std::move(topologyReachability), huntTransportCatalog(), topologyTimeUs}};
		}
		return input;
	};
	PlayerBotHuntRuntimeOutcome outcome = huntCoordinator.advancePlanning(planningInput(), now, fixtureObservation);
	if (outcome.invalidateCache) {
		const uint64_t invalidatedPlanningPass = outcome.planningPass;
		const uint64_t invalidatedScoringRevision = outcome.scoringRevision;
		const bool planningCancelled = outcome.planningCancelled;
		const char* cancellationReason = outcome.cancellationReason;
		PlayerBotHuntRegionPlanner::invalidateCache();
		PlayerBotHuntPlanningObservation refreshedObservation = fixtureObservation;
		refreshedObservation.invalidateCacheRevision = false;
		outcome = huntCoordinator.advancePlanning(planningInput(), now, refreshedObservation);
		outcome.planningCancelled = planningCancelled;
		outcome.cancellationReason = cancellationReason;
		outcome.staleRevision = true;
		outcome.invalidatedPlanningPass = invalidatedPlanningPass;
		outcome.invalidatedScoringRevision = invalidatedScoringRevision;
	}
	attribution.update(outcome.planningPass, outcome.scoringRevision,
	                   outcome.invalidatedPlanningPass, outcome.invalidatedScoringRevision);
	timing.mark(PlayerBotHuntSlicePart::Transport);
	transportWorkCount = static_cast<uint32_t>(outcome.transportWork.size());
	if (!outcome.transportWork.empty()) {
		std::vector<PlayerBotHuntRuntimeTransportObservation> observations;
		observations.reserve(outcome.transportWork.size());
		for (const PlayerBotHuntRuntimeTransportWork& work : outcome.transportWork) {
			std::optional<PlayerBotHuntTransportArrival> best;
			if (!work.arrivals) {
				observations.push_back({work.offerIndex, std::nullopt});
				continue;
			}
			for (const PlayerBotHuntTransportArrival& source : *work.arrivals) {
				if (!source.topologyReachability || !PlayerBotTopology::instance().reachable(
				        *source.topologyReachability, work.offer.provider) || source.fare > work.funds) continue;
				if (!playerBotNpcTravelOfferEligible(work.playerLevel, work.playerPremium,
				        work.funds - source.fare, work.offer.minimumLevel, work.offer.premiumRequired,
				        work.offer.price, work.offer.opaqueCondition, work.offer.opaqueAction)) continue;
				const uint32_t legSteps = playerBotNavigationDistance(source.position, work.offer.provider) + 1;
				PlayerBotHuntTransportArrival arrival{work.offer.destination,
				    source.fare + work.offer.price,
				    static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
				        static_cast<uint64_t>(source.estimatedSteps) + legSteps)), {}};
				if (!best || std::tie(arrival.fare, arrival.estimatedSteps) <
				             std::tie(best->fare, best->estimatedSteps)) best = std::move(arrival);
			}
			if (best) {
				best->topologyReachability = PlayerBotTopology::instance().reachabilityFrom(
				    best->position, work.canUseRope, work.canUseShovel, work.playerLevel);
			}
			observations.push_back({work.offerIndex, std::move(best)});
		}
		outcome = huntCoordinator.completeTransportWork(observations);
	}
	timing.mark(PlayerBotHuntSlicePart::Score);
	scoreWorkCount = static_cast<uint32_t>(outcome.scoreWork.size());
	if (!outcome.scoreWork.empty()) {
		const auto started = std::chrono::steady_clock::now();
		std::vector<PlayerBotHuntRuntimeScoreObservation> scores;
		scores.reserve(outcome.scoreWork.size());
		PlayerBotHuntRegionPlanner planner;
		for (const PlayerBotHuntRuntimeScoreWork& work : outcome.scoreWork) {
				auto score = planner.score(player, work.profile, work.cacheRevision, work.candidateIndex,
				                          work.excludedVariants, work.performance, work.huntDurationSeconds,
			                          work.topologyDistances.get());
			scores.push_back({work.candidateIndex, score.valid, score.candidateFactsAvailable,
			                  score.withinPlanningScope, std::move(score.region)});
		}
		const auto scoringElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - started).count();
		timing.mark(PlayerBotHuntSlicePart::ScoreCompletion);
		outcome = huntCoordinator.completeScoreWork(scores, scoringElapsedUs);
	}
	attribution.update(outcome.planningPass, outcome.scoringRevision,
	                   outcome.invalidatedPlanningPass, outcome.invalidatedScoringRevision);
	if (outcome.planningCancelled || outcome.staleRevision) {
		huntTravelWork.reset();
		huntRouteRetention.clear();
		selectedHuntRoute.reset();
	}
	if (outcome.planningCancelled) {
		sliceResult = attribution.invalidatedPass && attribution.planningPass != attribution.invalidatedPass ?
		    "restarted" : "cancelled";
	}
	slicePhase = outcome.staleRevision ? "stale_revision" : outcome.planningCancelled ? "cancelled" :
	             outcome.command == PlayerBotHuntRuntimeCommand::ScopeReevaluationPending ? "scope_wait" :
	             outcome.command == PlayerBotHuntRuntimeCommand::PlanningStarted ? "planning_started" :
	             outcome.command == PlayerBotHuntRuntimeCommand::PlanningScored ? "scored" :
	             outcome.command == PlayerBotHuntRuntimeCommand::PlanningYield ?
	                 (transportWorkCount ? "transport_yield" : "scoring_yield") : "route_selection";
	timing.mark(PlayerBotHuntSlicePart::Evidence);
	const auto planningAttribution = [](uint64_t planningPass, uint64_t scoringRevision) {
		return "\"planning_pass\":" + std::to_string(planningPass) + ",\"scoring_revision\":" +
		       std::to_string(scoringRevision);
	};
	fixtureDriver.observeHuntPlanning(outcome);
	if (outcome.planningCancelled && !outcome.staleRevision) {
		const uint64_t planningPass = outcome.invalidatedPlanningPass != 0 ?
		    outcome.invalidatedPlanningPass : outcome.planningPass;
		const uint64_t scoringRevision = outcome.invalidatedPlanningPass != 0 ?
		    outcome.invalidatedScoringRevision : outcome.scoringRevision;
		emit("hunt_region_scan", position, "\"phase\":\"cancelled\",\"reason\":" +
		     jsonString(outcome.cancellationReason) + "," + planningAttribution(planningPass, scoringRevision));
	}
	if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeReevaluationPending) {
		sliceResult = "waiting";
		if (retryAfter) *retryAfter = outcome.retryAfter;
		return false;
	}
	if (outcome.staleRevision) {
		const uint64_t planningPass = outcome.invalidatedPlanningPass != 0 ?
		    outcome.invalidatedPlanningPass : outcome.planningPass;
		const uint64_t scoringRevision = outcome.invalidatedPlanningPass != 0 ?
		    outcome.invalidatedScoringRevision : outcome.scoringRevision;
		emit("hunt_region_scan", position, "\"phase\":\"stale_revision\",\"reason\":" +
		     jsonString(outcome.cancellationReason ? outcome.cancellationReason : "cache_revision_changed") + "," +
		     planningAttribution(planningPass, scoringRevision));
	}
	if (const auto planning = huntCoordinator.planningSession();
	    planning && outcome.command != PlayerBotHuntRuntimeCommand::RegionSelected) {
		const char* phase = outcome.command == PlayerBotHuntRuntimeCommand::PlanningStarted ? "planning_started" :
		                    planning->transportPlanning() ? "transport_yield" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningYield ? "scoring_yield" :
		                    outcome.command == PlayerBotHuntRuntimeCommand::PlanningScored ? "scored" : "planning_yield";
		bool admitted = true;
		if (std::strstr(phase, "_yield")) {
			std::pair<uint64_t, std::string> key{outcome.planningPass, phase};
			if (key != huntScanYieldKey) {
				huntScanYieldKey = std::move(key);
				huntScanYieldRecords = playerbot::PlayerBotRecordThrottle();
			}
			admitted = huntScanYieldRecords.admit(std::chrono::steady_clock::now()).has_value();
		}
		if (admitted) emitHuntRegionPlanning(*planning, position, phase, outcome.planningPass, outcome.scoringRevision);
	}
	if (outcome.candidateSnapshot) {
		// Live-map scans score thousands of candidates, mostly rejected for
		// transport or lethality; full records for all of them filled the log
		// within minutes. Large scans detail only unrejected candidates, except
		// in the hunt-planning fixture, whose assertions inspect rejections.
		const bool detailRejected = fixtureDriver.detailAllHuntCandidates() ||
		                            outcome.candidates.size() <= maximumDetailedHuntCandidates;
		std::map<std::string, uint32_t> rejections;
		for (const PlayerBotHuntRegion& candidate : outcome.candidates) {
			if (!candidate.rejectionReason.empty()) {
				++rejections[candidate.rejectionReason];
				if (!detailRejected) continue;
			}
			++candidateEmissions;
			emitHuntRegionCandidate(candidate, position, outcome.planningPass, outcome.scoringRevision, "scored");
		}
		std::ostringstream summary;
		summary << planningAttribution(outcome.planningPass, outcome.scoringRevision)
		        << ",\"candidate_count\":" << outcome.candidates.size()
		        << ",\"detailed_count\":" << candidateEmissions
		        << ",\"detail\":\"" << (detailRejected ? "all" : "unrejected") << "\",\"rejections\":{";
		for (auto rejection = rejections.begin(); rejection != rejections.end(); ++rejection) {
			summary << (rejection == rejections.begin() ? "" : ",") << jsonString(rejection->first)
			        << ':' << rejection->second;
		}
		summary << '}';
		emit("hunt_region_candidate_summary", position, summary.str());
		std::vector<PlayerBotHuntRegion> fixtureRouteCandidates;
		const std::vector<PlayerBotHuntRegion>* routeCandidates = &outcome.routeCandidates;
		if (fixtureDriver.remoteHuntScenario()) {
			// Fixture-only: force the real Carlin-to-Darashia transport path.
			for (const PlayerBotHuntRegion& candidate : outcome.candidates) {
				const bool darashiaFixtureArea = candidate.center.x >= 33100 && candidate.center.x <= 33350 &&
				                                candidate.center.y >= 32300 && candidate.center.y <= 32650;
				if (!candidate.suitable || !candidate.reachable || candidate.topologyReachable || !darashiaFixtureArea) continue;
				fixtureRouteCandidates.push_back(candidate);
			}
			routeCandidates = &fixtureRouteCandidates;
		}
		timing.mark(PlayerBotHuntSlicePart::RouteBookkeeping);
		if (!huntCoordinator.beginRouteSelection(outcome.planningPass, outcome.scoringRevision,
		                                         *routeCandidates)) { sliceResult = "route_selection_rejected"; return false; }
	}
	timing.mark(PlayerBotHuntSlicePart::RouteBookkeeping);
	if (outcome.command == PlayerBotHuntRuntimeCommand::ScopeExhausted) {
		slicePhase = "scope_exhausted";
		sliceResult = "failed";
		const std::string attribution = planningAttribution(outcome.planningPass, outcome.scoringRevision);
		emit("hunt_region_selection", position, "\"result\":\"failed\",\"reason\":\"no_suitable_reachable_region\"," + attribution);
		emit("hunt_scope_exhausted", position, "\"reason\":\"local_scope_exhausted\"," + attribution +
		     ",\"attempt\":" + std::to_string(outcome.scopeExhaustionAttempt) + ",\"maximum_attempts\":3,\"retry_delay_ms\":" +
		     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(outcome.retryAfter).count()));
		stopForScopeExhaustion = outcome.stopForScopeExhaustion;
		return false;
	}
	if (!outcome.selectedRegion) {
		// Scoring and transport discovery are computation, not game actions.
		// The shared admission budget controls load; a legacy movement-sized
		// sleep after every small batch adds seconds without protecting gameplay.
		if (retryAfter && (outcome.command == PlayerBotHuntRuntimeCommand::PlanningStarted ||
		    outcome.command == PlayerBotHuntRuntimeCommand::PlanningYield ||
		    outcome.command == PlayerBotHuntRuntimeCommand::PlanningScored))
			*retryAfter = std::chrono::milliseconds(1);
		return false;
	}
	const auto routePotionReserve = [&](uint32_t outboundDanger, uint32_t returnDanger) {
		const uint32_t returning = recoveryPotionRouteReserve(player.getVocationId(), player.getMaxHealth(),
		    returnDanger, static_cast<uint32_t>(riskProfile.healthLossCost));
		const uint32_t outbound = outboundDanger == 0 ? 0 :
		    recoveryPotionRouteReserve(player.getVocationId(), player.getMaxHealth(),
		        outboundDanger, static_cast<uint32_t>(riskProfile.healthLossCost));
		return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,
		    static_cast<uint64_t>(returning) + outbound));
	};
	PlayerBotHuntRouteResult routeResult;
	bool routedThisTurn = false;
	// At most depot + discovery + final in one turn; never issue two route plans.
	for (uint32_t step = 0; step < 4; ++step) {
		timing.mark(PlayerBotHuntSlicePart::RouteChecks);
		const auto request = huntCoordinator.nextRouteRequest();
		if (!request) { sliceResult = "route_request_missing"; return false; }
		huntRouteRetention.begin(request->planningPass, request->scoringRevision);
		if (request->stage == PlayerBotHuntRouteStage::Outbound) {
			auto& retained = huntRouteRetention.outbound(request->sequence);
			retained.source = position;
			retained.destination = request->to;
		}
		auto* retained = huntRouteRetention.pending();
		std::shared_ptr<PlayerBotHuntTravelEvidence> completedEvidence;
		PlayerBotHuntRouteObservation observation;
		observation.riskProfile = riskProfile;
		if (request->stage == PlayerBotHuntRouteStage::Outbound ||
		    request->stage == PlayerBotHuntRouteStage::Depot ||
		    request->stage == PlayerBotHuntRouteStage::Supplier) {
			if (routedThisTurn) {
				sliceResult = "yield";
				if (retryAfter) *retryAfter = std::chrono::milliseconds(1);
				return false;
			}
			auto& routeTime = request->stage == PlayerBotHuntRouteStage::Outbound ? outboundRouteTime :
			                  request->stage == PlayerBotHuntRouteStage::Depot ? depotRouteTime : supplierRouteTime;
			switch (request->stage) {
			case PlayerBotHuntRouteStage::Outbound: ++outboundChecks; break;
			case PlayerBotHuntRouteStage::Depot: ++depotChecks; break;
			case PlayerBotHuntRouteStage::Supplier: ++supplierChecks; break;
			default: break;
			}
			// Empty depot lists are rejected without invoking the route planner.
			if (request->routeAvailable) {
				const auto routeStarted = std::chrono::steady_clock::now();
				auto pendingPlan = advanceHuntTravelRoute(player, *request,
				    request->stage == PlayerBotHuntRouteStage::Outbound ? position : request->from, routes, carriedGoldReserve, false,
				    nullptr, retained ? &completedEvidence : nullptr);
				routeTime += std::chrono::steady_clock::now() - routeStarted;
				routedThisTurn = true;
				if (!pendingPlan) {
					sliceResult = "route_pending";
					// This is computation, not a player action cooldown. The search
					// already bounds each turn; resume through the dispatcher promptly.
					if (retryAfter) *retryAfter = std::chrono::milliseconds(1);
					return false;
				}
				const auto& plan = *pendingPlan;
				observation.reached = plan.metrics.result == PlayerBotNavigationResult::Reached;
				observation.searchIncomplete = plan.metrics.result == PlayerBotNavigationResult::NodeLimit;
				observation.endpointRiskRejected = plan.metrics.result == PlayerBotNavigationResult::RiskRejected;
				observation.steps = static_cast<uint32_t>(plan.metrics.steps);
				observation.fare = plan.metrics.fare;
				observation.dangerCost = plan.metrics.dangerCost;
				observation.peakDanger = plan.metrics.maximumHealthLossPerSecond;
				observation.npcTravel = std::any_of(plan.steps.begin(), plan.steps.end(),
				    [](const PlayerBotNavigationStep& step) { return step.action == PlayerBotNavigationAction::NpcTravel; });
				observation.travelSeconds = plan.metrics.estimatedTravelSeconds > 0 ?
				    plan.metrics.estimatedTravelSeconds : playerBotHuntCoarseTravelSeconds(plan.metrics.steps, static_cast<const Creature&>(player).getStepSpeed());
				if (request->stage == PlayerBotHuntRouteStage::Outbound) {
					observation.huntDurationSeconds = duration;
					observation.staminaMultiplier = projectedHuntStaminaMultiplier(player, duration);
					if (observation.reached && playerBotNavigationRiskAccepts(riskProfile, observation.dangerCost,
					    observation.peakDanger)) {
						const auto discoveryStarted = std::chrono::steady_clock::now();
						observation.approaches = huntDepotExitCandidates(player, request->to);
						depotDiscoveryTime += std::chrono::steady_clock::now() - discoveryStarted;
						++depotDiscoveryCalls;
					}
					if (retained) retained->plan = std::move(*pendingPlan);
				}
			} else if (request->stage == PlayerBotHuntRouteStage::Depot) {
				++depotUnavailableChecks;
			}
			if (request->stage == PlayerBotHuntRouteStage::Depot && observation.reached &&
			    playerBotNavigationRiskAccepts(riskProfile, observation.dangerCost, observation.peakDanger)) {
				observation.potionReserve = routePotionReserve(request->outboundDangerCost, observation.dangerCost);
				observation.supplyProfile = huntPlanningFacts(player, huntCombatProfile(player)).supply;
				observation.funds = player.getMoney() + player.getBankBalance();
			}
		} else if (request->stage == PlayerBotHuntRouteStage::DiscoverSupply) {
			++discoveryChecks;
			const auto discoveryStarted = std::chrono::steady_clock::now();
			auto requiredItems = playerBotSupplyExitShopItems(supplyStocks(player),
			    recoveryPotionItemId(player.getVocationId()));
			for (uint16_t itemId : request->requiredSupplyItems) {
				if (std::find(requiredItems.begin(), requiredItems.end(), itemId) == requiredItems.end())
					requiredItems.push_back(itemId);
			}
			for (uint16_t itemId : requiredItems)
				observation.supplyApproachGroups.push_back({itemId, huntSupplyExitCandidates(player, request->from, itemId)});
			supplierDiscoveryTime += std::chrono::steady_clock::now() - discoveryStarted;
			++supplierDiscoveryCalls;
		} else if (request->stage == PlayerBotHuntRouteStage::Final) {
			observation.potionReserve = routePotionReserve(
			    request->outboundDangerCost, request->returnDangerCost);
			observation.supplyProfile = huntPlanningFacts(player, huntCombatProfile(player)).supply;
			observation.funds = player.getMoney() + player.getBankBalance();
			observation.recoverySpendingReserve = recoverySpendingReserve(
			    player, potionStockTarget(player, observation.potionReserve));
			std::vector<PlayerBotSupplyFloorCost> otherFloorCosts;
			for (const auto& stock : PlayerBotInventoryPolicy::additionalSupplyStocks(player)) {
				otherFloorCosts.push_back({stock.count, stock.rule.safetyFloor,
				    stock.count < stock.rule.safetyFloor ? cheapestShopPrice(player, stock.rule.itemId) : 0});
			}
			observation.recoverySpendingReserve = playerBotSupplyFloorSpendingReserve(
			    observation.recoverySpendingReserve, otherFloorCosts);
			observation.recoveryRouteHealthLoss =
			    (static_cast<double>(request->outboundDangerCost) + request->returnDangerCost) *
			    player.getMaxHealth() / riskProfile.healthLossCost;
			const uint32_t price = cheapestShopPrice(player, recoveryPotionItemId(player.getVocationId()));
			observation.healthPotionUnitPrice = price == UINT32_MAX ? 0 : price;
		}
		routeResult = huntCoordinator.observeRoute(*request, observation);
		if (!routeResult.accepted) {
			sliceResult = "route_observation_rejected";
			if (retryAfter) *retryAfter = std::chrono::milliseconds(SCHEDULER_MINTICKS);
			return false;
		}
		if (retained && completedEvidence) {
			if (request->stage == PlayerBotHuntRouteStage::Depot)
				completedEvidence->exit = PlayerBotHuntTravelEvidence::Exit::Depot;
			else if (request->stage == PlayerBotHuntRouteStage::Supplier) {
				completedEvidence->exit = PlayerBotHuntTravelEvidence::Exit::Supplier;
				completedEvidence->supplyItemId = request->supplyItemId;
			}
			// Keep only the selected endpoint's discovery dependencies. Failed
			// alternatives and earlier search attempts must not poison this proof.
			PlayerBotRouteChanges::Scope endpointDependencies(completedEvidence->watch);
			const bool endpointValid = huntTravelExitValid(player, *completedEvidence);
			retained->evidence.accept(request->stage, true, endpointValid ? completedEvidence : nullptr, request->supplyItemId);
		}
		if (routeResult.completedCandidate) {
			huntRouteRetention.complete(*routeResult.completedCandidate);
			timing.mark(PlayerBotHuntSlicePart::Evidence);
			++candidateEmissions;
			emitHuntRegionCandidate(*routeResult.completedCandidate, position,
			    routeResult.planningPass, routeResult.scoringRevision, "route_validation");
			timing.mark(PlayerBotHuntSlicePart::RouteChecks);
		}
		if (routeResult.terminal || routeResult.yield) break;
	}
	timing.mark(PlayerBotHuntSlicePart::RouteBookkeeping);
	if (!routeResult.terminal) {
		sliceResult = "yield";
		if (retryAfter) *retryAfter = std::chrono::milliseconds(SCHEDULER_MINTICKS);
		return false;
	}
	for (uint64_t variantId : routeResult.rejectedVariants) {
		huntCoordinator.rejectHuntVariant(variantId, now, std::chrono::minutes(10));
	}
	auto routeFailureCounts = [&]() {
		std::ostringstream fields;
		fields << '{';
		bool first = true;
		for (const auto& entry : routeResult.failureCounts) {
			if (!first) fields << ',';
			first = false;
			fields << jsonString(entry.first) << ':' << entry.second;
		}
		fields << '}';
		return fields.str();
	};
	std::optional<PlayerBotHuntRegion> safeSelection = std::move(routeResult.selectedRouteRegion);
	if (!safeSelection) {
		huntRouteRetention.clear();
		selectedHuntRoute.reset();
		sliceResult = routeResult.outcome == PlayerBotHuntRouteOutcome::Incomplete ? "incomplete" : "failed";
		if (routeResult.outcome != PlayerBotHuntRouteOutcome::NeedsSupplies &&
		    routeResult.outcome != PlayerBotHuntRouteOutcome::NeedsPreparation)
			emit("hunt_region_selection", position,
			     std::string(routeResult.outcome == PlayerBotHuntRouteOutcome::Incomplete ?
			         "\"result\":\"incomplete\",\"reason\":\"planning_incomplete\"" :
			         "\"result\":\"failed\",\"reason\":\"no_safe_route_candidate\"") + ",\"route_rejection_counts\":" +
			         routeFailureCounts() + "," + planningAttribution(routeResult.planningPass, routeResult.scoringRevision));
		huntCoordinator.completePlanningSelection();
		const auto facts = huntSupplyFacts(player);
		huntSupplyHandoff.blocked(routeResult.readiness, facts);
		bool preparationExhausted = false;
		if (routeResult.outcome != PlayerBotHuntRouteOutcome::Pending &&
		    routeResult.outcome != PlayerBotHuntRouteOutcome::Incomplete &&
		    (!routeResult.readiness.requirement || routeResult.readiness.requirement->kind == PlayerBotRequirementKind::Stock)) {
			uint64_t gap = 0;
			for (const auto& stock : supplyStocks(player)) {
				const uint32_t floor = std::max(stock.rule.safetyFloor, stock.rule.returnThreshold + 1);
				gap += floor - std::min(floor, stock.count);
			}
			preparationExhausted = preparationBudget.completed(gap, false, PlayerBotRequirementKind::Stock);
		}
		if (huntSupplyHandoff.noProgress(routeResult.outcome, facts) || preparationExhausted) {
			stop(routeResult.outcome == PlayerBotHuntRouteOutcome::Incomplete ?
			    "hunt_planning_incomplete" : "hunt_supply_no_progress", position);
			return false;
		}
		if (routeResult.outcome == PlayerBotHuntRouteOutcome::NeedsSupplies && routeResult.supplyShortage) {
			huntSupplyHandoff.require(std::move(routeResult.supplyShortage->requirements), facts);
			std::ostringstream requirements;
			requirements << '[';
			bool first = true;
			for (const auto& requirement : huntSupplyHandoff.requirements()) {
				if (!first) requirements << ',';
				first = false;
				requirements << "{\"kind\":" << jsonString(playerBotSupplyKindName(requirement.kind))
				             << ",\"required_stock\":" << requirement.count << '}';
			}
			requirements << ']';
			emit("hunt_region_selection", position,
			     "\"result\":\"needs_supplies\",\"reason\":\"hunt_supply_shortage\",\"atlas_variant_id\":" +
			         std::to_string(routeResult.supplyShortage->atlasVariantId) + ",\"requirements\":" +
			         requirements.str() + ",\"route_rejection_counts\":" + routeFailureCounts() + "," +
			         planningAttribution(routeResult.planningPass, routeResult.scoringRevision));
		}
		if (routeResult.readiness.requirement) {
			selectTopLevelGoal(player, position, "hunt_blocked_requirement");
			if (retryAfter) *retryAfter = std::chrono::milliseconds(navigationInterval);
			return false;
		}
		// No supported requirement: a completed recovery/selection pass without progress is bounded;
		// unfinished route work above never consumes this retry allowance.
		if (retryAfter) *retryAfter = std::chrono::seconds(30);
		return false;
	}
	huntPatrolValidationDestination.reset();
	huntPatrolValidatedDestination.reset();
	huntPatrolTrip.reset();
	huntTransitProgress.reset();
	PlayerBotHuntRegion selected = std::move(*safeSelection);
	preparationBudget.reset();
	huntSupplyHandoff.selected(selected.supplyRecovery &&
	    playerBotMandatorySupplyDeficit(supplyStocks(player), false).missing != 0);
	selectedHuntRoute = huntRouteRetention.take(selected);
	if (selectedHuntRoute) {
		selectedHuntRoute->revision = selected.atlasRevision;
		selectedHuntRoute->variant = selected.atlasVariantId;
	}
	huntReturnRouteDangerCost = selected.returnRouteDangerCost;
	huntReturnDestination = selected.exitDepotDestination;
	huntTravelBudgetPhase = HuntTravelBudgetPhase::Outbound;
	huntExitFareReserve = selected.exitFare;
	huntRecoveryPotionReserve = selected.recoveryPotionReserve;
	huntReturnCoverageVariantId = selected.atlasVariantId;
	huntReturnCoverage.validate(huntReturnCoverageVariantId, selected.atlasRevision,
	                            huntReturnCoverageContext(player, selected.destination));
	const uint32_t healthLossCost = static_cast<uint32_t>(riskProfile.healthLossCost);
	huntPotionReturnThreshold = recoveryPotionRouteReserve(
		player.getVocationId(), player.getMaxHealth(), huntReturnRouteDangerCost, healthLossCost);
	huntPotionRestockTarget = potionStockTarget(player, huntPotionReturnThreshold);
	emit("hunt_supply_reserve", position,
	     "\"source\":\"selected_return_route\",\"route_danger_cost\":" +
	         std::to_string(huntReturnRouteDangerCost) + ",\"health_loss_cost\":" + std::to_string(healthLossCost) +
	         ",\"maximum_health\":" + std::to_string(player.getMaxHealth()) + ",\"minimum_potion_healing\":" +
	         std::to_string(recoveryPotionMinimumHealing(player.getVocationId())) + ",\"return_threshold\":" +
	         std::to_string(huntPotionReturnThreshold) + ",\"restock_target\":" +
	         std::to_string(huntPotionRestockTarget));
	huntCoordinator.selectPlanningRegion(selected, huntPlayerObservation(player), now);
	emit("hunt_region_selection", position, "\"result\":\"selected\",\"region_id\":" + std::to_string(selected.id) +
		",\"selection_rule\":" + jsonString(playerBotHuntSelectionRule(selected)) +
		",\"cash_pressure\":" + (selected.cashPressure ? "true" : "false") +
		",\"available_gold\":" + std::to_string(player.getMoney() + player.getBankBalance()) +
		",\"cash_buffer\":" + std::to_string(playerBotHuntCashBuffer) +
		",\"coin_income_fallback\":" +
		    (selected.cashPressure && selected.coinGoldPerMinute <= 0 && selected.observedCoinGoldPerMinute <= 0 ?
		        "true" : "false") +
		",\"route_rejection_counts\":" + routeFailureCounts() + "," +
		planningAttribution(routeResult.planningPass, routeResult.scoringRevision) +
		",\"atlas_site_id\":" + std::to_string(selected.atlasSiteId) + ",\"atlas_variant_id\":" +
		std::to_string(selected.atlasVariantId) + ",\"atlas_pockets\":" + std::to_string(selected.atlasPocketCount) +
		",\"atlas_spawns\":" + std::to_string(selected.atlasSpawnCount) + ",\"atlas_floors\":" +
		std::to_string(selected.atlasFloorCount) + ",\"reason\":" + jsonString(reason) + ",\"center\":{\"x\":" +
		std::to_string(selected.center.x) + ",\"y\":" + std::to_string(selected.center.y) + ",\"z\":" +
		std::to_string(selected.center.z) + "}");
	if (const auto planning = huntCoordinator.planningSession()) {
		emitHuntRegionPlanning(*planning, position, "selected", routeResult.planningPass, routeResult.scoringRevision);
	}
	std::ostringstream speech;
	speech << "Going hunting to " << selected.destination.x << ',' << selected.destination.y << ','
	       << static_cast<uint16_t>(selected.destination.z) << ". Expecting: ";
	for (size_t index = 0; index < selected.monsters.size(); ++index) { if (index != 0) speech << ", "; speech << selected.monsters[index].name; }
	speech << ". Projected " << std::fixed << std::setprecision(0) << selected.projectedExperience << " experience after " << selected.estimatedTravelSeconds << " seconds travel.";
	say(player, speech.str());
	huntCoordinator.completePlanningSelection();
	sliceResult = "selected";
	return true;
	};
	const bool selected = run();
	timing.mark(PlayerBotHuntSlicePart::Other);
	if (!telemetry.terminalLogged()) {
		HuntPlanningSliceRecord record{slicePhase, sliceResult, reason, attribution, scheduleDelay, position,
		                               std::chrono::steady_clock::now(), {}};
		PlayerBotHuntSliceCounters& counters = record.counters;
		// Excludes this record's serialization and emission.
		counters.elapsedUs = counters.maxElapsedUs = timing.elapsedUs();
		counters.maxScheduleLateUs = scheduleDelay ? scheduleLateUs : 0;
		for (size_t part = 0; part < counters.partUs.size(); ++part) {
			counters.partUs[part] = timing.us(static_cast<PlayerBotHuntSlicePart>(part));
		}
		counters.transportWork = transportWorkCount;
		counters.scoreWork = scoreWorkCount;
		counters.candidateEmissions = candidateEmissions;
		counters.outboundChecks = outboundChecks;
		counters.depotChecks = depotChecks;
		counters.depotUnavailableChecks = depotUnavailableChecks;
		counters.supplierChecks = supplierChecks;
		counters.discoveryChecks = discoveryChecks;
		counters.depotDiscoveryCalls = depotDiscoveryCalls;
		counters.supplierDiscoveryCalls = supplierDiscoveryCalls;
		counters.outboundRoute = outboundRouteTime;
		counters.depotRoute = depotRouteTime;
		counters.supplierRoute = supplierRouteTime;
		counters.depotDiscovery = depotDiscoveryTime;
		counters.supplierDiscovery = supplierDiscoveryTime;
		counters.routes = routes;
		recordHuntPlanningSlice(std::move(record));
	}
	budgetCharge.finish(); // Include slice telemetry in actual charged service.
	if (stopForScopeExhaustion) stop("hunt_scope_exhausted", position);
	return selected;
}

void PlayerBotController::beginHuntCycle(Player* player, const Position& position, const char* reason)
{
	pendingHuntCompletionReason.clear();
	const uint32_t duration = static_cast<uint32_t>(std::max<int32_t>(1, g_config.getNumber(ConfigManager::PLAYERBOT_HUNT_DURATION_SECONDS)));
	huntCoordinator.beginHuntCycle(std::chrono::steady_clock::now(), duration);
	// A fallback patrol is already a deliberate hunt; selected regions have a
	// separate outbound leg before the bot enters their observed hunt area.
	huntRegionReached = !fixtureDriver.huntObservation().selectRegion;
	resetNavigation();
	huntPatrolTrip.reset();
	huntTransitProgress.reset();
	emit("action_result", position, "\"action\":\"hunt_cycle\",\"result\":\"started\",\"cycle\":" + std::to_string(huntCoordinator.completedHuntCycles()) + ",\"duration_seconds\":" + std::to_string(huntCoordinator.effectiveHuntDurationSeconds()));
	schedule(SCHEDULER_MINTICKS);
}

void PlayerBotController::startHunt(Player* player, const Position& position, const char* reason)
{
	if (!player) {
		stop("controlled_player_not_found", position);
		return;
	}
	if (!ensureCombatReady(player, position, reason)) {
		return;
	}
	progressionRuntime.enterHunt();
	setCyclePhase(CyclePhase::Hunt, position, reason);
	if (fixtureDriver.huntObservation().selectRegion && !huntCoordinator.huntActive()) {
		if (attackDefensiveThreat(player, position)) {
			schedule(navigationInterval);
			return;
		}
		std::chrono::steady_clock::duration retryAfter{};
		if (!selectHuntRegion(*player, position, "hunt_started", &retryAfter)) {
			if (telemetry.terminalLogged()) return;
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(retryAfter).count();
			schedule(static_cast<uint32_t>(delay > 0 ? delay : SCHEDULER_MINTICKS));
			return;
		}
	}
	beginHuntCycle(player, position, reason);
}

const char* PlayerBotController::continueHuntPatrolTrip(Player& player, const Position& currentPosition,
                                                        const Position& destination,
                                                        const std::set<Position>& blockedPositions,
                                                        bool startsNavigation)
{
	if (!huntPatrolTrip) return "no_validated_trip";
	HuntPatrolTrip& trip = *huntPatrolTrip;
	if (trip.destination != destination || trip.returnDestination != huntReturnDestination ||
	    trip.variant != huntReturnCoverageVariantId) return "trip_changed";
	if (trip.continuations >= maximumPatrolTripContinuations) return "continuation_limit";
	// Return coverage is the cached return validation; its context tracks level,
	// tools, topology and NPC generations.
	if (!huntReturnCoverage.covers(huntReturnCoverageVariantId, PlayerBotHuntRegionPlanner::getCacheRevision(),
	                               huntReturnCoverageContext(player, destination))) return "return_unvalidated";
	PlayerBotNavigationRoutePlan leg = planNavigationRoute(player, destination, blockedPositions);
	telemetry.recordPathfinding(leg.metrics.elapsed, leg.metrics.result == PlayerBotNavigationResult::Reached);
	// A leg may only board a boat the validated itinerary boards, within its fare,
	// under the same risk checks as ordinary navigation.
	const auto& offer = leg.metrics.firstNpcTravelOffer;
	const char* rejection =
	    leg.metrics.result != PlayerBotNavigationResult::Reached || leg.steps.empty() ? "leg_unreached" :
	    leg.metrics.fare > trip.fare ? "fare_above_validated" :
	    offer && std::none_of(trip.offers.begin(), trip.offers.end(), [&](const auto& step) {
		    return playerBotNpcTravelOfferMatches(*offer, step);
	    }) ? "unvalidated_travel_offer" :
	    !huntTravelFareAffordable(player, leg.metrics.fare, HuntTravelBudgetPhase::Outbound) ? "fare_unaffordable" :
	    playerBotNavigationRiskVerdict(riskProfile, leg.metrics) == PlayerBotNavigationRiskVerdict::Rejected ||
	        (leg.metrics.dangerEvidence == PlayerBotNavigationDangerEvidence::Coarse &&
	         playerBotNavigationLocalRiskVerdict(riskProfile, leg.metrics) == PlayerBotNavigationRiskVerdict::Rejected) ?
	        "leg_unsafe" : nullptr;
	if (rejection) return rejection;
	++trip.continuations;
	std::ostringstream fields;
	fields << "\"result\":\"patrol_leg_continued\",\"destination\":" << positionJson(destination)
	       << ",\"steps\":" << leg.steps.size() << ",\"fare\":" << leg.metrics.fare
	       << ",\"blocked_positions\":" << positionListJson(blockedPositions)
	       << ",\"continuations\":" << trip.continuations;
	emit("navigation_progress", currentPosition, fields.str());
	navigationRuntime.observePlan({PlayerBotNavigationGoal::exact(destination), std::move(leg), player.canDoAction(),
	                               startsNavigation, std::chrono::steady_clock::now()});
	huntPatrolValidatedDestination = destination;
	return nullptr;
}

void PlayerBotController::processTraversal(Player* player, const Position& currentPosition)
{
	// Only an explicit preflight yield retains work. Combat, service and other
	// interruptions discard both the pending leg and completed outbound evidence.
	bool retainPatrolSearch = false;
	struct PatrolSearchGuard {
		std::function<void()> cleanup;
		~PatrolSearchGuard() { cleanup(); }
	} patrolSearchGuard{[&] {
		if (!huntPatrolRouteSearch) return;
		if (retainPatrolSearch) {
			huntPatrolRouteSearch->scheduledGeneration = scheduledTurnGeneration;
			return;
		}
		huntPatrolRouteSearch.reset();
		huntPatrolValidationDestination.reset();
		huntPatrolValidationOrigin.reset();
		huntPatrolOutboundPlan.reset();
		playerBotHuntPlanningBudget().cancel(playerId, std::chrono::steady_clock::now());
	}};
	if (turnRouter.cyclePhase() == CyclePhase::Hunt && huntCoordinator.huntActive() && !huntRegionReached &&
	    huntCoordinator.insideHuntArea(currentPosition, Map::maxClientViewportX, Map::maxClientViewportX + 1,
	                                    Map::maxClientViewportY, Map::maxClientViewportY + 1)) {
		if (!ensureCombatReady(player, currentPosition, "hunt_arrival")) return;
		if (!prepareHuntDeparture(player, currentPosition)) {
			schedule(1000);
			return;
		}
		PlayerBotHuntPlanningProfile arrivalProfile = huntPlanningFacts(*player, huntCombatProfile(*player));
		PlayerBotHuntRuntimePlayerObservation arrival = huntPlayerObservation(*player);
		arrival.supplyCapability = playerBotSupplyCapability(arrivalProfile);
		const auto arrivalTime = std::chrono::steady_clock::now();
		if (!huntCoordinator.enterHuntArea(arrival, arrivalProfile.supply, arrivalTime)) {
			finishHuntAndReturn(player, currentPosition, huntCoordinator.huntDeadlineReached(arrivalTime) ?
			    "hunt_arrival_timeout" : "hunt_arrival_not_ready");
			return;
		}
		huntRegionReached = true;
		const PlayerBotHuntPatrolOutcome patrol = huntCoordinator.huntPatrolTarget();
		emit("hunt_area_entered", currentPosition,
		     "\"duration_seconds\":" + std::to_string(huntCoordinator.effectiveHuntDurationSeconds()) +
		         ",\"region_id\":" + (patrol.regionId ? std::to_string(*patrol.regionId) : "null") +
		         ",\"waypoint\":" + std::to_string(patrol.waypoint) + ",\"destination\":{\"x\":" +
		         std::to_string(patrol.destination.x) + ",\"y\":" + std::to_string(patrol.destination.y) +
		         ",\"z\":" + std::to_string(patrol.destination.z) + "}");
		if (fixtureDriver.remoteHuntScenario()) {
			finishHuntAndReturn(player, currentPosition, "remote_hunt_fixture_complete");
			return;
		}
	}
	if (progressionRuntime.readinessEquipmentPending()) {
		processReadinessEquipment(player, currentPosition);
		return;
	}
	if (huntCoordinator.hasDefensiveCombat()) {
		if (PlayerBotTransitCombat::lootDeadlineRequiresRelease(
		        turnRouter.scenarioStage() == ScenarioStage::LootCorpse,
		        huntCoordinator.lootTimedOut(std::chrono::steady_clock::now()))) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		interruptTrainerRouteWork();
		processDefensiveCombat(player, currentPosition);
		return;
	}
	if (attackDefensiveThreat(player, currentPosition)) {
		interruptTrainerRouteWork();
		schedule(navigationInterval);
		return;
	}
	PlayerBotTurnObservation turn;
	turn.trainerDiscoveryActive = trainerGoalSelectionPending || trainerDiscovery.has_value();
	turn.progressionActive = progressionRuntime.session().active() != PlayerBotProgressionProcedure::None;
	turn.magicTrainingActive = !preparationEvaluationPending && progressionRuntime.activeGoal() == TopLevelGoal::MagicTraining;
	turn.preparationActive = preparationEvaluationPending || preparationOption != PlayerBotPreparationOption::None;
	if (!turn.trainerDiscoveryActive && !turn.progressionActive && !turn.magicTrainingActive && !turn.preparationActive) {
		const bool inHuntPhase = turnRouter.cyclePhase() == CyclePhase::Hunt;
		const PlayerBotHuntTurnObservation hunt = huntCoordinator.observeTurn(
			inHuntPhase, fixtureDriver.huntObservation().selectRegion, std::chrono::steady_clock::now());
		turn.huntRegionSelectionRequired = hunt.regionSelectionRequired;
		turn.huntPlanningActive = hunt.planningActive;
		turn.lootNavigationSuspended = hunt.lootNavigationSuspended;
		turn.huntCycleFinished = hunt.cycleFinished;
	}
	const PlayerBotTurnCommand turnCommand = turnRouter.route(turn);
	// Release a queued patrol admission before another workflow requests the
	// same controller's budget slot.
	if (turnCommand != PlayerBotTurnCommand::Hunt) patrolSearchGuard.cleanup();

	if (turnCommand == PlayerBotTurnCommand::TrainerDiscovery) {
		if (selectTopLevelGoal(*player, currentPosition, "trainer_scan_continuation") &&
		    !trainerDiscovery && !preparationEvaluationPending) schedule(SCHEDULER_MINTICKS);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::Progression) {
		processProgression(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::Prepare) {
		processPreparation(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::MagicTraining) {
		processMagicTraining(*player, currentPosition);
		return;
	}
	if (turnRouter.cyclePhase() == CyclePhase::Hunt &&
	    !ensureCombatReady(player, currentPosition, "readiness_continuous_check", false)) {
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::StartHunt) {
		startHunt(player, currentPosition, "hunt_region_restart");
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::PlanHunt) {
		std::chrono::steady_clock::duration retryAfter{};
		if (selectHuntRegion(*player, currentPosition, "hunt_planning", &retryAfter)) {
			beginHuntCycle(player, currentPosition, "hunt_region_selected");
		} else {
			if (telemetry.terminalLogged()) return;
			const int64_t delay = std::chrono::duration_cast<std::chrono::milliseconds>(retryAfter).count();
			schedule(static_cast<uint32_t>(delay > 0 ? delay : SCHEDULER_MINTICKS));
		}
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::SuspendedLoot) {
		if (huntCoordinator.lootTimedOut(std::chrono::steady_clock::now())) {
			finishLootFailure(player, currentPosition, "corpse_inaccessible");
			schedule(navigationInterval);
			return;
		}
		if (attackDefensiveThreat(player, currentPosition)) {
			schedule(navigationInterval);
			return;
		}
		lootCorpse(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::Loot) {
		lootCorpse(player, currentPosition);
		return;
	}
	if (turnCommand == PlayerBotTurnCommand::FinishHunt) {
		finishHuntAndReturn(player, currentPosition,
		    huntCoordinator.huntActive() && !huntCoordinator.huntArrived() ? "hunt_arrival_timeout" : "hunt_deadline");
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::Service) {
		processService(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::ReturnToDepot) {
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		const PlayerBotDepotSnapshot depot = depotWorkflow.snapshot();
		if (!depot.hasSelectedDepot) {
			schedule(blockedRouteRetryInterval);
			return;
		}
		if (pauseDepotFixtureForRestart(*player, DepotRestartCheckpoint::Approach, currentPosition)) {
			return;
		}
		if (depotApproachStalled(*player, currentPosition, depot.selected.approachPosition, nullptr)) {
			schedule(blockedRouteRetryInterval);
			return;
		}
		PlayerBotNavigationRuntimeOutcome navigation;
		if (!processNavigation(player, currentPosition, depot.selected.approachPosition, &navigation)) {
			depotApproachStalled(*player, currentPosition, depot.selected.approachPosition, &navigation);
			return;
		}
		setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
		processDeposit(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::DepositLoot) {
		if (!discoverDepot(*player, currentPosition)) {
			return;
		}
		const PlayerBotDepotSnapshot depot = depotWorkflow.snapshot();
		if (!depot.hasSelectedDepot) {
			schedule(blockedRouteRetryInterval);
			return;
		}
		if (!Position::areInRange<1, 1, 0>(currentPosition, depot.selected.approachPosition)) {
			setCyclePhase(CyclePhase::ReturnToDepot, currentPosition, "displaced_during_deposit");
			resetNavigation();
			PlayerBotNavigationRuntimeOutcome navigation;
			if (!processNavigation(player, currentPosition, depot.selected.approachPosition, &navigation)) {
				depotApproachStalled(*player, currentPosition, depot.selected.approachPosition, &navigation);
				return;
			}
			setCyclePhase(CyclePhase::DepositLoot, currentPosition, "depot_reached");
		}
		processDeposit(player, currentPosition);
		return;
	}

	if (turnCommand == PlayerBotTurnCommand::TraversalCombat) {
		processTraversalCombat(player, currentPosition);
		return;
	}
	if (turnCommand != PlayerBotTurnCommand::Hunt) {
		schedule(navigationInterval);
		return;
	}
	if (attackVisibleMonster(player, currentPosition)) {
		schedule(navigationInterval);
		return;
	}
	if (trySupportSpell(player, currentPosition)) {
		schedule(navigationDecisionDelay(*player));
		return;
	}
	const PlayerBotHuntPatrolOutcome patrol = huntCoordinator.huntPatrolTarget();
	const auto now = std::chrono::steady_clock::now();
	PlayerBotNavigationRuntimeOutcome navigation;
	const bool activeRegionPatrol = huntCoordinator.huntActive();
	const bool inTransit = activeRegionPatrol && !huntRegionReached;
	auto observePatrolFailure = [&](const PlayerBotNavigationRuntimeOutcome& failure) {
		const auto stalledFor = huntTransitProgress.stalledFor(now);
		if (inTransit && (failure.oscillation || failure.stepFailureCount >= maximumRepeatedNavigationStepFailures) &&
		    stalledFor < transitStallLimit) {
			// A blocked corridor on the way usually clears. Keep the validated trip,
			// forget the blocked tiles and failure count, and plan the leg again.
			emit("navigation_progress", currentPosition,
			     std::string("\"result\":\"transit_blocked\",\"destination\":") + positionJson(patrol.destination) +
			         ",\"step_failures\":" + std::to_string(failure.stepFailureCount) +
			         ",\"oscillation\":" + (failure.oscillation ? "true" : "false") +
			         ",\"cause\":\"" + lastStepFailure.cause + "\",\"blocker\":" +
			         (lastStepFailure.blocker.empty() ? std::string("null") : jsonString(lastStepFailure.blocker)) +
			         ",\"stalled_ms\":" + std::to_string(stalledFor.count()));
			resetNavigation();
			schedule(transitBlockedRetryInterval);
			return;
		}
		if (failure.routeUnavailable && failure.plan.attempted) huntReturnCoverage.invalidate();
		if (fixtureDriver.navigationRecovery(failure.routeUnavailable).pause) navigationRuntime.clearBlockedPositions();
		const PlayerBotHuntPatrolOutcome recovery = huntCoordinator.observeHuntPatrolNavigation(failure, now,
			maximumRepeatedNavigationStepFailures, failure.routeUnsafe ? 1 : maximumPatrolRouteFailures);
		if (recovery.planningIncompleteAttempts != 0) {
			emit("hunt_region_patrol", currentPosition,
			     std::string("\"result\":\"") +
			         (recovery.command == PlayerBotHuntPatrolCommand::PlanningIncomplete ? "deferred" : "retry") +
			         "\",\"reason\":\"planning_incomplete\",\"attempt\":" +
			         std::to_string(recovery.planningIncompleteAttempts) + ",\"destination\":" + positionJson(patrol.destination));
			if (recovery.command == PlayerBotHuntPatrolCommand::PlanningIncomplete) {
				resetNavigation();
				huntPatrolTrip.reset();
				beginService(player, currentPosition, "hunt_region_planning_incomplete");
			} else {
				schedule(static_cast<uint32_t>(recovery.retryAfter.count()));
			}
			return;
		}
		if (recovery.command != PlayerBotHuntPatrolCommand::SkipWaypoint &&
		    recovery.command != PlayerBotHuntPatrolCommand::RegionExhausted) return;
		huntReturnCoverage.invalidate();
		const char* recoveryReason = failure.routeUnsafe ? "route_danger_above_tolerance" :
		    inTransit && stalledFor >= transitStallLimit ? "transit_stalled" : recovery.reason;
		const bool stepFailure = failure.stepFailureCount != 0 || failure.oscillation;
		emit("hunt_region_patrol", currentPosition, "\"result\":\"skipped\",\"reason\":" + jsonString(recoveryReason) +
			",\"in_transit\":" + (inTransit ? "true" : "false") +
			",\"stalled_ms\":" + std::to_string(inTransit ? stalledFor.count() : 0) +
			",\"step_failure_cause\":" + (stepFailure ? jsonString(lastStepFailure.cause) : std::string("null")) +
			",\"blocker\":" + (stepFailure && !lastStepFailure.blocker.empty() ? jsonString(lastStepFailure.blocker) :
			                   std::string("null")) +
			",\"step_failures\":" + std::to_string(recovery.stepFailures) + ",\"route_failures\":" + std::to_string(recovery.routeFailures) +
			",\"elapsed_ms\":" + std::to_string(recovery.elapsedMs) + ",\"expanded_nodes\":" + std::to_string(recovery.expandedNodes) +
			",\"region_id\":" + (recovery.regionId ? std::to_string(*recovery.regionId) : "null") +
			",\"destination\":{\"x\":" + std::to_string(recovery.destination.x) + ",\"y\":" +
			std::to_string(recovery.destination.y) + ",\"z\":" + std::to_string(recovery.destination.z) + "}");
		if (recovery.stepFailures != 0 || recovery.routeFailures != 0) telemetry.recordStuckEvent();
		navigationRuntime.resetPatrolRecovery();
		resetNavigation();
		huntPatrolTrip.reset();
		const char* stepFailureCause = lastStepFailure.cause;
		lastStepFailure = {"none", {}};
		if (recovery.command == PlayerBotHuntPatrolCommand::RegionExhausted) {
			telemetry.recordHuntAbort(std::string(recoveryReason) +
			                          (stepFailure ? std::string("/") + stepFailureCause : std::string()));
			beginService(player, currentPosition, "hunt_region_patrol_unreachable");
		}
	};
	if (activeRegionPatrol && huntPatrolValidatedDestination != patrol.destination && huntPatrolTrip &&
	    (!huntPatrolRouteSearch || huntPatrolRouteSearch->destination != patrol.destination)) {
		// Interruptions such as traversal combat reset navigation, not the trip.
		if (const char* reason = continueHuntPatrolTrip(*player, currentPosition, patrol.destination,
		                                                huntPatrolPreflightBlockedPositions, true)) {
			// A new waypoint is an expected new trip, not a rejected leg.
			if (std::strcmp(reason, "trip_changed") != 0) {
				emit("navigation_progress", currentPosition,
				     std::string("\"result\":\"patrol_trip_revalidated\",\"reason\":\"") + reason +
				         "\",\"destination\":" + positionJson(patrol.destination));
			}
			huntPatrolTrip.reset();
		} else {
			huntPatrolPreflightBlockedPositions.clear();
			schedule(SCHEDULER_MINTICKS);
			return;
		}
	}
	if (activeRegionPatrol && huntPatrolValidatedDestination != patrol.destination) {
		const uint64_t revision = PlayerBotHuntRegionPlanner::getCacheRevision();
		// resetNavigation clears the origin marker. The scheduled generation also
		// detects turns consumed by healing/food before processTraversal runs.
		if (!huntPatrolRouteSearch || huntPatrolValidationOrigin != currentPosition ||
		    huntPatrolRouteSearch->scheduledGeneration != scheduledTurnGeneration ||
		    huntPatrolRouteSearch->destination != patrol.destination ||
		    huntPatrolRouteSearch->returnDestination != huntReturnDestination ||
		    huntPatrolRouteSearch->revision != revision ||
		    huntPatrolRouteSearch->variant != huntReturnCoverageVariantId) {
			huntPatrolValidationDestination.reset();
			huntPatrolOutboundPlan.reset();
			huntPatrolValidationOrigin = currentPosition;
			huntPatrolRouteSearch.emplace();
			auto& search = *huntPatrolRouteSearch;
			search.destination = patrol.destination;
			search.returnDestination = huntReturnDestination;
			search.revision = revision;
			search.variant = huntReturnCoverageVariantId;
			search.pass = (uint64_t(1) << 60) | (uint64_t(playerId) << 24) | ++huntPatrolRouteSerial;
		}
		auto& search = *huntPatrolRouteSearch;
		const bool validateReturn = huntPatrolValidationDestination == patrol.destination;
		auto& budget = playerBotHuntPlanningBudget();
		const auto admission = budget.request(playerId, std::chrono::steady_clock::now());
		if (!admission.admitted) {
			retainPatrolSearch = true;
			schedule(static_cast<uint32_t>(std::clamp<int64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(admission.wait).count(), SCHEDULER_MINTICKS, 5000)));
			return;
		}
		PlayerBotPlanningBudget::Charge charge(budget, playerId);
		// The sliced engine watches its pending leg. Also protect the completed
		// outbound leg while return validation yields, including paid NPC anchors.
		auto routeFacts = [&] {
			const auto combat = huntCombatProfile(*player);
			std::ostringstream facts;
			facts.precision(17);
			facts << combat.level << ':' << combat.maximumHealth << ':' << combat.armor << ':' << combat.defense
			      << ':' << combat.attack << ':' << combat.attackSkill << ':' << combat.attackFactor
			      << ':' << combat.hitChance << ':' << combat.blockedByShield
			      << ':' << player->getMoney() << ':' << player->getBankBalance() << ':' << player->isPremium()
			      << ':' << player->isPzLocked() << ':' << player->getStepDuration()
			      << ':' << player->getStepDuration(DIRECTION_NORTHEAST)
			      << ':' << (g_game.findItemOfType(player, ropeItemId, true) != nullptr)
			      << ':' << (g_game.findItemOfType(player, 2554, true) != nullptr)
			      << ':' << (player->getGroup() ? player->getGroup()->flags : 0)
			      << ':' << (player->getGroup() && player->getGroup()->access)
			      << ':' << (player->getGuild() ? player->getGuild()->getId() : 0)
			      << ':' << (player->getGuildRank() ? player->getGuildRank()->id : 0)
			      << ':' << PlayerBotTopology::instance().generation() << ':' << g_game.getNpcGeneration()
			      << ':' << riskProfile.healthLossCost << ':' << riskProfile.maximumRouteHealthLoss
			      << ':' << riskProfile.maximumHealthLossPerSecond << ':' << huntExitFareReserve;
			if (search.outboundNpcId != 0) {
				const Npc* npc = g_game.getNpcByID(search.outboundNpcId);
				if (!npc || npc->isRemoved()) {
					facts << ":provider_unavailable";
				} else {
					// The spawn anchor, not the live position: captains wander, and the
					// executable approach already follows them.
					facts << ':' << npc->getID() << ':' << npc->getMasterPos();
					for (const auto& offer : npc->getTravelOffers()) {
						facts << ':' << offer.destination << ':' << offer.price << ':' << offer.level << ':' << offer.premium
						      << ':' << offer.hasOpaqueCondition << ':' << offer.hasOpaqueAction;
						for (const auto& word : offer.dialogue) facts << ':' << word.size() << ':' << word;
						const auto unavailable = unavailableTravelOffers.find({npc->getID(), offer.destination});
						facts << ':' << (unavailable != unavailableTravelOffers.end() && unavailable->second > now);
					}
				}
			}
			return facts.str();
		};
		if (validateReturn) {
			const char* restart = !huntPatrolOutboundPlan ? "outbound_plan_missing" :
			                      !search.watch.valid() ? "outbound_route_changed" :
			                      search.outboundFacts != routeFacts() ? "outbound_facts_changed" :
			                      search.outboundEvidence && !huntTravelEvidenceValid(*player,
			                          *huntPatrolValidationOrigin, patrol.destination, *search.outboundEvidence) ?
			                          "outbound_evidence_changed" : nullptr;
			if (restart) {
				// The guard below discards the search; the next turn starts over.
				std::ostringstream fields;
				fields << "\"result\":\"restarted\",\"reason\":\"" << restart << "\",\"phase\":\"patrol_preflight\""
				       << ",\"destination\":" << positionJson(patrol.destination);
				if (!search.watch.valid()) {
					fields << ",\"invalidation_reason\":\"" << PlayerBotRouteChanges::reasonName(search.watch.check())
					       << "\",\"invalidation_cause\":\"" << PlayerBotRouteChanges::causeName(search.watch.changedCause) << '"';
				}
				emit("navigation_progress", currentPosition, fields.str());
				PlayerBotNavigationPlanMetrics incomplete;
				incomplete.result = PlayerBotNavigationResult::NodeLimit;
				observePatrolFailure(playerBotHuntRejectedPatrolPreflight(incomplete, false));
				return;
			}
		}
		PlayerBotRouteChanges::Scope watch(search.watch);
		const Position routeStart = validateReturn ? patrol.destination : currentPosition;
		const Position routeDestination = validateReturn ? huntReturnDestination : patrol.destination;
		const HuntTravelBudgetPhase budgetPhase = validateReturn ? HuntTravelBudgetPhase::ReturnToDepot :
		                                                        HuntTravelBudgetPhase::Outbound;
		const uint64_t reserve = std::max(huntTravelReturnFareReserve(budgetPhase),
		                                  huntTravelRecoveryFundsReserve(*player, budgetPhase));
		PlayerBotNavigationRoutePlan preflight;
		// Evidence for a rejected preflight: which search ran, and its walking alternative.
		const char* preflightSource = "route_engine";
		std::optional<PlayerBotNavigationPlanMetrics> walkingMetrics;
		// The sliced engine proves the itinerary but has no blocked-position input.
		// Known blockers apply to the executable walk instead, so a blocked tile
		// near a boat never turns a paid route into a walking-only one.
		const std::set<Position>& blockedPositions = huntPatrolPreflightBlockedPositions;
		const bool avoidBlockers = !validateReturn && !blockedPositions.empty();
		{
			struct WorkSlot {
				std::shared_ptr<PlayerBotHuntTravelWork>& hunt;
				std::shared_ptr<PlayerBotHuntTravelWork>& patrol;
				WorkSlot(decltype(hunt) hunt, decltype(patrol) patrol) : hunt(hunt), patrol(patrol) { hunt.swap(patrol); }
				~WorkSlot() { hunt.swap(patrol); }
			} slot(huntTravelWork, search.work);
			PlayerBotHuntRouteRequest request;
			request.planningPass = search.pass;
			request.scoringRevision = revision;
			request.sequence = validateReturn ? 1 : 0;
			request.from = routeStart;
			request.to = routeDestination;
			request.preferSafeWalking = playerBotNavigationDistance(routeStart, routeDestination) <= 128;
			PlayerBotHuntRouteTiming timing;
			PlayerBotNavigationRoutePlan walking;
			std::optional<PlayerBotNavigationRoutePlan> planned;
			if (!validateReturn && selectedHuntRoute) {
				auto& retained = *selectedHuntRoute;
				const bool valid = retained.source == routeStart && retained.destination == routeDestination &&
				    retained.variant == huntReturnCoverageVariantId && retained.revision == revision &&
				    retained.evidence.valid([&](PlayerBotHuntTravelEvidence& leg) {
					    // Return/supply sources are synthetic; the actor snapshot still
					    // records the player's real position throughout selection.
					    return huntTravelEvidenceValid(*player, leg.source, leg.destination, leg);
				    });
				if (valid) {
					planned = std::move(retained.plan);
					search.outboundEvidence = std::move(retained.evidence.outbound);
					preflightSource = "selected_route";
					emit("navigation_progress", currentPosition,
					     "\"result\":\"selected_route_reused\",\"destination\":" + positionJson(routeDestination) +
					         ",\"steps\":" + std::to_string(planned->steps.size()) +
					         ",\"fare\":" + std::to_string(planned->metrics.fare));
				} else {
					// Selection's return evidence may have changed too. Do not reuse
					// its coverage when the fallback produces a new outbound route.
					huntReturnCoverage.invalidate();
				}
				selectedHuntRoute.reset(); // One departure attempt, never a general route cache.
			}
			if (!planned) planned = advanceHuntTravelRoute(*player, request, routeStart, timing, reserve, false, &walking,
			    validateReturn ? nullptr : &search.outboundEvidence);
			search.timing.add(timing);
			if (!planned && ++search.turns < maximumRouteSearchTurns) {
				retainPatrolSearch = true;
				schedule(routeSearchContinuationInterval);
				return;
			}
			if (!planned) {
				preflightSource = "search_turn_limit";
				preflight.metrics.attempted = true;
				preflight.metrics.result = PlayerBotNavigationResult::NodeLimit;
			} else {
				if (std::strcmp(preflightSource, "selected_route") != 0) walkingMetrics = walking.metrics;
				preflight = std::move(*planned);
				if (!validateReturn) search.outboundOffers = npcTravelSteps(preflight.steps);
				const auto paid = std::find_if(preflight.steps.begin(), preflight.steps.end(), [](const auto& step) {
					return step.action == PlayerBotNavigationAction::NpcTravel;
				});
				const auto crossesBlocker = [&blockedPositions](const auto& steps) {
					return std::any_of(steps.begin(), steps.end(), [&blockedPositions](const auto& step) {
						return blockedPositions.count(step.target) || blockedPositions.count(step.expectedPosition);
					});
				};
				if (!validateReturn && paid != preflight.steps.end() &&
				    preflight.metrics.result == PlayerBotNavigationResult::Reached &&
				    playerBotNavigationRiskVerdict(riskProfile, preflight.metrics) == PlayerBotNavigationRiskVerdict::Accepted) {
					// The engine returns its validated walk to the boarding NPC. Known
					// blockers reroute only that walk; the paid itinerary stays.
					const uint32_t npcId = paid->npcId;
					if (!avoidBlockers || !crossesBlocker(preflight.steps) ||
					    rerouteValidatedPaidApproach(*player, preflight, blockedPositions)) {
						search.outboundNpcId = npcId;
					} else {
						preflightSource = "blocked_position_walk";
						preflight = planCompleteNavigationRoute(*player, routeStart, routeDestination, blockedPositions);
						search.outboundOffers.clear();
					}
				} else if (avoidBlockers && paid == preflight.steps.end()) {
					// The engine's walk may cross a known blocker; replan it around them.
					preflightSource = "blocked_position_walk";
					preflight = planCompleteNavigationRoute(*player, routeStart, routeDestination, blockedPositions);
				}
			}
		}
		if (preflight.metrics.attempted) {
			telemetry.recordPathfinding(preflight.metrics.elapsed,
			                            preflight.metrics.result == PlayerBotNavigationResult::Reached);
		}
		const bool routeReached = preflight.metrics.result == PlayerBotNavigationResult::Reached &&
		    (routeStart == routeDestination || !preflight.steps.empty());
		const bool routeAffordable = routeReached && huntTravelFareAffordable(
		    *player, preflight.metrics.fare, budgetPhase);
		// This is an optional move to a proposed patrol goal. Its return route
		// must satisfy the same detailed safety limit before we move there.
		const bool routeSafe = routeAffordable &&
			playerBotNavigationRiskVerdict(riskProfile, preflight.metrics) == PlayerBotNavigationRiskVerdict::Accepted;
		if (!routeSafe) {
			navigation = playerBotHuntRejectedPatrolPreflight(preflight.metrics, routeAffordable);
			if (!routeReached && preflight.metrics.result != PlayerBotNavigationResult::NodeLimit)
				huntCoordinator.observeTransitMovementFailure(currentPosition);
			if (routeReached && !routeAffordable) {
				emit("navigation_progress", currentPosition,
				     "\"result\":\"skipped\",\"reason\":" +
				         jsonString(playerBotHuntTravelFareRejectionReason(budgetPhase)) +
				         ",\"phase\":\"patrol_preflight\",\"direction\":" +
				         jsonString(validateReturn ? "return" : "outbound") + ",\"budget_phase\":" +
				         jsonString(playerBotHuntTravelBudgetPhaseName(budgetPhase)) + ",\"fare\":" +
				         std::to_string(preflight.metrics.fare) + ",\"return_fare_reserve\":" +
				         std::to_string(huntTravelReturnFareReserve(budgetPhase)) +
				         ",\"recovery_funds_reserve\":" +
				         std::to_string(huntTravelRecoveryFundsReserve(*player, budgetPhase)));
			} else if (!routeReached) {
				// An unsafe walk is only "unknown" while paid alternatives remain
				// unexplored, so report the walking evidence alongside the result.
				std::ostringstream fields;
				fields << "\"result\":\"skipped\",\"reason\":\"route_not_reached\",\"phase\":\"patrol_preflight\""
				       << ",\"direction\":" << jsonString(validateReturn ? "return" : "outbound")
				       << ",\"source\":\"" << preflightSource << '"'
				       << ",\"route_result\":\"" << playerBotNavigationResultName(preflight.metrics.result) << '"'
				       << ",\"destination\":{\"x\":" << routeDestination.x << ",\"y\":" << routeDestination.y
				       << ",\"z\":" << unsigned(routeDestination.z) << "},\"search_turns\":" << search.turns
				       << ",\"route_expanded_nodes\":" << search.timing.localExpandedNodes
				       << ",\"route_unknown_connections\":" << search.timing.unknownConnections
				       << ",\"route_invalidations\":" << search.timing.invalidations
				       << ",\"route_invalidation_reason\":\"" << search.timing.invalidationReason << '"'
				       << ",\"route_invalidation_cause\":\"" << search.timing.invalidationCause << '"'
				       << ",\"route_transport_restarts\":" << search.timing.transportRestarts
				       << ",\"route_request_restart_limits\":" << search.timing.requestRestartLimits
				       << ",\"route_graph_labels\":" << search.timing.graphLabels
				       << ",\"blocked_positions\":" << positionListJson(blockedPositions);
				if (walkingMetrics) {
					fields << ",\"walking_result\":\"" << playerBotNavigationResultName(walkingMetrics->result) << '"'
					       << ",\"walking_steps\":" << walkingMetrics->steps
					       << ",\"walking_danger_cost\":" << walkingMetrics->dangerCost
					       << ",\"walking_maximum_health_loss_per_second\":" << walkingMetrics->maximumHealthLossPerSecond
					       << ",\"walking_risk_accepted\":" << (walkingMetrics->result == PlayerBotNavigationResult::Reached &&
					              playerBotNavigationRiskVerdict(riskProfile, *walkingMetrics) ==
					              PlayerBotNavigationRiskVerdict::Accepted ? "true" : "false");
				}
				emit("navigation_progress", currentPosition, fields.str());
			} else if (navigation.routeUnsafe) {
				emit("navigation_progress", currentPosition,
				     "\"result\":\"skipped\",\"reason\":\"route_danger_above_tolerance\",\"phase\":\"patrol_preflight\",\"direction\":" +
				         jsonString(validateReturn ? "return" : "outbound") + ",\"destination\":{\"x\":" +
				         std::to_string(routeDestination.x) + ",\"y\":" + std::to_string(routeDestination.y) +
				         ",\"z\":" + std::to_string(routeDestination.z) + "},\"danger_cost\":" +
				         std::to_string(preflight.metrics.dangerCost) + ",\"maximum_health_loss_per_second\":" +
				         std::to_string(preflight.metrics.maximumHealthLossPerSecond));
			}
			// schedule() keeps the earliest due turn, so do not install the
			// ordinary short retry before the incomplete-evidence backoff.
			if (preflight.metrics.result != PlayerBotNavigationResult::NodeLimit || navigation.routeUnsafe)
				schedule(navigation.routeUnsafe ? navigationDecisionDelay(*player) : blockedRouteRetryInterval);
			observePatrolFailure(navigation);
			return;
		}
		const uint64_t coverageRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
		bool destinationCovered = false;
		bool extendsCoverage = false;
		if (!validateReturn) {
			destinationCovered = huntReturnCoverage.covers(
			    huntReturnCoverageVariantId, coverageRevision,
			    huntReturnCoverageContext(*player, patrol.destination));
			extendsCoverage = huntReturnCoverage.covers(
			    huntReturnCoverageVariantId, coverageRevision,
			    huntReturnCoverageContext(*player, currentPosition)) &&
			    playerBotNavigationIsReversibleLocalWalk(currentPosition, patrol.destination, preflight);
			if (!destinationCovered && !extendsCoverage) {
				huntPatrolValidationDestination = patrol.destination;
				huntPatrolValidationOrigin = currentPosition;
				huntPatrolOutboundPlan = std::move(preflight);
				search.outboundFacts = routeFacts();
				search.turns = 0;
				search.timing = {};
				retainPatrolSearch = true;
				schedule(SCHEDULER_MINTICKS);
				return;
			}
		}
		if (validateReturn) {
			huntExitFareReserve = preflight.metrics.fare;
			huntReturnRouteDangerCost = preflight.metrics.dangerCost;
			const uint32_t requiredReserve = recoveryPotionRouteReserve(
				player->getVocationId(), player->getMaxHealth(), preflight.metrics.dangerCost,
				static_cast<uint32_t>(riskProfile.healthLossCost));
			if (requiredReserve > huntPotionReturnThreshold) {
				huntPotionReturnThreshold = requiredReserve;
				huntPotionRestockTarget = potionStockTarget(*player, requiredReserve);
				emit("hunt_supply_reserve", currentPosition,
				     "\"source\":\"patrol_return_route\",\"route_danger_cost\":" +
				         std::to_string(preflight.metrics.dangerCost) + ",\"return_threshold\":" +
				         std::to_string(huntPotionReturnThreshold) + ",\"restock_target\":" +
				         std::to_string(huntPotionRestockTarget));
			}
			if (!huntTravelFareAffordable(*player, huntPatrolOutboundPlan->metrics.fare,
			                              HuntTravelBudgetPhase::Outbound)) {
				navigation = playerBotHuntRejectedPatrolPreflight(huntPatrolOutboundPlan->metrics, false);
				emit("navigation_progress", currentPosition,
				     "\"result\":\"skipped\",\"reason\":" +
				         jsonString(playerBotHuntTravelFareRejectionReason(HuntTravelBudgetPhase::Outbound)) +
				         ",\"phase\":\"patrol_preflight\",\"direction\":\"outbound\",\"budget_phase\":" +
				         jsonString(playerBotHuntTravelBudgetPhaseName(HuntTravelBudgetPhase::Outbound)) +
				         ",\"fare\":" + std::to_string(huntPatrolOutboundPlan->metrics.fare) +
				         ",\"return_fare_reserve\":" + std::to_string(huntExitFareReserve) +
				         ",\"recovery_funds_reserve\":0");
				schedule(blockedRouteRetryInterval);
				observePatrolFailure(navigation);
				return;
			}
			huntReturnCoverage.validate(huntReturnCoverageVariantId, coverageRevision,
			                            huntReturnCoverageContext(*player, patrol.destination));
		} else if (extendsCoverage && !destinationCovered) {
			huntReturnCoverage.validate(huntReturnCoverageVariantId, coverageRevision,
			                            huntReturnCoverageContext(*player, patrol.destination));
		}
		PlayerBotNavigationRoutePlan outboundPlan = validateReturn ?
		    std::move(*huntPatrolOutboundPlan) : std::move(preflight);
		resetNavigation();
		huntPatrolTrip = HuntPatrolTrip{patrol.destination, huntReturnDestination, huntReturnCoverageVariantId,
		                                outboundPlan.metrics.fare, std::move(search.outboundOffers)};
		observeNavigationPlan(patrol.destination, std::move(outboundPlan.steps));
		huntPatrolValidatedDestination = patrol.destination;
		schedule(SCHEDULER_MINTICKS);
		return;
	}
	// Only navigation turns count; route searches and fights are idle gaps.
	if (inTransit) huntTransitProgress.observe(currentPosition, now);
	if (!processNavigation(player, currentPosition, patrol.destination, &navigation,
	                       playerBotNavigationMaximumExpandedNodes, false,
	                       activeRegionPatrol ? &riskProfile : nullptr, !activeRegionPatrol)) {
		if (activeRegionPatrol && navigation.routeRequest) {
			std::set<Position> blockedPositions = std::move(navigation.routeRequest->blockedPositions);
			// A failed final step suppresses the waypoint itself. Avoiding the goal
			// would make it unreachable; repeated step failures skip it instead.
			blockedPositions.erase(patrol.destination);
			// A finished leg or a local obstacle keeps the validated trip and its
			// navigation state; only a rejected leg revalidates the whole trip.
			const char* continuation = continueHuntPatrolTrip(*player, currentPosition, patrol.destination,
			                                                  blockedPositions, false);
			if (!continuation) {
				schedule(SCHEDULER_MINTICKS);
				return;
			}
			huntPatrolTrip.reset();
			std::ostringstream fields;
			fields << "\"result\":\"replan_requested\",\"phase\":\"patrol\""
			       << ",\"destination\":" << positionJson(patrol.destination)
			       << ",\"blocked_positions\":" << positionListJson(blockedPositions)
			       << ",\"step_failures\":" << navigation.stepFailureCount
			       << ",\"movement_result\":\"" << playerBotPendingMovementResultName(navigation.movementResult) << '"'
			       << ",\"failed_target\":" << (navigation.failedMovementTarget ?
			              positionJson(*navigation.failedMovementTarget) : std::string("null"))
			       << ",\"oscillation\":" << (navigation.oscillation ? "true" : "false")
			       << ",\"route_unavailable\":" << (navigation.routeUnavailable ? "true" : "false")
			       << ",\"continuation\":\"" << continuation << '"';
			emit("navigation_progress", currentPosition, fields.str());
			resetNavigation();
			huntPatrolPreflightBlockedPositions = std::move(blockedPositions);
			schedule(SCHEDULER_MINTICKS);
			return;
		}
		observePatrolFailure(navigation);
		return;
	}
	const PlayerBotHuntPatrolOutcome reached = huntCoordinator.observeHuntPatrolNavigation(navigation, now,
		maximumRepeatedNavigationStepFailures, maximumPatrolRouteFailures);
	if (reached.command == PlayerBotHuntPatrolCommand::WaypointReached) {
		if (!huntRegionReached) {
			if (!ensureCombatReady(player, currentPosition, "hunt_arrival")) return;
			PlayerBotHuntPlanningProfile arrivalProfile = huntPlanningFacts(*player, huntCombatProfile(*player));
			PlayerBotHuntRuntimePlayerObservation arrival = huntPlayerObservation(*player);
			arrival.supplyCapability = playerBotSupplyCapability(arrivalProfile);
			if (!huntCoordinator.enterHuntArea(arrival, arrivalProfile.supply, now)) {
				finishHuntAndReturn(player, currentPosition, huntCoordinator.huntDeadlineReached(now) ?
				    "hunt_arrival_timeout" : "hunt_arrival_not_ready");
				return;
			}
		}
		huntRegionReached = true;
		emit("action_result", currentPosition, "\"action\":\"hunt_waypoint\",\"result\":\"reached\",\"waypoint\":" +
			std::to_string(reached.waypoint) + ",\"region_id\":" + (reached.regionId ? std::to_string(*reached.regionId) : "null"));
	}
	schedule(reached.command == PlayerBotHuntPatrolCommand::WaitAtWaypoint ? 1000 : SCHEDULER_MINTICKS);
}
