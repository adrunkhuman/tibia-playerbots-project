#ifndef FS_PLAYERBOTHUNTROUTESELECTION_H
#define FS_PLAYERBOTHUNTROUTESELECTION_H

#include "playerbothuntregions.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

// Pure post-scoring policy. Runtime owns one selector per scored planning pass;
// the controller answers requests with route/discovery results and live economics.
// routeValidated on a candidate means a safe depot exit, not final admissibility.
enum class PlayerBotHuntRouteStage { Outbound, Depot, DiscoverSupply, Supplier, RejectSupply, Final, Done };

struct PlayerBotHuntRouteRequest {
	PlayerBotHuntRouteStage stage = PlayerBotHuntRouteStage::Done;
	uint64_t planningPass = 0;
	uint64_t scoringRevision = 0;
	uint64_t sequence = 0;
	bool routeAvailable = true;
	Position from;
	Position to;
	uint32_t outboundDangerCost = 0;
	uint32_t returnDangerCost = 0;
};

// Immutable facts for exactly one pending request; no world queries in the selector.
struct PlayerBotHuntRouteObservation {
	bool reached = false;
	uint32_t steps = 0;
	uint64_t fare = 0;
	uint32_t dangerCost = 0;
	double peakDanger = 0;
	double travelSeconds = 0;
	uint32_t huntDurationSeconds = 0; // Current outbound turn, not the scored snapshot.
	double staminaMultiplier = 1;
	bool npcTravel = false;
	std::vector<Position> approaches;
	uint32_t potionReserve = 0;
	PlayerBotSupplyProfile supplyProfile;
	uint64_t funds = 0;
	uint64_t recoverySpendingReserve = 0;
	double recoveryRouteHealthLoss = 0;
};

struct PlayerBotHuntRouteResult {
	uint64_t planningPass = 0;
	uint64_t scoringRevision = 0;
	std::optional<PlayerBotHuntRegion> completedCandidate;
	std::optional<PlayerBotHuntRegion> selectedRouteRegion;
	std::map<std::string, uint32_t> failureCounts;
	// Commit these cooldowns only at the terminal selection result, never on cancellation.
	std::vector<uint64_t> rejectedVariants;
	bool accepted = false; // False for stale/replayed observations.
	bool yield = false; // Defer the next request to the next scheduler turn.
	bool terminal = false;
};

class PlayerBotHuntRouteSelection
{
	public:
		explicit PlayerBotHuntRouteSelection(std::vector<PlayerBotHuntRegion> candidates);
		bool done() const { return finished; }
		PlayerBotHuntRouteRequest next();
		PlayerBotHuntRouteResult observe(const PlayerBotHuntRouteRequest& request,
		                                 const PlayerBotHuntRouteObservation& observation);

	private:
		PlayerBotHuntRouteResult reject(PlayerBotHuntRegion candidate);
		PlayerBotHuntRouteResult finish(PlayerBotHuntRegion candidate);
		void advanceCandidate();
		std::vector<PlayerBotHuntRegion> candidates;
		size_t index = 0;
		std::optional<PlayerBotHuntRegion> current;
		std::vector<Position> depots;
		size_t depotIndex = 0;
		std::vector<Position> suppliers;
		size_t supplierIndex = 0;
		std::optional<PlayerBotHuntRegion> best;
		std::map<std::string, uint32_t> failureCounts;
		std::vector<uint64_t> rejectedVariants;
		PlayerBotHuntRouteStage stage = PlayerBotHuntRouteStage::Outbound;
		bool finished = false;
		uint64_t sequence = 0;
		std::optional<PlayerBotHuntRouteRequest> pending;
};

#endif
