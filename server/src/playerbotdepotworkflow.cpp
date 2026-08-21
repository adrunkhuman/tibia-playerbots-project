#include "otpch.h"

#include "playerbotdepotworkflow.h"

void PlayerBotDepotWorkflow::reset()
{
	session.reset();
	discoveryCandidates.clear();
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
	nextCandidateOffset = 0;
	indexedCandidates = 0;
	inScopeCandidates = 0;
	standableCandidates = 0;
	suppressedApproaches = 0;
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
