#ifndef FS_PLAYERBOTPATHSEARCH_H
#define FS_PLAYERBOTPATHSEARCH_H

#include "playerbotnavigation.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

// A dispatcher-owned frontier. Callbacks are borrowed for one advance only;
// no Player, Tile, or cost-policy closure survives a yield.
class PlayerBotPathSearch {
public:
	struct Arc {
		PlayerBotNavigationStep step;
		uint32_t movement = 0;
		uint32_t danger = 0;
		double peak = 0;
	};
	PlayerBotPathSearch(Position start, PlayerBotNavigationGoal goal, uint64_t allowance, bool ordinaryOnly = false,
	                    bool sourceTree = false)
	    : start(start), goal(std::move(goal)), allowance(allowance), closest(start), ordinaryOnly(ordinaryOnly), sourceTree(sourceTree)
	{
		costs[key(start)] = 0;
		open.push({0, 0, start});
	}

	// Zero-risk preparation excludes unsafe arcs during search, not after a
	// cheaper dangerous path has displaced a longer safe label. Other total
	// risk tolerances retain the existing post-search acceptance policy.
	void constrain(const PlayerBotNavigationRiskProfile& risk) {
		if (risk.requiresZeroRisk()) {
			maximumPeakDanger = 0;
			zeroDangerOnly = true;
		}
	}

	// Call with the same positional peak samples used by incoming arcs, before
	// expanding. This is independent of graph size and preserves unknown limits.
	template<class Sample>
	bool rejectUnsafeEndpoints(Sample sample) {
		if (result) return *result == PlayerBotNavigationResult::RiskRejected;
		const auto peak = playerBotNavigationRejectedEndpointPeak(start, goal, maximumPeakDanger, sample);
		if (!peak) return false;
		summary.maximumHealthLossPerSecond = *peak;
		result = PlayerBotNavigationResult::RiskRejected;
		return true;
	}

	// Only adjacent, same-floor moves and door uses may use a geometric
	// lower bound. In particular, a walk into a redirect is not ordinary.
	static bool ordinaryArc(const Position& from, const PlayerBotNavigationStep& step)
	{
		if (step.target.z != from.z || step.expectedPosition != step.target) return false;
		if (step.action == PlayerBotNavigationAction::Move) {
			const int dx = int(step.target.x) - from.x, dy = int(step.target.y) - from.y;
			switch (step.direction) {
				case DIRECTION_NORTH: return dx == 0 && dy == -1;
				case DIRECTION_EAST: return dx == 1 && dy == 0;
				case DIRECTION_SOUTH: return dx == 0 && dy == 1;
				case DIRECTION_WEST: return dx == -1 && dy == 0;
				case DIRECTION_NORTHEAST: return dx == 1 && dy == -1;
				case DIRECTION_NORTHWEST: return dx == -1 && dy == -1;
				case DIRECTION_SOUTHEAST: return dx == 1 && dy == 1;
				case DIRECTION_SOUTHWEST: return dx == -1 && dy == 1;
				default: return false;
			}
		}
		if (step.action != PlayerBotNavigationAction::UseDoor) return false;
		const auto dx = Position::getDistanceX(from, step.target);
		const auto dy = Position::getDistanceY(from, step.target);
		return dx <= 1 && dy <= 1 && (dx != 0 || dy != 0);
	}

	// nullopt means pending, not NodeLimit. The total allowance is independent
	// of slice size. Count queue pops too, so stale labels cannot monopolize a turn.
	template<class Expand, class Reached, class Distance>
	std::optional<PlayerBotNavigationResult> advance(uint64_t slice, Expand expand, Reached reached, Distance distance,
	                                                 uint32_t minimumStepCost = 0)
	{
		return advance(slice, expand, reached, distance, minimumStepCost,
		               [&](const Position& p) { return sourceTree ? 0u : (reached(p) ? 0u : minimumStepCost); });
	}

