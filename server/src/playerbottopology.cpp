/** Shared static coarse topology for playerbot route planning. */
#include "otpch.h"

#include "playerbottopology.h"

#include "playerbotnavigation.h"

#include "housetile.h"
#include "item.h"
#include "map.h"
#include "teleport.h"
#include "tile.h"

#include <array>
#include <queue>
#include <unordered_set>

namespace {
	constexpr uint16_t sectorSize = 32;
	constexpr uint32_t topologyEdgeMovementCost = sectorSize * 10;
	constexpr std::array<Direction, 8> directions = {
		DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_SOUTH, DIRECTION_WEST,
		DIRECTION_SOUTHWEST, DIRECTION_SOUTHEAST, DIRECTION_NORTHWEST, DIRECTION_NORTHEAST,
	};

	Direction reverseDirection(Direction direction)
	{
		switch (direction) {
			case DIRECTION_NORTH: return DIRECTION_SOUTH;
			case DIRECTION_EAST: return DIRECTION_WEST;
			case DIRECTION_SOUTH: return DIRECTION_NORTH;
			case DIRECTION_WEST: return DIRECTION_EAST;
			case DIRECTION_SOUTHWEST: return DIRECTION_NORTHEAST;
			case DIRECTION_SOUTHEAST: return DIRECTION_NORTHWEST;
			case DIRECTION_NORTHWEST: return DIRECTION_SOUTHEAST;
			case DIRECTION_NORTHEAST: return DIRECTION_SOUTHWEST;
			default: return DIRECTION_NONE;
		}
	}
	constexpr std::array<uint16_t, 3> ladderIds = {1386, 3678, 5543};
	constexpr std::array<uint16_t, 1> downUseIds = {430};
	constexpr std::array<uint16_t, 4> ropeSpotIds = {384, 418, 8278, 8592};

