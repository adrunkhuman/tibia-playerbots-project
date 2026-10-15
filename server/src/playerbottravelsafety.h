#ifndef FS_PLAYERBOTTRAVELSAFETY_H
#define FS_PLAYERBOTTRAVELSAFETY_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "position.h"
#include "playerbotnavigation.h"

// Per-bot evidence, owned outside a controller so recovery need not forget it.
// Only caller-supplied observations change knowledge; there is no world access.
class PlayerBotTravelSafetyMemory {
public:
	using Clock = std::chrono::steady_clock;
	static constexpr size_t MaxThreats = 64;
	static constexpr size_t MaxClearTiles = 512;
	static constexpr std::chrono::seconds ThreatLifetime{120};
	static constexpr std::chrono::seconds ClearLifetime{5};

	struct Threat {
		uint32_t id;
		Position position;
		std::string species;
		Clock::time_point seen;
	};

	// Planning-visible changes only; refreshing an unchanged record's lifetime
	// does not invalidate caches. Expiry still does.
	uint64_t revision() const { return generation; }

	// Reject delayed/out-of-order evidence, including after an explicit expiry.
	// Reading snapshots never refreshes timestamps. Expiry changes revision once
	// per call, even when it removes several records of both kinds.
	void expire(Clock::time_point now) {
		if (now < latest) return;
		latest = now;
		const auto threatsBefore = remembered.size(), clearsBefore = cleared.size();
		remembered.erase(std::remove_if(remembered.begin(), remembered.end(), [&](const auto& threat) {
			return !fresh(threat.seen, now, ThreatLifetime);
		}), remembered.end());
		cleared.erase(std::remove_if(cleared.begin(), cleared.end(), [&](const auto& tile) {
			return !fresh(tile.seen, now, ClearLifetime);
		}), cleared.end());
		if (remembered.size() != threatsBefore || cleared.size() != clearsBefore) ++generation;
	}

	void observeThreat(uint32_t id, Position position, std::string species, Clock::time_point now) {
		if (now < latest) return;
		expire(now);
		bool changed = false;
		auto found = std::find_if(remembered.begin(), remembered.end(), [&](const auto& threat) {
			return threat.id == id;
		});
		if (found == remembered.end()) {
			evictOldest(remembered, MaxThreats);
			remembered.push_back({id, position, std::move(species), now});
			changed = true;
		} else {
			changed = found->position != position || found->species != species;
			if (changed) *found = {id, position, std::move(species), now};
			else found->seen = now;
		}
		const auto before = cleared.size();
		cleared.erase(std::remove_if(cleared.begin(), cleared.end(), [&](const auto& tile) {
			return tile.position == position;
		}), cleared.end());
		if (changed || cleared.size() != before) ++generation;
	}

	// Absence at one observed tile is not death, nor clearance of a region.
	void observeEmpty(Position position, Clock::time_point now) {
		if (now < latest) return;
		expire(now);
		const auto before = remembered.size();
		remembered.erase(std::remove_if(remembered.begin(), remembered.end(), [&](const auto& threat) {
			return threat.position == position;
		}), remembered.end());
		if (remembered.size() != before) ++generation;
	}

	std::vector<Threat> threats(Clock::time_point now) {
		expire(now);
		std::vector<Threat> result;
		for (const auto& threat : remembered) {
			if (fresh(threat.seen, now, ThreatLifetime)) result.push_back(threat);
		}
		return result;
	}

	// Use the current raid registry revision, not a controller-local counter.
	// An activation invalidates old clearance without removing any raid warning.
	void observeClear(Position position, uint64_t raidRevision, Clock::time_point now) {
		if (now < latest) return;
		expire(now);
		if (std::any_of(remembered.begin(), remembered.end(), [&](const auto& threat) {
			return threat.position == position;
		})) return; // Caller must actually observe absence before declaring clear.
		auto found = std::find_if(cleared.begin(), cleared.end(), [&](const auto& tile) {
			return tile.position == position;
		});
		if (found == cleared.end()) {
			evictOldest(cleared, MaxClearTiles);
			cleared.push_back({position, raidRevision, now});
		} else {
			if (found->raidRevision == raidRevision) {
				found->seen = now;
				return;
			}
			*found = {position, raidRevision, now};
		}
		++generation;
	}

