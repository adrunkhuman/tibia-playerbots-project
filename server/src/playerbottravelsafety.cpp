#include "otpch.h"

#include "playerbotcontroller.h"
#include "playerbotdangersamplekey.h"
#include "playerbothuntregionadapter.h"
#include "playerbothunttriptiming.h"
#include "playerbotraidhazards.h"
#include "monsters.h"

extern Monsters g_monsters;

namespace {
	constexpr auto observationInterval = std::chrono::milliseconds(250);
	constexpr auto retryInterval = std::chrono::seconds(1);
	constexpr auto logInterval = std::chrono::seconds(5);

	uint32_t distance(Position a, Position b)
	{
		return a.z == b.z ? std::max(Position::getDistanceX(a, b), Position::getDistanceY(a, b)) : UINT32_MAX;
	}

	bool visible(const Player& player, Position position)
	{
		return position.z == player.getPosition().z && player.canSee(position) &&
		       g_game.isSightClear(player.getPosition(), position, true);
	}
}

void PlayerBotController::observeTravelSafety(Player& player)
{
	const auto now = std::chrono::steady_clock::now();
	const auto raidRevision = PlayerBotRaidHazards::instance().revision();
	const Position origin = player.getPosition();
	if (origin == travelObservedPosition && raidRevision == travelObservedRaidRevision &&
	    now - travelObservedAt < observationInterval) return;
	travelObservedAt = now;
	travelObservedPosition = origin;
	travelObservedRaidRevision = raidRevision;
	travelSafety.expire(now);

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, origin);
	std::vector<Creature*> hostiles;
	for (Creature* creature : spectators) {
		const Monster* monster = creature->getMonster();
		if (!monster || creature->isRemoved() || !player.canSeeCreature(creature) ||
		    !visible(player, creature->getPosition()) || creature->isDead() || creature->getMaster() || !monster->isHostile()) continue;
		hostiles.push_back(creature);
	}
	// Keep nearest threats if a viewport is unusually crowded. Work/storage do
	// not grow with the world's total creature population.
	std::sort(hostiles.begin(), hostiles.end(), [&](const auto* a, const auto* b) {
		return std::make_pair(distance(origin, a->getPosition()), a->getID()) <
		       std::make_pair(distance(origin, b->getPosition()), b->getID());
	});
	for (const auto& old : travelSafety.threats(now)) {
		if (visible(player, old.position) && std::none_of(hostiles.begin(), hostiles.end(), [&](const auto* creature) {
			return creature->getPosition() == old.position;
		})) travelSafety.observeEmpty(old.position, now);
	}
	for (size_t i = 0; i < std::min(hostiles.size(), PlayerBotTravelSafetyMemory::MaxThreats); ++i) {
		const auto* creature = hostiles[i];
		travelSafety.observeThreat(creature->getID(), creature->getPosition(), creature->getName(), now);
	}

	const auto hazards = PlayerBotRaidHazards::instance().snapshot();
	const bool warningNearby = std::any_of(hazards->begin(), hazards->end(), [&](const auto& hazard) {
		return origin.z >= hazard.from.z && origin.z <= hazard.to.z &&
		       int(origin.x) + Map::maxClientViewportX + 1 >= hazard.from.x &&
		       int(origin.x) - Map::maxClientViewportX - 1 <= hazard.to.x &&
		       int(origin.y) + Map::maxClientViewportY + 1 >= hazard.from.y &&
		       int(origin.y) - Map::maxClientViewportY - 1 <= hazard.to.y;
	});
	if (!warningNearby) return;
	// Only an actual visible tile can contradict an innate warning. There is
	// no inspection of hidden occupancy, raid placement success, or removals.
	for (int dx = -Map::maxClientViewportX; dx <= Map::maxClientViewportX; ++dx) {
		for (int dy = -Map::maxClientViewportY; dy <= Map::maxClientViewportY; ++dy) {
			const int x = int(origin.x) + dx, y = int(origin.y) + dy;
			if (x < 0 || x > UINT16_MAX || y < 0 || y > UINT16_MAX) continue;
			const Position position(x, y, origin.z);
			if (!visible(player, position) || !g_game.map.getTile(position)) continue;
			bool covered = false, pending = false;
			for (const auto& hazard : *hazards) {
				if (position.x < hazard.from.x || position.x > hazard.to.x || position.y < hazard.from.y ||
				    position.y > hazard.to.y || position.z < hazard.from.z || position.z > hazard.to.z) continue;
				covered = true;
				pending = pending || hazard.pending;
			}
			// Clearance outside a warning changes no danger estimate. Do not
			// invalidate route frontiers merely because another tile enters view.
			if (!covered || pending) continue;
			if (std::any_of(hostiles.begin(), hostiles.end(), [&](const auto* creature) {
				return creature->getPosition() == position;
			})) continue;
			travelSafety.observeClear(position, raidRevision, now);
		}
	}
}

