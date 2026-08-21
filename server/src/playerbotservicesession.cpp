/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2019 Mark Samman
 */

#include "otpch.h"

#include "playerbotservicesession.h"

void PlayerBotServiceSession::reset()
{
	shop = {};
	bank = {};
	shopRetries = 0;
	bankRetries = 0;
	shopPending = false;
	depositComplete = false;
	depositPending = false;
	withdrawalPending = false;
}

bool PlayerBotServiceSession::hasShopTransaction() const
{
	return shopPending;
}

const PlayerBotServiceTransaction* PlayerBotServiceSession::shopTransaction() const
{
	return shopPending ? &shop : nullptr;
}

void PlayerBotServiceSession::beginShopTransaction(PlayerBotServiceTransaction transaction)
{
	shop = transaction;
	shopRetries = 0;
	shopPending = true;
}

PlayerBotServiceVerification PlayerBotServiceSession::verifyShopTransaction(
	uint32_t itemCount, uint64_t money, uint64_t balance, bool purchase, uint32_t unitPrice, uint32_t maximumRetries)
{
	const PlayerBotServiceTransaction before = shop;
	const uint64_t moneyDelta = static_cast<uint64_t>(shop.amount) * unitPrice;
	const bool itemChanged = purchase ? itemCount == shop.itemCount + shop.amount :
	                                  itemCount + shop.amount == shop.itemCount;
	const uint64_t expectedMoney = purchase ? (shop.money > moneyDelta ? shop.money - moneyDelta : 0) :
	                                           shop.money + moneyDelta;
	const uint64_t expectedBalance = purchase && moneyDelta > shop.money ?
	                                     shop.balance - (moneyDelta - shop.money) : shop.balance;
	if (itemChanged && money == expectedMoney && balance == expectedBalance) {
		shopPending = false;
		shopRetries = 0;
		return {before, PlayerBotServiceVerificationResult::Success};
	}
	if (itemCount != shop.itemCount || money != shop.money || balance != shop.balance) {
		return {before, PlayerBotServiceVerificationResult::Mismatch};
	}
	return {before, ++shopRetries >= maximumRetries ? PlayerBotServiceVerificationResult::Rejected :
	                                                PlayerBotServiceVerificationResult::Retry};
}

void PlayerBotServiceSession::beginBankDeposit(uint64_t money, uint64_t balance)
{
	bank = {0, 0, 0, money, balance};
	bankRetries = 0;
	depositPending = true;
}

PlayerBotServiceVerification PlayerBotServiceSession::verifyBankDeposit(uint64_t money, uint64_t balance,
	                                                                        uint32_t maximumRetries)
{
	const PlayerBotServiceTransaction before = bank;
	if (money == 0 && balance >= bank.balance + bank.money) {
		bankRetries = 0;
		depositPending = false;
		return {before, PlayerBotServiceVerificationResult::Success};
	}
	return {before, ++bankRetries >= maximumRetries ? PlayerBotServiceVerificationResult::Rejected :
	                                                PlayerBotServiceVerificationResult::Retry};
}

void PlayerBotServiceSession::beginBankWithdrawal(uint64_t balance, uint32_t amount)
{
	bank = {0, amount, 0, 0, balance};
	bankRetries = 0;
	withdrawalPending = true;
}

PlayerBotServiceVerification PlayerBotServiceSession::verifyBankWithdrawal(uint64_t money, uint64_t balance,
	                                                                           uint32_t maximumRetries)
{
	const PlayerBotServiceTransaction before = bank;
	if (money == bank.amount && balance + bank.amount == bank.balance) {
		bankRetries = 0;
		withdrawalPending = false;
		return {before, PlayerBotServiceVerificationResult::Success};
	}
	return {before, ++bankRetries >= maximumRetries ? PlayerBotServiceVerificationResult::Rejected :
	                                                PlayerBotServiceVerificationResult::Retry};
}
