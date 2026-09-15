// Standalone state-machine regression; no game world or Docker required.
#include <cassert>
#include <cstring>
#include <iostream>

#include "playerbotprogressionruntime.h"

namespace {
PlayerBotEquipmentOfferEvaluation bagPlan()
{
	PlayerBotEquipmentOfferEvaluation plan;
	plan.itemId = 1988;
	plan.price = 20;
	plan.slot = static_cast<slots_t>(3);
	plan.replacedItemId = 1987;
	plan.backpackAcquisition = true;
	plan.bagUpgrade = true;
	return plan;
}

void reachPurchase(PlayerBotProgressionRuntime& runtime)
{
	PlayerBotEquipmentPurchaseObservation observation;
	observation.depotReached = true;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);

	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Open);
	assert(std::strcmp(result.reason, "open_backpack_depot") == 0);

	observation.depotOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);

	observation.oldBagEquipped = true;
	observation.actionAvailable = false;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	assert(std::strcmp(result.reason, "action_unavailable") == 0);

	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	assert(std::strcmp(result.reason, "depot_stage_old_bag") == 0);

	observation.oldBagEquipped = false;
	observation.oldBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);

	observation.navigationReached = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
}

void bankFundingIsRequiredBeforeStaging()
{
	PlayerBotProgressionRuntime runtime;
	runtime.beginEquipmentPurchase(bagPlan());
	PlayerBotEquipmentPurchaseObservation observation;
	observation.depotReached = true;
	runtime.advanceEquipmentPurchase(observation, 3);
	observation.actionAvailable = true;
	runtime.advanceEquipmentPurchase(observation, 3);
	observation.depotOpen = true;
	runtime.advanceEquipmentPurchase(observation, 3);
	observation.oldBagEquipped = true;
	observation.bankFundingAvailable = false;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Failed);
	assert(std::strcmp(result.reason, "bank_funding_required") == 0);
}

void successfulFullBagUpgrade()
{
	PlayerBotProgressionRuntime runtime;
	runtime.beginEquipmentPurchase(bagPlan());
	reachPurchase(runtime);

	PlayerBotEquipmentPurchaseObservation observation;
	observation.oldBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	observation.shopReady = true;
	observation.fundingAvailable = true;
	observation.itemCount = 0;
	observation.money = 100;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Shop);
	assert(std::strcmp(result.reason, "purchase_equipment") == 0);

	observation.itemCount = 1;
	observation.money = 80;
	observation.equipmentVerified = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.transaction.amount == 1);

	observation.depotReached = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	observation.depotOpen = false;
	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Open);
	observation.depotOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);

	observation.actionAvailable = true;
	observation.backpackDestinationOpen = false;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Open);
	observation.backpackDestinationOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	assert(std::strcmp(result.reason, "depot_retrieve_old_bag") == 0);

	observation.oldBagAtDepot = false;
	observation.oldBagPreserved = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Succeeded);
	assert(std::strcmp(result.reason, "backpack_acquired") == 0);
}

void prePurchaseFailureRestoresBag(bool providerAvailable, bool fundingAvailable, const char* expectedReason)
{
	PlayerBotProgressionRuntime runtime;
	runtime.beginEquipmentPurchase(bagPlan());
	reachPurchase(runtime);

	PlayerBotEquipmentPurchaseObservation observation;
	observation.oldBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	observation.providerAvailable = providerAvailable;
	observation.shopReady = true;
	observation.fundingAvailable = fundingAvailable;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Retry);
	assert(std::strcmp(result.reason, "restore_old_bag") == 0);

	observation.depotReached = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.depotOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	assert(std::strcmp(result.reason, "depot_restore_old_bag") == 0);

	observation.oldBagAtDepot = false;
	observation.oldBagEquipped = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Failed);
	assert(std::strcmp(result.reason, expectedReason) == 0);
	runtime.finish();
	assert(runtime.active() == PlayerBotProgressionProcedure::None);
	assert(!runtime.equipmentPurchase().plan().backpackAcquisition);
}

void retrievalFailureIsBounded()
{
	PlayerBotProgressionRuntime runtime;
	runtime.beginEquipmentPurchase(bagPlan());
	reachPurchase(runtime);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.oldBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	observation.shopReady = true;
	observation.fundingAvailable = true;
	observation.money = 100;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.itemCount = 1;
	observation.money = 80;
	observation.equipmentVerified = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.depotReached = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.depotOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.actionAvailable = true;
	observation.backpackDestinationOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	for (int attempt = 0; attempt < 2; ++attempt) {
		result = runtime.advanceEquipmentPurchase(observation, 3);
		assert(result.type == PlayerBotProgressionOutcomeType::Retry);
		result = runtime.advanceEquipmentPurchase(observation, 3);
		assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	}
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Failed);
	assert(std::strcmp(result.reason, "old_bag_recovery_exhausted") == 0);
}

