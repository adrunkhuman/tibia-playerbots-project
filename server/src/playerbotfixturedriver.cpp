#include "otpch.h"

#include "playerbotfixturedriver.h"

#include "condition.h"
#include "player.h"
#include "playerbot.h"
#include "playerbothuntruntime.h"
#include "playerbotsurvivalruntime.h"
#include "playerbotspellcalibration.h"

namespace {
	constexpr uint32_t maximumRepeatedNavigationStepFailures = 3;
	constexpr uint32_t maximumCorpseNavigationFailures = 6;
	constexpr uint32_t maximumPatrolRouteFailures = 3;
	constexpr uint32_t depotRestartCheckpointStorage = 50096;
	constexpr uint32_t gameplayFixtureReadyStorage = 50099;
}

playerbot::PlayerBotFixtureDriver::PlayerBotFixtureDriver(const PlayerBotTestPolicy& policy) : policy(policy)
{
	if (policy.forceRepeatedNavigationStepFailures) forcedNavigationStepFailuresRemaining = maximumRepeatedNavigationStepFailures;
	if (policy.forceCorpseNavigationFailures) forcedNavigationStepFailuresRemaining = maximumCorpseNavigationFailures;
	if (policy.forcePatrolRouteFailures) forcedNavigationPlanFailuresRemaining = maximumPatrolRouteFailures;
}

bool playerbot::PlayerBotFixtureDriver::consumeNavigationStepFailure()
{
	if (forcedNavigationStepFailuresRemaining == 0) return false;
	--forcedNavigationStepFailuresRemaining;
	return true;
}

bool playerbot::PlayerBotFixtureDriver::forceNavigationPlanFailure() const { return forcedNavigationPlanFailuresRemaining != 0; }
void playerbot::PlayerBotFixtureDriver::observeNavigationPlan(bool attempted)
{
	if (attempted && forcedNavigationPlanFailuresRemaining != 0) --forcedNavigationPlanFailuresRemaining;
}
bool playerbot::PlayerBotFixtureDriver::forcedStepRecoveryPending() const
{
	return policy.forceRepeatedNavigationStepFailures && forcedNavigationStepFailuresRemaining != 0;
}
void playerbot::PlayerBotFixtureDriver::resetHuntPlanningRouteFailures() { forcedUnreachable = false; forcedNodeLimit = false; }

playerbot::PlayerBotFixtureRouteFailure playerbot::PlayerBotFixtureDriver::nextHuntPlanningRouteFailure()
{
	if (policy.forceFirstHuntCandidateUnreachable && !forcedUnreachable) { forcedUnreachable = true; return PlayerBotFixtureRouteFailure::Unreachable; }
	if (policy.forceSecondHuntCandidateNodeLimit && !forcedNodeLimit && forcedUnreachable) { forcedNodeLimit = true; return PlayerBotFixtureRouteFailure::NodeLimit; }
	return PlayerBotFixtureRouteFailure::None;
}

std::vector<playerbot::PlayerBotFixtureEvent> playerbot::PlayerBotFixtureDriver::applyHuntPlanningHooks(PlayerBotHuntRuntime& runtime)
{
	std::vector<PlayerBotFixtureEvent> events;
	if (policy.forceHuntScopeExhaustion) runtime.forceScopeExhaustionForTest();
	if (!policy.cancelHuntPlanningAtScoreBarrier || !runtime.planningActive()) return events;
	if (!planningCancelled) {
		planningCancelled = true;
		runtime.fixtureCancelPlanning();
		events.push_back({"hunt_region_scan", "\"phase\":\"cancelled\""});
		return events;
	}
	if (!planningRevisionInvalidated) {
		planningRevisionInvalidated = true;
		runtime.fixtureInvalidatePlanningRevision();
		events.push_back({"hunt_region_scan", "\"phase\":\"stale_revision\""});
	}
	return events;
}