	template<typename T, size_t N>
	bool contains(const std::array<T, N>& values, T value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	uint64_t positionKey(const Position& position)
	{
		return (static_cast<uint64_t>(position.z) << 32) |
		       (static_cast<uint64_t>(position.x) << 16) | position.y;
	}

	uint64_t sectorKey(const Position& position)
	{
		return (static_cast<uint64_t>(position.z) << 32) |
		       (static_cast<uint64_t>(position.x / sectorSize) << 16) | position.y / sectorSize;
	}

	const Item* staticDoor(const Tile& tile)
	{
		const TileItemVector* items = tile.getItemList();
		if (!items) return nullptr;
		const auto door = std::find_if(items->begin(), items->end(), [](const Item* item) {
			return item && playerBotIsTraversableDoor(*item);
		});
		return door == items->end() ? nullptr : *door;
	}

	uint32_t doorMinimumLevel(const Item& door)
	{
		return playerBotPassageMinimumLevel(door).value_or(std::numeric_limits<uint32_t>::max());
	}

	bool isStaticWalkTile(const Tile& tile)
	{
		if (!tile.getGround() || tile.hasFlag(TILESTATE_TELEPORT) || dynamic_cast<const HouseTile*>(&tile)) return false;
		return !tile.hasFlag(TILESTATE_BLOCKSOLID) || staticDoor(tile);
	}

	bool canTraversePortal(const Map* map, const PlayerBotTopologyPortal& portal,
	                       bool canUseRope, bool canUseShovel)
	{
		if (portal.action == PlayerBotTopologyPortalAction::UseRope) return canUseRope;
		if (portal.action != PlayerBotTopologyPortalAction::UseShovel) return true;
		const Tile* tile = map ? map->getTile(portal.target) : nullptr;
		const Item* passage = tile ? playerBotShovelPassageItem(*tile, portal.itemId) : nullptr;
		return passage && playerBotResolveShovelPassageAction(
		    portal.itemId, passage->getID(), canUseShovel).has_value();
	}
}

PlayerBotTopology& PlayerBotTopology::instance()
{
	static PlayerBotTopology topology;
	return topology;
}

void PlayerBotTopology::invalidate()
{
	++topologyGeneration;
	liveMap = nullptr;
	walkNodes.clear();
	nodeComponents.clear();
	edges.clear();
	componentEdges.clear();
	reachabilityCache.clear();
	topologyPortals.clear();
	components = 0;
}

void PlayerBotTopology::build(const Map& map)
{
	invalidate();
	liveMap = &map;
	size_t walkableTiles = 0;
	map.forEachTile([&walkableTiles](const Tile& tile) {
		if (isStaticWalkTile(tile)) ++walkableTiles;
	});
	std::unordered_set<uint64_t> redirectedDestinations;
	map.forEachTile([&map, &redirectedDestinations](const Tile& tile) {
		if (!isStaticWalkTile(tile)) return;
		for (Direction direction : directions) {
			PlayerBotWalkTransition transition;
			if (!playerBotResolveWalkTransition(tile.getPosition(), direction, transition) ||
			    transition.destination == transition.entry || !map.getTile(transition.destination)) continue;
			redirectedDestinations.insert(positionKey(transition.destination));
		}
	});
	walkNodes.reserve(walkableTiles + redirectedDestinations.size());
	std::vector<uint32_t> parents;
	std::vector<uint8_t> ranks;
	parents.reserve(walkableTiles + redirectedDestinations.size());
	ranks.reserve(walkableTiles + redirectedDestinations.size());
	map.forEachTile([this, &redirectedDestinations, &parents, &ranks](const Tile& tile) {
		if (!isStaticWalkTile(tile) &&
		    redirectedDestinations.find(positionKey(tile.getPosition())) == redirectedDestinations.end()) return;
		const uint32_t index = static_cast<uint32_t>(parents.size());
		walkNodes.emplace(positionKey(tile.getPosition()), index);
		parents.push_back(index);
		ranks.push_back(0);
	});

	auto findRoot = [&parents](uint32_t node) {
		uint32_t root = node;
		while (parents[root] != root) root = parents[root];
		while (parents[node] != node) {
			const uint32_t parent = parents[node];
			parents[node] = root;
			node = parent;
		}
		return root;
	};
	auto join = [&parents, &ranks, &findRoot](uint32_t left, uint32_t right) {
		left = findRoot(left);
		right = findRoot(right);
		if (left == right) return;
		if (ranks[left] < ranks[right]) std::swap(left, right);
		parents[right] = left;
		if (ranks[left] == ranks[right]) ++ranks[left];
	};
	map.forEachTile([this, &map, &join](const Tile& tile) {
		if (!isStaticWalkTile(tile)) return;
		if (staticDoor(tile)) return;
		const Position& position = tile.getPosition();
		const auto current = walkNodes.find(positionKey(position));
		if (current == walkNodes.end()) return;
		for (int32_t xOffset = -1; xOffset <= 1; ++xOffset) {
			for (int32_t yOffset = -1; yOffset <= 0; ++yOffset) {
				if ((xOffset == 0 && yOffset == 0) || (yOffset == 0 && xOffset >= 0)) continue;
				const int32_t x = static_cast<int32_t>(position.x) + xOffset;
				const int32_t y = static_cast<int32_t>(position.y) + yOffset;
				if (x < 0 || y < 0 || x > std::numeric_limits<uint16_t>::max() ||
				    y > std::numeric_limits<uint16_t>::max()) continue;
				const Position neighbor(static_cast<uint16_t>(x), static_cast<uint16_t>(y), position.z);
				if (sectorKey(position) != sectorKey(neighbor)) continue;
				const Tile* neighborTile = map.getTile(neighbor);
				if (!neighborTile || !isStaticWalkTile(*neighborTile) || staticDoor(*neighborTile)) continue;
				const auto entry = walkNodes.find(positionKey(neighbor));
				if (entry == walkNodes.end()) continue;
				PlayerBotWalkTransition forward;
				PlayerBotWalkTransition reverse;
				if (playerBotResolveWalkTransition(position, getDirectionTo(position, neighbor), forward) &&
				    forward.destination == neighbor &&
				    playerBotResolveWalkTransition(neighbor, reverseDirection(getDirectionTo(position, neighbor)), reverse) &&
				    reverse.destination == position) join(current->second, entry->second);
			}
		}
	});

	std::unordered_map<uint32_t, uint32_t> nodeIds;
	nodeIds.reserve(walkNodes.size() / 16);
	uint32_t nodeCount = 0;
	for (auto& [key, node] : walkNodes) {
		(void)key;
		const uint32_t root = findRoot(node);
		auto [entry, inserted] = nodeIds.emplace(root, nodeCount);
		if (inserted) ++nodeCount;
		node = entry->second;
	}
	edges.resize(nodeCount);

	auto addEdge = [this](uint32_t from, uint32_t to, const PlayerBotTopologyPortal& portal) {
		auto& outgoing = edges[from];
		if (std::none_of(outgoing.begin(), outgoing.end(), [to, &portal](const Edge& edge) {
			return edge.destinationNode == to && edge.portal.target == portal.target &&
			       edge.portal.destination == portal.destination && edge.portal.action == portal.action &&
			       edge.portal.itemId == portal.itemId && edge.portal.expectedItemId == portal.expectedItemId;
		})) {
			outgoing.push_back({to, portal});
			topologyPortals.push_back(portal);
		}
	};
	map.forEachTile([this, &map, &addEdge](const Tile& tile) {
		const Position& position = tile.getPosition();
		const auto current = walkNodes.find(positionKey(position));
		if (current == walkNodes.end()) return;
		const uint32_t from = current->second;
		for (Direction direction : directions) {
			PlayerBotWalkTransition transition;
			if (!playerBotResolveWalkTransition(position, direction, transition)) continue;
			const Tile* destinationTile = map.getTile(transition.destination);
			if (transition.destination == transition.entry &&
			    (!destinationTile || !isStaticWalkTile(*destinationTile))) continue;
			const auto entry = walkNodes.find(positionKey(transition.destination));
			if (entry == walkNodes.end() || entry->second == from) continue;
			const Tile* targetTile = map.getTile(transition.entry);
			const Item* targetDoor = targetTile ? staticDoor(*targetTile) : nullptr;
			const Item* targetPassage = targetTile ? playerBotShovelPassageItem(*targetTile) : nullptr;
			const auto shovelPassage = targetPassage ? playerBotShovelPassage(targetPassage->getID()) : std::nullopt;
			PlayerBotTopologyPortal portal{position, transition.target, transition.destination, direction};
			if (targetDoor) {
				portal.action = PlayerBotTopologyPortalAction::UseDoor;
				portal.itemId = targetDoor->getID();
				portal.expectedItemId = *playerBotPassageOpenItemId(*targetDoor);
				portal.minimumLevel = doorMinimumLevel(*targetDoor);
			} else if (shovelPassage && targetPassage->getID() == shovelPassage->openItemId &&
			           transition.destination != transition.entry) {
				// Keep the passage identity stable when an open hole later decays closed.
				portal.action = PlayerBotTopologyPortalAction::UseShovel;
				portal.itemId = shovelPassage->closedItemId;
				portal.expectedItemId = shovelPassage->openItemId;
			}
			addEdge(from, entry->second, portal);
		}
	});
	std::vector<PlayerBotTopologyComponentArc> walkArcs;
	for (uint32_t source = 0; source < edges.size(); ++source) {
		for (const Edge& edge : edges[source]) {
			walkArcs.push_back({source, edge.destinationNode, edge.portal.minimumLevel == 0});
		}
	}
	const std::vector<uint32_t> nodeRoots =
	    playerBotTopologyBidirectionalComponents(nodeCount, walkArcs);
	auto upperDestination = [this](const Position& target) -> std::optional<Position> {
		if (target.z == 0) return std::nullopt;
		const Position upper(target.x, target.y, target.z - 1);
		constexpr std::array<Direction, 8> preference = {
			DIRECTION_SOUTH, DIRECTION_NORTH, DIRECTION_EAST, DIRECTION_WEST,
			DIRECTION_SOUTHWEST, DIRECTION_SOUTHEAST, DIRECTION_NORTHWEST, DIRECTION_NORTHEAST,
		};
		for (Direction direction : preference) {
			const Position candidate = getNextPosition(direction, upper);
			if (walkNodes.find(positionKey(candidate)) != walkNodes.end()) return candidate;
		}
		return std::nullopt;
	};
	map.forEachTile([this, &addEdge, &upperDestination](const Tile& tile) {
		const Position& target = tile.getPosition();
		struct Transition {
			uint16_t itemId;
			uint16_t expectedItemId;
			PlayerBotTopologyPortalAction action;
		};
		std::vector<Transition> transitions;
		Item* ground = tile.getGround();
		if (ground && contains(ropeSpotIds, ground->getID())) {
			transitions.push_back({ground->getID(), 0, PlayerBotTopologyPortalAction::UseRope});
		}
		const Item* passageItem = playerBotShovelPassageItem(tile);
		const auto shovelPassage = passageItem ? playerBotShovelPassage(passageItem->getID()) : std::nullopt;
		if (shovelPassage && passageItem->getID() == shovelPassage->closedItemId) {
			transitions.push_back({shovelPassage->closedItemId, shovelPassage->openItemId,
			                       PlayerBotTopologyPortalAction::UseShovel});
		}
		if (const TileItemVector* items = tile.getItemList()) {
			for (const Item* item : *items) {
				if (contains(ladderIds, item->getID()) || contains(downUseIds, item->getID())) {
					transitions.push_back({item->getID(), 0, PlayerBotTopologyPortalAction::Use});
				}
			}
		}
		for (const Transition& transition : transitions) {
			std::optional<Position> destination;
			if (transition.action == PlayerBotTopologyPortalAction::UseShovel ||
			    contains(downUseIds, transition.itemId)) {
				if (target.z < MAP_MAX_LAYERS - 1) destination = Position(target.x, target.y, target.z + 1);
			} else {
				destination = upperDestination(target);
			}
			if (!destination) continue;
			const auto destinationNode = walkNodes.find(positionKey(*destination));
			if (destinationNode == walkNodes.end()) continue;
			for (Direction direction : directions) {
				const Position approach = getNextPosition(direction, target);
				const auto sourceNode = walkNodes.find(positionKey(approach));
				if (sourceNode == walkNodes.end()) continue;
				PlayerBotTopologyPortal portal{approach, target, *destination, DIRECTION_NONE,
				                                  transition.action, transition.itemId,
				                                  transition.expectedItemId};
				addEdge(sourceNode->second, destinationNode->second, portal);
			}
		}
	});

	nodeComponents.resize(nodeCount);
	std::unordered_map<uint32_t, uint32_t> componentIds;
	componentIds.reserve(nodeCount);
	for (uint32_t node = 0; node < nodeCount; ++node) {
		const uint32_t root = nodeRoots[node];
		auto [entry, inserted] = componentIds.emplace(root, components);
		if (inserted) ++components;
		nodeComponents[node] = entry->second;
	}
	componentEdges.resize(components);
	for (uint32_t sourceNode = 0; sourceNode < edges.size(); ++sourceNode) {
		const uint32_t sourceComponent = nodeComponents[sourceNode];
		for (const Edge& edge : edges[sourceNode]) {
			const uint32_t destinationComponent = nodeComponents[edge.destinationNode];
			if (sourceComponent == destinationComponent) continue;
			componentEdges[sourceComponent].push_back({destinationComponent, edge.portal});
		}
	}
}

std::optional<uint32_t> PlayerBotTopology::walkComponent(const Position& position) const
{
	const auto node = walkNodes.find(positionKey(position));
	if (node == walkNodes.end()) return std::nullopt;
	return nodeComponents[node->second];
}

bool PlayerBotTopology::sameWalkComponent(const Position& left, const Position& right) const
{
	const std::optional<uint32_t> leftComponent = walkComponent(left);
	const std::optional<uint32_t> rightComponent = walkComponent(right);
	return leftComponent && rightComponent && *leftComponent == *rightComponent;
}

bool PlayerBotTopology::sameWalkNode(const Position& left, const Position& right) const
{
	const auto leftNode = walkNodes.find(positionKey(left));
	const auto rightNode = walkNodes.find(positionKey(right));
	return leftNode != walkNodes.end() && rightNode != walkNodes.end() && leftNode->second == rightNode->second;
}

PlayerBotTopologyDistances PlayerBotTopology::distancesFrom(const Position& start, bool canUseRope,
	                                                         bool canUseShovel, uint32_t playerLevel) const
{
	PlayerBotTopologyDistances result;
	result.generation = topologyGeneration;
	result.costs.assign(edges.size(), std::numeric_limits<uint32_t>::max());
	const auto startEntry = walkNodes.find(positionKey(start));
	if (startEntry == walkNodes.end()) return result;
	std::queue<uint32_t> open;
	result.costs[startEntry->second] = 0;
	open.push(startEntry->second);
	while (!open.empty()) {
		const uint32_t current = open.front();
		open.pop();
		for (const Edge& edge : edges[current]) {
			if (!canTraversePortal(liveMap, edge.portal, canUseRope, canUseShovel) ||
			    edge.portal.minimumLevel > playerLevel) continue;
			if (result.costs[edge.destinationNode] != std::numeric_limits<uint32_t>::max()) continue;
			result.costs[edge.destinationNode] = result.costs[current] + 1;
			open.push(edge.destinationNode);
		}
	}
	return result;
}

std::optional<uint32_t> PlayerBotTopology::distanceTo(const PlayerBotTopologyDistances& distances,
	                                                    const Position& destination) const
{
	if (distances.generation != topologyGeneration) return std::nullopt;
	const auto destinationEntry = walkNodes.find(positionKey(destination));
	if (destinationEntry == walkNodes.end() || destinationEntry->second >= distances.costs.size()) return std::nullopt;
	const uint32_t cost = distances.costs[destinationEntry->second];
	return cost == std::numeric_limits<uint32_t>::max() ? std::nullopt : std::optional<uint32_t>(cost);
}

std::optional<uint32_t> PlayerBotTopology::distanceTo(const PlayerBotTopologyDistances& distances,
	                                                    const PlayerBotNavigationGoal& goal) const
{
	if (distances.generation != topologyGeneration) return std::nullopt;
	std::optional<uint32_t> best;
	auto include = [this, &distances, &best](const Position& position) {
		const std::optional<uint32_t> cost = distanceTo(distances, position);
		if (cost && (!best || *cost < *best)) best = cost;
	};
	if (goal.type == PlayerBotNavigationGoalType::Exact) {
		include(goal.position);
	} else if (goal.type == PlayerBotNavigationGoalType::AnyOf) {
		for (const Position& position : goal.positions) include(position);
	} else {
		for (int32_t zOffset = -goal.rangeZ; zOffset <= goal.rangeZ; ++zOffset) {
			const int32_t z = static_cast<int32_t>(goal.position.z) + zOffset;
			if (z < 0 || z >= MAP_MAX_LAYERS) continue;
			for (int32_t xOffset = -goal.rangeX; xOffset <= goal.rangeX; ++xOffset) {
				const int32_t x = static_cast<int32_t>(goal.position.x) + xOffset;
				if (x < 0 || x > std::numeric_limits<uint16_t>::max()) continue;
				for (int32_t yOffset = -goal.rangeY; yOffset <= goal.rangeY; ++yOffset) {
					const int32_t y = static_cast<int32_t>(goal.position.y) + yOffset;
					if (y < 0 || y > std::numeric_limits<uint16_t>::max()) continue;
					include(Position(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint8_t>(z)));
				}
			}
		}
	}
	return best;
}

std::shared_ptr<const PlayerBotTopologyReachability> PlayerBotTopology::reachabilityFrom(
	const Position& start, bool canUseRope, bool canUseShovel, uint32_t playerLevel) const
{
	const auto startNode = walkNodes.find(positionKey(start));
	if (startNode == walkNodes.end()) return {};
	const uint32_t startComponent = nodeComponents[startNode->second];
	const auto key = std::make_tuple(startComponent, canUseRope, canUseShovel, playerLevel);
	// Without a shovel, reachability depends on whether each known passage is
	// open right now, so an open/decay cycle cannot reuse a cached answer.
	if (canUseShovel) {
		if (auto found = reachabilityCache.find(key); found != reachabilityCache.end()) {
			if (auto cached = found->second.lock()) return cached;
			reachabilityCache.erase(found);
		}
	}
	constexpr size_t maximumReachabilityCacheEntries = 512;
	if (reachabilityCache.size() >= maximumReachabilityCacheEntries) {
		for (auto it = reachabilityCache.begin(); it != reachabilityCache.end();) {
			if (it->second.expired()) it = reachabilityCache.erase(it);
			else ++it;
		}
		if (reachabilityCache.size() >= maximumReachabilityCacheEntries) reachabilityCache.erase(reachabilityCache.begin());
	}
	auto result = std::make_shared<PlayerBotTopologyReachability>();
	result->generation = topologyGeneration;
	result->components.assign(components, 0);
	std::queue<uint32_t> open;
	result->components[startComponent] = 1;
	open.push(startComponent);
	while (!open.empty()) {
		const uint32_t current = open.front();
		open.pop();
		for (const ComponentEdge& edge : componentEdges[current]) {
			if (!canTraversePortal(liveMap, edge.portal, canUseRope, canUseShovel) ||
			    edge.portal.minimumLevel > playerLevel || result->components[edge.destination]) continue;
			result->components[edge.destination] = 1;
			open.push(edge.destination);
		}
	}
	if (canUseShovel) reachabilityCache.emplace(key, result);
	return result;
}

bool PlayerBotTopology::reachable(
	const PlayerBotTopologyReachability& reachability, const Position& destination) const
{
	if (reachability.generation != topologyGeneration) return false;
	const auto node = walkNodes.find(positionKey(destination));
	return node != walkNodes.end() && nodeComponents[node->second] < reachability.components.size() &&
	       reachability.components[nodeComponents[node->second]] != 0;
}

std::optional<PlayerBotTopologyRoute> PlayerBotTopology::route(
	const Position& start, const Position& destination, const std::set<Position>& blockedPositions,
	bool canUseRope, bool canUseShovel, uint32_t playerLevel,
	const PlayerBotNavigationCostPolicy* costPolicy) const
{
	const auto startEntry = walkNodes.find(positionKey(start));
	const auto destinationEntry = walkNodes.find(positionKey(destination));
	if (startEntry == walkNodes.end() || destinationEntry == walkNodes.end()) return std::nullopt;
	const uint32_t startNode = startEntry->second;
	const uint32_t destinationNode = destinationEntry->second;
	if (startNode == destinationNode) {
		const double danger = costPolicy ? costPolicy->dangerAt(destination) : 0;
		return PlayerBotTopologyRoute{destination, std::nullopt, 0, danger};
	}

	struct QueueEntry {
		uint64_t cost;
		uint32_t node;
		bool operator>(const QueueEntry& other) const { return cost > other.cost; }
	};
	std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> open;
	std::vector<uint64_t> costs(edges.size(), std::numeric_limits<uint64_t>::max());
	std::vector<uint32_t> dangerCosts(edges.size(), 0);
	std::vector<double> maximumDangers(edges.size(), 0);
	std::vector<std::optional<PlayerBotTopologyPortal>> firstPortals(edges.size());
	costs[startNode] = 0;
	open.push({0, startNode});
	while (!open.empty()) {
		const QueueEntry currentEntry = open.top();
		open.pop();
		if (currentEntry.cost != costs[currentEntry.node]) continue;
		const uint32_t current = currentEntry.node;
		if (current == destinationNode) break;
		for (const Edge& edge : edges[current]) {
			// The local navigator must first reach this approach before executing the portal.
			// Returning a blocked approach creates an impossible local subgoal.
			if (blockedPositions.find(edge.portal.approach) != blockedPositions.end() ||
			    blockedPositions.find(edge.portal.target) != blockedPositions.end() ||
			    blockedPositions.find(edge.portal.destination) != blockedPositions.end()) continue;
			if (!canTraversePortal(liveMap, edge.portal, canUseRope, canUseShovel) ||
			    edge.portal.minimumLevel > playerLevel) continue;
			const uint64_t edgeDanger = costPolicy ?
			    static_cast<uint64_t>(costPolicy->dangerCost(edge.portal.approach, costPolicy->topologyExposureMs)) +
			        costPolicy->dangerCost(edge.portal.destination, costPolicy->topologyExposureMs) : 0;
			const uint32_t dangerCost = static_cast<uint32_t>(std::min<uint64_t>(
			    edgeDanger, std::numeric_limits<uint32_t>::max()));
			const uint64_t newCost = currentEntry.cost + topologyEdgeMovementCost + dangerCost;
			if (newCost >= costs[edge.destinationNode]) continue;
			costs[edge.destinationNode] = newCost;
			dangerCosts[edge.destinationNode] = static_cast<uint32_t>(std::min<uint64_t>(
			    static_cast<uint64_t>(dangerCosts[current]) + dangerCost, std::numeric_limits<uint32_t>::max()));
			maximumDangers[edge.destinationNode] = std::max(maximumDangers[current], costPolicy ?
			    std::max(costPolicy->dangerAt(edge.portal.approach), costPolicy->dangerAt(edge.portal.destination)) : 0);
			firstPortals[edge.destinationNode] = current == startNode ? edge.portal : firstPortals[current];
			open.push({newCost, edge.destinationNode});
		}
	}
	if (costs[destinationNode] == std::numeric_limits<uint64_t>::max() || !firstPortals[destinationNode]) return std::nullopt;
	const PlayerBotTopologyPortal& portal = *firstPortals[destinationNode];
	const double destinationDanger = costPolicy ? costPolicy->dangerAt(destination) : 0;
	return PlayerBotTopologyRoute{portal.approach, portal,
	    static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(dangerCosts[destinationNode]) +
	        (costPolicy ? costPolicy->dangerCost(destination, 1000) : 0), std::numeric_limits<uint32_t>::max())),
	    std::max(maximumDangers[destinationNode], destinationDanger)};
}