	// A goal search needs an admissible heuristic; a reusable source tree needs
	// a consistent one so every settled label remains final after retargeting.
	template<class Expand, class Reached, class Distance, class Heuristic>
	std::optional<PlayerBotNavigationResult> advance(uint64_t slice, Expand expand, Reached reached, Distance distance,
	                                                 uint32_t /*minimumStepCost*/, Heuristic heuristic)
	{
		if (result) return result;
		if (sourceTree && targetSettled()) return result = PlayerBotNavigationResult::Reached;
		while (slice-- && !open.empty() && expanded < allowance) {
			const auto current = open.top();
			open.pop();
			if (costs.at(key(current.position)) != current.cost) continue;
			++expanded;
			if (sourceTree) {
				settled.insert(key(current.position));
			} else {
				if (distance(current.position) < distance(closest)) closest = current.position;
				if (reached(current.position)) {
					reconstruct(current.position, steps, summary);
					return result = PlayerBotNavigationResult::Reached;
				}
			}
			for (const auto& arc : expand(current.position)) {
				if ((ordinaryOnly && !ordinaryArc(current.position, arc.step)) ||
				    (zeroDangerOnly && arc.danger != 0) || !playerBotNavigationPeakAccepts(maximumPeakDanger, arc.peak)) continue;
				const Position to = arc.step.expectedPosition;
				const uint32_t cost = add(current.cost, add(arc.movement, arc.danger));
				const auto found = costs.find(key(to));
				if (found != costs.end() && found->second <= cost) continue;
				costs[key(to)] = cost;
				parents[key(to)] = {current.position, arc};
				const uint32_t remaining = sourceTree && goal.positions.empty() ? 0u : heuristic(to);
				open.push({add(cost, remaining), cost, to});
			}
			// Expand the goal's outgoing arcs before stopping: a later target
			// must be able to continue through this node without re-expanding it.
			if (sourceTree && targetSettled()) return result = PlayerBotNavigationResult::Reached;
		}
		if (open.empty()) result = PlayerBotNavigationResult::Unreachable;
		else if (expanded >= allowance) result = PlayerBotNavigationResult::NodeLimit;
		return result;
	}

