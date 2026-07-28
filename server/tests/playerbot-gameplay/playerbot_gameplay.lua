local botName = "Bot One"
local depotPosition = Position(32105, 32195, 8)
local depotTilePosition = Position(32105, 32196, 8)
local lootItemId = ITEM_GOLD_COIN
local lootCount = 37
local saleItemId = 2992
local potionItemId = 8704
local meatItemId = 2666
local emptyMonsterName = "Playerbot Empty Corpse"
local lootMonsterName = "Playerbot Loot Corpse"
local nonlootableMonsterName = "Playerbot Nonlootable Corpse"
local containerDeathItemMonsterName = "Playerbot Container Death Item"
local defensiveMonsterName = "Playerbot Defensive Threat"
local deathMonsterName = "Playerbot Death Threat"
local valueMonsterName = "Playerbot Value Corpse"
local healingPotionCount = 3

local function suppressNearbyMonsters(playerId)
    local player = Player(playerId)
    if not player or player:isRemoved() then
        return
    end
    for _, creature in ipairs(Game.getSpectators(player:getPosition(), true, false, 10, 10, 10, 10)) do
        if creature:isMonster() and creature:getName() ~= emptyMonsterName and creature:getName() ~= lootMonsterName and
            creature:getName() ~= nonlootableMonsterName and creature:getName() ~= containerDeathItemMonsterName and
            creature:getName() ~= valueMonsterName then
            creature:remove()
        end
    end
    addEvent(suppressNearbyMonsters, 100, playerId)
end

local function activateDefensiveMonster(playerId, monsterId)
    local player = Player(playerId)
    local monster = Monster(monsterId)
    assert(player and not player:isRemoved(), "Bot One disappeared before defensive blockers activated")
    assert(monster and monster:selectTarget(player), "defensive blocker could not target Bot One")
end

