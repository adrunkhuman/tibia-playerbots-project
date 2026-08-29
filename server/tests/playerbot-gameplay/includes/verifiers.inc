local F = PlayerbotGameplayFixture

function F.verifyReadiness(playerId, mode, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during readiness fixture")
    local ready = player:getVocation():getId() == 4 and player:getSlotItem(CONST_SLOT_ARMOR) and
        player:getSlotItem(CONST_SLOT_LEFT) and player:getItemCount(F.potionItemId) > 1
    if mode == "supplies" then ready = ready and player:getItemCount(F.potionItemId) >= 10 end
    if mode == "upgrade" then ready = ready and player:getSlotItem(CONST_SLOT_LEFT):getId() == F.pickupRewardId end
    if mode == "retention" then ready = ready and player:getItemCount(2050) >= 1 and player:getItemCount(F.starterWeaponId) >= 1 end
    if mode == "no_food" then ready = ready and player:getItemCount(F.meatItemId) == 0 end
    if mode == "food_capacity" then
        ready = ready and player:getFreeCapacity() == ItemType(2696):getWeight() * 6 and player:getItemCount(2696) == 2
    end
    if not ready and attempts > 0 then
        addEvent(F.verifyReadiness, 500, playerId, mode, attempts - 1)
        return
    end
    assert(ready, "combat readiness fixture did not reach its required state: " .. mode)
	if mode == "food_capacity" then
		local usedCapacity = player:getCapacity() - player:getFreeCapacity()
		assert(player:setCapacity(usedCapacity), "food-capacity fixture could not exhaust capacity during the hunt")
		addEvent(F.verifyActiveHuntCapacity, 5000, playerId)
		return
	end
    if mode == "supplies" or mode == "retention" then
        assert(player:teleportTo(F.depotPosition), "readiness fixture could not return to the depot")
    end
    print("PLAYERBOT_GAMEPLAY_TEST READINESS_" .. string.upper(mode) .. "_PASS")
end

function F.verifyLowWealth(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during low-wealth fixture")
    if (player:getMoney() ~= 56 or player:getBankBalance() ~= 0 or
        player:getSlotItem(CONST_SLOT_LEFT):getId() ~= F.pickupRewardId) and attempts > 0 then
        addEvent(F.verifyLowWealth, 500, playerId, attempts - 1)
        return
    end
    assert(player:getMoney() == 56 and player:getBankBalance() == 0 and
        player:getSlotItem(CONST_SLOT_LEFT):getId() == F.pickupRewardId,
        "service did not carry the available gold below the normal reserve")
    print("PLAYERBOT_GAMEPLAY_TEST READINESS_LOW_WEALTH_PASS")
end

function F.verifyEquipmentShadow(playerId, mode, money, leftItemId, rightItemId, armorItemId, position)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during equipment shadow fixture")
    local left = player:getSlotItem(CONST_SLOT_LEFT)
    local right = player:getSlotItem(CONST_SLOT_RIGHT)
    local armor = player:getSlotItem(CONST_SLOT_ARMOR)
    local current = player:getPosition()
    assert(player:getMoney() == money and left and left:getId() == leftItemId and
        ((rightItemId == 0 and not right) or (right and right:getId() == rightItemId)) and
        armor and armor:getId() == armorItemId and current.x == position.x and current.y == position.y and current.z == position.z,
        "equipment shadow fixture mutated player state")
    print("PLAYERBOT_GAMEPLAY_TEST " .. string.upper(mode) .. "_PASS")
end

function F.verifyEquipmentPurchase(playerId, rejected, resumed, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during equipment purchase fixture")
    local left = player:getSlotItem(CONST_SLOT_LEFT)
    local right = player:getSlotItem(CONST_SLOT_RIGHT)
    local equippedId = left and left:getId() == F.equipmentPurchaseItemId and F.equipmentPurchaseItemId or
        right and right:getId() == F.equipmentPurchaseItemId and F.equipmentPurchaseItemId or 0
    local totalMoney = player:getMoney() + player:getBankBalance()
    local complete = rejected and equippedId == 0 and left and left:getId() == F.starterWeaponId and
        player:getItemCount(F.equipmentPurchaseItemId) == 0 and totalMoney == 105 or
        not rejected and equippedId == F.equipmentPurchaseItemId and player:getItemCount(F.starterWeaponId) == 1 and
        totalMoney == 100
    if not complete and attempts > 0 then
        addEvent(F.verifyEquipmentPurchase, 500, playerId, rejected, resumed, attempts - 1)
        return
    end
    assert(complete, "equipment purchase fixture did not reach the expected persisted state")
    print("PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY" .. (rejected and "_REJECTED" or resumed and "_RESUME" or "") .. "_PASS")
end

function F.verifyEquipmentPurchaseSpace(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during equipment storage fixture")
    local left = player:getSlotItem(CONST_SLOT_LEFT)
    assert(left and left:getId() == F.starterWeaponId and player:getItemCount(F.equipmentPurchaseItemId) == 0 and
        player:getMoney() + player:getBankBalance() == 105,
        "equipment storage rejection mutated equipment or money")
    print("PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_SPACE_PASS")
end

function F.verifyOracleDeparture(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during Oracle departure")
    local town = player:getTown()
    local position = player:getPosition()
    local temple = Town(2):getTemplePosition()
    local complete = player:getVocation():getId() == 4 and town and town:getId() == 2 and
        position.x == temple.x and position.y == temple.y and position.z == temple.z
    if not complete and attempts > 0 then
        addEvent(F.verifyOracleDeparture, 250, playerId, attempts - 1)
        return
    end
    assert(complete, "Bot One did not become a Thais knight at the Oracle")
    print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_PASS")
end

function F.verifySpellTraining(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during spell training")
    local complete = player:hasLearnedSpell("Light Healing") and not player:hasLearnedSpell("Find Person") and
        player:getMoney() + player:getBankBalance() == 130
    if not complete and attempts > 0 then
        addEvent(F.verifySpellTraining, 500, playerId, attempts - 1)
        return
    end
    assert(player:hasLearnedSpell("Light Healing") and not player:hasLearnedSpell("Find Person"),
        "spell training did not prioritize Light Healing over Find Person")
    assert(player:getMoney() + player:getBankBalance() == 130, "spell training charged an unexpected amount")
    assert(player:getItemCount(F.potionItemId) >= 5 and player:getItemCount(F.meatItemId) >= 1,
        "spell training spent the recovery supplies")
    assert(player:setStorageValue(F.spellTrainingStorage, 1), "spell training completion marker could not persist")
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_PASS")
end

function F.verifyBerserkMultiTarget(playerId, attempts)
    local player = Player(playerId)
    local secondary = F.spellSecondaryTargetId ~= 0 and Monster(F.spellSecondaryTargetId) or nil
    assert(player and not player:isRemoved(), "Bot One disappeared during Berserk multi-target verification")
    local hitSecondary = secondary and not secondary:isRemoved() and secondary:getHealth() < 1000
    if (not hitSecondary or player:getMana() > 85) and attempts > 0 then
        addEvent(F.verifyBerserkMultiTarget, 250, playerId, attempts - 1)
        return
    end
    assert(hitSecondary, "Berserk did not damage the controlled secondary target")
    assert(player:getMana() <= 85, "Berserk did not consume its normal mana cost for the multi-target cast")
    F.cleanupSpellTargets()
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_USE_PASS")
end

function F.prepareBerserkMultiTarget(playerId, attempts)
    local player = Player(playerId)
    local primary = F.spellTargetId ~= 0 and Monster(F.spellTargetId) or nil
    assert(player and not player:isRemoved(), "Bot One disappeared before Berserk multi-target setup")
    if (not primary or primary:isRemoved() or player:getMana() > 85) and attempts > 0 then
        addEvent(F.prepareBerserkMultiTarget, 250, playerId, attempts - 1)
        return
    end
    assert(primary and not primary:isRemoved(), "Berserk single-target fixture lost its primary target")
    assert(player:getMana() <= 85, "Berserk single-target cast was not observed before multi-target setup")
    assert(F.keepSpellTargetAdjacent(player, primary), "Berserk fixture could not keep its primary target adjacent")
    local secondary = Game.createMonster(F.spellTargetMonsterName, player:getPosition(), true, true)
    assert(secondary, "Berserk fixture could not create a secondary target")
    F.spellSecondaryTargetId = secondary:getId()
    assert(F.placeSpellTarget(player, secondary, {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}}),
        "Berserk fixture could not place the secondary target in its area")
    assert(player:addMana(player:getMaxMana() - player:getMana()), "Berserk fixture could not restore multi-target mana")
    addEvent(F.verifyBerserkMultiTarget, 4750, playerId, 80)
end

function F.verifyBerserkSingleTarget(playerId, attempts)
    local player = Player(playerId)
    local primary = F.spellTargetId ~= 0 and Monster(F.spellTargetId) or nil
    assert(player and not player:isRemoved(), "Bot One disappeared during Berserk single-target verification")
    local hitPrimary = primary and not primary:isRemoved() and primary:getHealth() < 1000
    if (not hitPrimary or player:getMana() > 85) and attempts > 0 then
        addEvent(F.verifyBerserkSingleTarget, 250, playerId, attempts - 1)
        return
    end
    assert(hitPrimary, "Berserk did not damage its visible primary target")
    assert(player:getMana() <= 85, "Berserk did not consume its normal mana cost")
    addEvent(F.prepareBerserkMultiTarget, 0, playerId, 40)
end

function F.verifySpellUse(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during spell-use fixture")
    local target = F.spellTargetId ~= 0 and Monster(F.spellTargetId) or nil
    local recovered = player:getHealth() > 110
    local hasted = F.spellSupportObserved
    local damaged = F.spellTargetId ~= 0 and (not target or target:isRemoved() or target:getHealth() < 1000)
    local offenseCast = player:getMana() == 140
    if (not recovered or not hasted or not damaged or not offenseCast) and attempts > 0 then
        addEvent(F.verifySpellUse, 500, playerId, attempts - 1)
        return
    end
    assert(recovered, "Light Healing did not restore Bot One")
    assert(hasted, "Haste did not apply its normal condition")
    assert(damaged, "Whirlwind Throw did not damage its visible target")
    assert(offenseCast, "Whirlwind Throw did not consume its normal mana cost")
    F.cleanupSpellTargets()
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_USE_PASS")
end

function F.verifyHealing(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the healing test")
    local recovered = player:getHealth() * 100 > player:getMaxHealth() * 60
    local consumed = player:getItemCount(F.potionItemId) < F.healingPotionCount
    if (not recovered or not consumed) and attempts > 0 then
        addEvent(F.verifyHealing, 250, playerId, attempts - 1)
        return
    end
    assert(recovered, "Bot One did not heal above the configured health threshold")
    assert(consumed, "Bot One did not consume the selected recovery potion")
    print("PLAYERBOT_GAMEPLAY_TEST HEALING_STATE_PASS")
end

function F.verifyMovedProviderPurchase(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during moving-provider purchase fixture")
    local legs = player:getSlotItem(CONST_SLOT_LEGS)
    local complete = legs and legs:getId() == F.movedProviderPurchaseItemId and
        player:getItemCount(2468) == 1 and player:getItemCount(2376) == 1 and
        player:getMoney() + player:getBankBalance() == 100
    if not complete and attempts > 0 then
        addEvent(F.verifyMovedProviderPurchase, 500, playerId, attempts - 1)
        return
    end
    assert(complete, "moving-provider purchase did not preserve the displaced equipment and payment")
    print("PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_PROVIDER_MOVED_PASS")
end

function F.verifyCarlinLocalService(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during Carlin service")
	local complete = player:getItemCount(F.healthPotionItemId) >= 10 and player:getMoney() == 100 and
		player:getBankBalance() == 0
    if not complete and attempts > 0 then
        addEvent(F.verifyCarlinLocalService, 250, playerId, attempts - 1)
        return
    end
	assert(complete, "Carlin service did not restock at Rachel and withdraw its reserve from Eva")
    print("PLAYERBOT_GAMEPLAY_TEST CARLIN_LOCAL_SERVICE_PASS")
end

function F.verifyActiveHuntCapacity(playerId)
	local player = Player(playerId)
	assert(player and not player:isRemoved(), "Bot One disappeared after active-hunt capacity exhaustion")
	print("PLAYERBOT_GAMEPLAY_TEST READINESS_FOOD_CAPACITY_PASS")
end

function F.verifySellLoot(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during local sell loot")
    local chest = player:getDepotChest(F.PlayerbotThaisLockerDepotId(), false)
    local complete = chest and player:getItemCount(F.sellLootItemId) == 0 and
        chest:getItemCountById(F.sellLootItemId) == 0 and chest:getItemCountById(7635) == 0 and
        chest:getItemCountById(2050) == 1 and
        player:getMoney() > player:getStorageValue(50100)
    if not complete and attempts > 0 then
        addEvent(F.verifySellLoot, 500, playerId, attempts - 1)
        return
    end
    assert(complete, "local sell loot did not sell only the planned depot batch")
    print("PLAYERBOT_GAMEPLAY_TEST SELL_LOOT_PASS")
end

function F.verifyHealingResupply(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the healing resupply test")
    if player:getHealth() * 100 <= player:getMaxHealth() * 60 and attempts > 0 then
        addEvent(F.verifyHealingResupply, 250, playerId, attempts - 1)
        return
    end
    assert(player:getHealth() * 100 > player:getMaxHealth() * 60,
        "Bot One did not recover after starting without healing supplies")
    print("PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_STATE_PASS")
end

function F.verifyPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the pickup progression test")
    local equipped = player:getSlotItem(CONST_SLOT_LEFT)
    local complete = player:getStorageValue(F.pickupRewardStorage) == 1 and equipped and equipped:getId() == F.pickupRewardId
    if not complete and attempts > 0 then
        addEvent(F.verifyPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(F.pickupRewardStorage) == 1, "pickup reward storage was not persisted")
    assert(equipped and equipped:getId() == F.pickupRewardId, "pickup reward was not equipped")
    assert(player:getItemCount(F.starterWeaponId) == 1, "displaced starter weapon was not preserved")
    assert(player:getSpeed() > 220, "playerbot testing speed boost was not preserved")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_PASS")
end

function F.verifyNestedPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during nested pickup progression")
    local shield = player:getSlotItem(CONST_SLOT_RIGHT)
    local complete = player:getStorageValue(F.nestedRewardStorage) == 1 and shield and
        shield:getId() == F.nestedRewardShieldId
    if not complete and attempts > 0 then
        addEvent(F.verifyNestedPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(F.nestedRewardStorage) == 1, "nested reward storage was not persisted")
    assert(shield and shield:getId() == F.nestedRewardShieldId, "nested wooden shield was not equipped")
    assert(player:getItemCount(F.nestedRewardRootId) == 1, "nested reward root bag was not preserved")
    assert(player:getItemCount(2380) == 1, "nested reward hand axe was not preserved")
    assert(player:getItemCount(2175) == 1, "nested reward spellbook was not preserved")
    assert(player:getItemCount(F.starterWeaponId) == 1, "starter weapon was lost during nested equipment handling")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_PASS")
end

function F.verifyEconomicPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during economic pickup progression")
    local complete = player:getStorageValue(F.economicRewardStorage) == 1 and
        player:getItemCount(2050) >= 1 and player:getItemCount(2152) >= 15
    if not complete and attempts > 0 then
        addEvent(F.verifyEconomicPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(F.economicRewardStorage) == 1, "economic reward storage was not persisted")
    assert(player:getItemCount(2050) >= 1, "economic reward torch was not preserved")
    assert(player:getItemCount(2152) >= 15, "economic reward platinum coins were not merged and preserved")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_BUNDLE_PASS")
end

function F.verifyMainlandReward(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during mainland reward progression")
    local armor = player:getSlotItem(CONST_SLOT_ARMOR)
    local equipped = armor and armor:getId() == F.mainlandRewardItemId
    local complete = player:getStorageValue(F.mainlandRewardStorage) == 1 and equipped
    if not complete and attempts > 0 then
        addEvent(F.verifyMainlandReward, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(F.mainlandRewardStorage) == 1, "mainland reward storage was not persisted")
    assert(equipped, "mainland scale armor reward was not equipped")
    assert(player:getItemCount(F.starterArmorId) == 1, "displaced jacket was not preserved")
    assert(player:getItemCount(1987) >= 1, "mainland reward bag was not preserved")
    print("PLAYERBOT_GAMEPLAY_TEST MAINLAND_REWARD_PASS")
end

function F.triggerArbitrationInterrupt(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before the arbitration interrupt")
    player:addHealth(-math.floor(player:getMaxHealth() / 2))
    print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_INTERRUPT_TRIGGERED")
end

function F.verifyService(playerId, initialDepotBagCount, initialDepotSaleItemCount, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the gameplay test")

    local depotTile = Tile(F.depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local depositedBags = depotTile:getItemCountById(ITEM_BAG) - initialDepotBagCount
	local depositedSaleItems = depotTile:getItemCountById(F.saleItemId) - initialDepotSaleItemCount
	local complete = depositedBags == 1 and depositedSaleItems == 1 and player:getItemCount(F.saleItemId) == 0 and
		player:getItemCount(F.potionItemId) >= 10 and player:getMoney() == 100 and player:getBankBalance() == 1000
    if not complete and attempts > 0 then
		addEvent(F.verifyService, 500, playerId, initialDepotBagCount, initialDepotSaleItemCount, attempts - 1)
        return
    end

    assert(depositedBags == 1, "unexpected deposited loot-bag count: " .. depositedBags)
	assert(depositedSaleItems == 1 and player:getItemCount(F.saleItemId) == 0,
		"policy-approved loot was not deposited")
	assert(player:getItemCount(F.potionItemId) >= 10, "completed potion reserves were not preserved")
	assert(player:getMoney() == 100 and player:getBankBalance() == 1000,
		"completed cash reserves were not preserved")
    assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.seedArmorId, "seeded chain armor was deposited")
    local right = player:getSlotItem(CONST_SLOT_RIGHT)
    assert(right and right:getId() == F.seedShieldId, "seeded dwarven shield was deposited")
    assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == F.seedWeaponId, "seeded carlin sword was deposited")
    assert(player:getSlotItem(CONST_SLOT_FEET):getId() == F.seedBootsId, "seeded leather boots were deposited")
    assert(player:getItemCount(2120) == 1, "rope was deposited")
    assert(player:getItemCount(2554) == 1, "shovel was deposited")
    print("PLAYERBOT_GAMEPLAY_TEST SERVICE_PASS")
end

function F.verifyDepot(playerId, attempts)
    local player = Player(playerId)
    if not player or player:isRemoved() then
        if attempts > 0 then
            addEvent(F.verifyDepot, 500, playerId, attempts - 1)
        end
        return
    end
    local chest = player:getDepotChest(F.PlayerbotThaisLockerDepotId(), false)
    local restartPhase = os.getenv("PLAYERBOT_DEPOT_RESTART_PHASE") or ""
    local expectedLoot = restartPhase ~= "" and 2 or player:getStorageValue(F.depotFixtureStorage) == 2 and 3 or 2
    local priorGoldPersists = restartPhase == "" or restartPhase == "deposit" or restartPhase == "depart"
    local expectedPriorGold = priorGoldPersists and 1 or 0
    local complete = chest and chest:getItemCountById(F.depotLootItemId) == expectedLoot and
        chest:getItemCountById(ITEM_GOLD_COIN) == expectedPriorGold
    if not complete and attempts > 0 then
        addEvent(F.verifyDepot, 500, playerId, attempts - 1)
        return
    end
    assert(complete, "real Thais depot did not retain prior contents and nested loot")
    assert(player:getItemCount(F.depotLootItemId) == 2, "depot did not retain the preferred food reserve")
    assert(chest:getItemCountById(2380) == 1 and chest:getItemCountById(F.starterWeaponId) == 1,
        "depot did not retain inferior and displaced equipment")
    assert(player:getItemCount(2380) == 0 and player:getItemCount(F.starterWeaponId) == 0,
        "inferior or displaced equipment remained carried")
    assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.starterArmorId, "equipped armor was deposited")
    assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == F.seedWeaponId, "carried weapon upgrade was not equipped")
    assert(player:getSlotItem(CONST_SLOT_HEAD):getId() == 2461, "carried helmet upgrade was not equipped")
    assert(player:getSlotItem(CONST_SLOT_FEET):getId() == 2643, "carried boot upgrade was not equipped")
    assert(player:getItemCount(2120) == 1 and player:getItemCount(2554) == 1, "navigation tools were deposited")
    assert(player:getItemCount(F.potionItemId) >= 5 and player:getItemCount(F.meatItemId) >= 1, "supply reserves were deposited")
    print("PLAYERBOT_GAMEPLAY_TEST DEPOT_PASS")
end

function F.verifyDepotRiskFallback(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the depot risk fallback test")
    local position = player:getPosition()
    local crossedRamp = position.z == 6 and math.abs(position.x - 32915) <= 2 and math.abs(position.y - 32230) <= 2
    if not crossedRamp and attempts > 0 then
        addEvent(F.verifyDepotRiskFallback, 500, playerId, attempts - 1)
        return
    end
    assert(crossedRamp, "depot risk fallback did not cross the swamp-troll ramp")
    print("PLAYERBOT_GAMEPLAY_TEST DEPOT_RISK_FALLBACK_PASS")
end

function F.verifySlottedLoot(playerId, sellerAvailable, attempts)
    local player = Player(playerId)
    local chest = player and player:getDepotChest(F.PlayerbotThaisLockerDepotId(), false)
    local ammo = player and player:getSlotItem(CONST_SLOT_AMMO)
    local left = player and player:getSlotItem(CONST_SLOT_LEFT)
    local complete = player and not player:isRemoved() and (not ammo or ammo:getId() ~= F.slottedLootItemId) and
        left and left:getId() == F.slottedLootItemId and
        player:getItemCount(F.slottedLootItemId) == 1 and chest and
        chest:getItemCountById(F.slottedLootItemId) == (sellerAvailable and 0 or 1)
    if not complete and attempts > 0 then
        addEvent(F.verifySlottedLoot, 500, playerId, sellerAvailable, attempts - 1)
        return
    end
    assert(complete, "slotted loot did not reach its verified local disposition")
    print(sellerAvailable and "PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_SELLER_PASS" or
        "PLAYERBOT_GAMEPLAY_TEST SLOTTED_LOOT_NO_SELLER_PASS")
end
