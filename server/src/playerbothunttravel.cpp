#include "otpch.h"
#include "playerbotcontroller.h"
#include "playerbotnpccapabilities.h"
#include "playerbothuntwalk.h"
#include <iomanip>
#include "playerbotroutechanges.h"
#include "playerbottransportsearch.h"
#include "playerbothunttravelpolicy.h"
#include "groups.h"
#include "guild.h"

using namespace playerbot;

namespace {
	constexpr uint16_t shovelToolItemId = 2554;
	constexpr uint64_t localNodesPerTurn = 512;
	constexpr uint32_t graphOperationsPerTurn = 1024;
	constexpr std::chrono::milliseconds graphSliceTime(8);
	using OfferFact = std::tuple<uint32_t, Position, Position, uint32_t, uint32_t, bool, std::vector<std::string>>;
	using ActorFact = std::tuple<uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, float,
	                             uint64_t, bool, bool, bool, bool, uint32_t, uint32_t, bool,
	                             uint64_t, uint32_t, uint32_t, Position>;

	bool safe(const PlayerBotNavigationRoutePlan& p) {
		return p.metrics.result == PlayerBotNavigationResult::Reached &&
		       playerBotNavigationRiskAccepts({}, p.metrics.dangerCost, p.metrics.maximumHealthLossPerSecond);
	}
	double cost(const PlayerBotNavigationRoutePlan& p) {
		return p.metrics.estimatedTravelSeconds + p.metrics.fare / 10.0 + p.metrics.dangerCost / 10.0;
	}
}

// One pending selector request, including all walking/transport frontiers.
// Destroying this value cancels it; there is no callback, event, or live pointer.
struct PlayerBotHuntTravelWork {
	uint64_t pass = 0, revision = 0, sequence = 0, topologyGeneration = 0, riskRevision = 0;
	Position source, destination;
	ActorFact actor;
	std::vector<OfferFact> offers;
	PlayerBotRouteChanges::Watch watch;
	PlayerBotNavigationCostPolicy policy;
	PlayerBotNavigationRoutePlan walking;
	std::unique_ptr<PlayerBotHuntWalkSearch> local;
	std::string profile;
	std::chrono::steady_clock::duration connectionTime{};
	bool walkingDone = false;
	std::vector<Position> states;
	std::vector<uint32_t> providers;
	PlayerBotHuntProviderValidation providerValidation;
	PlayerBotHuntRequestRestartBudget restartBudget;
	std::unique_ptr<PlayerBotTransportSearch> transport;
	uint64_t npcNodes = 0;

	bool sameRequest(const PlayerBotHuntRouteRequest& r, Position from) const {
		return pass == r.planningPass && revision == r.scoringRevision && sequence == r.sequence &&
		       source == from && destination == r.to;
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
		if (safe(walking)) {
			incumbent.emplace();
			incumbent->cost = cost(walking);
			incumbent->seconds = walking.metrics.estimatedTravelSeconds;
			incumbent->danger = walking.metrics.dangerCost;
			incumbent->peak = walking.metrics.maximumHealthLossPerSecond;
		}
		std::vector<double> stateRanks;
		stateRanks.reserve(states.size());
		for (const auto& state : states) stateRanks.push_back(rank(state));
		transport = std::make_unique<PlayerBotTransportSearch>(states.size(), std::move(graph), money, incumbent,
		    std::move(stateRanks), states, std::move(providerPositions));
	}
};

