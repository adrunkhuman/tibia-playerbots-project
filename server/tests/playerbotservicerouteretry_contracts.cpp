#include "otpch.h"
#include "playerbotservicerouteretry.h"
#include "playerbotserviceworkflow.h"
#include "playerbotnavigationruntime.h"
#include "playerbotnavigationsession.h"

#include <cassert>

namespace {
using Retry = PlayerBotServiceRouteRetry;
using Decision = Retry::Decision;
using Result = PlayerBotNavigationResult;
using Command = PlayerBotServiceCommandType;
using namespace std::chrono_literals;
const Position origin(32352, 32227, 7), destination(32399, 32219, 7), exitTile(32351, 32228, 7);
constexpr uint32_t provider = 7;

struct Service {
	PlayerBotServiceWorkflow workflow;
	PlayerBotServiceObservation observation;
	PlayerBotEconomyCatalog catalog;
	PlayerBotDispositionPolicy disposition;

	Service() {
		workflow.reset(PlayerBotServiceIntent::ResupplyWithLocalSale);
		workflow.setLiquidationPlan({provider, {{7634, 10, 5, 0}}});
		observation.currentPosition = origin;
		observation.shops.push_back({provider, Position(32399, 32222, 7), {{7634, 0, 5, 0}}});
		observation.providers[provider] = {true, false, false, true, {{destination, 1, false}}};
		observation.maximumAttempts = 3;
	}

	void pending() {
		const auto command = workflow.advance(observation, catalog, disposition);
		assert(command.type == Command::ValidateProviderRoute);
		assert(command.providerId == provider && command.destination == destination);
		assert(workflow.unavailableProviders().empty());
	}

	void reached() {
		observation.approachRoute = {provider, destination, PlayerBotServiceRouteResult::Reached, 10};
		assert(workflow.advance(observation, catalog, disposition).outcome == PlayerBotServiceOutcome::Success);
		const auto command = workflow.advance(observation, catalog, disposition);
		assert(command.type == Command::NavigateProvider && command.destination == destination);
		assert(workflow.unavailableProviders().empty());
	}

