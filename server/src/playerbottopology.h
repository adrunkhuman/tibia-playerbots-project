/** Shared static coarse topology for playerbot route planning. */
#ifndef FS_PLAYERBOTTOPOLOGY_H
#define FS_PLAYERBOTTOPOLOGY_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "position.h"
#include "playerbotshortcutheuristic.h"

class Map;
struct PlayerBotNavigationGoal;
struct PlayerBotNavigationCostPolicy;

// A snapshot owns only values. valid()/estimate() synchronize pending map
// notifications; ordinary changes retain validity. After a shortcut change,
// restart the search with a fresh snapshot (including its queued priorities).
class PlayerBotTopologyHeuristic {
	friend class PlayerBotTopology;
public:
	bool valid() const;
	uint32_t estimate(Position position) const;
private:
	std::shared_ptr<const PlayerBotShortcutHeuristic> potential;
	uint64_t generation = 0, shortcutRevision = 0;
};

class PlayerBotTopologyDistances {
	friend class PlayerBotTopology;

	private:
		std::vector<uint32_t> costs;
		uint64_t generation = 0;
};

class PlayerBotTopologyReachability {
	friend class PlayerBotTopology;

	private:
		std::vector<uint8_t> components;
		uint64_t generation = 0;
};

enum class PlayerBotTopologyPortalAction : uint8_t {
	Move,
	Use,
	UseDoor,
	UseRope,
	UseShovel,
};

struct PlayerBotTopologyPortal {
	Position approach;
	Position target;
	Position destination;
	Direction direction = DIRECTION_NONE;
	PlayerBotTopologyPortalAction action = PlayerBotTopologyPortalAction::Move;
	uint16_t itemId = 0;
	uint16_t expectedItemId = 0;
	uint32_t minimumLevel = 0;
};

struct PlayerBotTopologyRoute {
	Position waypoint;
	std::optional<PlayerBotTopologyPortal> portal;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
};

struct PlayerBotTopologyItinerary {
	Position destination;
	std::vector<PlayerBotTopologyPortal> portals;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
	// Includes the start and destination nodes, even for a zero-crossing route.
	std::vector<uint32_t> nodes;
};

struct PlayerBotTopologyEdge {
	uint32_t destinationNode = 0;
	PlayerBotTopologyPortal portal;
};

struct PlayerBotTopologyComponentArc {
	uint32_t source = 0;
	uint32_t destination = 0;
	bool unconditional = false;
};

inline std::vector<uint32_t> playerBotTopologyBidirectionalComponents(
    uint32_t nodeCount, const std::vector<PlayerBotTopologyComponentArc>& arcs)
{
	std::vector<uint32_t> parents(nodeCount);
	for (uint32_t node = 0; node < nodeCount; ++node) parents[node] = node;
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
	auto join = [&parents, &findRoot](uint32_t left, uint32_t right) {
		left = findRoot(left);
		right = findRoot(right);
		if (left != right) parents[right] = left;
	};
	std::set<std::pair<uint32_t, uint32_t>> unconditional;
	for (const PlayerBotTopologyComponentArc& arc : arcs) {
		if (arc.unconditional && arc.source < nodeCount && arc.destination < nodeCount) {
			unconditional.emplace(arc.source, arc.destination);
		}
	}
	for (const auto& [source, destination] : unconditional) {
		if (unconditional.find({destination, source}) != unconditional.end()) join(source, destination);
	}
	for (uint32_t node = 0; node < nodeCount; ++node) parents[node] = findRoot(node);
	return parents;
}

class PlayerBotTopology
{
	public:
		static PlayerBotTopology& instance();

