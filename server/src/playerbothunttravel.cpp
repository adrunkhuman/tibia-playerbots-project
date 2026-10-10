#include "otpch.h"
#include "playerbotcontroller.h"
#include "playerbotnpccapabilities.h"
#include "playerbothuntwalk.h"
#include <cstring>
#include <iomanip>
#include "playerbotroutechanges.h"
#include "playerbottransportsearch.h"
#include "playerbothunttravelpolicy.h"
#include "playerbothunttravelevidence.h"
#include "groups.h"
#include "guild.h"

using namespace playerbot;

namespace {
	constexpr uint16_t shovelToolItemId = 2554;
	constexpr uint64_t localNodesPerTurn = 512;
	constexpr uint32_t graphOperationsPerTurn = 1024;
	constexpr std::chrono::milliseconds graphSliceTime(8);
	using OfferFact = PlayerBotHuntTravelEvidence::Offer;
	using ActorFact = PlayerBotHuntTravelEvidence::Actor;

	ActorFact actorFacts(Player& player, const PlayerBotCombatProfile& combat, bool rope, bool shovel) {
		return {combat.level, combat.maximumHealth, combat.armor, combat.defense, combat.attack,
		    combat.attackSkill, combat.attackFactor, player.getMoney() + player.getBankBalance(),
		    player.isPremium(), player.isPzLocked(), rope, shovel,
		    player.getStepDuration(), player.getStepDuration(DIRECTION_NORTHEAST), player.getGroup() && player.getGroup()->access,
		    player.getGroup() ? player.getGroup()->flags : 0, player.getGuild() ? player.getGuild()->getId() : 0,
		    player.getGuildRank() ? player.getGuildRank()->id : 0, player.getPosition(), combat.hitChance,
		    combat.blockedByShield};
	}

	using UnavailableOffers = std::map<std::pair<uint32_t, Position>, std::chrono::steady_clock::time_point>;
	std::vector<OfferFact> offerFacts(Player& player, Position source, uint64_t money, bool walkingOnly,
	                                  const UnavailableOffers& unavailableOffers) {
		std::vector<OfferFact> offers;
		if (!walkingOnly && !player.isPzLocked()) {
			for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Travel, source)) {
				for (const auto& offer : npc->getTravelOffers()) {
					if (!playerBotNpcTravelOfferEligible(player.getLevel(), player.isPremium(), money, offer.level,
					    offer.premium, offer.price, offer.hasOpaqueCondition, offer.hasOpaqueAction) || offer.destination == source) continue;
					const auto unavailable = unavailableOffers.find({npc->getID(), offer.destination});
					if (unavailable != unavailableOffers.end() && unavailable->second > std::chrono::steady_clock::now()) continue;
					offers.emplace_back(npc->getID(), npc->getPosition(), offer.destination, offer.price, offer.level,
					                    offer.premium, offer.dialogue);
				}
			}
		}
		std::sort(offers.begin(), offers.end());
		return offers;
	}

	bool safe(const PlayerBotNavigationRoutePlan& p, const PlayerBotNavigationRiskProfile& risk) {
		return p.metrics.result == PlayerBotNavigationResult::Reached &&
		       playerBotNavigationRiskAccepts(risk, p.metrics.dangerCost, p.metrics.maximumHealthLossPerSecond);
	}
	double cost(const PlayerBotNavigationRoutePlan& p, bool sellEconomy) {
		return playerBotTransportRouteCost(p.metrics.estimatedTravelSeconds, p.metrics.fare, p.metrics.dangerCost, sellEconomy);
	}
}

