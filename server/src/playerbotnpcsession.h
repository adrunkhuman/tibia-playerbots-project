/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef FS_PLAYERBOTNPCSESSION_H
#define FS_PLAYERBOTNPCSESSION_H

#include <cstdint>

enum class PlayerBotNpcSessionResult : uint8_t {
	Pending,
	Ready,
	Failed,
};

struct PlayerBotNpcSessionOutcome {
	PlayerBotNpcSessionResult result = PlayerBotNpcSessionResult::Pending;
	uint8_t actionsIssued = 0;
};

struct PlayerBotNpcShopObservation {
	bool shopOpen = false;
	bool otherShopOpen = false;
	bool greetingAcknowledged = false;
};

enum class PlayerBotNpcConversationStep : uint8_t {
	Greet,
	Request,
	Ready,
	Confirm,
	Verify,
};

class PlayerBotNpcSession
{
	public:
		void reset(uint32_t targetId = 0);

		uint32_t targetId() const { return targetNpcId; }
		bool targets(uint32_t npcId) const { return targetNpcId == npcId; }

		bool acceptReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type);
		void resetGreetingAcknowledgement() { greetingAcknowledged = false; }
		bool isGreetingAcknowledged() const { return greetingAcknowledged; }

	private:
	friend class PlayerBotServiceWorkflow;
	friend class PlayerBotProgressionRuntime;

		PlayerBotNpcConversationStep step() const { return conversationStep; }
		void setStep(PlayerBotNpcConversationStep step) { conversationStep = step; }
		void resetRetries() { retries = 0; }
		bool retryLimitReached(uint32_t maximumRetries);

		PlayerBotNpcSessionOutcome advanceShop(const PlayerBotNpcShopObservation& observation, uint32_t maximumRetries);
		uint32_t nextDelay() const { return pendingDelay; }

		uint32_t targetNpcId = 0;
		uint32_t retries = 0;
		uint32_t pendingDelay = 0;
		bool greetingAcknowledged = false;
		PlayerBotNpcConversationStep conversationStep = PlayerBotNpcConversationStep::Greet;
};

#endif
