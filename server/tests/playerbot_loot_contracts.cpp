// Standalone cargo planning and loot-workflow contracts; see playerbot_loot_contracts.sh.
#include <cassert>
#include <chrono>
#include <iostream>
#include <utility>

#include "playerbotlootpolicy.h"
#include "playerbotlootworkflow.h"

namespace {
const auto now = std::chrono::steady_clock::time_point{};
const void* pointer(uintptr_t value) { return reinterpret_cast<const void*>(value); }

PlayerBotLootItemSnapshot coin(uint8_t count = 1)
{
	PlayerBotLootItemSnapshot item;
	item.itemId = 2148;
	item.clientId = 3031;
	item.count = item.availableCount = count;
	item.unitWeight = 10;
	item.unitValue = 1;
	item.currency = true;
	item.stackable = true;
	item.source = pointer(1);
	item.item = pointer(2);
	return item;
}

PlayerBotLootInventorySnapshot inventoryWithContainer(const void* container, uint8_t size, uint8_t capacity,
	int8_t containerId = 1)
{
	PlayerBotLootInventorySnapshot inventory;
	inventory.containers.push_back({container, 1988, 2854, size, capacity, containerId});
	return inventory;
}

PlayerBotLootWorkflowSnapshot workflowSnapshot(PlayerBotLootInventorySnapshot inventory,
	std::vector<PlayerBotLootItemSnapshot> corpseItems)
{
	PlayerBotLootWorkflowSnapshot snapshot;
	snapshot.currentPosition = Position(100, 100, 7);
	snapshot.now = now;
	snapshot.discoveredCorpse = PlayerBotLootCorpseSnapshot{5964, 0, 7, snapshot.currentPosition};
	snapshot.corpseContainerOpen = true;
	snapshot.backpackAvailable = true;
	snapshot.backpackContainerOpen = true;
	snapshot.canDoAction = true;
	snapshot.inventory = std::move(inventory);
	snapshot.corpseItems = std::move(corpseItems);
	return snapshot;
}

PlayerBotLootWorkflow workflow()
{
	PlayerBotLootWorkflow result({4, 6, 3, std::chrono::seconds(2), std::chrono::seconds(20), 5});
	result.begin(10, Position(100, 100, 7), {5964, true}, Position(100, 100, 7), now);
	return result;
}

void observeInventoryItem(PlayerBotLootWorkflowSnapshot& snapshot, const void* container, const void* item,
	uint16_t itemId, uint16_t clientId, uint8_t count, uint8_t index = 0, int8_t containerId = 1)
{
	snapshot.inventoryItems.push_back({container, item, itemId, clientId, count, index, containerId});
}

void placementContracts()
{
	PlayerBotLootPolicy policy(5);
	const void* root = pointer(10);
	const void* nested = pointer(11);

	auto merging = inventoryWithContainer(root, 1, 1);
	merging.freeCapacity = 1000;
	merging.cargo.push_back({root, 2148, 3031, 95, 0, 10, 1, false, 1, pointer(12), true, false, 95});
	auto placement = policy.placementFor(coin(10), merging);
	assert(placement.result == PlayerBotLootPlacementResult::Placed && placement.count == 5);
	assert(placement.destination.merge && placement.destination.item == pointer(12));

	auto partial = inventoryWithContainer(root, 0, 1);
	partial.freeCapacity = 30;
	placement = policy.placementFor(coin(10), partial);
	assert(placement.result == PlayerBotLootPlacementResult::Placed && placement.count == 3);

	auto nestedDestination = inventoryWithContainer(root, 1, 1);
	nestedDestination.containers.push_back({nested, 1987, 2853, 0, 8, 2});
	nestedDestination.freeCapacity = 10;
	placement = policy.placementFor(coin(), nestedDestination);
	assert(placement.result == PlayerBotLootPlacementResult::Placed && placement.destination.container == nested);

	auto noCapacity = inventoryWithContainer(root, 0, 1);
	placement = policy.placementFor(coin(), noCapacity);
	assert(placement.result == PlayerBotLootPlacementResult::NoCapacity);

	auto noSlot = inventoryWithContainer(root, 4, 4);
	noSlot.freeCapacity = 1000;
	noSlot.cargo.push_back({root, 2120, 3003, 1, 0, 1800, 0, false, 1, pointer(13), false, false, 1}); // tool
	noSlot.cargo.push_back({root, 7618, 0, 10, 1, 180, 0, false, 1, pointer(14), true, false, 10}); // supply
	noSlot.cargo.push_back({root, 2395, 0, 1, 2, 3500, 118, false, 1, pointer(15), false, false, 1}); // equipment
	noSlot.cargo.push_back({root, 1949, 0, 1, 3, 100, 0, false, 1, pointer(16), false, false, 1}); // unknown
	placement = policy.placementFor(coin(), noSlot);
	assert(placement.result == PlayerBotLootPlacementResult::NoSlot);
	assert(!policy.replacementFor(coin(), noSlot).viable);

	// The 256-item scan boundary never exposes a destination whose contents were truncated.
	auto boundary = inventoryWithContainer(root, 255, 255);
	for (uintptr_t index = 0; index < 256; ++index) {
		boundary.cargo.push_back({root, 1949, 0, 1, static_cast<uint8_t>(index), 100, 0,
		                          false, 1, pointer(1000 + index), false, false, 1});
	}
	boundary.containers.push_back({nested, 1987, 2853, 0, 8, 2, false});
	boundary.freeCapacity = 10;
	assert(policy.placementFor(coin(), boundary).result == PlayerBotLootPlacementResult::NoSlot);
	boundary.containers.back().contentsComplete = true;
	assert(policy.placementFor(coin(), boundary).result == PlayerBotLootPlacementResult::Placed);
}

void replacementContracts()
{
	PlayerBotLootPolicy policy(5);
	const void* root = pointer(20);
	auto full = inventoryWithContainer(root, 1, 1);
	full.cargo.push_back({root, 2389, 3277, 1, 0, 2000, 3, true, 1, pointer(21), false, true, 1});
	auto replacement = policy.replacementFor(coin(), full);
	assert(replacement.viable && replacement.slotReplacement);
	assert(replacement.count == 1 && replacement.incomingCount == 1);
	assert(replacement.destination.container == root); // one 1 gp coin replaces a 3 gp/20 oz spear

	PlayerBotLootItemSnapshot gem = coin();
	gem.itemId = 2150;
	gem.currency = false;
	gem.unitValue = 2;
	assert(!policy.replacementFor(gem, full).viable); // ordinary acceptance still protects immediate value

	auto wholeStack = inventoryWithContainer(root, 1, 1);
	wholeStack.freeCapacity = 1000;
	wholeStack.cargo.push_back({root, 2671, 3582, 4, 0, 1200, 1, true, 1, pointer(22), true, true, 4});
	replacement = policy.replacementFor(coin(), wholeStack);
	assert(replacement.viable && replacement.slotReplacement && replacement.count == 4);

	auto weight = inventoryWithContainer(root, 1, 2);
	weight.freeCapacity = 0;
	weight.cargo.push_back({root, 2389, 3277, 1, 0, 2000, 3, true, 1, pointer(23), false, true, 1});
	replacement = policy.replacementFor(coin(100), weight);
	assert(replacement.viable && !replacement.slotReplacement && replacement.incomingCount == 100);
}

void workflowContracts()
{
	const void* root = pointer(30);
	auto inventory = inventoryWithContainer(root, 0, 2);
	inventory.freeCapacity = 20;
	auto item = coin(5);
	auto snapshot = workflowSnapshot(inventory, {item});
	auto moving = workflow();
	auto decision = moving.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::MoveItem && decision.command.count == 2);