// One pending selector request, including all walking/transport frontiers.
// Destroying this value cancels it; there is no callback, event, or live pointer.
struct PlayerBotHuntTravelWork {
	uint64_t pass = 0, revision = 0, sequence = 0, topologyGeneration = 0, riskRevision = 0;
	Position source, destination;
	std::vector<Position> destinations;
	std::set<Position> blockedPositions;
	ActorFact actor;
	std::vector<OfferFact> offers;
	PlayerBotRouteChanges::Watch watch;
	PlayerBotNavigationCostPolicy policy;
	PlayerBotNavigationRiskProfile riskProfile;
	PlayerBotNavigationRoutePlan walking;
	std::unique_ptr<PlayerBotHuntWalkSearch> local;
	std::string profile;
	std::chrono::steady_clock::duration connectionTime{};
	// Connections that expanded no tiles, by fallback reason. The first of each
	// is logged; slice counters cover the rest.
	std::set<std::string> trivialConnections;
	bool walkingDone = false;
	std::vector<Position> states;
	std::vector<uint32_t> providers;
	PlayerBotHuntProviderValidation providerValidation;
	PlayerBotHuntRequestRestartBudget restartBudget;
	std::unique_ptr<PlayerBotTransportSearch> transport;
	uint64_t npcNodes = 0;
	uint64_t transportReserve = 0;
	bool sellEconomy = false;
	bool walkingOnly = false;
	bool preferSafeWalking = false;

	bool sameRequest(const PlayerBotHuntRouteRequest& r, Position from, uint64_t reserve, bool sell,
	                 const PlayerBotNavigationRiskProfile& risk) const {
		return pass == r.planningPass && revision == r.scoringRevision && sequence == r.sequence &&
		       source == from && destination == r.to && destinations == (r.destinations.empty() ? std::vector<Position>{r.to} : r.destinations) && transportReserve == reserve && sellEconomy == sell &&
		       walkingOnly == r.walkingOnly && preferSafeWalking == r.preferSafeWalking && blockedPositions == r.blockedPositions &&
		       riskProfile.healthLossCost == risk.healthLossCost &&
		       riskProfile.maximumRouteHealthLoss == risk.maximumRouteHealthLoss &&
		       riskProfile.maximumHealthLossPerSecond == risk.maximumHealthLossPerSecond;
	}

	void beginTransport(uint64_t money) {
		local.reset();
		providers.clear();
		std::map<uint32_t, Position> live;
		for (const auto& offer : offers) live[std::get<0>(offer)] = std::get<1>(offer);
		providerValidation.reset(live);
		states = {source};
		std::map<Position, size_t> indices{{source, 0}};
		std::map<uint32_t, size_t> connections;
		std::vector<PlayerBotTransportSearch::Offer> graph;
		std::vector<Position> providerPositions;
		// Geometry only schedules graph work. Boats and portals can cross the
		// map instantly, so this estimate must never enter a cost bound.
		const auto rank = [&](Position p) {
			return double(std::max(Position::getDistanceX(p, destination), Position::getDistanceY(p, destination)));
		};
		for (const auto& offer : offers) {
			const auto [npc, position, to, fare, level, premium, dialogue] = offer;
			(void)level; (void)premium; (void)dialogue;
			if (!indices.count(to)) { indices[to] = states.size(); states.push_back(to); }
			if (!connections.count(npc)) {
				connections[npc] = providers.size(); providers.push_back(npc);
				providerPositions.push_back(position);
			}
			graph.push_back({connections.at(npc), indices.at(to), fare, rank(to)});
		}
		std::optional<PlayerBotTransportSearch::Label> incumbent;
		if (safe(walking, riskProfile)) {
			incumbent.emplace();
			incumbent->cost = cost(walking, sellEconomy);
			incumbent->seconds = walking.metrics.estimatedTravelSeconds;
			incumbent->danger = walking.metrics.dangerCost;
			incumbent->peak = walking.metrics.maximumHealthLossPerSecond;
		}
		std::vector<double> stateRanks;
		stateRanks.reserve(states.size());
		for (const auto& state : states) stateRanks.push_back(rank(state));
		transport = std::make_unique<PlayerBotTransportSearch>(states.size(), std::move(graph), money, incumbent,
		    std::move(stateRanks), states, std::move(providerPositions), sellEconomy, riskProfile);
	}
};

