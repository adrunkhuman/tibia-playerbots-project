/** Recovery attempts span selection, physical departure and the service return. */
#ifndef FS_PLAYERBOTRECOVERYPROGRESS_H
#define FS_PLAYERBOTRECOVERYPROGRESS_H

#include <cstdint>
#include <optional>

enum class PlayerBotRecoveryProgressResult { NotPending, Progress, NoProgress, Exhausted };

class PlayerBotRecoveryProgressBudget
{
public:
	static constexpr uint32_t maximumAttempts = 3;
	bool active() const { return bestShortfall.has_value(); }
	uint32_t failures() const { return failedAttempts; }
	int64_t best() const { return bestShortfall.value_or(INT64_MAX); }

	// Re-entering/selecting/re-equipping is not economic progress.
	void enter(int64_t shortfall) { if (!bestShortfall) bestShortfall = shortfall; }
	void selected() { if (active() && failedAttempts < maximumAttempts) pending = true; }
	PlayerBotRecoveryProgressResult returned(int64_t shortfall)
	{
		if (failedAttempts >= maximumAttempts) return PlayerBotRecoveryProgressResult::Exhausted;
		if (!pending) return PlayerBotRecoveryProgressResult::NotPending;
		pending = false;
		if (shortfall < *bestShortfall) {
			bestShortfall = shortfall;
			failedAttempts = 0;
			return PlayerBotRecoveryProgressResult::Progress;
		}
		return ++failedAttempts >= maximumAttempts ?
		    PlayerBotRecoveryProgressResult::Exhausted : PlayerBotRecoveryProgressResult::NoProgress;
	}
	// Only a completed exit from supply recovery retires the episode.
	void clear() { *this = {}; }

private:
	std::optional<int64_t> bestShortfall;
	uint32_t failedAttempts = 0;
	bool pending = false;
};

inline int64_t playerBotRecoveryFundingShortfall(uint64_t replacement, uint64_t funds)
{
	// Keep surplus negative: a missing nonconsuming normal weapon may still
	// need funding even when the consumable floors are affordable. Unknown
	// cost has a distinct sentinel and can never masquerade as funding.
	if (replacement == UINT64_MAX) return INT64_MAX;
	const uint64_t magnitude = replacement >= funds ? replacement - funds : funds - replacement;
	const int64_t bounded = static_cast<int64_t>(magnitude < uint64_t(INT64_MAX) ? magnitude : INT64_MAX - 1);
	return replacement >= funds ? bounded : -bounded;
}

#endif
