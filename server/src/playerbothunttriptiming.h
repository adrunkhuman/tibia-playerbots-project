#ifndef FS_PLAYERBOTHUNTTRIPTIMING_H
#define FS_PLAYERBOTHUNTTRIPTIMING_H

#include <algorithm>
#include <cmath>
#include <cstdint>

// Creature::getStepDuration rounds the terrain/speed quotient to 50 ms.
// Player walk delay doubles diagonal and floor-changing steps (lastStepCost),
// unlike the three-times diagonal path-search heuristic. No live-tile shortcut:
// callers supply the terrain of each route step's resulting tile.
inline uint32_t playerBotHuntStepMilliseconds(uint32_t groundSpeed, int32_t stepSpeed,
    bool diagonal = false, bool floorChange = false)
{
	const double base = std::ceil(1000.0 * (groundSpeed ? groundSpeed : 150) /
	    std::max<int32_t>(1, stepSpeed) / 50.0) * 50.0;
	return static_cast<uint32_t>(std::min<double>(UINT32_MAX, base * (diagonal || floorChange ? 2 : 1)));
}

inline uint32_t playerBotHuntActionMilliseconds(uint32_t movementMs, uint32_t configuredActionMs)
{
	// Interactions have a one-second modeled service/tool overhead. Movement
	// and the engine's action cooldown overlap; summing them double counts wait.
	return std::max(movementMs, std::max<uint32_t>(1000, configuredActionMs));
}

// Coarse topology does not contain terrain. Use ordinary terrain, not the
// player's current (possibly very fast) tile; detailed routes replace this.
inline double playerBotHuntCoarseTravelSeconds(uint32_t steps, int32_t stepSpeed)
{
	return steps * (playerBotHuntStepMilliseconds(150, stepSpeed) / 1000.0);
}

inline uint32_t playerBotHuntTransitSeconds(double estimatedTravelSeconds)
{
	// Planning and brief combat/tool delays need slack. Existing progress/stall
	// guards remain active; this is an absolute ceiling even while making progress.
	constexpr double maximumTransitSeconds = 3600;
	if (!std::isfinite(estimatedTravelSeconds)) return static_cast<uint32_t>(maximumTransitSeconds);
	return static_cast<uint32_t>(std::clamp(std::ceil(2 * std::max(0.0, estimatedTravelSeconds) + 60),
	    60.0, maximumTransitSeconds));
}

#endif
