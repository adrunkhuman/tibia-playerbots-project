/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotprogressionsession.h"

void PlayerBotRewardSession::begin(PlayerBotRewardPlan value)
{
	reward = std::move(value);
	currentStage = reward.resumeEquipment ? PlayerBotRewardStage::EquipReward : PlayerBotRewardStage::Travel;
	attempts = 0;
	claim = {};
	nestedContainer = {};
	displaced.clear();
}

void PlayerBotRewardSession::reset()
{
	reward = {};
	currentStage = PlayerBotRewardStage::Travel;
	attempts = 0;
	claim = {};
	nestedContainer = {};
	displaced.clear();
}

uint32_t PlayerBotRewardSession::beginContainerAccess(size_t depth)
{
	if (nestedContainer.depth != depth) {
		nestedContainer.depth = depth;
		nestedContainer.openAttempts = 0;
	}
	return ++nestedContainer.openAttempts;
}

void PlayerBotRewardSession::observeContainerOpen(size_t depth)
{
	if (nestedContainer.depth == depth) {
		nestedContainer = {};
	}
}

bool PlayerBotRewardSession::displacedItemsPreserved(const std::map<uint16_t, uint32_t>& counts) const
{
	for (const auto& [itemId, before] : displaced) {
		auto current = counts.find(itemId);
		if (current == counts.end() || current->second < before) return false;
	}
	return true;
}

void PlayerBotOracleDepartureSession::begin(PlayerBotOracleDeparturePlan value)
{
	departure = std::move(value);
	currentStage = PlayerBotOracleDepartureStage::Travel;
	attempts = 0;
}

void PlayerBotOracleDepartureSession::reset()
{
	departure = {};
	currentStage = PlayerBotOracleDepartureStage::Travel;
	attempts = 0;
}

void PlayerBotSpellTrainingSession::begin(PlayerBotSpellTrainingPlan value)
{
	training = std::move(value);
	currentStage = PlayerBotSpellTrainingStage::Travel;
	attempts = 0;
	beforeMoney = 0;
}

void PlayerBotSpellTrainingSession::reset()
{
	training = {};
	currentStage = PlayerBotSpellTrainingStage::Travel;
	attempts = 0;
	beforeMoney = 0;
}

void PlayerBotEquipmentPurchaseSession::begin(PlayerBotEquipmentOfferEvaluation value)
{
	purchase = std::move(value);
	currentStage = purchase.carried ? PlayerBotEquipmentPurchaseStage::Equip : PlayerBotEquipmentPurchaseStage::Travel;
	attempts = 0;
	displaced.clear();
	nestedContainer = {};
}

void PlayerBotEquipmentPurchaseSession::reset()
{
	purchase = {};
	currentStage = PlayerBotEquipmentPurchaseStage::Travel;
	attempts = 0;
	displaced.clear();
	nestedContainer = {};
}

bool PlayerBotEquipmentPurchaseSession::displacedItemsPreserved(const std::map<uint16_t, uint32_t>& counts) const
{
	for (const auto& [itemId, before] : displaced) {
		auto current = counts.find(itemId);
		if (current == counts.end() || current->second < before) return false;
	}
	return true;
}

uint32_t PlayerBotEquipmentPurchaseSession::beginContainerAccess(size_t depth)
{
	if (nestedContainer.depth != depth) {
		nestedContainer.depth = depth;
		nestedContainer.openAttempts = 0;
	}
	return ++nestedContainer.openAttempts;
}

void PlayerBotEquipmentPurchaseSession::observeContainerOpen(size_t depth)
{
	if (nestedContainer.depth == depth) nestedContainer = {};
}