	// Const clearance reads filter expired evidence without changing revision.
	// Call expire(now) before using revision() as a planning-cache key.
	bool clearAt(Position position, uint64_t raidRevision, Clock::time_point now) const {
		return std::any_of(cleared.begin(), cleared.end(), [&](const auto& tile) {
			return tile.position == position && tile.raidRevision == raidRevision && fresh(tile.seen, now, ClearLifetime);
		});
	}

	std::vector<Position> clearPositions(uint64_t raidRevision, Clock::time_point now) const {
		std::vector<Position> result;
		for (const auto& tile : cleared) {
			if (tile.raidRevision == raidRevision && fresh(tile.seen, now, ClearLifetime)) result.push_back(tile.position);
		}
		return result;
	}

private:
	struct ClearTile {
		Position position;
		uint64_t raidRevision;
		Clock::time_point seen;
	};

	static bool fresh(Clock::time_point seen, Clock::time_point now, std::chrono::seconds lifetime) {
		return now >= seen && now - seen < lifetime;
	}

	template<typename T>
	static void evictOldest(std::vector<T>& entries, size_t maximum) {
		if (entries.size() == maximum) {
			entries.erase(std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
				return a.seen < b.seen;
			}));
		}
	}

	std::vector<Threat> remembered;
	std::vector<ClearTile> cleared;
	Clock::time_point latest = Clock::time_point::min();
	uint64_t generation = 0;
};

// Immutable planning snapshot, in normalized health loss per second (not capped
// at 1). Combine with ordinary static-spawn danger using max, not addition: the
// same monsters may already be represented there. No flee/decision policy here.
class PlayerBotTravelDangerField {
public:
	static constexpr size_t MaxRectangles = 256;
	static constexpr size_t MaxRectangleBuckets = 64;
	static constexpr int BucketSize = 16;
	static constexpr int LocalRadius = 6;
	static constexpr size_t MaxLocalContributors = 3;

	struct Rectangle { Position from, to; double danger; };
	struct LocalThreat { Position position; double danger; };

	PlayerBotTravelDangerField(std::vector<Rectangle> rectangles, std::vector<LocalThreat> localThreats,
	                          std::vector<Position> clearPositions) : local(std::move(localThreats)) {
		for (const auto& position : clearPositions) cleared.insert(key(position.x, position.y, position.z));
		for (auto rectangle : rectangles) {
			if (!(rectangle.danger > 0)) continue;
			const auto from = rectangle.from, to = rectangle.to;
			rectangle.from = {std::min(from.x, to.x), std::min(from.y, to.y), std::min(from.z, to.z)};
			rectangle.to = {std::max(from.x, to.x), std::max(from.y, to.y), std::max(from.z, to.z)};
			if (areas.size() < MaxRectangles) {
				areas.push_back(rectangle);
			} else {
				// Defensive overflow: conservatively merge, never silently drop a
				// warning. Normal raid snapshots have at most 256 footprints.
				auto& tail = areas.back();
				tail.from = {std::min(tail.from.x, rectangle.from.x), std::min(tail.from.y, rectangle.from.y),
				             std::min(tail.from.z, rectangle.from.z)};
				tail.to = {std::max(tail.to.x, rectangle.to.x), std::max(tail.to.y, rectangle.to.y),
				           std::max(tail.to.z, rectangle.to.z)};
				tail.danger = std::max(tail.danger, rectangle.danger);
			}
		}
		for (size_t i = 0; i < areas.size(); ++i) {
			const auto& area = areas[i];
			const int x1 = area.from.x / BucketSize, x2 = area.to.x / BucketSize;
			const int y1 = area.from.y / BucketSize, y2 = area.to.y / BucketSize;
			const size_t count = size_t(x2 - x1 + 1) * (y2 - y1 + 1) * (int(area.to.z) - area.from.z + 1);
			if (count > MaxRectangleBuckets) {
				largeAreas.push_back(i);
				continue;
			}
			for (int z = area.from.z; z <= area.to.z; ++z) {
				for (int y = y1; y <= y2; ++y) {
					for (int x = x1; x <= x2; ++x) areaBuckets[key(x, y, z)].push_back(i);
				}
			}
		}
		// A threat is inserted once per touched bucket. A sample reads one bucket,
		// so even at bucket boundaries each threat contributes at most once.
		for (size_t i = 0; i < local.size(); ++i) {
			const auto& threat = local[i];
			if (!(threat.danger > 0)) continue;
			const int x1 = std::max(0, int(threat.position.x) - LocalRadius) / BucketSize;
			const int x2 = std::min(65535, int(threat.position.x) + LocalRadius) / BucketSize;
			const int y1 = std::max(0, int(threat.position.y) - LocalRadius) / BucketSize;
			const int y2 = std::min(65535, int(threat.position.y) + LocalRadius) / BucketSize;
			for (int y = y1; y <= y2; ++y) {
				for (int x = x1; x <= x2; ++x) localBuckets[key(x, y, threat.position.z)].push_back(i);
			}
		}
	}