std::string PlayerBotController::travelSafetyIdentity() const
{
	const auto now = std::chrono::steady_clock::now();
	travelSafety.expire(now);
	const uint64_t raid = PlayerBotRaidHazards::instance().revision();
	if (raid == 0 && travelSafety.threats(now).empty()) return {};
	return std::to_string(playerGuid) + ":" + std::to_string(raid) + ":" + std::to_string(travelSafety.revision());
}

std::string PlayerBotController::travelPlanningIdentity() const
{
	return playerBotTravelPlanningIdentity(playerGuid, PlayerBotRaidHazards::instance().revision());
}

PlayerBotTravelRouteExposure PlayerBotController::travelRouteExposure(const Player& player, const Position& source,
    const PlayerBotNavigationRoutePlan& route, const PlayerBotNavigationCostPolicy& policy) const
{
	Position current = source;
	PlayerBotTravelRouteExposure exposure;
	exposure.retain(route.steps, policy, [&](const PlayerBotNavigationStep& step) -> uint32_t {
		const Tile* tile = g_game.map.getTile(step.expectedPosition);
		const Item* ground = tile ? tile->getGround() : nullptr;
		const uint32_t movement = playerBotHuntStepMilliseconds(ground ? Item::items[ground->getID()].speed : 150,
		    static_cast<const Creature&>(player).getStepSpeed(), (step.direction & DIRECTION_DIAGONAL_MASK) != 0,
		    current.z != step.expectedPosition.z);
		current = step.expectedPosition;
		if (step.action == PlayerBotNavigationAction::NpcTravel) return 1000;
		if (step.action == PlayerBotNavigationAction::Move) return movement;
		const bool tool = step.action == PlayerBotNavigationAction::UseRope || step.action == PlayerBotNavigationAction::UseShovel;
		return playerBotHuntActionMilliseconds(movement, std::max<int32_t>(0, g_config.getNumber(tool ?
		    ConfigManager::EX_ACTIONS_DELAY_INTERVAL : ConfigManager::ACTIONS_DELAY_INTERVAL)));
	}, route.metrics.dangerCost, route.metrics.maximumHealthLossPerSecond);
	return exposure;
}

std::shared_ptr<const PlayerBotTravelDangerField> PlayerBotController::travelDangerField(const PlayerBotCombatProfile& combat) const
{
	const auto identity = travelSafetyIdentity();
	if (identity != travelFieldIdentity) {
		travelFields.clear();
		travelFieldIdentity = identity;
	}
	const auto key = PlayerBotDangerSampleKey(0, {}, combat).combat;
	if (const auto found = travelFields.find(key); found != travelFields.end()) return found->second;
	const auto now = std::chrono::steady_clock::now();
	const auto threats = travelSafety.threats(now);
	const auto hazards = PlayerBotRaidHazards::instance().snapshot();
	std::map<std::string, double> species;
	auto danger = [&](const std::string& name) {
		if (const auto found = species.find(name); found != species.end()) return found->second;
		const MonsterType* type = g_monsters.getMonsterType(name, false);
		// An unknown scripted species is not evidence of zero damage. Known
		// harmless species, however, must not manufacture a raid danger.
		const double value = !type ? 0.10 : !type->info.isHostile ? 0 :
		    PlayerBotHuntRegionAdapter::fightEstimate(combat, name, 1).incomingDamagePerSecond /
		        std::max<int32_t>(combat.maximumHealth, 1);
		species.emplace(name, value);
		return value;
	};
	std::vector<PlayerBotTravelDangerField::Rectangle> rectangles;
	for (const auto& hazard : *hazards) {
		std::array<double, PlayerBotTravelDangerField::MaxLocalContributors> strongest{};
		for (const auto& monster : hazard.monsters) {
			for (uint32_t i = 0; i < std::min<uint32_t>(monster.count, strongest.size()); ++i) {
				double contribution = danger(monster.name);
				for (auto& value : strongest) if (contribution > value) std::swap(contribution, value);
			}
		}
		double total = 0;
		for (const double value : strongest) total += value;
		rectangles.push_back({hazard.from, hazard.to, total});
	}
	std::vector<PlayerBotTravelDangerField::LocalThreat> local;
	for (const auto& threat : threats) local.push_back({threat.position, danger(threat.species)});
	auto field = std::make_shared<const PlayerBotTravelDangerField>(std::move(rectangles), std::move(local),
	    travelSafety.clearPositions(PlayerBotRaidHazards::instance().revision(), now));
	if (travelFields.size() >= 4) travelFields.clear();
	travelFields.emplace(key, field);
	return field;
}

