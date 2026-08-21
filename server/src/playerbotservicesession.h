/**
 * Per-bot shop and bank transaction state. NPC interaction, dispatch, and
 * telemetry remain controller responsibilities.
 */
#ifndef FS_PLAYERBOTSERVICESESSION_H
#define FS_PLAYERBOTSERVICESESSION_H

#include <cstdint>
#include <optional>

struct PlayerBotServiceTransaction {
	uint16_t itemId = 0;
	uint32_t amount = 0;
	uint32_t itemCount = 0;
	uint64_t money = 0;
	uint64_t balance = 0;
};

enum class PlayerBotServiceVerificationResult : uint8_t {
	Success,
	Retry,
	Mismatch,
	Rejected,
};

struct PlayerBotServiceVerification {
	PlayerBotServiceTransaction before;
	PlayerBotServiceVerificationResult result;
};

class PlayerBotServiceSession
{
	public:
	void reset();

	bool hasShopTransaction() const;
	const PlayerBotServiceTransaction* shopTransaction() const;
	void beginShopTransaction(PlayerBotServiceTransaction transaction);
	PlayerBotServiceVerification verifyShopTransaction(uint32_t itemCount, uint64_t money, uint64_t balance,
	                                                   bool purchase, uint32_t unitPrice, uint32_t maximumRetries);

	bool bankDepositComplete() const { return depositComplete; }
	void setBankDepositComplete(bool complete) { depositComplete = complete; depositPending = false; }
	bool hasBankDeposit() const { return depositPending; }
	void beginBankDeposit(uint64_t money, uint64_t balance);
	PlayerBotServiceVerification verifyBankDeposit(uint64_t money, uint64_t balance, uint32_t maximumRetries);
	void beginBankWithdrawal(uint64_t balance, uint32_t amount);
	bool hasBankWithdrawal() const { return withdrawalPending; }
	const PlayerBotServiceTransaction& bankTransaction() const { return bank; }
	PlayerBotServiceVerification verifyBankWithdrawal(uint64_t money, uint64_t balance, uint32_t maximumRetries);

	private:
	PlayerBotServiceTransaction shop;
	PlayerBotServiceTransaction bank;
	uint32_t shopRetries = 0;
	uint32_t bankRetries = 0;
	bool shopPending = false;
	bool depositComplete = false;
	bool depositPending = false;
	bool withdrawalPending = false;
};

#endif
