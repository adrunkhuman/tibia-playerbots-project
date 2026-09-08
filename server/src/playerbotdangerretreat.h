/** Pure danger-return combat policy. Navigation resets must not end retreat. */
#ifndef FS_PLAYERBOTDANGERRETREAT_H
#define FS_PLAYERBOTDANGERRETREAT_H

#include <chrono>
#include <cstdint>
#include <set>

class PlayerBotDangerRetreat
{
	public:
		void begin() { retreating = true; }
		void finish() { retreating = false; attempted.clear(); }
		bool active() const { return retreating; }
		bool allowsDefense(uint32_t id, bool routeCritical) const
		{
			return !retreating || (routeCritical && attempted.count(id) == 0);
		}
		void beginDefense(uint32_t id, std::chrono::steady_clock::time_point now)
		{
			if (!retreating) return;
			attempted.insert(id);
			deadline = now + std::chrono::seconds(5);
		}
		bool defenseExpired(std::chrono::steady_clock::time_point now) const
		{
			return retreating && now >= deadline;
		}

	private:
		bool retreating = false;
		// One short attempt per blocker per return; timeout must not restart combat.
		std::set<uint32_t> attempted;
		std::chrono::steady_clock::time_point deadline;
};

#endif
