#ifndef FS_PLAYERBOTTRANSPORTSEARCH_H
#define FS_PLAYERBOTTRANSPORTSEARCH_H

#include "playerbotnavigation.h"

#include <limits>
#include <map>
#include <queue>
#include <utility>

struct PlayerBotTransportSegment {
	PlayerBotNavigationResult result = PlayerBotNavigationResult::NodeLimit;
	double seconds = 0;
	uint32_t steps = 0;
	uint32_t danger = 0;
	double peak = 0;
	bool boundRejected = false; // Not unreachable, not a node-limit result.
};

// Keep fractional time throughout the search: rounding each leg breaks
// additive bounds. Sell-loot callers round only the final economic score.
inline double playerBotTransportRouteCost(double seconds, uint64_t fare, uint32_t danger, bool sellEconomy = false)
{
	return (sellEconomy ? seconds / 6.0 + fare : seconds + fare / 10.0) + danger / 10.0;
}

// Bounds, labels and incumbents must use the same request objective.
// Geometry is NEVER a lower bound: a single portal can cross the whole map.
inline bool playerBotTransportBoundRejects(double lowerCost, uint64_t minimumFare,
                                          double incumbentCost, uint64_t incumbentFare, bool incumbentSafe)
{
	return incumbentSafe && incumbentFare <= minimumFare && incumbentCost <= lowerCost;
}

// A* movement units are 10/cardinal, 30/diagonal, 20/use. A diagonal
// move through an open shovel passage carries 30 units but is represented as
// a one-second use step. Danger is already in cost units. This deliberately
// weak conversion covers all mixtures; never apply it to geometric distance.
inline double playerBotLocalRouteCostLowerBound(uint32_t frontierCost, double cardinalMs, double diagonalMs,
                                               bool sellEconomy = false)
{
	const double timeScale = sellEconomy ? 1.0 / 6.0 : 1.0;
	return frontierCost * std::min({timeScale / 30.0, timeScale * cardinalMs / 10000.0, timeScale * diagonalMs / 30000.0});
}

// Pure lazy transport graph. IDs index copied request facts, never live NPCs.
// Walking connections are evaluated in goal-directed order, while only
// nonnegative validated costs and fares can reject one. Distinct fare/risk
// labels survive at the same destination.
class PlayerBotTransportSearch {
public:
	// rank is a goal-directed scheduling hint, never a cost or a bound.
	struct Offer { size_t connection; size_t destination; uint64_t fare; double rank = 0; };
	struct Label {
		double cost = 0;
		// Only validated detailed walking costs may extend this bound.
		double lowerCost = 0;
		uint64_t fare = 0;
		uint32_t danger = 0;
		double peak = 0;
		double seconds = 0;
		uint32_t steps = 0;
		size_t state = 0;
		size_t firstOffer = SIZE_MAX;
	};
	struct Counters {
		uint64_t coarseQueries = 0; // Retained for observational compatibility; always zero.
		uint64_t localQueries = 0, cacheHits = 0, unknown = 0, boundRejects = 0, riskRejects = 0, labels = 0;
	};

	PlayerBotTransportSearch(size_t states, std::vector<Offer> offers, uint64_t money,
	                         std::optional<Label> walking = std::nullopt, std::vector<double> stateRanks = {},
	                         std::vector<Position> statePositions = {}, std::vector<Position> connectionPositions = {},
	                         bool sellEconomy = false, PlayerBotNavigationRiskProfile riskProfile = {})
	    : offers(std::move(offers)), money(money), sellEconomy(sellEconomy), riskProfile(riskProfile), labels(states), incumbent(walking), stateRanks(std::move(stateRanks)),
	      statePositions(std::move(statePositions)), connectionPositions(std::move(connectionPositions))
	{
		labels[0].push_back({});
		queue.push({{}, SIZE_MAX, 0, 0});
	}