void survivalInterruptionPreservesRecovery()
{
	PlayerBotProgressionRuntime runtime;
	runtime.resumeEquipmentBackpack(bagPlan(), true, false);
	runtime.beginEquipmentBackpackRecovery("healing_supply_missing", false);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.oldBagAtDepot = true;
	observation.depotReached = true;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	observation.depotOpen = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(std::strcmp(result.reason, "action_unavailable") == 0);
	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	observation.oldBagAtDepot = false;
	observation.oldBagEquipped = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Failed);
	assert(std::strcmp(result.reason, "healing_supply_missing") == 0);
}

void recoveryInterruptionIsIdempotent()
{
	PlayerBotProgressionRuntime runtime;
	runtime.resumeEquipmentBackpack(bagPlan(), true, false);
	runtime.beginEquipmentBackpackRecovery("healing_supply_missing", false);
	assert(runtime.equipmentBackpackRecoveryActive());
	PlayerBotEquipmentPurchaseObservation observation;
	observation.depotReached = true;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	assert(runtime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::OpenDepotForRetrieve);
	runtime.beginEquipmentBackpackRecovery("healing_supply_missing", false);
	assert(runtime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::OpenDepotForRetrieve);
}

void replacementBagIsStagedBeforeOriginalRestoration()
{
	PlayerBotProgressionRuntime runtime;
	runtime.resumeEquipmentBackpack(bagPlan(), true, false);
	runtime.beginEquipmentBackpackRecovery("purchased_backpack_missing", false);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.depotReached = true;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	observation.depotOpen = true;
	observation.oldBagAtDepot = true;
	observation.replacementBagEquipped = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	assert(runtime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::StageReplacementBackpack);
	observation.actionAvailable = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	assert(std::strcmp(result.reason, "depot_stage_replacement_bag") == 0);
	observation.replacementBagEquipped = false;
	observation.replacementBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(runtime.equipmentPurchase().stage() == PlayerBotEquipmentPurchaseStage::RestoreBackpack);
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Equip);
	assert(std::strcmp(result.reason, "depot_restore_old_bag") == 0);
	observation.oldBagAtDepot = false;
	observation.oldBagEquipped = true;
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Failed);
	assert(std::strcmp(result.reason, "purchased_backpack_missing") == 0);
}

void durableStageRehydration()
{
	PlayerBotProgressionRuntime staged;
	staged.resumeEquipmentBackpack(bagPlan(), true, false);
	PlayerBotEquipmentPurchaseObservation observation;
	observation.navigationReached = true;
	auto result = staged.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::None);
	observation.shopReady = true;
	observation.fundingAvailable = true;
	observation.backpackReceiptSafe = true;
	result = staged.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Shop);

	PlayerBotProgressionRuntime purchased;
	purchased.resumeEquipmentBackpack(bagPlan(), true, true);
	observation = {};
	observation.depotReached = false;
	result = purchased.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Navigate);
	assert(std::strcmp(result.reason, "recover_old_bag") == 0);
}

void rejectedReceiptIsBoundedAndRestored()
{
	PlayerBotProgressionRuntime runtime;
	runtime.beginEquipmentPurchase(bagPlan());
	reachPurchase(runtime);

	PlayerBotEquipmentPurchaseObservation observation;
	observation.oldBagAtDepot = true;
	observation.backpackReceiptSafe = true;
	observation.shopReady = true;
	observation.fundingAvailable = true;
	observation.money = 100;
	auto result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.command.type == PlayerBotProgressionCommandType::Shop);
	for (int attempt = 0; attempt < 2; ++attempt) {
		result = runtime.advanceEquipmentPurchase(observation, 3);
		assert(result.type == PlayerBotProgressionOutcomeType::Retry);
	}
	result = runtime.advanceEquipmentPurchase(observation, 3);
	assert(result.type == PlayerBotProgressionOutcomeType::Retry);
	assert(std::strcmp(result.reason, "restore_old_bag") == 0);
}
}

int main()
{
	bankFundingIsRequiredBeforeStaging();
	successfulFullBagUpgrade();
	prePurchaseFailureRestoresBag(true, false, "reserve_changed");
	prePurchaseFailureRestoresBag(false, true, "provider_unavailable");
	retrievalFailureIsBounded();
	survivalInterruptionPreservesRecovery();
	recoveryInterruptionIsIdempotent();
	replacementBagIsStagedBeforeOriginalRestoration();
	durableStageRehydration();
	rejectedReceiptIsBoundedAndRestored();
	std::cout << "playerbot equipment purchase contracts passed\n";
}
