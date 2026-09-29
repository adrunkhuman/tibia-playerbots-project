#ifndef FS_PLAYERBOTHUNTWALK_H
#define FS_PLAYERBOTHUNTWALK_H

#include "player.h"
#include "playerbotnavigation.h"
#include "playerbothuntcoarsepolicy.h"
#include "playerbotpathsearch.h"
#include "playerbottopology.h"
#include "playerbotroutecache.h"
#include "playerbotroutecorridor.h"
#include "playerbothunttiming.h"

#include <iomanip>
#include <sstream>

// One connection, not one bot. The coarse itinerary is retained while its local
// segments yield. On failure, try a different coarse itinerary, then search
// progressively wider connected-piece corridors before the whole live graph.
// Restricted failures never establish unreachability.
class PlayerBotHuntWalkSearch {
public:
	PlayerBotHuntWalkSearch(Position from, std::vector<Position> goals, uint64_t allowance)
	    : source(from), current(from), goals(std::move(goals)), allowance(allowance) {}

	std::optional<PlayerBotNavigationResult> advance(Player& player, const PlayerBotNavigationCostPolicy& policy,
	    bool rope, bool shovel, const std::string& profile, uint64_t riskRevision,
	    PlayerBotHuntRouteTiming& timing, uint64_t& budget, uint64_t planningPass)
	{
		if (result) return result;
		const auto& topology = PlayerBotTopology::instance();
		if (!initialized) {
			initialized = true;
			std::ostringstream identity;
			identity << profile << ':' << player.getID() << ':' << player.getGUID() << ':' << player.getPosition() << ':' << planningPass
			         << ':' << std::setprecision(17) << policy.risk.healthLossCost << ':'
			         << policy.risk.maximumRouteHealthLoss << ':' << policy.risk.maximumHealthLossPerSecond;
			treeKey = key(source, {}, identity.str(), topology.generation(), riskRevision);
			const auto geometryKey = key(source, {}, profile, topology.generation(), 0);
			std::shared_ptr<const PlayerBotTopologyDistances> distances;
			if (const auto* cached = geometry().lookup(geometryKey)) {
				distances = *cached;
				++timing.topologyCacheHits;
			} else {
				PlayerBotRouteChanges::Watch geometryWatch;
				{
					PlayerBotRouteChanges::Scope geometryDependencies(geometryWatch);
					++timing.topologyQueries;
					distances = std::make_shared<PlayerBotTopologyDistances>(topology.distancesFrom(
					    source, rope, shovel, player.getLevel(), &timing.topologyExpandedNodes));
				}
				geometry().insert(geometryKey, distances, std::move(geometryWatch),
				    topology.nodeCount() * sizeof(uint32_t) + geometryKey.size() * 2);
			}
			const auto coarse = playerBotHuntCoarseVerdict(source, goals,
			    [&](Position position) { return topology.walkNode(position).has_value(); },
			    [&](Position position) { return topology.distanceTo(*distances, position).has_value(); });
			if (coarse == PlayerBotHuntCoarseVerdict::Disconnected) {
				// The loaded topology models supported movement and tools. Map edits
				// that change connectivity require a topology rebuild (/reload).
				fallbackReason = "coarse_unreachable";
				++timing.coarseRejects;
				return result = PlayerBotNavigationResult::Unreachable;
			}
			if (coarse == PlayerBotHuntCoarseVerdict::Reachable) {
				++timing.topologyQueries;
				itinerary = topology.routeToAny(source, goals, {}, rope, shovel, player.getLevel(), &policy,
				                               &timing.topologyExpandedNodes);
			}
			// Off-graph endpoints and restricted itinerary failures retain live
			// validation; only a complete graph rejection is definitive.
			if (!itinerary) fallback(timing, "coarse_unavailable");
		}
		if (refining) return advanceRefinement(player, policy, rope, shovel, timing, budget);
		const PlayerBotNavigator navigator;
		if (unrestricted) return advanceTree(player, policy, timing, budget);
		while (budget && !result) {
			const bool crossing = portalIndex < itinerary->portals.size();
			activePortal = portalIndex;
			const auto segmentGoals = crossing ? std::vector<Position>{itinerary->portals[portalIndex].approach} : goals;
			if (!local) {
				if (expanded >= allowance) return result = PlayerBotNavigationResult::NodeLimit;
				segmentWatch = {};
				segmentKey = key(current, segmentGoals, profile, topology.generation(), riskRevision);
				bool reused = false;
				if (!unrestricted) {
					if (const auto* cached = cache().lookup(segmentKey)) {
						Position cursor = current;
						reused = true;
						for (const auto& step : *cached) {
							PlayerBotNavigationStep live;
							if (!navigator.resolveMove(player, cursor, step.direction, {}, live) ||
							    live.expectedPosition != step.expectedPosition || !PlayerBotPathSearch::ordinaryArc(cursor, live)) {
								reused = false; break;
							}
							cursor = step.expectedPosition;
						}
						if (reused) {
							append(player, policy, *cached);
							++timing.sharedCacheHits;
							++cacheHits;
						}
					}
					if (!reused) ++timing.sharedCacheMisses;
				}
				if (!reused) {
					// Reserve work for the unrestricted alternative if a preferred
					// itinerary proves difficult. Both modes share the original total.
					const uint64_t remaining = allowance - expanded;
					const uint64_t localAllowance = unrestricted ? remaining : std::min<uint64_t>(15000, std::max<uint64_t>(1, remaining / 4));
					local = std::make_unique<PlayerBotPathSearch>(current, PlayerBotNavigationGoal::anyOf(segmentGoals), localAllowance, !unrestricted);
					local->maximumPeakDanger = policy.risk.maximumHealthLossPerSecond;
					++timing.localSearches;
					if (!unrestricted) ++timing.ordinarySearches;
				}
			}
			if (local) {
				PlayerBotRouteChanges::Scope segmentDependencies(segmentWatch);
				const uint64_t before = local->expanded;
				const auto state = navigator.advance(player, *local, {}, budget, &policy);
				const uint64_t used = local->expanded - before;
				expanded += used;
				timing.localExpandedNodes += used;
				budget = 0;
				if (!state) return std::nullopt;
				if (*state != PlayerBotNavigationResult::Reached) {
					if (*state == PlayerBotNavigationResult::NodeLimit) ++timing.nodeLimits;
					fallback(timing, *state == PlayerBotNavigationResult::NodeLimit ? "segment_node_limit" : "segment_unreachable");
					return std::nullopt;
				}
				if (!unrestricted && std::all_of(local->steps.begin(), local->steps.end(), [](const auto& step) {
					return step.action == PlayerBotNavigationAction::Move;
				})) {
					cache().insert(segmentKey, local->steps, segmentWatch,
					    segmentKey.size() * 2 + local->steps.size() * sizeof(PlayerBotNavigationStep) + 1024);
				}
				append(player, policy, local->steps);
				local.reset();
			}
			if (crossing) {
				const auto& portal = itinerary->portals[portalIndex++];
				PlayerBotNavigationStep step;
				switch (portal.action) {
					case PlayerBotTopologyPortalAction::Move: step.action = PlayerBotNavigationAction::Move; break;
					case PlayerBotTopologyPortalAction::Use: step.action = PlayerBotNavigationAction::Use; break;
					case PlayerBotTopologyPortalAction::UseDoor: step.action = PlayerBotNavigationAction::UseDoor; break;
					case PlayerBotTopologyPortalAction::UseRope: step.action = PlayerBotNavigationAction::UseRope; break;
					case PlayerBotTopologyPortalAction::UseShovel: step.action = PlayerBotNavigationAction::UseShovel; break;
				}
				step.target = portal.target; step.expectedPosition = portal.destination;
				step.direction = portal.direction; step.itemId = portal.itemId; step.expectedItemId = portal.expectedItemId;
				step.topologyPortal = true;
				if (!navigator.validateStep(player, current, step, {})) {
					fallback(timing, "crossing_changed");
					return std::nullopt;
				}
				append(player, policy, {step});
			}
			const bool reached = std::find(goals.begin(), goals.end(), current) != goals.end();
			if (!playerBotNavigationRiskAccepts(policy.risk, summary.dangerCost, summary.maximumHealthLossPerSecond) && !unrestricted) {
				fallback(timing, "unsafe_prefix");
			} else if (reached) {
				return result = PlayerBotNavigationResult::Reached;
			}
			// Cached/zero-length pieces must also yield: no unbounded chain of
			// crossings can occupy the dispatcher without a node expansion.
			return std::nullopt;
		}
		return result;
	}

