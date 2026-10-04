#ifndef FS_PLAYERBOTHUNTROUTERETENTION_H
#define FS_PLAYERBOTHUNTROUTERETENTION_H

#include "playerbothuntregions.h"

#include <optional>
#include <utility>

// Controller-owned evidence: at most the current outbound request and the best
// fully validated candidate. The pure selector never owns executable plans.
template<class Evidence>
class PlayerBotHuntRouteRetention {
public:
	void begin(uint64_t planningPass, uint64_t scoringRevision) {
		if (pass != planningPass || revision != scoringRevision) {
			clear();
			pass = planningPass;
			revision = scoringRevision;
		}
	}
	Evidence& outbound(uint64_t requestSequence) {
		if (!current || sequence != requestSequence) {
			current.emplace();
			sequence = requestSequence;
		}
		return *current;
	}
	Evidence* pending() { return current ? &*current : nullptr; }
	void complete(const PlayerBotHuntRegion& candidate) {
		if (current && candidate.suitable && candidate.routeValidated && candidate.rejectionReason.empty() &&
		    (!best || playerBotPreferHuntRegion(candidate, best->region))) {
			best = Winner{candidate, std::move(*current)};
		}
		current.reset();
	}
	std::optional<Evidence> take(const PlayerBotHuntRegion& selected) {
		std::optional<Evidence> result;
		if (best && best->region.id == selected.id && best->region.atlasVariantId == selected.atlasVariantId &&
		    best->region.atlasRevision == selected.atlasRevision && best->region.destination == selected.destination)
			result = std::move(best->evidence);
		clear();
		return result;
	}
	void clear() { current.reset(); best.reset(); pass = revision = sequence = 0; }
private:
	struct Winner { PlayerBotHuntRegion region; Evidence evidence; };
	uint64_t pass = 0, revision = 0, sequence = 0;
	std::optional<Evidence> current;
	std::optional<Winner> best;
};

#endif