bool PlayerBotController::huntTravelExitValid(Player& player, const PlayerBotHuntTravelEvidence& evidence) const
{
	using Exit = PlayerBotHuntTravelEvidence::Exit;
	if (evidence.exit == Exit::None) return true;
	const Position approach = evidence.destination;
	if (!playerBotStableApproachTile(g_game.map.getTile(approach), player)) return false;
	if (evidence.exit == Exit::Depot) {
		for (const auto& [depotId, lockers] : g_game.map.getDepotLockerPositions()) {
			for (const Position& locker : lockers) {
				if (locker == approach || !Position::areInRange<1, 1, 0>(locker, approach)) continue;
				uint16_t itemId = 0;
				if (findDepotLocker(locker, depotId, itemId)) return true;
			}
		}
		return false;
	}
	const uint16_t itemId = evidence.supplyItemId ? evidence.supplyItemId : recoveryPotionItemId(player.getVocationId());
	for (const auto& [id, npc] : g_game.getNpcs()) {
		(void)id;
		if (!npc || !playerBotNpcHasCapability(*npc, PlayerBotNpcCapability::Shop) ||
		    npc->getPosition() == approach || !Position::areInRange<3, 3, 0>(npc->getPosition(), approach)) continue;
		if (std::any_of(npc->getShopOffers().begin(), npc->getShopOffers().end(), [itemId](const ShopInfo& offer) {
			return offer.itemId == itemId && offer.buyPrice != 0;
		})) return true;
	}
	return false;
}

bool PlayerBotController::huntTravelEvidenceValid(Player& player, const Position& source,
    const Position& destination, PlayerBotHuntTravelEvidence& evidence)
{
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
	                                                 PlayerBotEquipmentAdapter::loadout(player));
	const bool rope = g_game.findItemOfType(&player, ropeItemId, true) != nullptr;
	const bool shovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	if (!evidence.validContext(source, destination, actorFacts(player, combat, rope, shovel),
	    PlayerBotTopology::instance().generation(), PlayerBotHuntRegionPlanner::getCacheRevision(), riskProfile)) return false;
	if (evidence.paid && !evidence.validOffers(offerFacts(player, source, playerBotHuntTransportFunds(
	    player.getMoney() + player.getBankBalance(), evidence.transportReserve), evidence.walkingOnly, unavailableTravelOffers))) return false;
	if (!huntTravelExitValid(player, evidence)) return false;
	PlayerBotRouteChanges::absorb(evidence.watch);
	return true;
}

const char* PlayerBotController::trainerTravelExecutionChange(Player& player, PlayerBotHuntTravelEvidence& evidence)
{
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
	                                                 PlayerBotEquipmentAdapter::loadout(player));
	const bool rope = g_game.findItemOfType(&player, ropeItemId, true) != nullptr;
	const bool shovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	return evidence.executionContextChange(actorFacts(player, combat, rope, shovel), static_cast<const Creature&>(player).getStepSpeed(),
	    PlayerBotTopology::instance().generation(), PlayerBotHuntRegionPlanner::getCacheRevision(), riskProfile);
}