	// Exact source and chosen-container deltas verify the planned partial move.
	snapshot.corpseItems[0].availableCount = snapshot.corpseItems[0].count = 3;
	snapshot.inventory.freeCapacity = 0;
	snapshot.inventory.cargo.push_back({root, 2148, 3031, 2, 0, 10, 1, false, 1, pointer(31), true, false, 2});
	observeInventoryItem(snapshot, root, pointer(31), 2148, 3031, 2);
	snapshot.inventory.itemCounts[2148] = 2;
	decision = moving.advance(snapshot);
	assert(decision.lootVerification && decision.lootVerification->moved &&
	       decision.lootVerification->movedCount == 2);
	assert(decision.command.outcome == PlayerBotLootOutcome::NoCapacity);

	// Pending destination verification uses the complete direct observation, not capped policy cargo.
	auto boundaryMove = workflow();
	inventory = inventoryWithContainer(root, 0, 1);
	inventory.freeCapacity = 10;
	snapshot = workflowSnapshot(inventory, {coin()});
	assert(boundaryMove.advance(snapshot).command.type == PlayerBotLootCommandType::MoveItem);
	snapshot.corpseItems.clear();
	snapshot.inventory.containers[0].size = 1;
	snapshot.inventory.containers[0].contentsComplete = false;
	snapshot.inventory.freeCapacity = 0;
	observeInventoryItem(snapshot, root, pointer(2), 2148, 3031, 1);
	snapshot.inventory.itemCounts[2148] = 1;
	decision = boundaryMove.advance(snapshot);
	assert(decision.lootVerification && decision.lootVerification->moved);