bool PlayerBotController::travelSafetyRequired(const Player& player) const
{
	if (travelWithdrawing || (travelDeferred && std::chrono::steady_clock::now() < travelRetryAt)) return true;
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player), PlayerBotEquipmentAdapter::loadout(player));
	return !player.getTile()->hasFlag(TILESTATE_PROTECTIONZONE) &&
	       travelDangerField(combat)->dangerAt(player.getPosition()) > riskProfile.maximumHealthLossPerSecond;
}

bool PlayerBotController::travelStepSafe(const Player& player, const PlayerBotNavigationStep& step) const
{
	const Tile* tile = g_game.map.getTile(step.expectedPosition);
	if (tile && tile->hasFlag(TILESTATE_PROTECTIONZONE)) return true;
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(player), PlayerBotEquipmentAdapter::loadout(player));
	return travelDangerField(combat)->dangerAt(step.expectedPosition) <= riskProfile.maximumHealthLossPerSecond;
}

void PlayerBotController::deferUnsafeTravel(const Position& position, double danger, const char* reason)
{
	const auto now = std::chrono::steady_clock::now();
	if (!travelDeferred || now - travelSafetyLoggedAt >= logInterval) {
		emit("travel_safety", position, "\"action\":\"defer\",\"reason\":" + playerbot::jsonString(reason) +
		    ",\"danger\":" + std::to_string(danger) + ",\"knowledge\":\"raid_and_observation\"" +
		    ",\"raid_revision\":" + std::to_string(PlayerBotRaidHazards::instance().revision()) +
		    ",\"observation_revision\":" + std::to_string(travelSafety.revision()));
		travelSafetyLoggedAt = now;
	}
	travelDeferred = true;
	travelRetryAt = now + retryInterval;
}

void PlayerBotController::resumeSafeTravel(const Position& position)
{
	if (!travelDeferred && !travelWithdrawing) return;
	emit("travel_safety", position, "\"action\":\"resume\",\"reason\":\"route_revalidated\",\"danger\":0");
	travelDeferred = travelWithdrawing = false;
}

void PlayerBotController::approachTravelFrontier(Player& player, const PlayerBotNavigationGoal& goal)
{
	// A remote warning can cut the only route even after a raid is cleared.
	// Approach only across currently visible, zero-exposure ordinary ground to
	// gain local evidence. Never interpret a retry timer as proof of clearance.
	if (player.getWalkDelay() > 0 || !player.canDoAction()) return;
	const Position origin = player.getPosition();
	const auto cost = navigationCostPolicy(player);
	if (cost.dangerAt(origin) != 0) return;
	const PlayerBotNavigator navigator;
	static constexpr Direction directions[] = {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
	    DIRECTION_NORTHEAST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST, DIRECTION_NORTHWEST};
	for (const auto direction : directions) {
		const Position target = getNextPosition(direction, origin);
		const Tile* tile = g_game.map.getTile(target);
		PlayerBotNavigationStep step;
		if (goal.distance(target) >= goal.distance(origin) || !visible(player, target) || !tile || tile->getTopCreature() ||
		    tile->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT) || cost.dangerAt(target) != 0 ||
		    !navigator.resolveMove(player, origin, direction, {}, step) || step.expectedPosition != target) continue;
		executeNavigationStep(&player, step);
		return;
	}
}

