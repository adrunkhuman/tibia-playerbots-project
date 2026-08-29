local F = PlayerbotGameplayFixture

function F.PlayerbotThaisLockerDepotId()
    local tile = Tile(Position(32352, 32225, 7))
    assert(tile, "real Thais locker tile is unavailable")
    for _, item in ipairs(tile:getItems() or {}) do
        if isDepot(item:getUniqueId()) then
            local depotId = getDepotId(item:getUniqueId())
            assert(depotId, "real Thais locker depot ID is unavailable")
            return depotId
        end
    end
    error("real Thais locker is unavailable")
end

function F.removeAll(player, itemId)
    local count = player:getItemCount(itemId)
    if count > 0 then
        assert(player:removeItem(itemId, count), "fixture could not remove item " .. itemId)
    end
end

function F.selectHealthPotion(player)
    F.potionItemId = player:getVocation():getId() == 0 and F.smallHealthPotionItemId or F.healthPotionItemId
end

function F.removeAllHealthPotions(player)
    F.removeAll(player, F.smallHealthPotionItemId)
    F.removeAll(player, F.healthPotionItemId)
end

function F.restoreRookgaardBaseline(player, acceptSeedBaseline)
    local armor = player:getSlotItem(CONST_SLOT_ARMOR)
    local baselineArmor = armor and (armor:getId() == 2463 or acceptSeedBaseline and armor:getId() == F.seedArmorId)
    if player:getLevel() ~= 8 or player:getVocation():getId() ~= 4 or not baselineArmor then
        return
    end

    assert(player:setVocation(0), "fixture could not restore the vocationless baseline")
    local experience = player:getExperience()
    if experience > 0 then
        assert(player:removeExperience(experience), "fixture could not restore level 1")
    end
    local rookgaard = Town(6)
    assert(rookgaard and player:setTown(rookgaard), "fixture could not restore Rookgaard")
    assert(player:teleportTo(rookgaard:getTemplePosition()), "fixture could not reach the Rookgaard temple")
    for _, slot in ipairs({CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET}) do
        local item = player:getSlotItem(slot)
        if item then
            assert(item:remove(), "fixture could not clear equipment slot " .. slot)
        end
    end
    for _, itemId in ipairs({2457, 2463, 2525, 2376, 2647, 2643}) do
        F.removeAll(player, itemId)
    end
    assert(player:addItem(F.starterArmorId, 1, false, 1, CONST_SLOT_ARMOR), "fixture could not restore starter armor")
    assert(player:addItem(F.starterWeaponId, 1, false, 1, CONST_SLOT_LEFT), "fixture could not restore starter weapon")
    assert(player:getSlotItem(CONST_SLOT_ARMOR) and player:getSlotItem(CONST_SLOT_ARMOR):getId() == F.starterArmorId,
        "fixture did not equip starter armor")
    assert(player:getSlotItem(CONST_SLOT_LEFT) and player:getSlotItem(CONST_SLOT_LEFT):getId() == F.starterWeaponId,
        "fixture did not equip starter weapon")
end

function F.removeNearbyMonsters(player)
    for _, creature in ipairs(Game.getSpectators(player:getPosition(), true, false, 10, 10, 10, 10)) do
        if creature:isMonster() then
            creature:remove()
        end
    end
end

function F.suppressNearbyMonsters(playerId)
    local player = Player(playerId)
    if not player or player:isRemoved() then
        return
    end
    for _, creature in ipairs(Game.getSpectators(player:getPosition(), true, false, 10, 10, 10, 10)) do
        if creature:isMonster() and creature:getName() ~= F.emptyMonsterName and creature:getName() ~= F.lootMonsterName and
            creature:getName() ~= F.nonlootableMonsterName and creature:getName() ~= F.containerDeathItemMonsterName and
            creature:getName() ~= F.valueMonsterName and creature:getName() ~= "Playerbot Food Cap Corpse" and
            creature:getName() ~= F.spellTargetMonsterName and creature:getName() ~= F.corpseBlockerMonsterName and
            creature:getName() ~= F.defensiveMonsterName then
            creature:remove()
        end
    end
    addEvent(F.suppressNearbyMonsters, 100, playerId)
end