	double dangerAt(Position position) const {
		const auto bucket = key(position.x / BucketSize, position.y / BucketSize, position.z);
		double footprint = 0;
		if (cleared.count(key(position.x, position.y, position.z)) == 0) {
			const auto sample = [&](size_t i) {
				const auto& area = areas[i];
				if (position.x >= area.from.x && position.x <= area.to.x &&
				    position.y >= area.from.y && position.y <= area.to.y &&
				    position.z >= area.from.z && position.z <= area.to.z) footprint = std::max(footprint, area.danger);
			};
			const auto found = areaBuckets.find(bucket);
			if (found != areaBuckets.end()) for (const auto i : found->second) sample(i);
			for (const auto i : largeAreas) sample(i);
		}
		std::array<double, MaxLocalContributors> strongest{};
		const auto found = localBuckets.find(bucket);
		if (found != localBuckets.end()) {
			for (const auto i : found->second) {
				const auto& threat = local[i];
				const int distance = std::max(std::abs(int(position.x) - threat.position.x),
				                              std::abs(int(position.y) - threat.position.y));
				if (distance >= LocalRadius) continue;
				// Same-floor Chebyshev distance matches eight-direction tile travel.
				double contribution = threat.danger * (distance <= 1 ? 1.0 : double(LocalRadius - distance) / (LocalRadius - 1));
				for (auto& value : strongest) {
					if (contribution > value) std::swap(contribution, value);
				}
			}
		}
		for (const auto value : strongest) footprint += value;
		return footprint;
	}

private:
	static uint64_t key(int x, int y, int z) {
		return (uint64_t(z) << 32) | (uint64_t(y) << 16) | uint64_t(x);
	}

	std::vector<Rectangle> areas;
	std::vector<LocalThreat> local;
	std::unordered_set<uint64_t> cleared;
	std::unordered_map<uint64_t, std::vector<size_t>> areaBuckets, localBuckets;
	std::vector<size_t> largeAreas;
};

// An observation revision identifies an immutable field, not a planning epoch.
// Only innate raid changes restart a request; selected legs are checked against
// fresh observations before admission/reuse instead of rescanning its frontier.
inline std::string playerBotTravelPlanningIdentity(uint32_t guid, uint64_t raidRevision) {
	return std::to_string(guid) + ":" + std::to_string(raidRevision);
}

// Concrete exposure of the retained executable leg. Keep the accepted total
// (including non-executable paid/coarse tails), and charge any increased local
// exposure against that same cumulative budget. Old exposure is conservative:
// expiry/clearance cannot manufacture more budget for an already accepted trip.
struct PlayerBotTravelRouteExposure {
	struct Sample { Position position; uint32_t milliseconds; double danger; };
	std::vector<Sample> samples;
	PlayerBotNavigationRiskProfile risk;
	uint32_t dangerCost = 0;
	double peak = 0;

	template<class Steps, class Exposure>
	void retain(const Steps& steps, const PlayerBotNavigationCostPolicy& policy, Exposure exposure,
	            uint32_t totalDanger, double maximumPeak) {
		risk = policy.risk;
		dangerCost = totalDanger;
		peak = maximumPeak;
		samples.clear();
		uint64_t localDanger = 0;
		for (const auto& step : steps) {
			const Sample sample{step.expectedPosition, exposure(step), policy.dangerAt(step.expectedPosition)};
			samples.push_back(sample);
			localDanger = std::min<uint64_t>(UINT32_MAX, localDanger + policy.dangerCostForSample(sample.danger, sample.milliseconds));
			peak = std::max(peak, sample.danger);
		}
		// Legacy callers may retain only steps. Never install them with zero
		// exposure, and never discard the larger validated whole-trip budget.
		dangerCost = std::max(dangerCost, static_cast<uint32_t>(localDanger));
	}

