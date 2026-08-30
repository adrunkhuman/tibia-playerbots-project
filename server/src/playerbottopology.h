/** Shared static coarse topology for playerbot route planning. */
#ifndef FS_PLAYERBOTTOPOLOGY_H
#define FS_PLAYERBOTTOPOLOGY_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <unordered_map>
#include <vector>

#include "position.h"

class Map;
struct PlayerBotNavigationGoal;
struct PlayerBotNavigationCostPolicy;

class PlayerBotTopologyDistances {
	friend class PlayerBotTopology;

	private:
		std::vector<uint32_t> costs;
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
	uint32_t minimumLevel = 0;
};

struct PlayerBotTopologyRoute {
	Position waypoint;
	std::optional<PlayerBotTopologyPortal> portal;
	uint32_t dangerCost = 0;
	double maximumHealthLossPerSecond = 0;
};

class PlayerBotTopology
{
	public:
		static PlayerBotTopology& instance();

		void build(const Map& map);
		std::optional<uint32_t> walkComponent(const Position& position) const;
		bool sameWalkComponent(const Position& left, const Position& right) const;
		bool sameWalkNode(const Position& left, const Position& right) const;
		std::optional<PlayerBotTopologyRoute> route(const Position& start, const Position& destination,
		                                                const std::set<Position>& blockedPositions,
		                                                bool canUseRope = true, bool canUseShovel = true,
		                                                uint32_t playerLevel = std::numeric_limits<uint32_t>::max(),
		                                                const PlayerBotNavigationCostPolicy* costPolicy = nullptr) const;
		PlayerBotTopologyDistances distancesFrom(const Position& start, bool canUseRope = true,
		                                               bool canUseShovel = true,
		                                               uint32_t playerLevel = std::numeric_limits<uint32_t>::max()) const;
		std::optional<uint32_t> distanceTo(const PlayerBotTopologyDistances& distances,
		                                   const Position& destination) const;
		std::optional<uint32_t> distanceTo(const PlayerBotTopologyDistances& distances,
		                                   const PlayerBotNavigationGoal& goal) const;
		const std::vector<PlayerBotTopologyPortal>& portals() const { return topologyPortals; }
		size_t tileCount() const { return walkNodes.size(); }
		uint32_t componentCount() const { return components; }
		uint32_t nodeCount() const { return static_cast<uint32_t>(edges.size()); }
		uint64_t generation() const { return topologyGeneration; }

	private:
		struct Edge {
			uint32_t destinationNode = 0;
			PlayerBotTopologyPortal portal;
		};

		std::unordered_map<uint64_t, uint32_t> walkNodes;
		std::vector<uint32_t> nodeComponents;
		std::vector<std::vector<Edge>> edges;
		std::vector<PlayerBotTopologyPortal> topologyPortals;
		uint32_t components = 0;
		uint64_t topologyGeneration = 0;
};

#endif
