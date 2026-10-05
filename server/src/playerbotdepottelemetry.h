/** Constant-space, per-controller depot discovery evidence; never affects decisions. */
#ifndef FS_PLAYERBOTDEPOTTELEMETRY_H
#define FS_PLAYERBOTDEPOTTELEMETRY_H

#include <ostream>
#include <utility>
#include "playerbotdepotworkflow.h"
#include "playerbotnavigationruntime.h"
#include "playerbottelemetry.h"

namespace playerbot {
class PlayerBotDepotTelemetry {
public:
	using Clock = std::chrono::steady_clock;
	enum class Result { Started, Progress, Fallback, Selected, Arrived, Failed, Cancelled };
	enum class Intent { Optional, ForcedReturn, Liquidation };
	struct Counters {
		uint64_t scans = 0, routeSlices = 0, validations = 0, accepted = 0;
		uint64_t unknown = 0, unsafe = 0, unexecutable = 0, expandedNodes = 0;
		uint64_t budgetDenials = 0, approachFailures = 0, selections = 0;
		int64_t scanUs = 0, routeUs = 0, budgetRetryUs = 0, budgetWaitUs = 0;
	};
	struct Route {
		PlayerBotDepotCandidate candidate;
		Position origin;
		size_t candidateOffset = 0;
		PlayerBotNavigationResult result = PlayerBotNavigationResult::Unreachable;
		PlayerBotNavigationDangerEvidence evidence = PlayerBotNavigationDangerEvidence::Coarse;
		PlayerBotNavigationRiskVerdict verdict = PlayerBotNavigationRiskVerdict::Unknown;
		uint64_t fare = 0;
		uint32_t dangerCost = 0;
		double maximumHealthLossPerSecond = 0;
		bool executable = false, fareAccepted = false, preferredExit = false, riskFallback = false;
		const char* planner = "none";
	};
	struct Record {
		uint64_t episode = 0;
		Result result;
		Intent intent;
		Position origin;
		int64_t elapsedUs = 0;
		Counters counters;
		PlayerBotDepotSnapshot snapshot;
		std::optional<Route> route;
	};

	bool active() const { return running; }
	void begin(Clock::time_point now, const Position& origin, Intent intent)
	{
		if (running) return;
		running = true;
		++episode;
		started = lastRecord = now;
		this->origin = origin;
		this->intent = intent;
		counts = {};
		revision = reportedRevision = 0;
		current = {};
		lastRoute.reset();
		waitingSince.reset();
		fallbackReported = selectedReported = false;
	}
	void observe(const PlayerBotDepotSnapshot& snapshot) { current = snapshot; }
	void scan(std::chrono::microseconds elapsed) { ++counts.scans; scanBookkeeping(elapsed); }
	void scanBookkeeping(std::chrono::microseconds elapsed) { counts.scanUs += elapsed.count(); ++revision; }
	void denied(Clock::time_point now, std::chrono::microseconds retry)
	{
		++counts.budgetDenials;
		++revision;
		counts.budgetRetryUs += retry.count();
		if (!waitingSince) waitingSince = now;
	}
	void admitted(Clock::time_point now)
	{
		if (!waitingSince) return;
		counts.budgetWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(now - *waitingSince).count();
		waitingSince.reset();
		++revision;
	}
	void pending(std::chrono::microseconds elapsed) { ++counts.routeSlices; counts.routeUs += elapsed.count(); ++revision; }
	void validated(Route route, PlayerBotDepotRouteResult outcome, uint64_t expandedNodes,
	               std::chrono::microseconds elapsed)
	{
		pending(elapsed);
		++counts.validations;
		counts.expandedNodes += expandedNodes;
		counts.accepted += outcome == PlayerBotDepotRouteResult::Reached;
		counts.unknown += outcome == PlayerBotDepotRouteResult::Unknown;
		counts.unsafe += outcome == PlayerBotDepotRouteResult::Unsafe;
		counts.unexecutable += outcome == PlayerBotDepotRouteResult::Unreachable;
		lastRoute = std::move(route);
	}
	void approachFailed() { ++counts.approachFailures; ++revision; }

	std::optional<Record> report(Clock::time_point now, Result result)
	{
		if (!running) return std::nullopt;
		const bool closing = result == Result::Arrived || result == Result::Failed || result == Result::Cancelled;
		bool immediate = closing || result == Result::Started;
		if (result == Result::Selected) {
			++counts.selections;
			++revision;
			immediate = !selectedReported;
			selectedReported = true;
		} else if (result == Result::Fallback) {
			immediate = !fallbackReported;
			fallbackReported = true;
		}
		// A selected route being walked is not new discovery work. Existing
		// navigation/summary events cover movement; do not repeat its payload.
		if (!immediate && (revision == reportedRevision || now - lastRecord < progressRecordInterval)) return std::nullopt;
		// Repeated fallback/selection transitions share the progress throttle,
		// including across rescans. They cannot bypass it with a changing key.
		if (!immediate) result = Result::Progress;
		lastRecord = now;
		reportedRevision = revision;
		Record record{episode, result, intent, origin,
		    std::chrono::duration_cast<std::chrono::microseconds>(now - started).count(), counts, current, lastRoute};
		if (waitingSince) {
			record.counters.budgetWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(now - *waitingSince).count();
		}
		if (closing) running = false;
		return record;
	}

private:
	bool running = false, fallbackReported = false, selectedReported = false;
	uint64_t episode = 0, revision = 0, reportedRevision = 0;
	Clock::time_point started, lastRecord;
	std::optional<Clock::time_point> waitingSince;
	Position origin;
	Intent intent = Intent::Optional;
	Counters counts;
	PlayerBotDepotSnapshot current;
	std::optional<Route> lastRoute;
};
}

#endif