	const std::vector<Position>& destinations() const { return goals; }
	const char* fallbackReason = "none";
	Position source, current;
	uint64_t expanded = 0, cacheHits = 0;
	std::deque<PlayerBotNavigationStep> steps;
	PlayerBotNavigationCostSummary summary;
	double seconds = 0;
	bool unrestricted = false, incompleteReused = false;
	uint32_t alternateItineraries = 0, corridorSearches = 0, corridorRadius = 0, heuristicRestarts = 0;

private:
	static PlayerBotRouteCache<std::string, std::shared_ptr<const PlayerBotTopologyDistances>>& geometry() {
		static PlayerBotRouteCache<std::string, std::shared_ptr<const PlayerBotTopologyDistances>> shared(16 * 1024 * 1024, 64);
		return shared;
	}
	struct SourceTree {
		PlayerBotPathSearch search;
		PlayerBotRouteChanges::Watch watch;
		PlayerBotTopologyHeuristic heuristic;
		SourceTree(Position source, uint64_t allowance, double peak, PlayerBotTopologyHeuristic heuristic)
		    : search(source, PlayerBotNavigationGoal::anyOf({}), allowance, false, true), heuristic(std::move(heuristic)) {
			search.maximumPeakDanger = peak;
		}
	};
	static PlayerBotRouteCache<std::string, std::shared_ptr<SourceTree>>& trees() {
		static PlayerBotRouteCache<std::string, std::shared_ptr<SourceTree>> shared(256 * 1024 * 1024, 8);
		return shared;
	}
	void ensureHeuristic(PlayerBotHuntRouteTiming& timing) {
		if (heuristic && heuristic->valid()) return;
		if (heuristic) {
			// A new shortcut can lower priorities even outside the tiles already
			// explored. Never resume old A* priorities on that changed graph.
			++heuristicRestarts; ++timing.heuristicRestarts;
			local.reset(); tree.reset();
		}
		heuristic = PlayerBotTopology::instance().heuristicTo(goals);
		++timing.heuristicBuilds;
	}
	std::optional<PlayerBotNavigationResult> advanceRefinement(Player& player,
	    const PlayerBotNavigationCostPolicy& policy, bool rope, bool shovel,
	    PlayerBotHuntRouteTiming& timing, uint64_t& budget) {
		const auto& topology = PlayerBotTopology::instance();
		if (expanded >= allowance) return result = PlayerBotNavigationResult::NodeLimit;
		if (!alternativeTried) {
			alternativeTried = true;
			if (itinerary && !itinerary->portals.empty()) {
				// Exclude a coarse arc only to generate another preference. Its
				// crossings remain eligible in corridor and unrestricted searches.
				const size_t arc = std::min(activePortal, itinerary->portals.size() - 1);
				const std::set<std::pair<uint32_t, uint32_t>> excluded{
				    {itinerary->nodes[arc], itinerary->nodes[arc + 1]}};
				++timing.topologyQueries;
				auto alternate = topology.routeToAny(source, goals, {}, rope, shovel, player.getLevel(),
				    &policy, &timing.topologyExpandedNodes, &excluded);
				if (alternate) {
					corridor->addSeeds(alternate->nodes);
					itinerary = std::move(alternate);
					portalIndex = 0; refining = false;
					++alternateItineraries; ++timing.alternateItineraries;
					return std::nullopt;
				}
			}
		}
		if (!corridor || corridorStage >= corridorRadii.size()) {
			refining = false; unrestricted = true;
			return advanceTree(player, policy, timing, budget);
		}
		ensureHeuristic(timing);
		if (!local) {
			corridorRadius = corridorRadii[corridorStage];
			corridor->widen(corridorRadius, [&](uint32_t node) -> const auto& { return topology.outgoing(node); });
			// Each restricted attempt uses only a fraction of the remaining
			// allowance. The whole live graph always gets the majority left.
			const uint64_t remaining = allowance - expanded;
			const uint64_t limit = std::min<uint64_t>(8000, std::max<uint64_t>(1, remaining / 8));
			local = std::make_unique<PlayerBotPathSearch>(source, PlayerBotNavigationGoal::anyOf(goals), limit);
			local->unbounded = true;
			local->maximumPeakDanger = policy.risk.maximumHealthLossPerSecond;
			++corridorSearches; ++timing.corridorSearches; ++timing.localSearches;
			if (corridorStage) ++timing.corridorWidenings;
		}
		const auto before = local->expanded;
		const auto state = PlayerBotNavigator{}.advance(player, *local, {}, budget, &policy, false, &*heuristic, &*corridor);
		const auto used = local->expanded - before;
		expanded += used; timing.localExpandedNodes += used; budget = 0;
		if (!state) return std::nullopt;
		if (*state == PlayerBotNavigationResult::Reached && playerBotNavigationRiskAccepts(
		        policy.risk, local->summary.dangerCost, local->summary.maximumHealthLossPerSecond)) {
			append(player, policy, local->steps);
			return result = *state;
		}
		local.reset(); ++corridorStage;
		return std::nullopt;
	}
	std::optional<PlayerBotNavigationResult> advanceTree(Player& player, const PlayerBotNavigationCostPolicy& policy,
	    PlayerBotHuntRouteTiming& timing, uint64_t& budget) {
		if (!tree) {
			// A connection does not refill a shared source's expansion allowance.
			// If no new work is allowed and no target is settled, retain unknown
			// evidence cheaply without rebuilding a potential or queue priorities.
			if (const auto* cached = trees().lookup(treeKey); cached && (*cached)->heuristic.valid() &&
			    ((*cached)->search.result == PlayerBotNavigationResult::Unreachable ||
			     (*cached)->search.expanded >= allowance - expanded)) {
				std::deque<PlayerBotNavigationStep> path;
				PlayerBotNavigationCostSummary cost;
				if (!(*cached)->search.extract(goals, path, cost)) {
					++timing.sourceTreeHits;
					if ((*cached)->search.result == PlayerBotNavigationResult::Unreachable)
						return result = PlayerBotNavigationResult::Unreachable;
					incompleteReused = true;
					++timing.incompleteCacheHits;
					return result = PlayerBotNavigationResult::NodeLimit;
				}
			}
		}
		ensureHeuristic(timing);
		if (!tree) {
			if (auto cached = trees().take(treeKey); cached && (*cached)->heuristic.valid()) {
				tree = std::move(*cached);
				++timing.sourceTreeHits;
			} else {
				tree = std::make_shared<SourceTree>(source, allowance - expanded,
				    policy.risk.maximumHealthLossPerSecond, *heuristic);
				++timing.localSearches;
			}
			// Retarget settled evidence with a consistent potential; unlike a
			// source flood this frontier is pulled toward the requested targets.
			tree->heuristic = *heuristic;
			tree->search.retarget(PlayerBotNavigationGoal::anyOf(goals),
			    [&](Position p) { return heuristic->estimate(p); });
			tree->search.allowance = std::max(tree->search.expanded, allowance - expanded);
			++timing.guidedSearches;
		}
		PlayerBotRouteChanges::Scope treeDependencies(tree->watch);
		const auto before = tree->search.expanded;
		const auto state = PlayerBotNavigator{}.advance(player, tree->search, {}, budget, &policy, false, &*heuristic);
		const auto used = tree->search.expanded - before;
		expanded += used; timing.localExpandedNodes += used; budget = 0;
		if (!state) return std::nullopt;
		if (*state == PlayerBotNavigationResult::Reached) {
			std::deque<PlayerBotNavigationStep> path;
			PlayerBotNavigationCostSummary cost;
			tree->search.extract(goals, path, cost);
			append(player, policy, path);
		} else if (*state == PlayerBotNavigationResult::NodeLimit) ++timing.nodeLimits;
		result = *state;
		// Cached settled labels remain final under any consistent target
		// potential. Charge the maximum potential grid along with the tree.
		trees().insert(treeKey, tree, tree->watch, tree->search.memoryBytes() +
		    PlayerBotShortcutHeuristic::maximumCells * sizeof(uint32_t) + treeKey.size() * 2);
		return result;
	}
	static PlayerBotRouteCache<std::string, std::deque<PlayerBotNavigationStep>>& cache() {
		static PlayerBotRouteCache<std::string, std::deque<PlayerBotNavigationStep>> shared;
		return shared;
	}
	static std::string key(Position from, const std::vector<Position>& goals, const std::string& profile,
	                       uint64_t generation, uint64_t riskRevision) {
		std::ostringstream out;
		out << profile << ':' << generation << ':' << riskRevision << ':' << from;
		for (const auto& goal : goals) out << ':' << goal;
		return out.str();
	}
	void fallback(PlayerBotHuntRouteTiming& timing, const char* reason) {
		fallbackReason = reason;
		refining = true;
		++timing.hierarchyFallbacks;
		if (!corridor && itinerary) corridor.emplace(PlayerBotTopology::instance().nodeCount(), itinerary->nodes);
		local.reset(); current = source; steps.clear(); summary = {}; seconds = 0;
	}
	void append(Player& player, const PlayerBotNavigationCostPolicy& policy,
	            const std::deque<PlayerBotNavigationStep>& additions) {
		for (const auto& step : additions) {
			const bool move = step.action == PlayerBotNavigationAction::Move;
			const uint32_t exposure = move ? player.getStepDuration(step.direction) : 1000;
			seconds += exposure / 1000.0;
			summary.movementCost = add(summary.movementCost, move ? ((step.direction & DIRECTION_DIAGONAL_MASK) ? 30 : 10) : 20);
			summary.dangerCost = add(summary.dangerCost, policy.dangerCost(step.expectedPosition, exposure));
			summary.maximumHealthLossPerSecond = std::max(summary.maximumHealthLossPerSecond, policy.dangerAt(step.expectedPosition));
			steps.push_back(step); current = step.expectedPosition;
		}
	}
	static uint32_t add(uint32_t a, uint32_t b) { return static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(a) + b)); }
	std::vector<Position> goals;
	uint64_t allowance;
	bool initialized = false;
	size_t portalIndex = 0, activePortal = 0;
	bool refining = false, alternativeTried = false;
	inline static constexpr std::array<uint32_t, 3> corridorRadii{0, 1, 3};
	size_t corridorStage = 0;
	std::optional<PlayerBotRouteCorridor> corridor;
	std::optional<PlayerBotTopologyHeuristic> heuristic;
	std::optional<PlayerBotTopologyItinerary> itinerary;
	std::unique_ptr<PlayerBotPathSearch> local;
	PlayerBotRouteChanges::Watch segmentWatch;
	std::string segmentKey, treeKey;
	std::shared_ptr<SourceTree> tree;
	std::optional<PlayerBotNavigationResult> result;
};

#endif