	void reject() {
		observation.approachRoute = {provider, destination, PlayerBotServiceRouteResult::Unreachable};
		const auto command = workflow.advance(observation, catalog, disposition);
		assert(command.type == Command::Wait && command.outcome == PlayerBotServiceOutcome::Retry);
		assert(workflow.unavailableProviders().count(provider) == 1);
	}
};

uint32_t exposure(const PlayerBotNavigationStep&) { return 1000; }

void transientDeferrals()
{
	Service service;
	Retry retry;
	PlayerBotNavigationCostPolicy policy;
	policy.transientHealthLossPerSecond = [](Position) { return 0.4; };
	PlayerBotNavigationRoutePlan rejected;
	rejected.metrics.result = Result::RiskRejected;
	rejected.metrics.transientRiskRejected = true;
	rejected.metrics.maximumHealthLossPerSecond = 0.4;
	for (int turn = 0; turn < 60; ++turn) {
		service.pending();
		const auto threat = playerBotNavigationTransientRejection(rejected, policy, exposure);
		assert(threat == 0.4);
		assert(retry.observe(provider, origin, destination, rejected.metrics.result, threat.has_value(), false) ==
		       Decision::TransientThreat);
		assert(retry.retryCount() == 0);
		// Production deferral leaves the route observation absent, not Unreachable.
	}
	PlayerBotNavigationRoutePlan safe;
	safe.metrics.result = Result::Reached;
	assert(!playerBotNavigationTransientRejection(safe, policy, exposure));
	assert(retry.observe(provider, origin, destination, Result::Reached, false, false) == Decision::Proceed);
	service.reached();
	// Revalidation of an already-selected approach must preserve it too.
	for (int turn = 0; turn < 30; ++turn) {
		assert(retry.observe(provider, origin, destination, Result::RiskRejected, true, false) == Decision::TransientThreat);
		const auto command = service.workflow.advance(service.observation, service.catalog, service.disposition);
		assert(command.type == Command::NavigateProvider && command.destination == destination);
		assert(service.workflow.unavailableProviders().empty());
	}

	// A fresh preparation policy must retain zero-risk acceptance even for
	// below-normal-ceiling exposure on an otherwise accepted route.
	PlayerBotNavigationStep step;
	step.target = step.expectedPosition = destination;
	safe.steps.push_back(step);
	policy.transientHealthLossPerSecond = [](Position) { return 0.001; };
	assert(!playerBotNavigationTransientRejection(safe, policy, exposure));
	policy.risk.maximumHealthLossPerSecond = policy.risk.maximumRouteHealthLoss = 0;
	assert(playerBotNavigationTransientRejection(safe, policy, exposure) == 0.001);

	// An unrelated warning cannot turn a static failure into a transient wait.
	rejected.metrics.transientRiskRejected = false;
	assert(!playerBotNavigationTransientRejection(rejected, policy, exposure));
	assert(retry.observe(provider, origin, destination, Result::RiskRejected, false, false) == Decision::Proceed);
}

void blockedExitExpires()
{
	Service service;
	Retry retry;
	PlayerBotNavigationSession navigation;
	const auto epoch = std::chrono::steady_clock::time_point{} + 100s;
	navigation.suppress(exitTile, epoch + 30s);
	for (int second = 0; second < 30; ++second) {
		service.pending();
		const auto blocked = navigation.activeBlockedPositions(epoch + std::chrono::seconds(second));
		assert(blocked.count(exitTile) == 1);
		assert(retry.observe(provider, origin, destination, Result::Unreachable, false, !blocked.empty()) ==
		       Decision::TemporaryExclusion);
	}
	assert(navigation.activeBlockedPositions(epoch + 30s).empty());
	assert(retry.observe(provider, origin, destination, Result::Reached, false, false) == Decision::Proceed);
	service.reached();

	// Expiry does not convert a genuinely disconnected route into success.
	Service disconnected;
	disconnected.pending();
	assert(retry.observe(provider, origin, destination, Result::Unreachable, false, false) == Decision::Proceed);
	disconnected.reject();
}

void incompleteIsBounded()
{
	Service service;
	Retry retry;
	service.pending();
	for (uint32_t attempt = 1; attempt <= Retry::MaximumRetries; ++attempt) {
		assert(retry.observe(provider, origin, destination, Result::NodeLimit, false, false) == Decision::RetryIncomplete);
		assert(retry.retryCount() == attempt);
		service.pending(); // A freshly rebuilt frontier does not renew retries.
	}
	assert(retry.observe(provider, origin, destination, Result::NodeLimit, false, false) == Decision::IncompleteExhausted);
	assert(retry.retryCount() == 3 && service.workflow.unavailableProviders().empty());
	service.pending(); // Exhaustion stops the controller, not the provider workflow.
	assert(retry.observe(provider, origin, destination, Result::NodeLimit, false, false) == Decision::IncompleteExhausted);

	// All three identity fields renew the allowance; no frontier ID is used.
	for (const auto& context : {
	    std::make_tuple(provider + 1, origin, destination),
	    std::make_tuple(provider + 1, exitTile, destination),
	    std::make_tuple(provider + 1, exitTile, origin)}) {
		const auto [npc, from, to] = context;
		assert(retry.observe(npc, from, to, Result::NodeLimit, false, false) == Decision::RetryIncomplete);
		assert(retry.retryCount() == 1);
	}
	for (const auto completed : {Result::Reached, Result::Unreachable, Result::RiskRejected}) {
		assert(retry.observe(provider, origin, destination, completed, false, false) == Decision::Proceed);
		assert(retry.retryCount() == 0);
		assert(retry.observe(provider, origin, destination, Result::NodeLimit, false, false) == Decision::RetryIncomplete);
		assert(retry.retryCount() == 1);
	}
	retry.reset();
	assert(retry.retryCount() == 0);
	assert(Retry::RetryDelayMs >= 500 && Retry::RetryDelayMs <= 1000);
}
}

int main()
{
	transientDeferrals();
	blockedExitExpires();
	incompleteIsBounded();
	std::cout << "service route retry contracts passed\n";
}
