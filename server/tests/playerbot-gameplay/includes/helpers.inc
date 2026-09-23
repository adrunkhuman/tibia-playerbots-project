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

function F.clearBackpack(backpack)
    for _, item in ipairs(backpack:getItems() or {}) do
        assert(item:remove(), "cargo fixture could not clear backpack item " .. item:getId())
    end
end

function F.seedProtectedCargo(backpack, freeSlots, nestedBag)
    assert(backpack:addItem(F.potionItemId, 10), "cargo fixture could not add protected potions")
    assert(backpack:addItem(F.shovelItemId, 1), "cargo fixture could not add the protected shovel")
    if nestedBag then assert(backpack:addItem(ITEM_BAG, 1), "cargo fixture could not add the nested bag") end
    while backpack:getSize() < backpack:getCapacity() - freeSlots do
        assert(backpack:addItem(F.ropeItemId, 1), "cargo fixture could not fill protected slots")
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

-- Match server/schema/insertPlayerbots.sql, not the Rookgaard starter fixtures.
function F.installSeededHuntLoadout(player, mode)
    assert(player:getLevel() == 8 and player:getVocation():getId() == 4,
        "hunt fixture requires the seeded level-8 Knight")
    local money, bank = player:getMoney(), player:getBankBalance()
    local loadout = {
        {CONST_SLOT_HEAD, 2480}, {CONST_SLOT_ARMOR, F.seedArmorId},
        {CONST_SLOT_RIGHT, F.seedShieldId}, {CONST_SLOT_LEFT, F.seedWeaponId},
        {CONST_SLOT_LEGS, 2468}, {CONST_SLOT_FEET, F.seedBootsId},
    }
    for _, entry in ipairs(loadout) do
        local slot, id = entry[1], entry[2]
        local previous = player:getSlotItem(slot)
        if previous then assert(previous:remove(), "hunt fixture could not clear slot " .. slot) end
        assert(player:addItem(id, 1, false, 1, slot), "hunt fixture could not install seeded item " .. id)
        local equipped = player:getSlotItem(slot)
        assert(equipped and equipped:getId() == id, "hunt fixture did not equip seeded item " .. id)
    end
    for _, entry in ipairs({{SKILL_FIST, 10}, {SKILL_CLUB, 10}, {SKILL_SWORD, 20},
        {SKILL_AXE, 10}, {SKILL_DISTANCE, 10}, {SKILL_SHIELD, 20}, {SKILL_FISHING, 10}}) do
        local skill, level = entry[1], entry[2]
        assert(player:addSkillLevel(skill, level - player:getSkillLevel(skill)),
            "hunt fixture could not restore seeded skill " .. skill)
        assert(player:getSkillLevel(skill) == level and player:getSkillTries(skill) == 0,
            "hunt fixture has the wrong seeded skill " .. skill)
    end
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
    assert(backpack and backpack:getId() == 1988 and player:getItemCount(2120) == 1 and player:getItemCount(2554) == 1,
        "hunt fixture lacks the seeded backpack or tools")
    -- Normal first-login gifts can occupy the ammo slot; do not require a
    -- pristine SQL inventory after firstitems.lua has already run.
    assert(not player:getSlotItem(CONST_SLOT_NECKLACE) and not player:getSlotItem(CONST_SLOT_RING),
        "hunt fixture has unseeded combat accessories")
    assert(player:getMagicLevel() == 0 and player:getMaxMana() == 35 and player:getCapacity() == 47000,
        "hunt fixture has the wrong seeded magic or capacity")
    assert(player:getMoney() == money and player:getBankBalance() == bank, "hunt fixture changed funds")
    print("PLAYERBOT_GAMEPLAY_TEST HUNT_MAINLAND_LOADOUT_PASS " .. mode ..
        " head=2480 armor=2464 right=2530 left=2395 legs=2468 feet=2643 sword=20 shielding=20")
end

F.supplyRecoveryRegisteredMonsters = {}

function F.armSupplyRecoveryIncome(player)
    local position = player:getPosition()
    local center = F.supplyRecoveryHuntCenter
    if position.z ~= center.z or math.abs(position.x - center.x) > 30 or math.abs(position.y - center.y) > 30 then
        return
    end
    for _, creature in ipairs(Game.getSpectators(position, true, false, 30, 30, 30, 30)) do
        if creature:isMonster() and creature:getName():lower() == "spider" and
            not F.supplyRecoveryRegisteredMonsters[creature:getId()] then
            assert(creature:registerEvent("PlayerbotSupplyRecoveryCorpseDeath"),
                "supply recovery fixture could not arm spider income")
            F.supplyRecoveryRegisteredMonsters[creature:getId()] = true
        end
    end
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
            creature:getName() ~= F.cargoCoinStackMonsterName and creature:getName() ~= F.spellTargetMonsterName and creature:getName() ~= F.corpseBlockerMonsterName and
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
