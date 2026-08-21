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