bool PlayerBotController::processTravelSafety(Player* player, const Position& position)
{
	const auto now = std::chrono::steady_clock::now();
	// Safety turns bypass the ordinary spell-verification tail. Retire casts
	// here too, or the first heal would block every subsequent healing spell.
	verifySpellCast(*player, position);
	if (travelEscapeExpected) {
		if (position == *travelEscapeExpected || now - travelEscapeStarted >= playerbot::navigationStepTimeout) {
			emit("travel_safety_step", position, "\"result\":" + playerbot::jsonString(position == *travelEscapeExpected ? "completed" : "failed") +
			    ",\"from_x\":" + std::to_string(travelEscapeFrom.x) + ",\"from_y\":" + std::to_string(travelEscapeFrom.y));
			travelEscapeExpected.reset();
		}
	}
	const auto combat = equipmentPolicy.combatProfile(PlayerBotEquipmentAdapter::player(*player), PlayerBotEquipmentAdapter::loadout(*player));
	const auto field = travelDangerField(combat);
	const double danger = player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) ? 0 : field->dangerAt(position);
	const bool unsafe = danger > riskProfile.maximumHealthLossPerSecond;
	if (!unsafe && (!travelWithdrawing || danger <= riskProfile.maximumHealthLossPerSecond * 0.35)) {
		travelWithdrawing = false;
		if (now >= travelRetryAt) return false;
		schedule(playerbot::blockedRouteRetryInterval);
		return true;
	}
	if (!travelWithdrawing) {
		// Explicit safety interruption, rather than an incidental navigation
		// reset. The current goal remains; its route must be revalidated later.
		cancelHuntPlanning("travel_threat", position);
		interruptTrainerRouteWork();
		if (huntCoordinator.hasDefensiveCombat()) finishDefensiveCombat(player, position, "skipped", "travel_threat");
		if (huntCoordinator.traversalTarget()) finishTraversalCombat(player, position, "travel_threat");
		resetNavigation();
		travelWithdrawing = true;
	}
	g_game.playerSetAttackedCreature(playerId, 0);
	g_game.playerFollowCreature(playerId, 0);
	g_game.playerStopAutoWalk(playerId);
	deferUnsafeTravel(position, danger, "observed_threat");
	// Defensive actions retain their normal costs and verification, but their
	// Wait result must not starve movement while the monster keeps attacking.
	handleHealing(player, position);
	if (!turnRouter.running() || telemetry.terminalLogged()) return true;
	if (player->getWalkDelay() > 0 || travelEscapeExpected) {
		schedule(100);
		return true;
	}
	const PlayerBotNavigator navigator;
	static constexpr Direction directions[] = {DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
	    DIRECTION_NORTHEAST, DIRECTION_SOUTHEAST, DIRECTION_SOUTHWEST, DIRECTION_NORTHWEST};
	std::vector<PlayerBotTravelWithdrawalCandidate> candidates;
	std::map<Position, PlayerBotNavigationStep> steps;
	const auto threats = travelSafety.threats(now);
	auto separation = [&](const Position& candidate) {
		uint32_t nearest = UINT32_MAX;
		for (const auto& threat : threats) nearest = std::min(nearest, distance(candidate, threat.position));
		return nearest;
	};
	const uint32_t currentSeparation = separation(position);
	for (const auto direction : directions) {
		const Position target = getNextPosition(direction, position);
		const Tile* tile = g_game.map.getTile(target);
		PlayerBotNavigationStep step;
		if (!tile || tile->getTopCreature() || !visible(*player, target) ||
		    !navigator.resolveMove(*player, position, direction, {}, step) || step.expectedPosition != target ||
		    tile->hasFlag(TILESTATE_FLOORCHANGE | TILESTATE_TELEPORT)) continue;
		const bool refuge = tile->hasFlag(TILESTATE_PROTECTIONZONE) && !player->isPzLocked();
		// Do not escape a raid by stepping into a worse static spawn hazard.
		const double nextDanger = refuge ? 0 : std::max(field->dangerAt(target), PlayerBotHuntRegionAdapter::travelDanger(combat, target));
		// The full-damage band includes adjacent tiles. Permit one step out
		// of that plateau only if separation from every nearby threat improves.
		const double score = nextDanger == danger && separation(target) > currentSeparation ? nextDanger - 1e-6 : nextDanger;
		candidates.push_back({target, score, refuge});
		steps.emplace(target, step);
	}
	if (const auto target = playerBotChooseTravelWithdrawal(danger, candidates)) {
		travelEscapeDispatch = true;
		const bool dispatched = executeNavigationStep(player, steps.at(*target));
		travelEscapeDispatch = false;
		if (dispatched) {
			travelEscapeExpected = *target;
			travelEscapeFrom = position;
			travelEscapeStarted = now;
			emit("travel_safety", position, "\"action\":\"withdraw\",\"reason\":\"lower_exposure\",\"danger\":" +
			    std::to_string(danger) + ",\"next_danger\":" + std::to_string(field->dangerAt(*target)) +
			    ",\"x\":" + std::to_string(target->x) + ",\"y\":" + std::to_string(target->y));
		}
	}
	// A trapped or faster-than-us pursuer may still kill the player. Never
	// manufacture a teleport, hidden route, or unbounded escape search.
	schedule(100);
	return true;
}
