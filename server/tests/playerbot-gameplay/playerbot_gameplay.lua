local botName = "Bot One"
local depotPosition = Position(32105, 32195, 8)
local depotTilePosition = Position(32105, 32196, 8)
local lootItemId = ITEM_GOLD_COIN
local lootCount = 37
local emptyMonsterName = "Playerbot Empty Corpse"
local lootMonsterName = "Playerbot Loot Corpse"

local function suppressNearbyMonsters(playerId)
    local player = Player(playerId)
    if not player or player:isRemoved() then
        return
    end
    for _, creature in ipairs(Game.getSpectators(player:getPosition(), true, false, 10, 10, 10, 10)) do
        if creature:isMonster() and creature:getName() ~= emptyMonsterName and creature:getName() ~= lootMonsterName then
            creature:remove()
        end
    end
    addEvent(suppressNearbyMonsters, 100, playerId)
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

local function verifyDeposit(playerId, initialDepotBagCount, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the gameplay test")

    local depotTile = Tile(depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local depositedBags = depotTile:getItemCountById(ITEM_BAG) - initialDepotBagCount
    if depositedBags < 1 and attempts > 0 then
        addEvent(verifyDeposit, 500, playerId, initialDepotBagCount, attempts - 1)
        return
    end

    assert(depositedBags == 1, "unexpected deposited loot-bag count: " .. depositedBags)
    assert(player:getItemCount(lootItemId) == 0, "deposited loot remained in inventory")
    assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == 2463, "plate armor was deposited")
    assert(player:getSlotItem(CONST_SLOT_RIGHT):getId() == 2521, "dark shield was deposited")
    assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == 2392, "fire sword was deposited")
    assert(player:getSlotItem(CONST_SLOT_FEET):getId() == 2195, "Boots of Haste were deposited")
    assert(player:getItemCount(2120) == 1, "rope was deposited")
    assert(player:getItemCount(2554) == 1, "shovel was deposited")
    print("PLAYERBOT_GAMEPLAY_TEST DEPOSIT_PASS")
end

local login = CreatureEvent("zzPlayerbotGameplayRegression")

function login.onLogin(player)
    if player:getName() ~= botName then
        return true
    end

    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "cycle"
    assert(mode == "cycle" or mode == "navigation" or mode == "corpse", "unknown PLAYERBOT_GAMEPLAY_MODE: " .. mode)
    assert(player:teleportTo(depotPosition), "fake depot position could not be restored")
    if mode == "navigation" then
        createTemporaryBlockers()
        suppressNearbyMonsters(player:getId())
        print("PLAYERBOT_GAMEPLAY_TEST NAVIGATION_START")
        return true
    end
    if mode == "corpse" then
        suppressNearbyMonsters(player:getId())
        addEvent(spawnCorpseMonster, 500, player:getId(), emptyMonsterName)
        addEvent(spawnCorpseMonster, 8000, player:getId(), lootMonsterName)
        print("PLAYERBOT_GAMEPLAY_TEST CORPSE_START")
        return true
    end

    local existingGold = player:getItemCount(lootItemId)
    if existingGold > 0 then
        assert(player:removeItem(lootItemId, existingGold), "existing gold could not be removed")
    end
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
    assert(backpack and backpack:getId() == ITEM_BACKPACK, "seeded backpack is missing")
    local lootBag = backpack:addItem(ITEM_BAG, 1)
    assert(lootBag, "test loot bag could not be added")
    assert(lootBag:addItem(lootItemId, lootCount), "test loot could not be added")

    local depotTile = Tile(depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local initialDepotBagCount = depotTile:getItemCountById(ITEM_BAG)
    addEvent(verifyDeposit, 500, player:getId(), initialDepotBagCount, 40)
    print("PLAYERBOT_GAMEPLAY_TEST START")
    return true
end

login:register()