	PlayerBotNavigationCostSummary refreshed(const PlayerBotTravelDangerField& field) const {
		PlayerBotNavigationCostPolicy policy;
		policy.risk = risk;
		policy.expectedHealthLossPerSecond = [](Position) { return 0.; };
		PlayerBotNavigationCostSummary result;
		result.dangerCost = dangerCost;
		result.maximumHealthLossPerSecond = peak;
		bool increased = false;
		for (const auto& sample : samples) {
			const double danger = std::max(sample.danger, field.dangerAt(sample.position));
			increased = increased || danger > sample.danger;
			const uint32_t added = policy.dangerCostForSample(danger, sample.milliseconds) -
			    policy.dangerCostForSample(sample.danger, sample.milliseconds);
			result.dangerCost = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(result.dangerCost) + added));
			result.maximumHealthLossPerSecond = std::max(result.maximumHealthLossPerSecond, danger);
		}
		// Static weighted routes may already exceed tolerance. Do not relabel
		// their existing failure as a transient one without increased exposure.
		result.transientRiskRejected = increased && !playerBotNavigationRiskAccepts(
		    risk, result.dangerCost, result.maximumHealthLossPerSecond);
		return result;
	}

	void prepend(const PlayerBotTravelRouteExposure& connector) {
		samples.insert(samples.begin(), connector.samples.begin(), connector.samples.end());
		dangerCost = static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, uint64_t(dangerCost) + connector.dangerCost));
		peak = std::max(peak, connector.peak);
	}

	// Ordinary navigation need not react to threats behind the actor. Its
	// execution gate still checks every re-resolved landing before dispatch.
	void passed(Position position) {
		const auto found = std::find_if(samples.begin(), samples.end(), [&](const auto& sample) {
			return sample.position == position;
		});
		if (found != samples.end()) samples.erase(samples.begin(), std::next(found));
	}
};

// Only for an already verified reversible ordinary walk. Reverse the edges,
// not just the sample order: the reverse landing includes the original source.
inline std::deque<PlayerBotNavigationStep> playerBotTravelReverseWalk(
    Position source, const std::deque<PlayerBotNavigationStep>& steps) {
	std::deque<PlayerBotNavigationStep> reversed;
	for (auto step : steps) {
		const auto next = step.expectedPosition;
		step.target = step.expectedPosition = source;
		step.direction = static_cast<Direction>(step.direction ^ ((step.direction & DIRECTION_DIAGONAL_MASK) ? 3 : 2));
		reversed.push_front(std::move(step));
		source = next;
	}
	return reversed;
}

struct PlayerBotTravelWithdrawalCandidate {
	Position position;
	double danger;
	bool refuge = false;
};

inline constexpr size_t PlayerBotTravelMaxWithdrawalNeighbours = 8;
inline constexpr double PlayerBotTravelWithdrawalEpsilon = 1e-9;

// The caller supplies walkable neighbours and owns plateau escape/scoring.
// Only the first eight entries are considered. Require a real danger reduction,
// then prefer refuge, then lowest danger; exact ties retain caller order.
inline std::optional<Position> playerBotChooseTravelWithdrawal(
    double currentDanger, const std::vector<PlayerBotTravelWithdrawalCandidate>& neighbours) {
	const PlayerBotTravelWithdrawalCandidate* best = nullptr;
	for (size_t i = 0; i < std::min(neighbours.size(), PlayerBotTravelMaxWithdrawalNeighbours); ++i) {
		const auto& candidate = neighbours[i];
		if (!(candidate.danger >= 0) || !(candidate.danger < currentDanger - PlayerBotTravelWithdrawalEpsilon)) continue;
		if (!best || (candidate.refuge && !best->refuge) ||
		    (candidate.refuge == best->refuge && candidate.danger < best->danger)) best = &candidate;
	}
	if (!best) return std::nullopt;
	return best->position;
}

#endif
