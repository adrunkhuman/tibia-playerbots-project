/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "otpch.h"

#include "const.h"
#include "playerbotnpcsession.h"

namespace {
	constexpr uint32_t npcReplyDelay = 1000;
}

void PlayerBotNpcSession::reset(uint32_t targetId)
{
	targetNpcId = targetId;
	retries = 0;
	pendingDelay = 0;
	greetingAcknowledged = false;
	conversationStep = PlayerBotNpcConversationStep::Greet;
}

bool PlayerBotNpcSession::acceptReply(uint32_t playerId, uint32_t replyingPlayerId, uint32_t npcId, uint8_t type)
{
	if (replyingPlayerId != playerId || npcId != targetNpcId || type != TALKTYPE_PRIVATE_NP) {
		return false;
	}
	greetingAcknowledged = true;
	return true;
}

bool PlayerBotNpcSession::retryLimitReached(uint32_t maximumRetries)
{
	return ++retries >= maximumRetries;
}

PlayerBotNpcSessionOutcome PlayerBotNpcSession::advanceShop(const PlayerBotNpcShopObservation& observation, uint32_t maximumRetries)
{
	if (observation.shopOpen) {
		if (conversationStep != PlayerBotNpcConversationStep::Verify) {
			conversationStep = PlayerBotNpcConversationStep::Ready;
			retries = 0;
		}
		return {PlayerBotNpcSessionResult::Ready, 0};
	}
	if (conversationStep == PlayerBotNpcConversationStep::Greet) {
		conversationStep = PlayerBotNpcConversationStep::Request;
		pendingDelay = npcReplyDelay;
		return {PlayerBotNpcSessionResult::Pending, 1};
	}
	if (conversationStep == PlayerBotNpcConversationStep::Request) {
		if (!observation.greetingAcknowledged) {
			if (retryLimitReached(maximumRetries)) return {PlayerBotNpcSessionResult::Failed, 0};
			conversationStep = PlayerBotNpcConversationStep::Greet;
			pendingDelay = npcReplyDelay;
			return {PlayerBotNpcSessionResult::Pending, 0};
		}
		conversationStep = PlayerBotNpcConversationStep::Ready;
		pendingDelay = npcReplyDelay;
		return {PlayerBotNpcSessionResult::Pending, 1};
	}

	if (retryLimitReached(maximumRetries)) {
		return {PlayerBotNpcSessionResult::Failed, 0};
	}
	conversationStep = PlayerBotNpcConversationStep::Greet;
	pendingDelay = 0;
	return {PlayerBotNpcSessionResult::Pending, 0};
}