void playerbot::PlayerBotFixtureDriver::beginDelayedInitialization()
{
	delayedInitializationPending = policy.magicTrainingFixture || policy.deferProgressionFixtureInitialization;
}
playerbot::PlayerBotFixtureInitialization playerbot::PlayerBotFixtureDriver::delayedInitializationStatus(Player& player)
{
	if (!delayedInitializationPending) return PlayerBotFixtureInitialization::NotPending;
	if (policy.deferProgressionFixtureInitialization) {
		int32_t ready = -1;
		player.getStorageValue(gameplayFixtureReadyStorage, ready);
		if (ready == 2) { delayedInitializationPending = false; return PlayerBotFixtureInitialization::Cancelled; }
		if (ready != 1) return PlayerBotFixtureInitialization::Waiting;
	}
	delayedInitializationPending = false;
	return PlayerBotFixtureInitialization::Ready;
}
bool playerbot::PlayerBotFixtureDriver::consumeDepotRestartCheckpoint(Player& player, DepotRestartCheckpoint checkpoint)
{
	if (policy.depotRestartCheckpoint != checkpoint) return false;
	int32_t consumed = -1;
	if (player.getStorageValue(depotRestartCheckpointStorage, consumed) && consumed == 1) return false;
	player.addStorageValue(depotRestartCheckpointStorage, 1);
	return true;
}
bool playerbot::PlayerBotFixtureDriver::completeEquipmentPurchase(Player& player) const
{
	if (!policy.equipmentPurchaseFixture) return false;
	player.addStorageValue(gameplayFixtureReadyStorage, -1);
	return true;
}
uint64_t playerbot::PlayerBotFixtureDriver::observedMagicTrainingMana(uint64_t engineObservation) const
{
	return policy.forceMagicTrainingVerificationFailure ? engineObservation + 1 : engineObservation;
}

std::vector<playerbot::PlayerBotFixtureEvent> playerbot::PlayerBotFixtureDriver::runAdaptiveChallenge(Player& player, PlayerBotHuntRuntime& runtime)
{
	if (!policy.adaptiveChallengeFixture || adaptiveChallengeRun) return {};
	adaptiveChallengeRun = true;
	std::vector<PlayerBotFixtureEvent> events;
	auto evidence = [&](double seconds, uint32_t kills, uint32_t recoveries, bool death = false) {
		runtime.fixtureResetCombatEvidence();
		runtime.fixtureObserveCombat({seconds != 0, seconds, player.getMaxHealth(), player.getMaxHealth(), 1});
		for (uint32_t i = 0; i < kills; ++i) runtime.fixtureObserveKill();
		for (uint32_t i = 0; i < recoveries; ++i) runtime.fixtureObserveRecovery(true);
		if (death) runtime.fixtureObserveDeath();
		const auto update = runtime.fixtureUpdateChallenge(300, player.getMaxHealth());
		std::ostringstream fields;
		fields << std::fixed << std::setprecision(3) << "\"result\":" << jsonString(playerBotHuntChallengeResultName(update.result))
		       << ",\"reason\":\"adaptive_challenge_fixture\",\"frontier_before\":" << update.frontierBefore
		       << ",\"frontier_after\":" << update.frontierAfter << ",\"hold_qualifying_hunts\":" << static_cast<uint16_t>(update.qualifyingHuntsToHold)
		       << ",\"active_combat_seconds\":" << update.combat.activeSeconds << ",\"active_combat_uptime\":" << update.activeCombatUptime
		       << ",\"kills\":" << update.combat.kills << ",\"minimum_active_combat_seconds\":" << update.minimumActiveCombatSeconds
		       << ",\"minimum_kills\":" << update.minimumKills << ",\"minimum_health\":" << (update.combat.minimumHealth == std::numeric_limits<int32_t>::max() ? 0 : update.combat.minimumHealth)
		       << ",\"verified_recoveries\":" << update.verifiedRecoveries << ",\"retreat\":false,\"danger\":" << (update.combat.dangerObserved ? "true" : "false")
		       << ",\"death\":" << (update.combat.deathObserved ? "true" : "false");
		events.push_back({"hunt_challenge_frontier", fields.str()});
	};
	runtime.fixtureResetCombatEvidence();
	runtime.fixtureObserveCombat({false, 30, player.getMaxHealth(), player.getMaxHealth(), 1});
	const double idle = runtime.fixtureCombatSummary().activeSeconds;
	runtime.fixtureObserveCombat({true, 30, player.getMaxHealth(), player.getMaxHealth(), 1});
	const double active = runtime.fixtureCombatSummary().activeSeconds;
	evidence(0, 0, 0); evidence(30, 0, 0); evidence(30, 1, 0); evidence(30, 1, 0); evidence(30, 1, 1); evidence(30, 1, 0); evidence(30, 1, 0); evidence(30, 1, 0); evidence(0, 0, 0, true);
	const Item* weapon = player.getWeapon(true);
	const PlayerBotCombatProfile profile{player.getLevel(), player.getMaxHealth(), player.getArmor(), player.getDefense(), weapon ? weapon->getAttack() : 7, weapon ? player.getWeaponSkill(weapon) : player.getSkillLevel(SKILL_FIST), player.getAttackFactor()};
	PlayerBotHuntRegion current, equipped;
	runtime.fixtureScoreRegion(player, profile, current, equipped);
	const PlayerBotRecoveryPrediction recovery = playerBotPredictRecovery(playerBotHuntPlanningProfile(player, profile, runtime.huntPolicy().challengeFrontier()), 30);
	PlayerBotHuntRegion inBand; inBand.score = 10; inBand.suitable = inBand.reachable = inBand.inChallengeBand = true;
	PlayerBotHuntRegion easier; easier.score = 1000; easier.suitable = easier.reachable = true;
	std::vector<PlayerBotHuntRegion> exhausted(1); exhausted.front().suitable = true;
	std::ostringstream fields;
	fields << std::fixed << std::setprecision(2) << "\"recovery_total\":" << recovery.totalMinimumHealing << ",\"recovery_spell_legal\":" << (recovery.lightHealingLegal ? "true" : "false") << ",\"recovery_spell_casts\":" << recovery.spellCasts << ",\"equipment_pressure_before\":" << current.threatRatio << ",\"equipment_pressure_after\":" << equipped.threatRatio << ",\"idle_observed_seconds\":" << idle << ",\"active_observed_seconds\":" << active << ",\"in_band_outranks_easier\":" << (playerBotPreferHuntRegion(inBand, easier) ? "true" : "false") << ",\"wounded_lethal\":" << (playerBotPredictedLethal(40, 40) ? "true" : "false") << ",\"zero_health_lethal\":" << (playerBotPredictedLethal(0, 0) ? "true" : "false") << ",\"helper_scope_exhausted\":" << (playerBotHuntScopeExhausted(exhausted) ? "true" : "false");
	events.push_back({"adaptive_challenge_fixture", fields.str()});
	runtime.fixtureResetCombatEvidence();
	return events;
}

