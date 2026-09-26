#ifndef FS_PLAYERBOTLIFECYCLE_H
#define FS_PLAYERBOTLIFECYCLE_H

#include <cstdint>

// Dispatcher-owned tokens are never reused, even if the same GUID is reserved again.
// A cancelled scheduler task may already be queued on the dispatcher.
class PlayerBotLifecycleClock
{
public:
	uint64_t renew(uint64_t& slot) { return slot = ++issued; }
	static bool accepts(uint64_t slot, uint64_t token) { return slot != 0 && slot == token; }

private:
	uint64_t issued = 0;
};

// A reserved identity remains protected even when activation or recovery fails.
// Only explicit removal/shutdown (or external removal of a live bot) is terminal.
class PlayerBotLifecycleState
{
public:
	enum class Phase : uint8_t { Reserved, Active, Recovering, Terminal };

	Phase phase() const { return current; }
	bool managed() const { return current != Phase::Terminal; }
	bool active() const { return current == Phase::Active; }
	bool recovering() const { return current == Phase::Recovering; }

	bool activate()
	{
		if (current != Phase::Reserved && current != Phase::Recovering) return false;
		current = Phase::Active;
		return true;
	}

	bool beginRecovery()
	{
		if (!active()) return false;
		current = Phase::Recovering;
		return true;
	}

	void suspend()
	{
		if (managed()) current = Phase::Reserved;
	}

	bool terminate()
	{
		if (!managed()) return false;
		current = Phase::Terminal;
		return true;
	}

private:
	Phase current = Phase::Reserved;
};

#endif