function F.activateDefensiveMonster(playerId, monsterId)
    local player = Player(playerId)
    local monster = Monster(monsterId)
    assert(player and not player:isRemoved(), "Bot One disappeared before defensive blockers activated")
    assert(monster and monster:selectTarget(player), "defensive blocker could not target Bot One")
end

function F.moveEquipmentProvider(playerId, startX, startY, startZ, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before the equipment provider moved")
    local current = player:getPosition()
    if current.x == startX and current.y == startY and current.z == startZ then
        assert(attempts < 200, "Bot One did not start the equipment-provider route")
        addEvent(F.moveEquipmentProvider, 100, playerId, startX, startY, startZ, attempts + 1)
        return
    end

    local provider
    for _, creature in ipairs(Game.getSpectators(current, true, false, 200, 200, 200, 200)) do
        local npc = Npc(creature)
        if npc and npc:getName() == "Cornelia" then
            provider = npc
            break
        end
    end
    assert(provider and not provider:isRemoved(), "equipment provider was unavailable for relocation")
    local origin = provider:getPosition()
    for radius = 7, 10 do
        for x = -radius, radius do
            for y = -radius, radius do
                if math.max(math.abs(x), math.abs(y)) == radius then
                    local destination = Position(origin.x + x, origin.y + y, origin.z)
                    local tile = Tile(destination)
                    if tile and tile:isWalkable() and provider:teleportTo(destination, true) then
                        print("PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_PROVIDER_MOVED " .. provider:getId() .. " " ..
                            origin.x .. ":" .. origin.y .. ":" .. origin.z .. " " ..
                            destination.x .. ":" .. destination.y .. ":" .. destination.z)
                        return
                    end
                end
            end
        end
    end
    error("equipment provider could not be relocated to a walkable tile")
end

function F.removeBlockers(firstId, secondId, thirdId)
    for _, blockerId in ipairs({firstId, secondId, thirdId}) do
        local blocker = Npc(blockerId)
        if blocker then
            blocker:remove()
        end
    end
    print("PLAYERBOT_GAMEPLAY_TEST BLOCKERS_REMOVED")
end

function F.createTemporaryBlockers()
    local blockerPositions = {
        Position(32105, 32194, 8),
        Position(32104, 32195, 8),
        Position(32104, 32194, 8),
    }
    local blockerNames = {"Cipfried", "Seymour", "Tom"}
    local blockerIds = {}
    for index, position in ipairs(blockerPositions) do
        local blocker = Game.createNpc(blockerNames[index], position, true, true)
        assert(blocker, "temporary navigation blocker could not be created")
        blockerIds[#blockerIds + 1] = blocker:getId()
    end
    addEvent(F.removeBlockers, 3000, blockerIds[1], blockerIds[2], blockerIds[3])
end

function F.setupSlottedLoot(player, sellerAvailable)
    local town = Town(F.thaisTownId)
    assert(town and player:setTown(town), "slotted loot fixture could not select Thais")
    assert(player:teleportTo(Position(32345, 32225, 7)), "slotted loot fixture could not reach Naji")
    local left = player:getSlotItem(CONST_SLOT_LEFT)
    local right = player:getSlotItem(CONST_SLOT_RIGHT)
    if left then assert(left:remove(), "slotted loot fixture could not clear the left hand") end
    if right then assert(right:remove(), "slotted loot fixture could not clear the right hand") end
    local weapon = Game.createItem(F.slottedLootItemId, 1)
    assert(weapon and player:addItemEx(weapon, true, CONST_SLOT_LEFT) == RETURNVALUE_NOERROR,
        "slotted loot fixture could not equip its protected weapon")
    local ammo = player:getSlotItem(CONST_SLOT_AMMO)
    local chest = player:getDepotChest(F.PlayerbotThaisLockerDepotId(), true)
    local persistedDeposit = not ammo and chest and chest:getItemCountById(F.slottedLootItemId) == 1
    assert((ammo and ammo:getId() == F.slottedLootItemId) or persistedDeposit,
        "slotted loot fixture did not load mace 2398 in the ammunition slot")
    F.suppressNearbyMonsters(player:getId())
    addEvent(F.verifySlottedLoot, 500, player:getId(), sellerAvailable, 480)
end