	// An unchanged exact source/destination snapshot is stale once, then suppressed.
	auto stale = workflow();
	inventory = inventoryWithContainer(root, 0, 2);
	inventory.freeCapacity = 10;
	snapshot = workflowSnapshot(inventory, {coin()});
	assert(stale.advance(snapshot).command.type == PlayerBotLootCommandType::MoveItem);
	decision = stale.advance(snapshot);
	assert(decision.lootVerification && !decision.lootVerification->moved);
	assert(decision.command.type == PlayerBotLootCommandType::Finish);

	// Closed nested containers are opened through a normal access command before inspection.
	auto nested = workflow();
	inventory = inventoryWithContainer(root, 1, 1);
	inventory.freeCapacity = 10;
	inventory.containerAccess.push_back({pointer(32), root, pointer(32), 1987, 2853, 0, 1});
	snapshot = workflowSnapshot(inventory, {coin()});
	decision = nested.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::OpenCargo &&
	       decision.command.containerAccess.container == pointer(32));
	decision = nested.advance(snapshot);
	assert(decision.command.outcome == PlayerBotLootOutcome::NoSlot); // access attempt is bounded

	// Slot replacement commits a feasible destination before the whole spear is discarded.
	auto replacing = workflow();
	inventory = inventoryWithContainer(root, 1, 1);
	inventory.cargo.push_back({root, 2389, 3277, 1, 0, 2000, 3, true, 1, pointer(33), false, true, 1});
	snapshot = workflowSnapshot(inventory, {coin()});
	decision = replacing.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::DiscardCargo && decision.command.count == 1);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.cargo.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.groundItemCounts[2389] = 1;
	snapshot.groundItems.push_back({pointer(33), 2389, 3277, 1, 0, snapshot.currentPosition});
	decision = replacing.advance(snapshot);
	assert(decision.discardVerification && decision.discardVerification->discarded);
	assert(decision.command.type == PlayerBotLootCommandType::MoveItem && decision.command.count == 1);
	snapshot.corpseItems.clear();
	snapshot.inventory.containers[0].size = 1;
	snapshot.inventory.freeCapacity = 1990;
	snapshot.inventory.cargo.push_back({root, 2148, 3031, 1, 0, 10, 1, false, 1, pointer(34), true, false, 1});
	observeInventoryItem(snapshot, root, pointer(34), 2148, 3031, 1);
	snapshot.inventory.itemCounts[2148] = 1;
	decision = replacing.advance(snapshot);
	assert(decision.lootVerification && decision.lootVerification->moved);
}