	// Change only queue priorities, not accumulated path evidence. A consistent
	// potential makes settled costs final even when the target changes. Check
	// the frontier bound before accepting a previously settled any-of target.
	template<class Heuristic>
	void retarget(PlayerBotNavigationGoal nextGoal, Heuristic heuristic) {
		goal = std::move(nextGoal);
		auto& nodes = open.entries();
		nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& node) {
			return costs.at(key(node.position)) != node.cost;
		}), nodes.end());
		for (auto& node : nodes) node.estimate = add(node.cost, heuristic(node.position));
		open.rebuild();
		result.reset();
		summary = {};
	}

	// Queries do not consume the frontier. Only settled goals have final
	// costs; a merely discovered goal may still get cheaper later.
	// A source goal is valid even before the first advance (or with allowance 0).
	bool extract(const std::vector<Position>& goals, std::deque<PlayerBotNavigationStep>& outSteps,
	             PlayerBotNavigationCostSummary& outSummary, Position* reachedPosition = nullptr) const
	{
		outSteps.clear();
		outSummary = {};
		if (!sourceTree) return false;
		Position best;
		uint32_t bestCost = UINT32_MAX;
		bool found = false;
		for (const Position& candidate : goals) {
			const uint64_t candidateKey = key(candidate);
			if (candidate != start && !settled.count(candidateKey)) continue;
			const auto label = costs.find(candidateKey);
			if (label != costs.end() && (!found || label->second < bestCost)) {
				best = candidate;
				bestCost = label->second;
				found = true;
			}
		}
		if (!found) return false;
		reconstruct(best, outSteps, outSummary);
		if (reachedPosition) *reachedPosition = best;
		return true;
	}

	// Conservative node/bucket estimate for sizing a shared request-local cache.
	size_t memoryBytes() const
	{
		return sizeof(*this) +
		    costs.bucket_count() * sizeof(void*) + costs.size() * (sizeof(decltype(costs)::value_type) + 3 * sizeof(void*)) +
		    parents.bucket_count() * sizeof(void*) + parents.size() * (sizeof(decltype(parents)::value_type) + 3 * sizeof(void*) + 256) +
		    settled.bucket_count() * sizeof(void*) + settled.size() * (sizeof(uint64_t) + 3 * sizeof(void*)) +
		    open.capacityBytes() * 2 + goal.positions.capacity() * sizeof(Position) +
		    steps.size() * sizeof(PlayerBotNavigationStep) * 2;
	}

	uint32_t frontierLowerCost(uint32_t minimumStepCost = 0) const {
		if (open.empty()) return UINT32_MAX;
		return open.top().estimate > minimumStepCost ? open.top().estimate - minimumStepCost : 0;
	}

	Position start;
	PlayerBotNavigationGoal goal;
	uint64_t allowance;
	uint64_t expanded = 0;
	Position closest;
	std::deque<PlayerBotNavigationStep> steps;
	PlayerBotNavigationCostSummary summary;
	std::optional<PlayerBotNavigationResult> result;
	bool ordinaryOnly = false;
	// Set before the first advance. An empty any-of goal floods all reachable
	// nodes; a nonempty goal stops once its cheapest settled target is proven.
	bool sourceTree = false;
	// Hunt searches use the whole live graph, including shortcuts that leave
	// the legacy rectangular bounds. Generic movement keeps those bounds.
	bool unbounded = false;
	// Hunt-only necessary safety condition; generic navigation is unrestricted.
	double maximumPeakDanger = std::numeric_limits<double>::infinity();
	bool zeroDangerOnly = false;

private:
	struct Node {
		uint32_t estimate;
		uint32_t cost;
		Position position;
		bool operator>(const Node& other) const {
			return estimate != other.estimate ? estimate > other.estimate : cost < other.cost;
		}
	};
	struct Parent { Position from; Arc arc; };
	struct Frontier : std::priority_queue<Node, std::vector<Node>, std::greater<Node>> {
		size_t capacityBytes() const { return this->c.capacity() * sizeof(Node); }
		std::vector<Node>& entries() { return this->c; }
		void rebuild() { std::make_heap(this->c.begin(), this->c.end(), this->comp); }
	};
	bool targetSettled() const {
		if (goal.type != PlayerBotNavigationGoalType::AnyOf || goal.positions.empty()) return false;
		for (Position candidate : goal.positions) {
			const auto label = costs.find(key(candidate));
			if ((candidate == start || settled.count(key(candidate))) && label != costs.end() &&
			    (open.empty() || label->second <= open.top().estimate)) return true;
		}
		return false;
	}
	void reconstruct(Position goalPosition, std::deque<PlayerBotNavigationStep>& outSteps,
	                 PlayerBotNavigationCostSummary& outSummary) const
	{
		Position cursor = goalPosition;
		while (cursor != start) {
			const auto& parent = parents.at(key(cursor));
			outSteps.push_front(parent.arc.step);
			outSummary.movementCost = add(outSummary.movementCost, parent.arc.movement);
			outSummary.dangerCost = add(outSummary.dangerCost, parent.arc.danger);
			outSummary.maximumHealthLossPerSecond = std::max(outSummary.maximumHealthLossPerSecond, parent.arc.peak);
			cursor = parent.from;
		}
	}
	static uint64_t key(Position p) { return (uint64_t(p.z) << 32) | (uint64_t(p.x) << 16) | p.y; }
	static uint32_t add(uint32_t a, uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }
	Frontier open;
	std::unordered_map<uint64_t, uint32_t> costs;
	std::unordered_map<uint64_t, Parent> parents;
	std::unordered_set<uint64_t> settled;
};

#endif
