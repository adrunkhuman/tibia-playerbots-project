#include "otpch.h"

#include "creature.h"
#include "playerbotdepotworkflow.h"

void PlayerBotDepotWorkflow::reset()
{
	session.reset();
	discoveryCandidates.clear();
	routeCandidate.reset();
	riskFallback.reset();
	validatingRiskFallback = false;
	nextCandidateOffset = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
	unsafeRouteCandidates = 0;
}

void PlayerBotDepotWorkflow::clearDiscovery()
{
	session.clearDiscovery();
	discoveryCandidates.clear();
	routeCandidate.reset();
	riskFallback.reset();
	validatingRiskFallback = false;
	nextCandidateOffset = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
	unsafeRouteCandidates = 0;
}

PlayerBotDepotCommand PlayerBotDepotWorkflow::advance(const PlayerBotDepotObservation& observation, uint32_t routeBudget,
	uint32_t maximumDiscoveryAttempts, std::chrono::steady_clock::duration suppression)
{
	session.expireRejectedApproaches(observation.now);
	if (observation.actionResult == PlayerBotDepotActionResult::SelectedLockerUnavailable) {
		clearDiscovery();
		session.resetAttempts();
		session.discover();
	}
	if (observation.actionResult == PlayerBotDepotActionResult::MoveDestinationUnavailable && session.hasPendingMove()) {
		session.clearMove();
		session.openChest();
	}
	if (observation.actionResult == PlayerBotDepotActionResult::RejectedMoveDiscarded && session.hasPendingMove()) {
		session.clearMove();
		session.resetAttempts();
		session.deposit();
	}
	if (session.hasPendingMove() && observation.move.observed) {
		const PlayerBotDepotMoveVerification verification = session.verifyMove(
			observation.move.inventoryCount, observation.move.destinationCount, maximumDiscoveryAttempts);
		PlayerBotDepotTelemetry telemetry;
		telemetry.moveVerification = verification;
		if (verification.result == PlayerBotDepotMoveResult::Mismatch || verification.result == PlayerBotDepotMoveResult::Rejected) {
			return command(PlayerBotDepotCommandType::Fail, PlayerBotDepotOutcome::Rejected, telemetry);
		}
		if (verification.result == PlayerBotDepotMoveResult::Retry) {
			return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Retry, telemetry);
		}
		return command(PlayerBotDepotCommandType::SelectDeposit, PlayerBotDepotOutcome::Moved, telemetry);
	}
	if (session.stage() == PlayerBotDepotStage::Discover && !routeCandidate && discoveryCandidates.empty()) {
		if (!observation.scan.observed) return command(PlayerBotDepotCommandType::Scan, PlayerBotDepotOutcome::Pending);
		clearDiscovery();
		session.prepareCandidates(observation.currentPosition);
		indexedCandidates = observation.scan.indexedCandidates;
		inScopeCandidates = observation.scan.inScopeCandidates;
		standableCandidates = observation.scan.standableCandidates;
		for (const auto& candidate : observation.scan.candidates) {
			if (session.isApproachRejected(candidate.approachPosition)) { ++suppressedApproaches; continue; }
			recordCandidate(candidate);
		}
		sortCandidates();
	}
	if (session.stage() == PlayerBotDepotStage::Approach && observation.atApproach) {
		session.openLocker();
	}
	if (session.stage() == PlayerBotDepotStage::OpenLocker && observation.lockerOpen) {
		session.resetAttempts();
		session.openChest();
	}
	if (session.stage() == PlayerBotDepotStage::OpenChest && !observation.lockerOpen) {
		session.openLocker();
	}
	if (session.stage() == PlayerBotDepotStage::OpenChest && observation.chestOpen) {
		session.resetAttempts();
		session.deposit();
	}
	if (session.stage() == PlayerBotDepotStage::Deposit && observation.deposit.observed && !observation.deposit.hasDepositableItem) {
		session.depart();
	}
	if (session.stage() == PlayerBotDepotStage::Discover && !hasCandidates()) {
		if (suppressedApproaches != 0) {
			return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Deferred);
		}
		const uint32_t attempts = session.incrementAttempts();
		return command(attempts >= maximumDiscoveryAttempts ? PlayerBotDepotCommandType::Fail : PlayerBotDepotCommandType::Wait,
		               PlayerBotDepotOutcome::Unavailable);
	}
	if (session.stage() == PlayerBotDepotStage::Discover) {
		if (routeCandidate) {
			if (observation.routeResult == PlayerBotDepotRouteResult::NotObserved) {
				return command(PlayerBotDepotCommandType::ValidateRoute, PlayerBotDepotOutcome::Pending);
			}
			if (observation.routeResult == PlayerBotDepotRouteResult::Reached) {
				const PlayerBotDepotCandidate selected = *routeCandidate;
				const bool selectedRiskFallback = validatingRiskFallback;
				routeCandidate.reset();
				validatingRiskFallback = false;
				session.select(selected);
				PlayerBotDepotTelemetry telemetry;
				telemetry.routeSteps = observation.routeSteps;
				telemetry.expandedNodes = observation.expandedNodes;
				telemetry.dangerCost = observation.dangerCost;
				telemetry.maximumHealthLossPerSecond = observation.maximumHealthLossPerSecond;
				telemetry.riskFallback = selectedRiskFallback;
				return command(PlayerBotDepotCommandType::Navigate, PlayerBotDepotOutcome::Ready, telemetry);
			}
			if (observation.routeResult == PlayerBotDepotRouteResult::Unsafe && !validatingRiskFallback) {
				recordUnsafeCandidate(observation);
			} else {
				session.rejectApproach(routeCandidate->approachPosition, observation.now + suppression);
			}
			routeCandidate.reset();
			validatingRiskFallback = false;
		}
		for (uint32_t attempt = 0; attempt < routeBudget && hasNextCandidate(); ++attempt) {
			routeCandidate = takeNextCandidate();
			if (routeCandidate) return command(PlayerBotDepotCommandType::ValidateRoute, PlayerBotDepotOutcome::Pending);
		}
		if (hasNextCandidate()) return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Retry);
		if (riskFallback) {
			routeCandidate = riskFallback->candidate;
			riskFallback.reset();
			validatingRiskFallback = true;
			return command(PlayerBotDepotCommandType::ValidateRoute, PlayerBotDepotOutcome::Retry);
		}
		const uint32_t attempts = session.incrementAttempts();
		if (attempts >= maximumDiscoveryAttempts) return command(PlayerBotDepotCommandType::Fail, PlayerBotDepotOutcome::Unavailable);
		clearDiscovery();
		session.discover();
		return command(PlayerBotDepotCommandType::Scan, PlayerBotDepotOutcome::Retry);
	}
	if (session.stage() == PlayerBotDepotStage::Approach && observation.routeResult == PlayerBotDepotRouteResult::Unreachable) {
		session.rejectApproach(session.approachPosition(), observation.now + suppression);
		clearDiscovery();
		session.discover();
		return command(PlayerBotDepotCommandType::Scan, PlayerBotDepotOutcome::Retry);
	}
	if (!observation.atApproach) return command(PlayerBotDepotCommandType::Navigate, PlayerBotDepotOutcome::Pending);
	if (!observation.lockerOpen) {
		if (!observation.canDoAction) return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Pending);
		if (session.attempts() >= maximumDiscoveryAttempts) return command(PlayerBotDepotCommandType::Fail, PlayerBotDepotOutcome::Rejected);
		session.incrementAttempts();
		return command(PlayerBotDepotCommandType::OpenLocker, PlayerBotDepotOutcome::Pending);
	}
	if (!observation.chestOpen) {
		if (!observation.canDoAction) return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Pending);
		if (session.attempts() >= maximumDiscoveryAttempts) return command(PlayerBotDepotCommandType::Fail, PlayerBotDepotOutcome::Rejected);
		session.incrementAttempts();
		return command(PlayerBotDepotCommandType::OpenChest, PlayerBotDepotOutcome::Pending);
	}
	if (session.stage() == PlayerBotDepotStage::Depart) return command(PlayerBotDepotCommandType::Depart, PlayerBotDepotOutcome::Success);
	if (session.stage() == PlayerBotDepotStage::Deposit && observation.deposit.observed && observation.deposit.hasDepositableItem) {
		if (!observation.canDoAction) return command(PlayerBotDepotCommandType::Wait, PlayerBotDepotOutcome::Pending);
		session.beginMove(observation.deposit.move);
		return command(PlayerBotDepotCommandType::MoveDeposit, PlayerBotDepotOutcome::Ready);
	}
	return command(PlayerBotDepotCommandType::SelectDeposit, PlayerBotDepotOutcome::Pending);
}