		void build(const Map& map);
		void invalidate();
		PlayerBotTopologyHeuristic heuristicTo(const std::vector<Position>& exactGoals) const;
		// Lower bound on movement cost FROM start, with extra directed shortcuts
		// such as NPC travel. The relaxed grid is symmetric, so reversing every
		// shortcut turns the reverse potential into a forward one.
		PlayerBotTopologyHeuristic heuristicFrom(const Position& start,
		                                         const std::vector<PlayerBotShortcut>& extraShortcuts) const;
		std::optional<uint32_t> walkNode(const Position& position) const;
		// Directed adjacency for graph-hop corridor expansion. References and
		// node IDs remain valid only until the next topology build/invalidation.
		const std::vector<PlayerBotTopologyEdge>& outgoing(uint32_t node) const;
		std::optional<uint32_t> walkComponent(const Position& position) const;
		bool sameWalkComponent(const Position& left, const Position& right) const;
		bool sameWalkNode(const Position& left, const Position& right) const;
		std::optional<PlayerBotTopologyRoute> route(const Position& start, const Position& destination,
		                                                const std::set<Position>& blockedPositions,
		                                                bool canUseRope = true, bool canUseShovel = true,
		                                                uint32_t playerLevel = std::numeric_limits<uint32_t>::max(),
		                                                const PlayerBotNavigationCostPolicy* costPolicy = nullptr,
		                                                uint64_t* expandedNodes = nullptr) const;
		// Retain the full sequence of crossings; an empty sequence means the
		// selected destination is in the starting walk piece.
		std::optional<PlayerBotTopologyItinerary> routeToAny(
		    const Position& start, const std::vector<Position>& destinations,
		    const std::set<Position>& blockedPositions, bool canUseRope = true, bool canUseShovel = true,
		    uint32_t playerLevel = std::numeric_limits<uint32_t>::max(),
		    const PlayerBotNavigationCostPolicy* costPolicy = nullptr, uint64_t* expandedNodes = nullptr,
		    const std::set<std::pair<uint32_t, uint32_t>>* excludedArcs = nullptr) const;
		PlayerBotTopologyDistances distancesFrom(const Position& start, bool canUseRope = true,
		                                               bool canUseShovel = true,
		                                               uint32_t playerLevel = std::numeric_limits<uint32_t>::max(),
		                                               uint64_t* expandedNodes = nullptr) const;
		std::optional<uint32_t> distanceTo(const PlayerBotTopologyDistances& distances,
		                                   const Position& destination) const;
		std::optional<uint32_t> distanceTo(const PlayerBotTopologyDistances& distances,
		                                   const PlayerBotNavigationGoal& goal) const;
		std::shared_ptr<const PlayerBotTopologyReachability> reachabilityFrom(
		    const Position& start, bool canUseRope = true, bool canUseShovel = true,
		    uint32_t playerLevel = std::numeric_limits<uint32_t>::max()) const;
		bool reachable(const PlayerBotTopologyReachability& reachability, const Position& destination) const;
		const std::vector<PlayerBotTopologyPortal>& portals() const { return topologyPortals; }
		size_t tileCount() const { return walkNodes.size(); }
		uint32_t componentCount() const { return components; }
		uint32_t nodeCount() const { return static_cast<uint32_t>(edges.size()); }
		uint64_t generation() const { return topologyGeneration; }

	private:
		friend class PlayerBotTopologyHeuristic;
		using Edge = PlayerBotTopologyEdge;
		void synchronizeShortcuts() const;
		mutable PlayerBotShortcutIndex shortcutIndex;
		Position minimumPosition, maximumPosition;
		struct ComponentEdge {
			uint32_t destination = 0;
			PlayerBotTopologyPortal portal;
		};

		const Map* liveMap = nullptr;
		std::unordered_map<uint64_t, uint32_t> walkNodes;
		std::vector<uint32_t> nodeComponents;
		std::vector<std::vector<Edge>> edges;
		std::vector<std::vector<ComponentEdge>> componentEdges;
		mutable std::map<std::tuple<uint32_t, bool, bool, uint32_t>,
		                 std::weak_ptr<const PlayerBotTopologyReachability>> reachabilityCache;
		std::vector<PlayerBotTopologyPortal> topologyPortals;
		uint32_t components = 0;
		uint64_t topologyGeneration = 0;
};

#endif