local function spawnDefensiveMonsters(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before defensive combat test spawn")
    local origin = player:getPosition()
    local spawned = 0
    for _, position in ipairs({
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
        Position(origin.x + 1, origin.y + 1, origin.z),
        Position(origin.x + 1, origin.y - 1, origin.z),
        Position(origin.x - 1, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y - 1, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(defensiveMonsterName, position, true, true)
            assert(monster, "defensive blocker could not be created")
            addEvent(activateDefensiveMonster, 2000, playerId, monster:getId())
            spawned = spawned + 1
        end
    end
    assert(spawned >= 1, "no adjacent defensive blocker could be created")
    print("PLAYERBOT_GAMEPLAY_TEST DEFENSIVE_BLOCKERS_SPAWNED " .. spawned)
end

local function spawnDeathMonster(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before death telemetry test")
    local origin = player:getPosition()
    for _, position in ipairs({
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(deathMonsterName, position, true, true)
            assert(monster and monster:selectTarget(player), "death test monster could not target Bot One")
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_THREAT_SPAWNED")
            return
        end
    end
    error("no adjacent tile was available for death telemetry test monster")
end

local function spawnCorpseMonster(playerId, monsterName)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before corpse test spawn")
    local origin = player:getPosition()
    local candidates = {
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
    }
    for _, position in ipairs(candidates) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(monsterName, position, true, true)
            assert(monster, "corpse test monster could not be created: " .. monsterName)
            print("PLAYERBOT_GAMEPLAY_TEST SPAWNED " .. monsterName)
            return
        end
    end
    error("no adjacent tile was available for corpse test monster")
end

local function removeBlockers(firstId, secondId, thirdId)
    for _, blockerId in ipairs({firstId, secondId, thirdId}) do
        local blocker = Npc(blockerId)
        if blocker then
            blocker:remove()
        end
    end
    print("PLAYERBOT_GAMEPLAY_TEST BLOCKERS_REMOVED")
end

local function createTemporaryBlockers()
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
    addEvent(removeBlockers, 3000, blockerIds[1], blockerIds[2], blockerIds[3])
end

local function verifyHealing(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the healing test")
    local recovered = player:getHealth() * 100 > player:getMaxHealth() * 60
    local consumed = player:getItemCount(potionItemId) < healingPotionCount
    if (not recovered or not consumed) and attempts > 0 then
        addEvent(verifyHealing, 250, playerId, attempts - 1)
        return
    end
    assert(recovered, "Bot One did not heal above the configured health threshold")
    assert(consumed, "Bot One did not consume a small health potion")
    print("PLAYERBOT_GAMEPLAY_TEST HEALING_STATE_PASS")
end

local function verifyHealingResupply(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the healing resupply test")
    if player:getHealth() * 100 <= player:getMaxHealth() * 60 and attempts > 0 then
        addEvent(verifyHealingResupply, 250, playerId, attempts - 1)
        return
    end
    assert(player:getHealth() * 100 > player:getMaxHealth() * 60,
        "Bot One did not recover after starting without healing supplies")
    print("PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_STATE_PASS")
end

local function verifyService(playerId, initialDepotBagCount, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the gameplay test")

    local depotTile = Tile(depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local depositedBags = depotTile:getItemCountById(ITEM_BAG) - initialDepotBagCount
    local complete = depositedBags == 1 and player:getItemCount(saleItemId) == 0 and
        player:getItemCount(potionItemId) >= 5 and player:getItemCount(meatItemId) >= 1 and
        player:getMoney() == 100 and player:getBankBalance() == 10034
    if not complete and attempts > 0 then
        addEvent(verifyService, 500, playerId, initialDepotBagCount, attempts - 1)
        return
    end

    assert(depositedBags == 1, "unexpected deposited loot-bag count: " .. depositedBags)
    assert(player:getItemCount(saleItemId) == 0, "policy-approved loot was not sold")
    assert(player:getItemCount(potionItemId) >= 5, "small health potion reserve was not purchased")
    assert(player:getItemCount(meatItemId) >= 1, "meat reserve was not purchased")
    assert(player:getMoney() == 100, "banker did not leave the carried gold reserve")
    assert(player:getBankBalance() == 10034, "shop purchases and bank transactions produced the wrong balance")
    assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == 2463, "plate armor was deposited")
    assert(player:getSlotItem(CONST_SLOT_RIGHT):getId() == 2521, "dark shield was deposited")
    assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == 2392, "fire sword was deposited")
    assert(player:getSlotItem(CONST_SLOT_FEET):getId() == 2195, "Boots of Haste were deposited")
    assert(player:getItemCount(2120) == 1, "rope was deposited")
    assert(player:getItemCount(2554) == 1, "shovel was deposited")
    print("PLAYERBOT_GAMEPLAY_TEST SERVICE_PASS")
end

local login = CreatureEvent("zzPlayerbotGameplayRegression")

function login.onLogin(player)
    if player:getName() ~= botName then
        return true
    end

    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "cycle"
    assert(mode == "cycle" or mode == "navigation" or mode == "corpse" or mode == "death" or mode == "healing" or
        mode == "healing_resupply" or mode == "value", "unknown PLAYERBOT_GAMEPLAY_MODE: " .. mode)
    assert(player:teleportTo(depotPosition), "fake depot position could not be restored")
    if mode == "death" then
        addEvent(spawnDeathMonster, 100, player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST DEATH_START")
        return true
    end
    if mode == "navigation" then
        createTemporaryBlockers()
        suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST NAVIGATION_START")
        return true
    end
    if mode == "healing" then
        suppressNearbyMonsters(player:getId())
        assert(player:getItemCount(potionItemId) == 0, "healing fixture expected no seeded potions")
        assert(player:addItem(potionItemId, healingPotionCount), "healing fixture could not add small health potions")
        assert(player:setHealth(100), "healing fixture could not lower Bot One's health")
        addEvent(verifyHealing, 250, player:getId(), 40)
        print("PLAYERBOT_GAMEPLAY_TEST HEALING_START")
        return true
    end
    if mode == "healing_resupply" then
        suppressNearbyMonsters(player:getId())
        assert(player:getItemCount(potionItemId) == 0, "healing resupply fixture expected no seeded potions")
        assert(player:setHealth(100), "healing resupply fixture could not lower Bot One's health")
        addEvent(verifyHealingResupply, 250, player:getId(), 240)
        print("PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_START")
        return true
    end
    if mode == "corpse" then
        assert(not ItemType(ITEM_GOLD_COIN):isCorpse() and not ItemType(ITEM_GOLD_COIN):isContainer(),
            "non-lootable corpse fixture must not be an openable corpse")
        assert(not ItemType(ITEM_BAG):isCorpse() and ItemType(ITEM_BAG):isContainer(),
            "container death-item fixture must be a non-corpse container")
        suppressNearbyMonsters(player:getId())
        addEvent(spawnCorpseMonster, 500, player:getId(), nonlootableMonsterName)
        addEvent(spawnCorpseMonster, 5000, player:getId(), emptyMonsterName)
        addEvent(spawnCorpseMonster, 12500, player:getId(), lootMonsterName)
        addEvent(spawnCorpseMonster, 20000, player:getId(), containerDeathItemMonsterName)
        print("PLAYERBOT_GAMEPLAY_TEST CORPSE_START")
        return true
    end
    if mode == "value" then
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack and backpack:addItem(saleItemId, 1), "low-value replacement cargo could not be added")
        assert(player:removeMoney(player:getMoney()), "value fixture could not force bank-funded supply purchases")
        local incomingWeight = ItemType(2826):getWeight()
        local usedCapacity = player:getCapacity() - player:getFreeCapacity()
        assert(incomingWeight > 1, "value replacement fixture has invalid incoming weight")
        player:setCapacity(usedCapacity + incomingWeight - 1)
        suppressNearbyMonsters(player:getId())
        addEvent(spawnCorpseMonster, 500, player:getId(), valueMonsterName)
        print("PLAYERBOT_GAMEPLAY_TEST VALUE_START")
        return true
    end

    assert(player:getMoney() == 200, "Bot One initial backpack purse was not 200 gp")
    assert(player:getBankBalance() == 10000, "Bot One initial bank balance was not 10000 gp")
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
    assert(backpack and backpack:getId() == ITEM_BACKPACK, "seeded backpack is missing")
    local lootBag = backpack:addItem(ITEM_BAG, 1)
    assert(lootBag, "test loot bag could not be added")
    assert(lootBag:addItem(lootItemId, lootCount), "test loot could not be added")
    assert(backpack:addItem(saleItemId, 1), "NPC-discovered sellable loot could not be added")
    addEvent(spawnDefensiveMonsters, 100, player:getId())

    local depotTile = Tile(depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local initialDepotBagCount = depotTile:getItemCountById(ITEM_BAG)
    addEvent(verifyService, 500, player:getId(), initialDepotBagCount, 500)
    print("PLAYERBOT_GAMEPLAY_TEST START")
    return true
end

login:register()