void PlayerBotDepotWorkflow::recordCandidate(PlayerBotDepotCandidate candidate)
{
	discoveryCandidates.push_back(candidate);
}

void PlayerBotDepotWorkflow::recordUnsafeCandidate(const PlayerBotDepotObservation& observation)
{
	++unsafeRouteCandidates;
	UnsafeCandidate candidate{*routeCandidate, observation.dangerCost,
	                          observation.maximumHealthLossPerSecond, observation.routeSteps};
	if (!riskFallback || std::tie(candidate.dangerCost, candidate.maximumHealthLossPerSecond, candidate.routeSteps,
	                            candidate.candidate.depotId, candidate.candidate.lockerPosition,
	                            candidate.candidate.approachPosition) <
	                    std::tie(riskFallback->dangerCost, riskFallback->maximumHealthLossPerSecond,
	                            riskFallback->routeSteps, riskFallback->candidate.depotId,
	                            riskFallback->candidate.lockerPosition, riskFallback->candidate.approachPosition)) {
		riskFallback = std::move(candidate);
	}
}

void PlayerBotDepotWorkflow::sortCandidates()
{
	std::sort(discoveryCandidates.begin(), discoveryCandidates.end(), [](const auto& left, const auto& right) {
		return left.distance != right.distance ? left.distance < right.distance :
		       left.depotId != right.depotId ? left.depotId < right.depotId :
		       left.lockerPosition != right.lockerPosition ? left.lockerPosition < right.lockerPosition :
		       left.approachPosition < right.approachPosition;
	});
}

