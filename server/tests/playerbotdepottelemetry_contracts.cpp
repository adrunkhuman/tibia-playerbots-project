#include "playerbotdepottelemetry.h"

#include <cassert>
#include <iostream>

using Telemetry = playerbot::PlayerBotDepotTelemetry;
using namespace std::chrono;

int main()
{
	const auto epoch = Telemetry::Clock::time_point{};
	const Position origin(100, 100, 7);
	Telemetry telemetry;
	assert(!telemetry.report(epoch, Telemetry::Result::Progress));
	telemetry.begin(epoch, origin, Telemetry::Intent::ForcedReturn);
	assert(telemetry.report(epoch, Telemetry::Result::Started));

	// The incident-sized unknown queue produces no per-candidate records.
	PlayerBotDepotSnapshot snapshot;
	snapshot.candidateCount = 220;
	snapshot.hasRouteCandidate = true;
	Telemetry::Route route;
	route.origin = origin;
	route.result = PlayerBotNavigationResult::Reached;
	route.executable = route.fareAccepted = true;
	route.planner = "hunt_travel";
	for (int i = 0; i < 220; ++i) {
		route.candidate.approachPosition = Position(109 + i, 100, 7);
		route.candidateOffset = snapshot.candidateOffset = i + 1;
		snapshot.routeCandidate = route.candidate;
		telemetry.observe(snapshot);
		telemetry.validated(route, PlayerBotDepotRouteResult::Unknown, 30, microseconds(100));
		assert(!telemetry.report(epoch + milliseconds(i * 10), Telemetry::Result::Progress));
	}
	snapshot.validatingRiskFallback = true;
	telemetry.observe(snapshot);
	const auto fallback = telemetry.report(epoch + seconds(3), Telemetry::Result::Fallback);
	assert(fallback && fallback->counters.validations == 220 && fallback->counters.unknown == 220);
	assert(fallback->counters.unsafe == 0 && fallback->counters.accepted == 0);
	assert(fallback->snapshot.candidateOffset == 220 && fallback->snapshot.candidateCount == 220);
	assert(fallback->route->candidate.approachPosition == Position(328, 100, 7));
	assert(fallback->counters.routeUs == 22000 && fallback->counters.expandedNodes == 6600);
	assert(!telemetry.report(epoch + seconds(4), Telemetry::Result::Fallback));
	assert(!telemetry.report(epoch + seconds(7), Telemetry::Result::Progress));
	assert(!telemetry.report(epoch + seconds(8), Telemetry::Result::Progress));

	// Denied retries do not double-count overlapping requested waits.
	telemetry.denied(epoch + seconds(9), seconds(5));
	telemetry.denied(epoch + seconds(10), seconds(5));
	auto waiting = telemetry.report(epoch + seconds(13), Telemetry::Result::Progress);
	assert(waiting && waiting->counters.budgetDenials == 2);
	assert(waiting->counters.budgetRetryUs == 10000000);
	assert(waiting->counters.budgetWaitUs == 4000000);
	telemetry.admitted(epoch + seconds(15));
	telemetry.pending(microseconds(50));
	telemetry.scan(microseconds(75));
	route.evidence = PlayerBotNavigationDangerEvidence::Detailed;
	telemetry.validated(route, PlayerBotDepotRouteResult::Reached, 1, microseconds(25));
	const auto selected = telemetry.report(epoch + seconds(15), Telemetry::Result::Selected);
	assert(selected && selected->counters.budgetWaitUs == 6000000);
	assert(selected->counters.routeSlices == 222 && selected->counters.validations == 221);
	assert(selected->counters.scans == 1 && selected->counters.scanUs == 75);
	assert(selected->counters.accepted == 1 && selected->counters.selections == 1);
	assert(!telemetry.report(epoch + seconds(21), Telemetry::Result::Progress));
	const auto arrival = telemetry.report(epoch + seconds(22), Telemetry::Result::Arrived);
	assert(arrival && arrival->elapsedUs == 22000000);
	assert(!telemetry.active());
	assert(!telemetry.report(epoch + seconds(30), Telemetry::Result::Progress));
	assert(!telemetry.report(epoch + seconds(30), Telemetry::Result::Cancelled));

	// Fast rescans and repeated selections/fallbacks share one fixed throttle.
	telemetry.begin(epoch + seconds(30), origin, Telemetry::Intent::Optional);
	const auto started = telemetry.report(epoch + seconds(30), Telemetry::Result::Started);
	assert(started && started->episode == 2 && started->counters.validations == 0);
	assert(!started->route && started->snapshot.candidateCount == 0);
	int records = 1;
	for (int i = 1; i <= 6000; ++i) {
		const auto now = epoch + seconds(30) + milliseconds(i * 10);
		telemetry.scan(microseconds(1));
		telemetry.approachFailed();
		records += telemetry.report(now, Telemetry::Result::Fallback).has_value();
		records += telemetry.report(now, Telemetry::Result::Selected).has_value();
		records += telemetry.report(now, Telemetry::Result::Progress).has_value();
	}
	// Started + first fallback + first selection + at most 12 periodic records.
	assert(records <= 15);
	const auto failed = telemetry.report(epoch + seconds(91), Telemetry::Result::Failed);
	assert(failed && failed->counters.approachFailures == 6000 && failed->counters.selections == 6000);
	assert(failed->counters.scans == 6000);
	assert(!telemetry.report(epoch + seconds(92), Telemetry::Result::Progress));

	// A terminal cancellation flushes an ongoing budget wait once.
	telemetry.begin(epoch + seconds(100), origin, Telemetry::Intent::Liquidation);
	telemetry.denied(epoch + seconds(101), seconds(5));
	const auto cancelled = telemetry.report(epoch + seconds(102), Telemetry::Result::Cancelled);
	assert(cancelled && cancelled->counters.budgetWaitUs == 1000000);
	assert(cancelled->intent == Telemetry::Intent::Liquidation);
	assert(!telemetry.report(epoch + seconds(103), Telemetry::Result::Cancelled));
	std::cout << "depot telemetry contracts passed; 6000 retry cycles emitted " << records << " records in 60 seconds\n";
}