std::optional<PlayerBotNavigationRoutePlan> PlayerBotController::advanceHuntTravelRoute(
	Player& player, const PlayerBotHuntRouteRequest& request, const Position& source, PlayerBotHuntRouteTiming& timing)
{
	const auto began = std::chrono::steady_clock::now();
	timing.requestSequence = request.sequence;
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player),
	                                                 PlayerBotEquipmentAdapter::loadout(player));
	const bool rope = g_game.findItemOfType(&player, playerbot::ropeItemId, true) != nullptr;
	const bool shovel = g_game.findItemOfType(&player, shovelToolItemId, true) != nullptr;
	const uint64_t funds = player.getMoney() + player.getBankBalance();
	const uint64_t money = playerBotHuntTransportFunds(funds, carriedGoldReserve);
	timing.transportSpendableFunds = money;
	const ActorFact actor{combat.level, combat.maximumHealth, combat.armor, combat.defense, combat.attack,
	    combat.attackSkill, combat.attackFactor, funds, player.isPremium(), player.isPzLocked(), rope, shovel,
	    player.getStepDuration(), player.getStepDuration(DIRECTION_NORTHEAST), player.getGroup() && player.getGroup()->access,
	    player.getGroup() ? player.getGroup()->flags : 0, player.getGuild() ? player.getGuild()->getId() : 0,
	    player.getGuildRank() ? player.getGuildRank()->id : 0, player.getPosition()};
	std::vector<OfferFact> offers;
	if (!player.isPzLocked()) {
		for (Npc* npc : playerBotNpcProviders(g_game.getNpcs(), PlayerBotNpcCapability::Travel, source)) {
			for (const auto& offer : npc->getTravelOffers()) {
				if (!playerBotNpcTravelOfferEligible(player.getLevel(), player.isPremium(), money, offer.level,
				    offer.premium, offer.price, offer.hasOpaqueCondition, offer.hasOpaqueAction) || offer.destination == source) continue;
				const auto unavailable = unavailableTravelOffers.find({npc->getID(), offer.destination});
				if (unavailable != unavailableTravelOffers.end() && unavailable->second > std::chrono::steady_clock::now()) continue;
				offers.emplace_back(npc->getID(), npc->getPosition(), offer.destination, offer.price, offer.level,
				                    offer.premium, offer.dialogue);
			}
		}
	}
	// Canonical order makes NPC provider iteration changes irrelevant.
	std::sort(offers.begin(), offers.end());
	const uint64_t generation = PlayerBotTopology::instance().generation();
	const uint64_t riskRevision = PlayerBotHuntRegionPlanner::getCacheRevision();
	const bool sameRequest = huntTravelWork && huntTravelWork->sameRequest(request, source);
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
		w.walking.metrics.attempted = true;
		w.walking.metrics.result = PlayerBotNavigationResult::Reached;
		w.walking.metrics.waypoint = request.to;
		++timing.attempts;
	}
	const auto work = huntTravelWork; // Keep the scoped tile observer alive through return.
	auto& w = *work;
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
				    result.metrics.dangerCost, result.metrics.maximumHealthLossPerSecond, true);
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
	uint64_t localBudget = localNodesPerTurn;
	auto connectionEvent = [&](const char* kind, PlayerBotNavigationResult result) {
		std::ostringstream fields;
		fields << "\"planning_pass\":" << w.pass << ",\"scoring_revision\":" << w.revision
		       << ",\"request_sequence\":" << w.sequence << ",\"kind\":\"" << kind << "\""
		       << ",\"from\":{\"x\":" << w.local->source.x << ",\"y\":" << w.local->source.y << ",\"z\":" << unsigned(w.local->source.z) << "}"
		       << ",\"to\":{\"x\":" << w.local->current.x << ",\"y\":" << w.local->current.y << ",\"z\":" << unsigned(w.local->current.z) << "}"
		       << ",\"result\":\"" << (result == PlayerBotNavigationResult::Reached ? "reached" : result == PlayerBotNavigationResult::NodeLimit ? "node_limit" : "unreachable") << "\""
		       << ",\"expanded_nodes\":" << w.local->expanded << ",\"cache_hits\":" << w.local->cacheHits
		       << ",\"incomplete_reused\":" << (w.local->incompleteReused ? "true" : "false")
		       << ",\"unrestricted_fallback\":" << (w.local->unrestricted ? "true" : "false")
		       << ",\"alternate_itineraries\":" << w.local->alternateItineraries
		       << ",\"corridor_searches\":" << w.local->corridorSearches
		       << ",\"corridor_radius\":" << w.local->corridorRadius
		       << ",\"heuristic_restarts\":" << w.local->heuristicRestarts
		       << ",\"elapsed_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(w.connectionTime).count();
		fields << ",\"fallback_reason\":\"" << w.local->fallbackReason << "\",\"requested_targets\":[";
		bool first = true;
		for (const auto& target : w.local->destinations()) {
			if (!first) fields << ',';
			first = false;
			fields << "{\"x\":" << target.x << ",\"y\":" << target.y << ",\"z\":" << unsigned(target.z) << '}';
		}
		fields << ']';
		emit("hunt_route_connection", player.getPosition(), fields.str());
		w.connectionTime = {};
	};
	if (!w.walkingDone) {
		if (!w.local) w.local = std::make_unique<PlayerBotHuntWalkSearch>(source, std::vector<Position>{w.destination}, playerBotNavigationMaximumExpandedNodes);
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
		w.walkingDone = true;
		timing.walkingReached += *result == PlayerBotNavigationResult::Reached;
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
		if (final) targets.push_back(w.destination);
		else {
			const auto provider = w.providerValidation.use(w.providers[connection]);
			for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) {
				if (x || y) targets.emplace_back(provider.x + x, provider.y + y, provider.z);
			}
		}
		if (!w.local) {
			if (!final) targets.erase(std::remove_if(targets.begin(), targets.end(), [&](Position target) {
				Tile* tile = g_game.map.getTile(target);
				return !tile || tile->queryAdd(0, player, 1, FLAG_IGNOREBLOCKCREATURE) != RETURNVALUE_NOERROR;
			}), targets.end());
			if (targets.empty()) return PlayerBotTransportSegment{};
			if (w.npcNodes >= playerBotNavigationMaximumExpandedNodes)
				return PlayerBotTransportSegment{PlayerBotNavigationResult::NodeLimit};
			w.local = std::make_unique<PlayerBotHuntWalkSearch>(from, targets,
			    std::min<uint64_t>(30000, playerBotNavigationMaximumExpandedNodes - w.npcNodes));
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
	PlayerBotNavigationRoutePlan result = std::move(w.walking);
	if (const auto& paid = w.transport->bestPaid; paid && (!safe(result) || paid->cost < cost(result))) {
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
		// Estimate-only marker. Movement callers still use planHuntTravelRoute and
		// its live, same-floor NPC approach validation before executing travel.
		const auto& offer = w.offers.at(paid->firstOffer);
		PlayerBotNavigationStep step;
		step.action = PlayerBotNavigationAction::NpcTravel;
		step.npcId = std::get<0>(offer); step.target = w.providerValidation.use(step.npcId); step.expectedPosition = std::get<2>(offer);
		step.price = std::get<3>(offer); step.minimumLevel = std::get<4>(offer); step.premium = std::get<5>(offer);
		step.dialogue = std::get<6>(offer);
		result.steps.push_back(std::move(step));
	}
	result.metrics.result = playerBotHuntAggregateRouteResult(result.metrics.result,
	    result.metrics.dangerCost, result.metrics.maximumHealthLossPerSecond, w.transport->incomplete);
	huntTravelWork.reset();
	return result;
}
