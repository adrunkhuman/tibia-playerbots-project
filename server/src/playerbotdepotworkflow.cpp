#include "otpch.h"

#include "playerbotdepotworkflow.h"

void PlayerBotDepotWorkflow::reset()
{
	session.reset();
	discoveryCandidates.clear();
	routeCandidate.reset();
	nextCandidateOffset = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
}

void PlayerBotDepotWorkflow::clearDiscovery()
{
	session.clearDiscovery();
	discoveryCandidates.clear();
	routeCandidate.reset();
	nextCandidateOffset = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
}

PlayerBotDepotCommand PlayerBotDepotWorkflow::advance(const PlayerBotDepotObservation& observation, uint32_t routeBudget,
	uint32_t maximumDiscoveryAttempts, std::chrono::steady_clock::duration suppression)
{
	session.expireRejectedApproaches(observation.now);
	if (session.stage() == PlayerBotDepotStage::Discover) {
		if (!observation.discoveryObserved) return {PlayerBotDepotCommandType::Scan};
		clearDiscovery();
		beginDiscovery(observation.currentPosition);
		for (const auto& candidate : observation.candidates) {
			if (session.isApproachRejected(candidate.approachPosition)) { ++suppressedApproaches; continue; }
			recordCandidate(candidate);
		}
		sortCandidates();
	}
	if (session.stage() == PlayerBotDepotStage::Discover && !hasCandidates()) {
		const uint32_t attempts = session.incrementAttempts();
		return {attempts >= maximumDiscoveryAttempts ? PlayerBotDepotCommandType::Fail : PlayerBotDepotCommandType::Wait,
		        PlayerBotDepotOutcome::Unavailable};
	}
	if (session.stage() == PlayerBotDepotStage::Discover) {
		if (routeCandidate) {
			if (!observation.routeObserved) return {PlayerBotDepotCommandType::ValidateRoute, PlayerBotDepotOutcome::Pending, *routeCandidate};
			if (observation.routeReachable) {
				const PlayerBotDepotCandidate selected = *routeCandidate;
				routeCandidate.reset();
				select(selected);
				return {PlayerBotDepotCommandType::Navigate, PlayerBotDepotOutcome::Ready, selected};
			}
			session.rejectApproach(routeCandidate->approachPosition, observation.now + suppression);
			routeCandidate.reset();
		}
		for (uint32_t attempt = 0; attempt < routeBudget && hasNextCandidate(); ++attempt) {
			routeCandidate = takeNextCandidate();
			if (routeCandidate) return {PlayerBotDepotCommandType::ValidateRoute, PlayerBotDepotOutcome::Pending, *routeCandidate};
		}
		return {hasNextCandidate() ? PlayerBotDepotCommandType::Wait : PlayerBotDepotCommandType::Fail,
		        hasNextCandidate() ? PlayerBotDepotOutcome::Retry : PlayerBotDepotOutcome::Unavailable};
	}
	if (!observation.atApproach) return {PlayerBotDepotCommandType::Navigate, PlayerBotDepotOutcome::Pending,
	                                     {session.depotId(), session.lockerItemId(), session.lockerPosition(), session.approachPosition()}};
	if (!observation.lockerOpen) return {PlayerBotDepotCommandType::OpenLocker};
	if (!observation.chestOpen) return {PlayerBotDepotCommandType::OpenChest};
	if (!observation.hasDepositableItem) return {PlayerBotDepotCommandType::Depart, PlayerBotDepotOutcome::Success};
	return {observation.canDoAction ? PlayerBotDepotCommandType::SelectDeposit : PlayerBotDepotCommandType::Wait};
}

void PlayerBotDepotWorkflow::recordCandidate(PlayerBotDepotCandidate candidate)
{
	discoveryCandidates.push_back(candidate);
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
