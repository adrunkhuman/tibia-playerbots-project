#ifndef FS_PLAYERBOTROUTECORRIDOR_H
#define FS_PLAYERBOTROUTECORRIDOR_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

// Directed graph-hop allowance around itinerary nodes. Node IDs and the
// outgoing callback must refer to the same topology generation. A missing
// tile-to-node mapping is not evidence that the tile is unreachable: callers
// must eventually try an unrestricted search rather than reject it forever.
class PlayerBotRouteCorridor {
public:
	PlayerBotRouteCorridor(uint32_t nodeCount, const std::vector<uint32_t>& seeds)
	    : allowed(nodeCount, 0), seeded(nodeCount, 0)
	{
		addSeeds(seeds);
	}

	void addSeeds(const std::vector<uint32_t>& nodes)
	{
		for (uint32_t node : nodes) {
			if (node >= allowed.size() || seeded[node]) continue;
			seeded[node] = allowed[node] = 1;
			seeds.push_back(node);
		}
	}

	// Recompute from the union of seeds; repeated calls only add membership.
	// Outgoing(node) supplies edges with a destinationNode field. After adding
	// seeds, call widen again to extend the new seeds to the desired radius.
	template<class Outgoing>
	void widen(uint32_t radius, Outgoing&& outgoing)
	{
		std::vector<uint8_t> visited(allowed.size(), 0);
		std::queue<std::pair<uint32_t, uint32_t>> pending;
		for (uint32_t node : seeds) {
			visited[node] = 1;
			pending.emplace(node, 0);
		}
		while (!pending.empty()) {
			const auto [node, depth] = pending.front();
			pending.pop();
			allowed[node] = 1;
			if (depth == radius) continue;
			for (const auto& edge : outgoing(node)) {
				const uint32_t next = edge.destinationNode;
				if (next >= allowed.size() || visited[next]) continue;
				visited[next] = 1;
				pending.emplace(next, depth + 1);
			}
		}
	}

	bool contains(std::optional<uint32_t> node) const
	{
		return node && *node < allowed.size() && allowed[*node];
	}

	size_t seedCount() const { return seeds.size(); }
	uint32_t nodeCount() const { return static_cast<uint32_t>(allowed.size()); }

private:
	std::vector<uint8_t> allowed;
	std::vector<uint8_t> seeded;
	std::vector<uint32_t> seeds;
};

#endif
