#include "playerbotplanningbudget.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

using Budget = PlayerBotPlanningBudget;
using namespace std::chrono;

int main()
{
	const Budget::Time epoch{};
	const auto us = [](int64_t value) { return microseconds(value); };
	{
		Budget budget(epoch);
		assert(!budget.request(0, epoch).queued);
		assert(budget.request(1, epoch).admitted);
		assert(!budget.request(1, epoch).admitted);
		assert(budget.request(2, epoch).queued);
		assert(budget.request(3, epoch).queued);
		assert(!budget.finish(3, epoch + us(5000)));
		assert(budget.finish(1, epoch + us(5000)));
		assert(budget.metrics(epoch + us(5000)).consumedUs == 5000);
		assert(budget.request(2, epoch + us(5000)).admitted);
		budget.cancel(3, epoch + us(5000));
		assert(budget.metrics(epoch + us(5000)).waiting == 0);
		budget.cancel(2, epoch + us(7000));
		assert(budget.metrics(epoch + us(7000)).consumedUs == 7000);
	}
	{
		Budget budget(epoch);
		assert(budget.request(1, epoch).admitted);
		assert(budget.request(2, epoch).queued);
		// One cooperative slice can overrun the whole burst. The next one
		// cannot start until its debt and the admission threshold are repaid.
		assert(budget.finish(1, epoch + us(50000)));
		auto denied = budget.request(2, epoch + us(50000));
		assert(denied.queued && denied.debtUs == 13000);
		assert(denied.wait == us(28000));
		assert(!budget.request(1, epoch + us(50000)).admitted);
		assert(!budget.request(2, epoch + us(77999)).admitted);
		assert(budget.request(2, epoch + us(78000)).admitted);
		assert(budget.finish(2, epoch + us(83000)));
		assert(budget.metrics(epoch + us(83000)).consumedUs == 55000);
	}
	{
		Budget budget(epoch);
		assert(budget.request(1, epoch).admitted);
		assert(budget.request(2, epoch).queued);
		assert(budget.finish(1, epoch + seconds(5)));
		const auto wait = budget.request(2, epoch + seconds(5)).wait;
		assert(wait > seconds(4));
		assert(budget.request(3, epoch + seconds(5) + Budget::headLease).queued);
		assert(budget.request(2, epoch + seconds(5) + wait).admitted);
		assert(budget.finish(2, epoch + seconds(5) + wait));
	}
	{
		Budget budget(epoch);
		assert(budget.request(1, epoch).admitted);
		assert(budget.request(2, epoch).queued);
		assert(budget.request(3, epoch).queued);
		assert(budget.finish(1, epoch));
		budget.cancel(2, epoch);
		assert(budget.request(3, epoch).admitted);
		assert(budget.finish(3, epoch));
		// A disappeared head has a bounded lease. An active call, even if
		// very long, never expires queued requests while it blocks dispatch.
		assert(budget.request(4, epoch).admitted);
		assert(budget.request(5, epoch).queued);
		assert(budget.finish(4, epoch + seconds(2)));
		assert(budget.request(5, epoch + seconds(2)).queued);
		budget.cancel(5, epoch + seconds(2));
	}
	{
		Budget budget(epoch);
		assert(budget.request(1, epoch).admitted);
		assert(budget.request(2, epoch).queued);
		assert(budget.request(3, epoch).queued);
		assert(budget.finish(1, epoch));
		// The head never returns, but another request can eventually pass it.
		assert(budget.request(3, epoch + Budget::headLease - us(1)).queued);
		assert(budget.request(3, epoch + Budget::headLease).admitted);
		assert(budget.finish(3, epoch + Budget::headLease));
	}
	{
		Budget budget(epoch);
		assert(budget.request(1, epoch).admitted);
		for (uint64_t id = 2; id <= Budget::maxWaiting + 1; ++id) {
			assert(budget.request(id, epoch).queued);
		}
		assert(!budget.request(1000, epoch).queued);
		budget.cancel(2, epoch);
		assert(budget.request(1000, epoch).queued);
	}
	{
		// Ten simultaneous requesters, each consuming 5 seconds total in
		// 5 ms cooperative slices. Virtual time avoids a 100-second test.
		Budget budget(epoch);
		std::array<int, 10> slices{};
		Budget::Time now = epoch;
		int64_t last = 0;
		for (uint64_t id = 1; id <= 10; ++id) {
			const auto attempt = budget.request(id, now);
			if (id == 1) assert(attempt.admitted);
			else assert(attempt.queued);
		}
		assert(budget.finish(1, now + us(5000)));
		now += us(5000);
		slices[0]++;
		int count = 1;
		while (count < 10000) {
			bool advanced = false;
			Budget::Microseconds earliest = Budget::pollDelay;
			for (uint64_t id = 1; id <= 10; ++id) {
				if (slices[id - 1] == 1000) continue;
				const auto admission = budget.request(id, now);
				if (!admission.admitted) {
					assert(admission.queued);
					earliest = std::min(earliest, admission.wait);
					continue;
				}
				assert(budget.finish(id, now + us(5000)));
				now += us(5000);
				++slices[id - 1];
				++count;
				last = duration_cast<microseconds>(now - epoch).count();
				// Actual charged service across the dispatcher, with no per-bot
				// allowance. Only the in-flight slice may exceed the envelope.
				assert(budget.metrics(now).consumedUs <=
				       Budget::burstUs + last / 2 + 5000);
				advanced = true;
				// FIFO means no controller gets two turns before all ten get one.
				int minSlices = slices[0], maxSlices = slices[0];
				for (int value : slices) {
					minSlices = std::min(minSlices, value);
					maxSlices = std::max(maxSlices, value);
				}
				assert(maxSlices - minSlices <= 1);
				break;
			}
			if (!advanced) now += earliest;
		}
		for (int value : slices) assert(value == 1000);
		const auto state = budget.metrics(now);
		assert(state.consumedUs == 50000000);
		assert(state.waiting == 0 && state.debtUs <= 4000);
		assert(last >= 99000000 && last <= 101000000);
	}
	std::cout << "planning budget contracts passed\n";
}