std::vector<playerbot::PlayerBotFixtureEvent> playerbot::PlayerBotFixtureDriver::runSpellCalibration(Player& player, PlayerBotSurvivalRuntime& runtime)
{
	if (!policy.spellCalibrationFixture) return {};
	std::vector<PlayerBotFixtureEvent> events;
	auto observe = [&](const char* spell, const char* target, const PlayerBotSpellEnvelope& envelope, PlayerBotSpellEvidence evidence, int32_t value, const char* phase) {
		const auto& profile = runtime.observeCalibrationFixture(spell, target, envelope, evidence, value);
		const bool math = std::strcmp(phase, "low_confidence") == 0 || std::strcmp(phase, "gradual_ranking") == 0 || std::strcmp(phase, "bounded_range") == 0;
		std::ostringstream fields;
		fields << "\"source\":" << jsonString(math ? "profile_math" : "classifier_helper") << ",\"phase\":" << jsonString(phase) << ",\"spell\":" << jsonString(spell) << ",\"target_class\":" << jsonString(target) << ",\"evidence\":" << jsonString(playerBotSpellEvidenceName(evidence)) << ",\"engine_bounds\":{\"minimum\":" << envelope.minimum << ",\"maximum\":" << envelope.maximum << ",\"duration_ms\":" << envelope.durationMs << "},\"calibration\":{\"accepted\":" << profile.accepted << ",\"rejected\":" << profile.rejected << ",\"ambiguous\":" << profile.ambiguous << ",\"minimum\":" << profile.minimum << ",\"maximum\":" << profile.maximum << ",\"conservative\":" << profile.conservative << ",\"ranking\":" << profile.ranking << ",\"confidence\":" << profile.confidence << '}';
		if (std::strcmp(phase, "low_confidence") == 0) fields << ",\"policy_unchanged\":true";
		events.push_back({"spell_calibration", fields.str()});
	};
	const auto* healing = playerBotSpellDescriptor("Light Healing"); const auto* ranged = playerBotSpellDescriptor("Whirlwind Throw"); const auto* melee = playerBotSpellDescriptor("Berserk"); const auto* support = playerBotSpellDescriptor("Haste");
	if (!healing || !ranged || !melee || !support) return events;
	const auto healEnvelope = playerBotSpellEnvelope(player, *healing); const auto rangeEnvelope = playerBotSpellEnvelope(player, *ranged); const auto meleeEnvelope = playerBotSpellEnvelope(player, *melee); const auto supportEnvelope = playerBotSpellEnvelope(player, *support);
	PlayerBotSpellObservation heal{true, false, false, true, false, false, false, std::max(1, healEnvelope.minimum)};
	observe(healing->name, "self", healEnvelope, playerBotClassifySpellObservation(healing->role, heal, healEnvelope.maximum + 1, healEnvelope), heal.value, "isolated_healing"); observe(healing->name, "self", healEnvelope, playerBotClassifySpellObservation(healing->role, heal, healEnvelope.maximum, healEnvelope), heal.value, "healing_equality_exact"); observe(healing->name, "self", healEnvelope, PlayerBotSpellEvidence::CensoredOverheal, 0, "overheal_censored"); observe(healing->name, "self", healEnvelope, PlayerBotSpellEvidence::ConcurrentDamage, 0, "concurrent_damage"); observe(healing->name, "self", healEnvelope, PlayerBotSpellEvidence::CastNotVerified, 0, "rejected_cast");
	PlayerBotSpellObservation damage{true, false, false, true, false, false, false, std::max(1, rangeEnvelope.minimum)};
	observe(ranged->name, "monster:fixture", rangeEnvelope, playerBotClassifySpellObservation(ranged->role, damage, 0, rangeEnvelope), damage.value, "single_target_damage"); observe(ranged->name, "monster:fixture", rangeEnvelope, PlayerBotSpellEvidence::MeleeOrOtherBotDamage, 0, "melee_ambiguous"); observe(ranged->name, "monster:fixture", rangeEnvelope, PlayerBotSpellEvidence::OtherAttacker, 0, "other_attacker_ambiguous"); observe(ranged->name, "monster:fixture", rangeEnvelope, PlayerBotSpellEvidence::TargetLost, 0, "target_loss_ambiguous"); observe(melee->name, "monster:fixture", meleeEnvelope, PlayerBotSpellEvidence::MultiTarget, 0, "multi_target_ambiguous"); observe(support->name, "self", supportEnvelope, PlayerBotSpellEvidence::Accepted, supportEnvelope.durationMs, "support_duration"); observe(support->name, "self", supportEnvelope, PlayerBotSpellEvidence::PreexistingOrReplacedCondition, 0, "support_preexisting_or_replaced");
	for (uint16_t sample = 0; sample < 9; ++sample) observe(ranged->name, "monster:fixture", rangeEnvelope, PlayerBotSpellEvidence::Accepted, sample == 8 ? 70000 : rangeEnvelope.maximum + sample, sample == 0 ? "low_confidence" : sample == 8 ? "bounded_range" : "gradual_ranking");
	for (uint8_t profile = 0; profile <= 12; ++profile) { runtime.observeCalibrationFixture(ranged->name, "monster:eviction-" + std::to_string(profile), rangeEnvelope, PlayerBotSpellEvidence::Accepted, rangeEnvelope.minimum); if (auto evicted = runtime.takeCalibrationEviction()) events.push_back({"spell_calibration_eviction", "\"source\":\"profile_math\",\"evicted_profile\":" + jsonString(*evicted) + ",\"profile_count\":" + std::to_string(runtime.calibrationSize())}); }
	const size_t before = runtime.calibrationSize(); runtime.clearCalibration(); events.push_back({"spell_calibration", "\"source\":\"profile_math\",\"phase\":\"fixture_profile_clear\",\"profiles_before\":" + std::to_string(before) + ",\"profiles_after\":" + std::to_string(runtime.calibrationSize()) + ",\"persistent\":false"});
	return events;
}