bool PlayerBotDepotWorkflow::hasNextCandidate() const
{
	return nextCandidateOffset < discoveryCandidates.size();
}

std::optional<PlayerBotDepotCandidate> PlayerBotDepotWorkflow::takeNextCandidate()
{
	if (!hasNextCandidate()) {
		return std::nullopt;
	}
	return discoveryCandidates[nextCandidateOffset++];
}

std::optional<std::chrono::steady_clock::time_point> PlayerBotDepotWorkflow::earliestRejectedApproachExpiry() const
{
	const auto& rejected = session.rejectedApproaches();
	if (rejected.empty()) {
		return std::nullopt;
	}
	auto earliest = rejected.begin()->second;
	for (const auto& entry : rejected) {
		earliest = std::min(earliest, entry.second);
	}
	return earliest;
}

PlayerBotDepotSnapshot PlayerBotDepotWorkflow::snapshot() const
{
	PlayerBotDepotSnapshot result;
	result.stage = session.stage();
	result.hasSelectedDepot = session.hasSelectedDepot();
	result.selected = {session.depotId(), session.lockerItemId(), session.lockerPosition(), session.approachPosition()};
	result.hasRouteCandidate = routeCandidate.has_value();
	if (routeCandidate) result.routeCandidate = *routeCandidate;
	result.validatingRiskFallback = validatingRiskFallback;
	result.hasPendingMove = session.hasPendingMove();
	if (result.hasPendingMove) result.pendingMove = session.move();
	result.attempts = session.attempts();
	result.indexedCandidates = indexedCandidates;
	result.inScopeCandidates = inScopeCandidates;
	result.standableCandidates = standableCandidates;
	result.suppressedApproaches = suppressedApproaches;
	result.unsafeRouteCandidates = unsafeRouteCandidates;
	result.retryAt = earliestRejectedApproachExpiry();
	return result;
}

PlayerBotDepotCommand PlayerBotDepotWorkflow::command(PlayerBotDepotCommandType type, PlayerBotDepotOutcome outcome,
	PlayerBotDepotTelemetry telemetry) const
{
	return {type, outcome, snapshot(), std::move(telemetry)};
}