std::optional<PlayerBotNavigationRoutePlan> PlayerBotController::advanceHuntTravelRoute(
	Player& player, const PlayerBotHuntRouteRequest& request, const Position& source, PlayerBotHuntRouteTiming& timing,
	uint64_t transportReserve, bool sellEconomy, PlayerBotNavigationRoutePlan* walkingAlternative,
	std::shared_ptr<PlayerBotHuntTravelEvidence>* completedEvidence,
	const PlayerBotNavigationRiskProfile* riskOverride)
{
	// All request, cache, search, and acceptance dependencies use one policy.
	const PlayerBotNavigationRiskProfile& riskProfile = riskOverride ? *riskOverride : this->riskProfile;
	if (completedEvidence) completedEvidence->reset();
	const auto began = std::chrono::steady_clock::now();
	timing.requestSequence = request.sequence;
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
	                                                 PlayerBotEquipmentAdapter::loadout(player));
	const bool rope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
	const bool shovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	const uint64_t funds = player.getMoney() + player.getBankBalance();
	const uint64_t money = playerBotHuntTransportFunds(funds, transportReserve);
	timing.transportSpendableFunds = money;
	const ActorFact actor = actorFacts(player, combat, rope, shovel);
	const auto offers = offerFacts(player, source, money, request.walkingOnly, unavailableTravelOffers);
	const uint64_t generation = PlayerBotTopology::instance().generation();
	const uint64_t riskRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
	const bool sameRequest = huntTravelWork && huntTravelWork->sameRequest(request, source, transportReserve, sellEconomy, riskProfile);
	const auto journalReason = sameRequest ? huntTravelWork->watch.check() : PlayerBotRouteChanges::Reason::None;
	const char* invalidationReason = !sameRequest ? "request" :
	    huntTravelWork->actor != actor ? "actor" :
	    huntTravelWork->topologyGeneration != generation ? "topology" :
	    huntTravelWork->riskRevision != riskRevision ? "risk" :
	    PlayerBotRouteChanges::reasonName(journalReason);
	const bool reuse = sameRequest && huntTravelWork->actor == actor &&
	    huntTravelWork->topologyGeneration == generation && huntTravelWork->riskRevision == riskRevision &&
	    journalReason == PlayerBotRouteChanges::Reason::None;
	if (!reuse) {
		PlayerBotHuntRequestRestartBudget restartBudget;
		if (sameRequest) {
			++timing.invalidations;
			timing.invalidationReason = invalidationReason;
			timing.invalidationCause = PlayerBotRouteChanges::causeName(huntTravelWork->watch.changedCause);
			timing.invalidationChangedTile = huntTravelWork->watch.changedTile;
			timing.invalidationJournalDelta = huntTravelWork->watch.delta;
			timing.invalidationWatchedTiles = huntTravelWork->watch.tiles.size();
			restartBudget = huntTravelWork->restartBudget;
			if (!restartBudget.retry()) {
				++timing.requestRestartLimits;
				huntTravelWork.reset();
				PlayerBotNavigationRoutePlan unknown;
				unknown.metrics.attempted = true;
				unknown.metrics.result = PlayerBotNavigationResult::NodeLimit;
				return unknown;
			}
		}
		huntTravelWork = std::make_shared<PlayerBotHuntTravelWork>();
		auto& w = *huntTravelWork;
		w.pass = request.planningPass; w.revision = request.scoringRevision; w.sequence = request.sequence;
		w.source = source; w.destination = request.to;
		w.destinations = request.destinations.empty() ? std::vector<Position>{request.to} : request.destinations;
		w.blockedPositions = request.blockedPositions;
		w.transportReserve = transportReserve; w.sellEconomy = sellEconomy;
		w.walkingOnly = request.walkingOnly; w.preferSafeWalking = request.preferSafeWalking;
		w.actor = actor; w.topologyGeneration = generation;
		auto profile = actor;
		std::get<7>(profile) = 0; // Money does not change a walking connection.
		std::get<18>(profile) = Position{};
		std::ostringstream key;
		key << std::setprecision(17);
		std::apply([&](const auto&... fields) { ((key << fields << ';'), ...); }, profile);
		w.profile = key.str();
		w.restartBudget = restartBudget;
		w.riskRevision = riskRevision;
		w.policy = navigationCostPolicy(player);
		w.policy.risk = riskProfile;
		w.riskProfile = riskProfile;
		w.walking.metrics.attempted = true;
		w.walking.metrics.result = PlayerBotNavigationResult::Reached;
		w.walking.metrics.waypoint = request.to;
		++timing.attempts;
	}
	const auto work = huntTravelWork; // Keep the scoped tile observer alive through return.
	auto& w = *work;
	auto retainEvidence = [&](const PlayerBotNavigationRoutePlan& route) {
		if (!completedEvidence || !safe(route, riskProfile)) return;
		auto evidence = std::make_shared<PlayerBotHuntTravelEvidence>();
		evidence->source = w.source; evidence->destination = w.destination;
		evidence->actor = w.actor; evidence->topologyGeneration = w.topologyGeneration;
		evidence->riskRevision = w.riskRevision; evidence->transportReserve = w.transportReserve;
		evidence->risk = w.riskProfile; evidence->watch = w.watch;
		evidence->retainExecutionLeg(route, static_cast<const Creature&>(player).getStepSpeed());
		evidence->paid = route.metrics.firstNpcTravelOffer.has_value();
		evidence->walkingOnly = w.walkingOnly;
		if (evidence->paid) { evidence->offers = w.offers; evidence->providers = w.providerValidation; }
		*completedEvidence = std::move(evidence);
	};
	if (!w.transport) {
		// NPC movement/offers cannot invalidate the independent walking frontier.
		w.offers = offers;
	} else {
		std::map<uint32_t, Position> live;
		for (const auto& offer : offers) live[std::get<0>(offer)] = std::get<1>(offer);
		// Compare offer semantics separately from positions. Canonical order is
		// NPC ID first, so movement does not reorder a provider's offers.
		auto semantics = offers;
		if (semantics.size() == w.offers.size()) {
			for (size_t i = 0; i < semantics.size(); ++i) std::get<1>(semantics[i]) = std::get<1>(w.offers[i]);
		}
		if (semantics != w.offers || !w.providerValidation.update(live)) {
			++timing.invalidations;
			if (!w.providerValidation.restartAllowed()) {
				++timing.transportRestartLimits;
				// No stale paid result can escape. Walking remains usable if safe;
				// otherwise this is incomplete evidence, never unreachability.
				auto result = std::move(w.walking);
				result.metrics.result = playerBotHuntAggregateRouteResult(result.metrics.result,
				    result.metrics.dangerCost, result.metrics.maximumHealthLossPerSecond, true, riskProfile);
				retainEvidence(result);
				huntTravelWork.reset();
				return result;
			}
			++timing.transportRestarts;
			w.offers = offers;
			w.beginTransport(money);
		}
	}
	PlayerBotRouteChanges::Scope watch(w.watch);
	PlayerBotRouteChanges::read(player.getPosition()); // PZ checks also read the actor's current tile.
	const auto& policy = w.policy;
	w.walking.metrics.dangerAware = policy.enabled();
	// This necessary condition applies to every route, including NPC travel.
	// Keep explicit endpoint evidence instead of an empty failed-plan summary.
	auto rejectedEndpoints = [&](Position from, const std::vector<Position>& targets, const char* kind) {
		const auto peak = playerBotNavigationRejectedEndpointPeak(from, PlayerBotNavigationGoal::anyOf(targets),
		    riskProfile.maximumHealthLossPerSecond, [&](Position p) { return policy.dangerAt(p); });
		if (!peak) return peak;
		++timing.riskRejects;
		std::ostringstream fields;
		fields << std::setprecision(17) << "\"planning_pass\":" << w.pass << ",\"scoring_revision\":" << w.revision
		       << ",\"request_sequence\":" << w.sequence << ",\"kind\":\"" << kind << "\""
		       << ",\"from\":{\"x\":" << from.x << ",\"y\":" << from.y << ",\"z\":" << unsigned(from.z) << "}"
		       << ",\"result\":\"risk_rejected\",\"fallback_reason\":\"endpoint_peak_danger\",\"expanded_nodes\":0"
		       << ",\"endpoint_minimum_peak\":" << *peak << ",\"maximum_peak\":" << riskProfile.maximumHealthLossPerSecond
		       << ",\"requested_target_count\":" << targets.size() << ",\"requested_targets\":[";
		for (size_t i = 0; i < std::min<size_t>(targets.size(), 8); ++i) {
			if (i) fields << ',';
			fields << "{\"x\":" << targets[i].x << ",\"y\":" << targets[i].y << ",\"z\":" << unsigned(targets[i].z) << '}';
		}
		fields << ']';
		emit("hunt_route_connection", player.getPosition(), fields.str());
		return peak;
	};
	if (!w.walkingDone && !w.local) {
		if (const auto peak = rejectedEndpoints(source, w.destinations, "walking")) {
			w.walking.metrics.result = PlayerBotNavigationResult::RiskRejected;
			w.walking.metrics.maximumHealthLossPerSecond = *peak;
			if (walkingAlternative) *walkingAlternative = w.walking;
			PlayerBotNavigationRoutePlan rejected = std::move(w.walking);
			huntTravelWork.reset();
			return rejected;
		}
	}
	uint64_t localBudget = localNodesPerTurn;
	auto connectionEvent = [&](const char* kind, PlayerBotNavigationResult result) {
		// Transport searches probe hundreds of arrivals per request; most end in
		// a graph verdict or cache hit without expanding a tile.
		if (w.local->expanded == 0 && !w.trivialConnections.emplace(w.local->fallbackReason).second) {
			w.connectionTime = {};
			return;
		}
		// Ordinary successes dominated live logs; slice counters already total
		// them. Keep failures, refinement fallbacks, and expensive successes.
		constexpr uint64_t expensiveConnectionNodes = 10000;
		if (result == PlayerBotNavigationResult::Reached && std::strcmp(w.local->fallbackReason, "none") == 0 &&
		    !w.local->unrestricted && w.local->expanded < expensiveConnectionNodes) {
			w.connectionTime = {};
			return;
		}
		constexpr size_t loggedTargets = 8;
		std::ostringstream fields;
		fields << "\"planning_pass\":" << w.pass << ",\"scoring_revision\":" << w.revision
		       << ",\"request_sequence\":" << w.sequence << ",\"kind\":\"" << kind << "\""
		       << ",\"from\":{\"x\":" << w.local->source.x << ",\"y\":" << w.local->source.y << ",\"z\":" << unsigned(w.local->source.z) << "}"
		       << ",\"to\":{\"x\":" << w.local->current.x << ",\"y\":" << w.local->current.y << ",\"z\":" << unsigned(w.local->current.z) << "}"
		       << ",\"result\":\"" << playerBotNavigationResultName(result) << "\""
		       << ",\"expanded_nodes\":" << w.local->expanded << ",\"cache_hits\":" << w.local->cacheHits
		       << ",\"incomplete_reused\":" << (w.local->incompleteReused ? "true" : "false")
		       << ",\"unrestricted_fallback\":" << (w.local->unrestricted ? "true" : "false")
		       << ",\"alternate_itineraries\":" << w.local->alternateItineraries
		       << ",\"corridor_searches\":" << w.local->corridorSearches
		       << ",\"corridor_radius\":" << w.local->corridorRadius
		       << ",\"heuristic_restarts\":" << w.local->heuristicRestarts
		       << ",\"elapsed_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(w.connectionTime).count();
		const auto& targets = w.local->destinations();
		fields << ",\"fallback_reason\":\"" << w.local->fallbackReason << "\",\"requested_target_count\":"
		       << targets.size() << ",\"requested_targets\":[";
		for (size_t i = 0; i < std::min(targets.size(), loggedTargets); ++i) {
			if (i != 0) fields << ',';
			fields << "{\"x\":" << targets[i].x << ",\"y\":" << targets[i].y << ",\"z\":" << unsigned(targets[i].z) << '}';
		}
		fields << ']';
		emit("hunt_route_connection", player.getPosition(), fields.str());
		w.connectionTime = {};
	};
	if (!w.walkingDone) {
		if (!w.local) w.local = std::make_unique<PlayerBotHuntWalkSearch>(source, w.destinations, playerBotNavigationMaximumExpandedNodes, w.blockedPositions);
		const auto before = w.local->expanded;
		const auto result = w.local->advance(player, policy, rope, shovel, w.profile, riskRevision, timing, localBudget, w.pass);
		timing.walkingExpandedNodes += w.local->expanded - before;
		const auto elapsed = std::chrono::steady_clock::now() - began;
		w.connectionTime += elapsed;
		timing.walkingTime += elapsed;
		if (!result) { ++timing.yields; return std::nullopt; }
		connectionEvent("walking", *result);
		w.walking.metrics.result = *result;
		w.walking.metrics.expandedNodes = w.local->expanded;
		w.walking.metrics.movementCost = w.local->summary.movementCost;
		w.walking.metrics.dangerCost = w.local->summary.dangerCost;
		w.walking.metrics.maximumHealthLossPerSecond = w.local->summary.maximumHealthLossPerSecond;
		w.walking.metrics.estimatedTravelSeconds = w.local->seconds;
		w.walking.steps = std::move(w.local->steps);
		w.walking.metrics.steps = w.walking.steps.size();
		if (*result == PlayerBotNavigationResult::Reached)
			w.walking.metrics.waypoint = w.walking.steps.empty() ? source : w.walking.steps.back().expectedPosition;
		w.walkingDone = true;
		timing.walkingReached += *result == PlayerBotNavigationResult::Reached;
		if (w.preferSafeWalking && safe(w.walking, riskProfile) && !w.walking.steps.empty()) {
			if (walkingAlternative) *walkingAlternative = w.walking;
			PlayerBotNavigationRoutePlan route = std::move(w.walking);
			retainEvidence(route);
			huntTravelWork.reset();
			return route;
		}
		w.beginTransport(money);
	}

	const auto npcBegan = std::chrono::steady_clock::now();
	const auto prior = w.transport->counters;
	bool queryPending = false;
	auto pending = [&]() -> std::optional<PlayerBotTransportSegment> { queryPending = true; return std::nullopt; };
	auto query = [&](size_t state, size_t connection, bool /*detailed*/) -> std::optional<PlayerBotTransportSegment> {
		const bool final = connection == SIZE_MAX;
		const auto from = w.states[state];
		std::vector<Position> targets;
		if (final) targets = w.destinations;
		else {
			const auto provider = w.providerValidation.use(w.providers[connection]);
			for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) {
				if (x || y) targets.emplace_back(provider.x + x, provider.y + y, provider.z);
			}
		}
		if (!w.local) {
			if (!final) targets.erase(std::remove_if(targets.begin(), targets.end(), [&](Position target) {
				// Redirect tiles cannot be conversation goals. The topology omits
				// teleports too; keeping one hides a provable disconnection behind
				// a full detailed search.
				return !playerBotStableApproachTile(g_game.map.getTile(target), player);
			}), targets.end());
			if (targets.empty()) return PlayerBotTransportSegment{};
			if (const auto peak = rejectedEndpoints(from, targets, final ? "transport_destination" : "npc_approach")) {
				return PlayerBotTransportSegment{PlayerBotNavigationResult::RiskRejected, 0, 0, 0, *peak};
			}
			// An any-of request keeps its safe alternatives; do not guide coarse
			// routing toward an endpoint the detailed graph can never enter.
			targets.erase(std::remove_if(targets.begin(), targets.end(), [&](Position target) {
				return target != from && !playerBotNavigationPeakAccepts(riskProfile.maximumHealthLossPerSecond, policy.dangerAt(target));
			}), targets.end());
			if (w.npcNodes >= playerBotNavigationMaximumExpandedNodes)
				return PlayerBotTransportSegment{PlayerBotNavigationResult::NodeLimit};
			w.local = std::make_unique<PlayerBotHuntWalkSearch>(from, targets,
			    std::min<uint64_t>(30000, playerBotNavigationMaximumExpandedNodes - w.npcNodes), w.blockedPositions);
		}
		if (localBudget == 0) return pending();
		const auto connectionBegan = std::chrono::steady_clock::now();
		const auto before = w.local->expanded;
		const auto result = w.local->advance(player, policy, rope, shovel, w.profile, riskRevision, timing, localBudget, w.pass);
		w.npcNodes += w.local->expanded - before;
		w.connectionTime += std::chrono::steady_clock::now() - connectionBegan;
		if (!result) return pending();
		connectionEvent(final ? "transport_destination" : "npc_approach", *result);
		PlayerBotTransportSegment segment{*result, w.local->seconds, static_cast<uint32_t>(w.local->steps.size()),
		    w.local->summary.dangerCost, w.local->summary.maximumHealthLossPerSecond};
		if (state == 0 && !final) segment.path = std::move(w.local->steps);
		w.local.reset();
		return segment;
	};
	bool done = false;
	// Cheap cached graph operations share a time slice instead of each small
	// batch imposing another scheduler wait. Individual topology queries remain atomic.
	const auto graphDeadline = began + graphSliceTime;
	for (uint32_t operation = 0; operation < graphOperationsPerTurn &&
	     std::chrono::steady_clock::now() < graphDeadline; ++operation) {
		if (w.transport->advance(query)) { done = true; break; }
		if (queryPending) break;
	}
	const auto& counters = w.transport->counters;
	timing.connectionCacheHits += counters.cacheHits - prior.cacheHits;
	timing.boundRejects += counters.boundRejects - prior.boundRejects;
	timing.riskRejects += counters.riskRejects - prior.riskRejects;
	timing.coarseConnections += counters.coarseQueries - prior.coarseQueries;
	timing.localConnections += counters.localQueries - prior.localQueries;
	timing.unknownConnections += counters.unknown - prior.unknown;
	timing.graphLabels += counters.labels - prior.labels;
	timing.npcTime += std::chrono::steady_clock::now() - npcBegan;
	if (!done) { ++timing.yields; return std::nullopt; }

	if (w.transport->bestPaid) {
		++timing.npcReturned; ++timing.npcReached;
		timing.npcReturnedExpandedNodes += w.npcNodes;
	}
	if (walkingAlternative) *walkingAlternative = w.walking;
	PlayerBotNavigationRoutePlan result = std::move(w.walking);
	if (const auto& paid = w.transport->bestPaid; paid && (!safe(result, riskProfile) || paid->cost < cost(result, w.sellEconomy))) {
		result = {};
		result.metrics.attempted = true;
		result.metrics.result = PlayerBotNavigationResult::Reached;
		result.metrics.expandedNodes = w.npcNodes;
		result.metrics.steps = paid->steps;
		result.metrics.estimatedTravelSeconds = paid->seconds;
		result.metrics.fare = paid->fare;
		result.metrics.dangerCost = paid->danger;
		result.metrics.maximumHealthLossPerSecond = paid->peak;
		result.metrics.dangerAware = policy.enabled();
		// Executable through boarding: the validated walk to the first NPC, then
		// travel. Later legs are replanned from each arrival.
		const auto& offer = w.offers.at(paid->firstOffer);
		PlayerBotNavigationStep step;
		step.action = PlayerBotNavigationAction::NpcTravel;
		step.npcId = std::get<0>(offer); step.target = w.providerValidation.use(step.npcId); step.expectedPosition = std::get<2>(offer);
		step.price = std::get<3>(offer); step.minimumLevel = std::get<4>(offer); step.premium = std::get<5>(offer);
		step.dialogue = std::get<6>(offer);
		if (const PlayerBotTransportSegment* firstLeg = w.transport->firstLeg(*paid)) {
			result.steps = firstLeg->path;
			result.metrics.localDangerCost = firstLeg->danger;
			result.metrics.localMaximumHealthLossPerSecond = firstLeg->peak;
		}
		result.metrics.firstNpcTravelOffer = PlayerBotNpcTravelOfferIdentity{
		    step.npcId, step.expectedPosition, step.price, step.minimumLevel, step.premium, step.dialogue};
		result.steps.push_back(std::move(step));
	}
	result.metrics.result = playerBotHuntAggregateRouteResult(result.metrics.result,
	    result.metrics.dangerCost, result.metrics.maximumHealthLossPerSecond, w.transport->incomplete, riskProfile);
	retainEvidence(result);
	huntTravelWork.reset();
	return result;
}
