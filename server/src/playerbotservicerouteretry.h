#ifndef FS_PLAYERBOTSERVICEROUTERETRY_H
#define FS_PLAYERBOTSERVICEROUTERETRY_H

#include "playerbotnavigation.h"

#include <optional>
#include <tuple>

// A new frontier is not a new service request. Keep incomplete-search retries
// outside the sliced search so rebuilding it cannot renew the allowance.
class PlayerBotServiceRouteRetry {
public:
	enum class Decision { Proceed, TransientThreat, TemporaryExclusion, RetryIncomplete, IncompleteExhausted };
	static constexpr uint32_t MaximumRetries = 3;
	static constexpr uint32_t RetryDelayMs = 1000;

	Decision observe(uint32_t provider, Position origin, Position destination,
	                 PlayerBotNavigationResult result, bool transientRejected, bool temporarilyBlocked)
	{
		const Context next{provider, origin, destination};
		if (!context || *context != next) {
			context = next;
			retries = 0;
		}
		if (result != PlayerBotNavigationResult::NodeLimit) retries = 0;
		if (transientRejected) return Decision::TransientThreat;
		if (result == PlayerBotNavigationResult::Unreachable && temporarilyBlocked) return Decision::TemporaryExclusion;
		if (result != PlayerBotNavigationResult::NodeLimit) return Decision::Proceed;
		if (retries == MaximumRetries) return Decision::IncompleteExhausted;
		++retries;
		return Decision::RetryIncomplete;
	}

	void reset() { context.reset(); retries = 0; }
	uint32_t retryCount() const { return retries; }

private:
	using Context = std::tuple<uint32_t, Position, Position>;
	std::optional<Context> context;
	uint32_t retries = 0;
};

#endif
