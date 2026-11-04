/** Expected physical hit after the engine's random armor and shield blocks.
 * This is a cost expectation, not a burst/survival bound.
 */
#ifndef FS_PLAYERBOTPHYSICALDAMAGE_H
#define FS_PLAYERBOTPHYSICALDAMAGE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

inline double playerBotExpectedPhysicalHit(int32_t minimum, int32_t maximum, int32_t armor, int32_t defense)
{
	if (minimum > maximum) std::swap(minimum, maximum);
	minimum = std::max(0, minimum);
	maximum = std::max(minimum, maximum);
	armor = std::max(0, armor);
	defense = std::max(0, defense);
	// Creature::blockHit: both ranges are inclusive; small armor blocks one.
	const int32_t armorLow = armor > 3 ? armor / 2 : armor > 0 ? 1 : 0;
	const int32_t armorHigh = armor > 3 ? armor - (armor % 2 + 1) : armorLow;
	const int32_t defenseLow = defense / 2;
	const int64_t minimumBlock = int64_t(armorLow) + defenseLow;
	const int64_t maximumBlock = int64_t(armorHigh) + defense;
	auto afterBlock = [&](int64_t damage) {
		if (damage <= minimumBlock) return 0.0;
		if (damage >= maximumBlock) return damage - (minimumBlock + maximumBlock) / 2.0;
		// Sum max(0, damage - shield - armor) over two independent uniform
		// integer ranges via finite differences of the triangular sums.
		auto sum = [](double n) { return n > 0 ? n * (n + 1) * (n + 2) / 6 : 0.0; };
		const double residual = damage - minimumBlock;
		const double shields = defense - defenseLow + 1;
		const double armors = armorHigh - armorLow + 1;
		return (sum(residual) - sum(residual - shields) - sum(residual - armors) +
		        sum(residual - shields - armors)) / (shields * armors);
	};
	if (maximum <= minimumBlock) return 0;
	if (minimum == maximum) return afterBlock(minimum);
	if (minimum >= maximumBlock) return (int64_t(minimum) + maximum - minimumBlock - maximumBlock) / 2.0;

	// tools::normal_random rounds N(0.5,0.25) inside [0,1], but maps tails
	// back to the two integer midpoints rather than clamping to the ends.
	auto cdf = [](double x) { return 0.5 * (1 + std::erf((x - 0.5) / (0.25 * std::sqrt(2.0)))); };
	const int64_t width = int64_t(maximum) - minimum;
	double result = afterBlock(minimum + width / 2) * cdf(0) +
	                afterBlock(minimum + (width + 1) / 2) * (1 - cdf(1));
	double previous = cdf(0);
	for (int64_t offset = 0; offset <= width; ++offset) {
		const double next = cdf(std::min(1.0, (offset + 0.5) / width));
		result += (next - previous) * afterBlock(minimum + offset);
		previous = next;
	}
	return std::max(0.0, result);
}

#endif