void replacementRecoveryContracts()
{
	const void* root = pointer(40);

	// Do not discard a stack when ground autostacking would split it across two objects.
	auto groundSplitInventory = inventoryWithContainer(root, 1, 1);
	groundSplitInventory.cargo.push_back({root, 2671, 3582, 10, 0, 1200, 1, true, 1,
	                                     pointer(43), true, true, 10});
	auto groundSplitSnapshot = workflowSnapshot(groundSplitInventory, {coin()});
	groundSplitSnapshot.groundItems.push_back({pointer(44), 2671, 3582, 95, 0,
	                                           groundSplitSnapshot.currentPosition});
	auto groundSplit = workflow();
	auto groundSplitDecision = groundSplit.advance(groundSplitSnapshot);
	assert(groundSplitDecision.command.type == PlayerBotLootCommandType::Wait &&
	       groundSplitDecision.command.outcome == PlayerBotLootOutcome::NoSlot);
	assert(groundSplit.advance(groundSplitSnapshot).command.type == PlayerBotLootCommandType::Finish);

	auto makeReplacementSnapshot = [&]() {
		auto inventory = inventoryWithContainer(root, 1, 1);
		inventory.cargo.push_back({root, 2389, 3277, 1, 0, 2000, 3, true, 1,
		                           pointer(41), false, true, 1});
		auto snapshot = workflowSnapshot(inventory, {coin()});
		observeInventoryItem(snapshot, root, pointer(41), 2389, 3277, 1);
		return snapshot;
	};

	// If the corpse disappears after discard, recover that exact spear to its freed slot.
	auto recovering = workflow();
	auto snapshot = makeReplacementSnapshot();
	auto decision = recovering.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.containers[0].contentsComplete = false;
	snapshot.inventory.cargo.clear();
	snapshot.inventoryItems.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.corpseItems.clear();
	snapshot.groundItemCounts[2389] = 1;
	snapshot.groundItems.push_back({pointer(41), 2389, 3277, 1, 0, snapshot.currentPosition});
	decision = recovering.advance(snapshot);
	assert(decision.discardVerification && decision.discardVerification->discarded);
	assert(decision.command.type == PlayerBotLootCommandType::RecoverCargo && decision.command.count == 1);
	snapshot.inventory.containers[0].size = 1;
	snapshot.inventoryItems.push_back({root, pointer(41), 2389, 3277, 1, 0, 1});
	snapshot.groundItems.clear();
	snapshot.groundItemCounts.clear();
	decision = recovering.advance(snapshot);
	assert(decision.recoveryVerification && decision.recoveryVerification->recovered &&
	       decision.recoveryVerification->recoveredCount == 1);

	// Recovery plans the container's first partial autostack and verifies the full split result.
	auto stackedRecovery = workflow();
	auto stackedInventory = inventoryWithContainer(root, 2, 2);
	stackedInventory.cargo.push_back({root, 2671, 3582, 10, 0, 1200, 1, true, 1,
	                                  pointer(45), true, true, 10});
	stackedInventory.cargo.push_back({root, 2671, 3582, 0, 1, 1200, 1, false, 1,
	                                  pointer(46), true, false, 95});
	snapshot = workflowSnapshot(stackedInventory, {coin()});
	observeInventoryItem(snapshot, root, pointer(45), 2671, 3582, 10, 0);
	observeInventoryItem(snapshot, root, pointer(46), 2671, 3582, 95, 1);
	assert(stackedRecovery.advance(snapshot).command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 1;
	snapshot.inventory.cargo.erase(snapshot.inventory.cargo.begin());
	snapshot.inventoryItems.clear();
	observeInventoryItem(snapshot, root, pointer(46), 2671, 3582, 95, 0);
	snapshot.inventory.freeCapacity = 12000;
	snapshot.corpseItems.clear();
	snapshot.groundItemCounts[2671] = 10;
	snapshot.groundItems.push_back({pointer(45), 2671, 3582, 10, 0, snapshot.currentPosition});
	decision = stackedRecovery.advance(snapshot);
	assert(decision.discardVerification && decision.discardVerification->discarded);
	assert(decision.command.type == PlayerBotLootCommandType::RecoverCargo && decision.command.count == 10);
	assert(decision.command.itemDestination.merge && decision.command.itemDestination.item == pointer(46) &&
	       decision.command.itemDestination.availableCount >= 10);
	snapshot.inventory.containers[0].size = 2;
	snapshot.inventoryItems.clear();
	observeInventoryItem(snapshot, root, pointer(46), 2671, 3582, 100, 0);
	observeInventoryItem(snapshot, root, pointer(47), 2671, 3582, 5, 1);
	snapshot.groundItems.clear();
	snapshot.groundItemCounts.clear();
	decision = stackedRecovery.advance(snapshot);
	assert(decision.recoveryVerification && decision.recoveryVerification->recovered &&
	       decision.recoveryVerification->recoveredCount == 10);

	// A failed incoming pickup also rolls the exact discarded spear back.
	auto failedPickup = workflow();
	snapshot = makeReplacementSnapshot();
	assert(failedPickup.advance(snapshot).command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.cargo.clear();
	snapshot.inventoryItems.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.groundItems.push_back({pointer(41), 2389, 3277, 1, 0, snapshot.currentPosition});
	snapshot.groundItemCounts[2389] = 1;
	decision = failedPickup.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::MoveItem);
	decision = failedPickup.advance(snapshot); // unchanged source and destination: pickup failed
	assert(decision.lootVerification && !decision.lootVerification->moved);
	assert(decision.command.type == PlayerBotLootCommandType::RecoverCargo);
	decision = failedPickup.advance(snapshot); // unchanged again: the single recovery attempt failed
	assert(decision.recoveryVerification && !decision.recoveryVerification->recovered);
	assert(decision.command.type == PlayerBotLootCommandType::Wait &&
	       decision.command.outcome == PlayerBotLootOutcome::CargoRecoveryImpossible);

	// Adapter rejection of a stale pickup plan also enters recovery instead of retrying pickup.
	auto rejectedPickup = workflow();
	snapshot = makeReplacementSnapshot();
	assert(rejectedPickup.advance(snapshot).command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.cargo.clear();
	snapshot.inventoryItems.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.groundItems.push_back({pointer(41), 2389, 3277, 1, 0, snapshot.currentPosition});
	snapshot.groundItemCounts[2389] = 1;
	assert(rejectedPickup.advance(snapshot).command.type == PlayerBotLootCommandType::MoveItem);
	rejectedPickup.cancelPendingLootMove();
	decision = rejectedPickup.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::RecoverCargo);

	// A same-ID ground item cannot stand in for the exact discarded object.
	auto unrelated = workflow();
	snapshot = makeReplacementSnapshot();
	assert(unrelated.advance(snapshot).command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.cargo.clear();
	snapshot.inventoryItems.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.groundItems.push_back({pointer(41), 2389, 3277, 1, 0, snapshot.currentPosition});
	snapshot.groundItemCounts[2389] = 1;
	snapshot.canDoAction = false;
	decision = unrelated.advance(snapshot);
	assert(decision.discardVerification && decision.discardVerification->discarded &&
	       decision.command.type == PlayerBotLootCommandType::Wait);
	snapshot.canDoAction = true;
	snapshot.corpseItems.clear();
	snapshot.groundItems.clear();
	snapshot.groundItems.push_back({pointer(42), 2389, 3277, 1, 0, snapshot.currentPosition});
	decision = unrelated.advance(snapshot);
	assert(decision.command.type == PlayerBotLootCommandType::Wait &&
	       decision.command.outcome == PlayerBotLootOutcome::CargoRecoveryImpossible);

	// Starting a new corpse session clears a deferred replacement and recovery plan.
	auto restarted = workflow();
	snapshot = makeReplacementSnapshot();
	assert(restarted.advance(snapshot).command.type == PlayerBotLootCommandType::DiscardCargo);
	snapshot.inventory.containers[0].size = 0;
	snapshot.inventory.cargo.clear();
	snapshot.inventoryItems.clear();
	snapshot.inventory.freeCapacity = 2000;
	snapshot.groundItems.push_back({pointer(41), 2389, 3277, 1, 0, snapshot.currentPosition});
	snapshot.groundItemCounts[2389] = 1;
	snapshot.canDoAction = false;
	assert(restarted.advance(snapshot).command.type == PlayerBotLootCommandType::Wait);
	restarted.begin(11, snapshot.currentPosition, {5964, true}, snapshot.currentPosition, now);
	auto freshInventory = inventoryWithContainer(root, 0, 1);
	freshInventory.freeCapacity = 10;
	auto fresh = workflowSnapshot(freshInventory, {coin()});
	assert(restarted.advance(fresh).command.type == PlayerBotLootCommandType::MoveItem);
}
}

int main()
{
	placementContracts();
	replacementContracts();
	workflowContracts();
	replacementRecoveryContracts();
	std::cout << "playerbot loot contracts passed\n";
}
