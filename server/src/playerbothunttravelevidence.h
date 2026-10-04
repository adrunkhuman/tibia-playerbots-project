#ifndef FS_PLAYERBOTHUNTTRAVELEVIDENCE_H
#define FS_PLAYERBOTHUNTTRAVELEVIDENCE_H

#include "playerbothunttravelpolicy.h"
#include "playerbothuntrouteselection.h"
#include "playerbotroutechanges.h"

#include <memory>
#include <tuple>

// Completed route facts only; no search frontier or live engine pointers. The
// watch covers all detailed connections, not just the executable first paid leg.
struct PlayerBotHuntTravelEvidence {
	using Actor = std::tuple<uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, float,
	                         uint64_t, bool, bool, bool, bool, uint32_t, uint32_t, bool,
	                         uint64_t, uint32_t, uint32_t, Position>;
	using Offer = std::tuple<uint32_t, Position, Position, uint32_t, uint32_t, bool, std::vector<std::string>>;
	Position source, destination;
	Actor actor;
	uint64_t topologyGeneration = 0, riskRevision = 0, transportReserve = 0;
	PlayerBotNavigationRiskProfile risk;
	PlayerBotRouteChanges::Watch watch;
	std::vector<Offer> offers;
	PlayerBotHuntProviderValidation providers;
	bool paid = false, walkingOnly = false;
	enum class Exit { None, Depot, Supplier };
	Exit exit = Exit::None;

	bool validContext(Position from, Position to, const Actor& liveActor, uint64_t topology,
	                  uint64_t revision, const PlayerBotNavigationRiskProfile& liveRisk) {
		return source == from && destination == to && actor == liveActor && topologyGeneration == topology &&
		    riskRevision == revision && risk.healthLossCost == liveRisk.healthLossCost &&
		    risk.maximumRouteHealthLoss == liveRisk.maximumRouteHealthLoss &&
		    risk.maximumHealthLossPerSecond == liveRisk.maximumHealthLossPerSecond && watch.valid();
	}
	// Canonically sorted by provider ID and offer semantics. Provider motion
	// within talking range is allowed, but used anchors never drift.
	bool validOffers(const std::vector<Offer>& liveOffers) {
		if (!paid) return true;
		std::map<uint32_t, Position> live;
		for (const auto& offer : liveOffers) live[std::get<0>(offer)] = std::get<1>(offer);
		auto semantics = liveOffers;
		if (semantics.size() == offers.size()) {
			for (size_t i = 0; i < semantics.size(); ++i) std::get<1>(semantics[i]) = std::get<1>(offers[i]);
		}
		return semantics == offers && providers.update(live);
	}
};

// Exactly the successful legs required by one candidate. A restarted request
// replaces its proof; rejected alternatives and abandoned work are never merged.
struct PlayerBotHuntRouteEvidence {
	std::shared_ptr<PlayerBotHuntTravelEvidence> outbound, depot, supplier;
	bool supplierRequired = false;

	void accept(PlayerBotHuntRouteStage stage, bool accepted,
	            std::shared_ptr<PlayerBotHuntTravelEvidence> completed) {
		if (!accepted) return;
		if (stage == PlayerBotHuntRouteStage::Supplier) supplierRequired = true;
		if (!completed) return;
		switch (stage) {
			case PlayerBotHuntRouteStage::Outbound: outbound = std::move(completed); break;
			case PlayerBotHuntRouteStage::Depot: depot = std::move(completed); break;
			case PlayerBotHuntRouteStage::Supplier: supplier = std::move(completed); break;
			default: break;
		}
	}
	template<class Validate>
	bool valid(Validate validate) {
		return outbound && depot && (!supplierRequired || supplier) &&
		    validate(*outbound) && validate(*depot) && (!supplier || validate(*supplier));
	}
};

#endif
