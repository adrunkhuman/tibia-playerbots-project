#ifndef FS_PLAYERBOTHUNTSUPPLYHANDOFF_H
#define FS_PLAYERBOTHUNTSUPPLYHANDOFF_H

#include "playerbothuntrouteselection.h"

// Controller-local evidence. Position is deliberately absent: returning to the
// same shop is not progress. A successful hunt or changed planning facts is.
struct PlayerBotHuntSupplyFacts {
	PlayerBotSupplyCapabilitySnapshot capability;
	PlayerBotHuntReturnCoverageContext dependencies;
	uint64_t atlasRevision = 0;
	uint64_t funds = 0;
	uint64_t stockKey = 0;
	int32_t health = 0;

	bool sameDependencies(const PlayerBotHuntSupplyFacts& other) const
	{
		return capability == other.capability && dependencies == other.dependencies &&
		       atlasRevision == other.atlasRevision;
	}
	bool operator==(const PlayerBotHuntSupplyFacts& other) const
	{
		return sameDependencies(other) && funds == other.funds && stockKey == other.stockKey && health == other.health;
	}
};

class PlayerBotHuntSupplyHandoff
{
	public:
		const PlayerBotSupplyRequirements& requirements() const { return required; }
		const PlayerBotGoalReadiness& readiness() const { return goalReadiness; }
		void blocked(PlayerBotGoalReadiness result, const PlayerBotHuntSupplyFacts& facts)
		{
			goalReadiness = std::move(result);
			if (goalReadiness.state != PlayerBotReadiness::Pending) incompleteAttempts = 0;
			evidence = facts;
			if (goalReadiness.requirement && goalReadiness.requirement->kind == PlayerBotRequirementKind::Stock)
				required = goalReadiness.requirement->stocks;
		}
		// Verified prerequisite effects permit a fresh evaluator, never execution
		// of a route cached before shopping, item use, healing, or rest.
		void replan() { goalReadiness = {}; }
		bool refresh(const PlayerBotHuntSupplyFacts& facts)
		{
			if (!evidence || evidence->sameDependencies(facts)) return false;
			required.clear();
			evidence.reset();
			goalReadiness = {};
			incompleteAttempts = 0;
			return true;
		}
		void require(PlayerBotSupplyRequirements supplies, const PlayerBotHuntSupplyFacts& facts)
		{
			incompleteAttempts = 0;
			required = std::move(supplies);
			goalReadiness = {PlayerBotReadiness::Blocked, PlayerBotPreparationRequirement{
			    PlayerBotRequirementKind::Stock, required, 0, false}};
			evidence = facts;
		}
		void selected(bool retainShortage = false)
		{
			// A recovery outing earns the missing stock; accepting that outing
			// does not satisfy the ordinary hunt requirement it is funding.
			if (!retainShortage) {
				required.clear();
				evidence.reset();
			}
			goalReadiness = {};
			failures.clear();
			incompleteAttempts = 0;
		}
		// Only a completed selection counts. Pending routes/search yields are
		// neither failure nor progress, and cannot terminal-stop a controller.
		bool noProgress(PlayerBotHuntRouteOutcome outcome, const PlayerBotHuntSupplyFacts& facts)
		{
			if (outcome == PlayerBotHuntRouteOutcome::Pending) return false;
			// Completed allowance exhaustion has a retry bound, not a danger
			// verdict. Currency/health fluctuations cannot grant unlimited scans.
			if (outcome == PlayerBotHuntRouteOutcome::Incomplete) return ++incompleteAttempts >= 3;
			if (outcome == PlayerBotHuntRouteOutcome::Selected) { selected(); return false; }
			// Compare completed outcomes, not intermediate shopping snapshots.
			// Returning to previously failed stock/funds is not new progress,
			// even if another completed pass saw a temporarily larger stack.
			for (auto& failure : failures) {
				if (failure.facts == facts) return ++failure.attempts >= 3;
			}
			if (failures.size() == 8) failures.erase(failures.begin());
			failures.push_back({facts, 1});
			return false;
		}

	private:
		uint32_t incompleteAttempts = 0;
		PlayerBotGoalReadiness goalReadiness;
		PlayerBotSupplyRequirements required;
		std::optional<PlayerBotHuntSupplyFacts> evidence;
		struct Failure {
			PlayerBotHuntSupplyFacts facts;
			uint32_t attempts = 0;
		};
		// Small controller-local history; real changes may introduce new facts
		// indefinitely, but cannot grow retained failure evidence without bound.
		std::vector<Failure> failures;
};

#endif
