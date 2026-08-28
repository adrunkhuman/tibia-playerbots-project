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
#include <numeric>
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
	constexpr std::array<uint16_t, 4> shovelHoleIds = {468, 481, 483, 7932};

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
			if (!item) return false;
			const ItemType& type = Item::items[item->getID()];
			return playerBotIsTraversableDoor(*item) &&
			       type.description != "It is locked.";
		});
		return door == items->end() ? nullptr : *door;
	}

	uint32_t doorMinimumLevel(const Item& door)
	{
		const ItemType& type = Item::items[door.getID()];
		return type.levelDoor == 0 || door.getActionId() < type.levelDoor ? 0 :
		       door.getActionId() - type.levelDoor;
	}

	bool isStaticWalkTile(const Tile& tile)
	{
		if (!tile.getGround() || tile.hasFlag(TILESTATE_TELEPORT) || dynamic_cast<const HouseTile*>(&tile)) return false;
		return !tile.hasFlag(TILESTATE_BLOCKSOLID) || staticDoor(tile);
	}
}

PlayerBotTopology& PlayerBotTopology::instance()
{
	static PlayerBotTopology topology;
	return topology;
}

void PlayerBotTopology::build(const Map& map)
{
	++topologyGeneration;
	walkNodes.clear();
	nodeComponents.clear();
	edges.clear();
	topologyPortals.clear();
	components = 0;
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

	std::vector<uint32_t> nodeParents(nodeCount);
	std::iota(nodeParents.begin(), nodeParents.end(), 0);
	auto findNodeRoot = [&nodeParents](uint32_t node) {
		uint32_t root = node;
		while (nodeParents[root] != root) root = nodeParents[root];
		while (nodeParents[node] != node) {
			const uint32_t parent = nodeParents[node];
			nodeParents[node] = root;
			node = parent;
		}
		return root;
	};
	auto joinNodes = [&nodeParents, &findNodeRoot](uint32_t left, uint32_t right) {
		left = findNodeRoot(left);
		right = findNodeRoot(right);
		if (left != right) nodeParents[right] = left;
	};
	auto addEdge = [this](uint32_t from, uint32_t to, const PlayerBotTopologyPortal& portal) {
		auto& outgoing = edges[from];
		if (std::none_of(outgoing.begin(), outgoing.end(), [to, &portal](const Edge& edge) {
			return edge.destinationNode == to && edge.portal.target == portal.target &&
			       edge.portal.destination == portal.destination && edge.portal.action == portal.action &&
			       edge.portal.itemId == portal.itemId;
		})) {
			outgoing.push_back({to, portal});
			topologyPortals.push_back(portal);
		}
	};
	map.forEachTile([this, &map, &addEdge, &joinNodes](const Tile& tile) {
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
			PlayerBotTopologyPortal portal{position, transition.target, transition.destination, direction};
			if (targetDoor) {
				portal.action = PlayerBotTopologyPortalAction::UseDoor;
				portal.itemId = targetDoor->getID();
				portal.minimumLevel = doorMinimumLevel(*targetDoor);
			}
			addEdge(from, entry->second, portal);
			joinNodes(from, entry->second);
		}
	});
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
		std::vector<std::pair<uint16_t, PlayerBotTopologyPortalAction>> transitions;
		Item* ground = tile.getGround();
		if (ground && contains(ropeSpotIds, ground->getID())) {
			transitions.emplace_back(ground->getID(), PlayerBotTopologyPortalAction::UseRope);
		}
		if (ground && contains(shovelHoleIds, ground->getID())) {
			transitions.emplace_back(ground->getID(), PlayerBotTopologyPortalAction::UseShovel);
		}
		if (const TileItemVector* items = tile.getItemList()) {
			for (const Item* item : *items) {
				if (contains(ladderIds, item->getID()) || contains(downUseIds, item->getID())) {
					transitions.emplace_back(item->getID(), PlayerBotTopologyPortalAction::Use);
				}
			}
		}
		for (const auto& [itemId, action] : transitions) {
			std::optional<Position> destination;
			if (action == PlayerBotTopologyPortalAction::UseShovel || contains(downUseIds, itemId)) {
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
				PlayerBotTopologyPortal portal{approach, target, *destination, DIRECTION_NONE, action, itemId};
				addEdge(sourceNode->second, destinationNode->second, portal);
			}
		}
	});

	nodeComponents.resize(nodeCount);
	std::unordered_map<uint32_t, uint32_t> componentIds;
	componentIds.reserve(nodeCount);
	for (uint32_t node = 0; node < nodeCount; ++node) {
		const uint32_t root = findNodeRoot(node);
		auto [entry, inserted] = componentIds.emplace(root, components);
		if (inserted) ++components;
		nodeComponents[node] = entry->second;
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
			if ((edge.portal.action == PlayerBotTopologyPortalAction::UseRope && !canUseRope) ||
			    (edge.portal.action == PlayerBotTopologyPortalAction::UseShovel && !canUseShovel) ||
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
			if (blockedPositions.find(edge.portal.target) != blockedPositions.end() ||
			    blockedPositions.find(edge.portal.destination) != blockedPositions.end()) continue;
			if ((edge.portal.action == PlayerBotTopologyPortalAction::UseRope && !canUseRope) ||
			    (edge.portal.action == PlayerBotTopologyPortalAction::UseShovel && !canUseShovel) ||
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