	// One graph operation, or one slice of a pending detailed search. The
	// callback retains its bool argument for callers; it is always true.
	template<class Query>
	bool advance(Query query)
	{
		if (!active) {
			if (queue.empty()) return true;
			active = queue.top(); queue.pop();
		}
		const Task task = *active;
		const auto& live = labels[task.label.state];
		if (std::none_of(live.begin(), live.end(), [&](const Label& l) { return equal(l, task.label); })) {
			active.reset();
			queryStarted = false;
			return false;
		}
		if (reject(task.bound, task.label.fare + (task.offer < offers.size() ? offers[task.offer].fare : 0))) {
			++counters.boundRejects;
			active.reset();
			return false;
		}
		if (task.offer == SIZE_MAX) {
			active.reset();
			++counters.labels;
			// Try connections toward the goal before speculative long final walks.
			// Neither rank nor geometry can reject a task or certify a route.
			if (task.label.state != 0) queue.push({task.label, offers.size(), task.label.lowerCost,
			    task.label.lowerCost + 2 * stateRank(task.label.state)});
			for (size_t i = 0; i < offers.size(); ++i) {
				if (offers[i].fare > money - task.label.fare) continue;
				const double bound = task.label.lowerCost + playerBotTransportRouteCost(1.0, offers[i].fare, 0, sellEconomy);
				queue.push({task.label, i, bound, bound + offers[i].rank + approachRank(task.label.state, offers[i].connection)});
			}
			return false;
		}
		const bool final = task.offer == offers.size();
		const size_t connection = final ? SIZE_MAX : offers[task.offer].connection;
		const auto key = std::make_pair(task.label.state, connection);
		PlayerBotTransportSegment segment;
		if (auto found = connections.find(key); found != connections.end()) {
			segment = found->second;
			++counters.cacheHits;
		} else {
			if (!queryStarted) {
				++counters.localQueries;
				queryStarted = true;
			}
			const auto result = query(task.label.state, connection, true);
			if (!result) return false;
			segment = *result;
			connections.emplace(key, segment);
			if (segment.boundRejected) ++counters.boundRejects;
			else if (segment.result == PlayerBotNavigationResult::NodeLimit) {
				++counters.unknown;
				incomplete = true;
			}
		}
		queryStarted = false;
		active.reset();
		if (segment.result != PlayerBotNavigationResult::Reached) return false;
		Label next = task.label;
		next.lowerCost += playerBotTransportRouteCost(segment.seconds, 0, segment.danger, sellEconomy);
		next.seconds += segment.seconds;
		next.steps += segment.steps;
		next.danger = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(next.danger) + segment.danger));
		next.peak = std::max(next.peak, segment.peak);
		if (!final) {
			next.fare += offers[task.offer].fare;
			next.lowerCost += playerBotTransportRouteCost(1.0, offers[task.offer].fare, 0, sellEconomy);
			next.seconds += 1.0;
			++next.steps;
			next.state = offers[task.offer].destination;
			if (next.firstOffer == SIZE_MAX) next.firstOffer = task.offer;
		}
		next.cost = playerBotTransportRouteCost(next.seconds, next.fare, next.danger, sellEconomy);
		// Both accumulated danger and peak danger are monotone. No extension
		// of this prefix can satisfy the same route safety limits.
		if (!safe(next)) {
			++counters.riskRejects;
			return false;
		}
		if (final) {
			if (safe(next)) {
				// Keep cheap-fare alternatives even when a faster expensive route exists.
				if (!bestPaid || next.cost < bestPaid->cost) bestPaid = next;
				const auto covers = [](const Label& a, const Label& b) { return a.cost <= b.cost && a.fare <= b.fare; };
				if (std::none_of(paidIncumbents.begin(), paidIncumbents.end(), [&](const Label& l) { return covers(l, next); })) {
					paidIncumbents.erase(std::remove_if(paidIncumbents.begin(), paidIncumbents.end(),
					    [&](const Label& l) { return covers(next, l); }), paidIncumbents.end());
					paidIncumbents.push_back(next);
				}
			}
			return false;
		}
		auto& at = labels[next.state];
		if (std::any_of(at.begin(), at.end(), [&](const Label& l) { return dominates(l, next); })) return false;
		at.erase(std::remove_if(at.begin(), at.end(), [&](const Label& l) { return dominates(next, l); }), at.end());
		at.push_back(next);
		queue.push({next, SIZE_MAX, next.lowerCost, next.lowerCost + stateRank(next.state)});
		return false;
	}

	std::optional<Label> bestPaid;
	bool incomplete = false;
	Counters counters;

private:
	struct Task {
		Label label;
		size_t offer;
		double bound;
		double priority;
		bool operator>(const Task& other) const { return priority > other.priority; }
	};
	bool safe(const Label& l) const { return playerBotNavigationRiskAccepts(riskProfile, l.danger, l.peak); }
	static bool dominates(const Label& a, const Label& b) {
		return a.cost <= b.cost && a.lowerCost <= b.lowerCost && a.fare <= b.fare && a.danger <= b.danger && a.peak <= b.peak;
	}
	static bool equal(const Label& a, const Label& b) {
		return a.cost == b.cost && a.lowerCost == b.lowerCost && a.fare == b.fare && a.danger == b.danger && a.peak == b.peak;
	}
	double stateRank(size_t state) const { return state < stateRanks.size() ? stateRanks[state] : 0; }
	double approachRank(size_t state, size_t connection) const {
		if (state >= statePositions.size() || connection >= connectionPositions.size()) return 0;
		const auto& from = statePositions[state];
		const auto& to = connectionPositions[connection];
		return std::max(Position::getDistanceX(from, to), Position::getDistanceY(from, to));
	}
	bool reject(double bound, uint64_t fare) const {
		if (incumbent && playerBotTransportBoundRejects(bound, fare, incumbent->cost, incumbent->fare, safe(*incumbent))) return true;
		return std::any_of(paidIncumbents.begin(), paidIncumbents.end(), [&](const Label& paid) {
			return playerBotTransportBoundRejects(bound, fare, paid.cost, paid.fare, safe(paid));
		});
	}
	std::vector<Offer> offers;
	uint64_t money;
	bool sellEconomy;
	PlayerBotNavigationRiskProfile riskProfile;
	std::vector<std::vector<Label>> labels;
	std::optional<Label> incumbent;
	std::vector<double> stateRanks;
	std::vector<Position> statePositions, connectionPositions;
	std::vector<Label> paidIncumbents;
	std::priority_queue<Task, std::vector<Task>, std::greater<Task>> queue;
	std::optional<Task> active;
	bool queryStarted = false;
	// Exactly one immutable request context; unknown stays unknown. Destroy on
	// cancellation/context change. No cross-pass negative reachability cache.
	std::map<std::pair<size_t, size_t>, PlayerBotTransportSegment> connections;
};

#endif
