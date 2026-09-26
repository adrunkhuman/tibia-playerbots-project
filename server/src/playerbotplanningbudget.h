#ifndef FS_PLAYERBOTPLANNINGBUDGET_H
#define FS_PLAYERBOTPLANNINGBUDGET_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

// One instance per dispatcher, shared by all hunt controllers. All calls are made on
// that dispatcher; this is cooperative admission, not a deadline or preemption.
// Credit refills at 50% of elapsed time (50 ms per 100 ms), capped at a 12 ms
// burst. Actual elapsed planning time is charged, including any slice overrun.
// Call request before selectHuntRegion, finish after it (including failure/yield),
// and cancel when a controller stops or abandons planning. Use a stable, nonzero
// controller ID; retry a denied request after the returned delay.
class PlayerBotPlanningBudget {
public:
	using Clock = std::chrono::steady_clock;
	using Time = Clock::time_point;
	using Microseconds = std::chrono::microseconds;

	static constexpr int64_t burstUs = 12000;
	static constexpr int64_t admissionUs = 1000;
	static constexpr size_t maxWaiting = 256;
	static constexpr Microseconds pollDelay{10000};
	static constexpr Microseconds headLease{250000};

	struct Result {
		bool admitted = false;
		bool queued = false; // false on capacity exhaustion or invalid ID
		Microseconds wait{};
		int64_t creditUs = 0;
		int64_t debtUs = 0;
		int64_t consumedUs = 0;
		size_t waiting = 0;
	};

	explicit PlayerBotPlanningBudget(Time now) : updated(now), headDeadline(now + headLease) {}

	Result request(uint64_t id, Time now)
	{
		refill(now);
		if (!id || id == active) return result(false, false, pollDelay);
		// An absent head cannot hold the queue forever. Debt waits extend its lease
		// to the next eligible time, so legitimate long waits do not lose their turn.
		pruneHead(now);
		auto it = std::find(waiting.begin(), waiting.end(), id);
		if (it == waiting.end()) {
			if (waiting.size() == maxWaiting) return result(false, false, pollDelay);
			waiting.push_back(id);
			if (waiting.size() == 1) headDeadline = now + headLease;
		}
		if (active || waiting.front() != id) return result(false, true, pollDelay);
		if (tokensUs < admissionUs) {
			// One microsecond of credit is earned every two microseconds elapsed.
			const Microseconds delay{2 * (admissionUs - tokensUs)};
			headDeadline = now + delay + headLease;
			return result(false, true, delay);
		}
		waiting.pop_front();
		active = id;
		started = now;
		if (!waiting.empty()) headDeadline = now + headLease;
		return result(true, false, Microseconds::zero());
	}

	// Charge measured elapsed service, including overruns. Returns false for a
	// mismatched/non-active ID; in that case no state is changed.
	bool finish(uint64_t id, Time now)
	{
		if (!id || id != active) return false;
		const Time end = std::max(now, started);
		const int64_t cost = std::chrono::duration_cast<Microseconds>(end - started).count();
		// Charge before refilling: the dispatcher earns credit during this
		// synchronous work too, even when the slice exceeds the burst cap.
		tokensUs -= cost;
		consumedUs += cost;
		refill(end);
		active = 0;
		if (!waiting.empty()) headDeadline = end + headLease;
		return true;
	}

	// Cancel a queued request immediately. Canceling an active request charges
	// elapsed work exactly as finish does; call only after its work has ended.
	void cancel(uint64_t id, Time now)
	{
		if (!id) return;
		if (id == active) {
			finish(id, now);
			return;
		}
		refill(now);
		const auto it = std::find(waiting.begin(), waiting.end(), id);
		if (it == waiting.end()) return;
		const bool head = it == waiting.begin();
		waiting.erase(it);
		if (head && !waiting.empty()) headDeadline = now + headLease;
	}

	Result metrics(Time now)
	{
		refill(now);
		return result(false, false, Microseconds::zero());
	}

	bool isActive(uint64_t id) const { return id != 0 && id == active; }

	// Own one admitted call; explicit finish can precede terminal handling,
	// while destruction still charges early returns and exception paths.
	class Charge {
	public:
		Charge(PlayerBotPlanningBudget& budget, uint64_t id) : budget(budget), id(id) {}
		Charge(const Charge&) = delete;
		Charge& operator=(const Charge&) = delete;
		~Charge() { finish(); }
		void finish()
		{
			if (id) {
				budget.finish(id, Clock::now());
				id = 0;
			}
		}
	private:
		PlayerBotPlanningBudget& budget;
		uint64_t id;
	};

private:
	void refill(Time now)
	{
		if (now <= updated) return; // Monotonic caller clock; tolerate a stale observation.
		const int64_t elapsed = std::chrono::duration_cast<Microseconds>(now - updated).count();
		if (elapsed <= 0) return;
		// Preserve sub-microsecond clock ticks rather than losing refill over calls.
		updated += Microseconds(elapsed);
		// Saturating the credit first avoids overflow after a long idle period.
		const int64_t needed = burstUs - tokensUs;
		tokensUs = elapsed / 2 >= needed ? burstUs : tokensUs + elapsed / 2;
		// Carry the odd microsecond into the next update.
		if (elapsed % 2 && tokensUs < burstUs) {
			++refillRemainder;
			if (refillRemainder == 2) { ++tokensUs; refillRemainder = 0; }
		}
		if (tokensUs == burstUs) refillRemainder = 0;
	}

	void pruneHead(Time now)
	{
		if (active) return;
		while (!waiting.empty() && now >= headDeadline) {
			waiting.pop_front();
			headDeadline = now + headLease;
		}
	}

	Result result(bool admitted, bool queued, Microseconds delay) const
	{
		return {admitted, queued, delay, std::max<int64_t>(0, tokensUs),
		        std::max<int64_t>(0, -tokensUs), consumedUs, waiting.size()};
	}

	Time updated;
	Time started{};
	Time headDeadline;
	int64_t tokensUs = burstUs;
	int64_t consumedUs = 0;
	int refillRemainder = 0;
	uint64_t active = 0;
	std::deque<uint64_t> waiting;
};

// Inline accessor has one function-local static across translation units.
// Dispatcher-only: neither access nor the budget is thread-safe.
inline PlayerBotPlanningBudget& playerBotHuntPlanningBudget()
{
	static PlayerBotPlanningBudget budget(PlayerBotPlanningBudget::Clock::now());
	return budget;
}

#endif