std::vector<playerbot::PlayerBotFixtureEvent> playerbot::PlayerBotFixtureDriver::runMagicTraining(Player& player)
{
	if (!policy.magicTrainingFixture) return {};
	std::vector<PlayerBotFixtureEvent> events;
	const auto defaultForecast = player.getManaRegenerationForecast();
	std::ostringstream initial; initial << "\"source\":\"authoritative_forecast\",\"case\":\"active_default\",\"active\":" << (defaultForecast ? "true" : "false"); if (defaultForecast) initial << ",\"gain\":" << defaultForecast->gain << ",\"interval\":" << defaultForecast->interval << ",\"remaining\":" << defaultForecast->remaining; events.push_back({"magic_training_fixture", initial.str()});
	ConditionRegeneration active(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 10000); active.setParam(CONDITION_PARAM_MANAGAIN, 10); active.setParam(CONDITION_PARAM_MANATICKS, 1000); active.executeCondition(&player, 250);
	if (const auto forecast = active.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL)) { std::ostringstream fields; fields << "\"source\":\"authoritative_forecast\",\"case\":\"active\",\"gain\":" << forecast->gain << ",\"interval\":" << forecast->interval << ",\"remaining\":" << forecast->remaining << ",\"exact_full_predicted\":" << 990 + forecast->gain << ",\"exact_full_overflow\":false,\"overflow_predicted\":" << 995 + forecast->gain << ",\"overflow_wasted\":" << 995 + forecast->gain - 1000; events.push_back({"magic_training_fixture", fields.str()}); }
	ConditionRegeneration finite(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 500); finite.setParam(CONDITION_PARAM_MANAGAIN, 10); finite.setParam(CONDITION_PARAM_MANATICKS, 1000); finite.executeCondition(&player, 250); events.push_back({"magic_training_fixture", std::string("\"source\":\"authoritative_forecast\",\"case\":\"finite_final_tick\",\"active\":") + (finite.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false")}); finite.setParam(CONDITION_PARAM_MANATICKS, 3000); events.push_back({"magic_training_fixture", std::string("\"source\":\"authoritative_forecast\",\"case\":\"finite_expires_before_tick\",\"active\":") + (finite.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false")});
	ConditionRegeneration nonDefault(CONDITIONID_COMBAT, CONDITION_REGENERATION, 10000); nonDefault.setParam(CONDITION_PARAM_MANAGAIN, 4); nonDefault.setParam(CONDITION_PARAM_MANATICKS, 1000); nonDefault.executeCondition(&player, 500); const auto nonDefaultForecast = nonDefault.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL); events.push_back({"magic_training_fixture", "\"source\":\"authoritative_forecast\",\"case\":\"non_default\",\"active\":" + std::string(nonDefaultForecast ? "true" : "false") + ",\"gain\":" + std::to_string(nonDefaultForecast ? nonDefaultForecast->gain : 0) + ",\"remaining\":" + std::to_string(nonDefaultForecast ? nonDefaultForecast->remaining : 0)});
	if (player.getZone() != ZONE_PROTECTION) {
		auto addForecastCondition = [&player](ConditionId_t id, uint32_t gain, uint32_t interval) {
			auto* condition = new ConditionRegeneration(id, CONDITION_REGENERATION, 10000);
			condition->setParam(CONDITION_PARAM_MANAGAIN, gain);
			condition->setParam(CONDITION_PARAM_MANATICKS, interval);
			condition->executeCondition(&player, 500);
			player.addCondition(condition);
		};
		addForecastCondition(CONDITIONID_COMBAT, 3, 1000);
		addForecastCondition(CONDITIONID_HEAD, 7, 2000);
		const auto aggregated = player.getManaRegenerationForecast();
		events.push_back({"magic_training_fixture", "\"source\":\"authoritative_forecast\",\"case\":\"earliest_same_engine_cycle\",\"active\":" + std::string(aggregated ? "true" : "false") + ",\"gain\":" + std::to_string(aggregated ? aggregated->gain : 0) + ",\"remaining\":" + std::to_string(aggregated ? aggregated->remaining : 0)});
		player.removeCondition(CONDITION_REGENERATION, CONDITIONID_COMBAT);
		player.removeCondition(CONDITION_REGENERATION, CONDITIONID_HEAD);
	}
	ConditionRegeneration expired(CONDITIONID_DEFAULT, CONDITION_REGENERATION, 0); expired.setParam(CONDITION_PARAM_MANAGAIN, 10); expired.setParam(CONDITION_PARAM_MANATICKS, 1000); events.push_back({"magic_training_fixture", std::string("\"source\":\"authoritative_forecast\",\"case\":\"expired\",\"active\":") + (expired.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false")});
	if (player.getZone() == ZONE_PROTECTION) events.push_back({"magic_training_fixture", std::string("\"source\":\"authoritative_forecast\",\"case\":\"protection_zone\",\"active\":") + (active.getManaForecast(player, EVENT_CREATURE_THINK_INTERVAL) ? "true" : "false")});
	return events;
}
