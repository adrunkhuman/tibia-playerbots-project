local botName = "Bot One"
local depotPosition = Position(32105, 32195, 8)
local depotTilePosition = Position(32105, 32196, 8)
local thaisTownId = 2
local depotLootItemId = 2684
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
local levelEightMonsterName = "Playerbot Level Eight Target"
local deathMonsterName = "Playerbot Death Threat"
local valueMonsterName = "Playerbot Value Corpse"
local healingPotionCount = 3
local starterArmorId = 2650
local starterWeaponId = 2382
local pickupRewardId = 2384
local pickupRewardStorage = 64120
local nestedRewardStorage = 50083
local nestedRewardRootId = 1994
local nestedRewardShieldId = 2512
local economicRewardStorage = 50082
local departureRecoveryStorage = 50090
local depotFixtureStorage = 50095
local spellTrainingStorage = 50097
local deathLoginCount = 0
local removeAll

local function restoreRookgaardBaseline(player)
    local armor = player:getSlotItem(CONST_SLOT_ARMOR)
    if player:getLevel() ~= 8 or player:getVocation():getId() ~= 4 or not armor or armor:getId() ~= 2463 then
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
        removeAll(player, itemId)
    end
    assert(player:addItem(starterArmorId, 1, false, 1, CONST_SLOT_ARMOR), "fixture could not restore starter armor")
    assert(player:addItem(starterWeaponId, 1, false, 1, CONST_SLOT_LEFT), "fixture could not restore starter weapon")
    assert(player:getSlotItem(CONST_SLOT_ARMOR) and player:getSlotItem(CONST_SLOT_ARMOR):getId() == starterArmorId,
        "fixture did not equip starter armor")
    assert(player:getSlotItem(CONST_SLOT_LEFT) and player:getSlotItem(CONST_SLOT_LEFT):getId() == starterWeaponId,
        "fixture did not equip starter weapon")
end

removeAll = function(player, itemId)
    local count = player:getItemCount(itemId)
    if count > 0 then
        assert(player:removeItem(itemId, count), "fixture could not remove item " .. itemId)
    end
end

local function verifyReadiness(playerId, mode, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during readiness fixture")
    local ready = player:getVocation():getId() == 4 and player:getSlotItem(CONST_SLOT_ARMOR) and
        player:getSlotItem(CONST_SLOT_LEFT) and player:getItemCount(potionItemId) >= 5 and player:getItemCount(meatItemId) >= 1
    if mode == "upgrade" then ready = ready and player:getSlotItem(CONST_SLOT_LEFT):getId() == pickupRewardId end
    if mode == "retention" then ready = ready and player:getItemCount(2050) >= 1 and player:getItemCount(starterWeaponId) >= 1 end
    if not ready and attempts > 0 then
        addEvent(verifyReadiness, 500, playerId, mode, attempts - 1)
        return
    end
    assert(ready, "combat readiness fixture did not reach its required state: " .. mode)
    if mode == "supplies" or mode == "retention" then
        assert(player:teleportTo(depotPosition), "readiness fixture could not return to the depot")
    end
    print("PLAYERBOT_GAMEPLAY_TEST READINESS_" .. string.upper(mode) .. "_PASS")
end

local function verifyOracleDeparture(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during Oracle departure")
    local town = player:getTown()
    local position = player:getPosition()
    local temple = Town(2):getTemplePosition()
    local complete = player:getVocation():getId() == 4 and town and town:getId() == 2 and
        position.x == temple.x and position.y == temple.y and position.z == temple.z
    if not complete and attempts > 0 then
        addEvent(verifyOracleDeparture, 250, playerId, attempts - 1)
        return
    end
    assert(complete, "Bot One did not become a Thais knight at the Oracle")
    print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_PASS")
end

local function verifySpellTraining(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during spell training")
    local complete = player:hasLearnedSpell("Find Person") and player:getMoney() + player:getBankBalance() == 220
    if not complete and attempts > 0 then
        addEvent(verifySpellTraining, 500, playerId, attempts - 1)
        return
    end
    assert(player:hasLearnedSpell("Find Person"), "spell training did not learn Find Person")
    assert(player:getMoney() + player:getBankBalance() == 220, "spell training charged an unexpected amount")
    assert(player:getItemCount(potionItemId) >= 5 and player:getItemCount(meatItemId) >= 1,
        "spell training spent the recovery supplies")
    assert(player:setStorageValue(spellTrainingStorage, 1), "spell training completion marker could not persist")
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_PASS")
end

local function removeNearbyMonsters(player)
    for _, creature in ipairs(Game.getSpectators(player:getPosition(), true, false, 10, 10, 10, 10)) do
        if creature:isMonster() then
            creature:remove()
        end
    end
end

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

local function hidePursuitMonster(playerId, monsterId, distance)
    local player = Player(playerId)
    local monster = Monster(monsterId)
    assert(player and not player:isRemoved(), "Bot One disappeared before target pursuit")
    assert(monster and not monster:isRemoved(), "target pursuit monster disappeared before target loss")
    local position = player:getPosition()
    assert(monster:teleportTo(Position(position.x + distance, position.y, position.z), true),
        "target pursuit fixture could not hide the monster")
    print("PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_HIDDEN " .. monsterId)
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

local function prepareDeath(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before death recovery setup")
    assert(player:teleportTo(depotPosition), "death recovery fixture could not leave the temple protection zone")
    removeNearbyMonsters(player)
    spawnDeathMonster(playerId)
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

local function spawnDepartureDefensiveThreat(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before departure defensive combat")
    local origin = player:getPosition()
    for _, position in ipairs({
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(defensiveMonsterName, position, true, true)
            assert(monster and monster:selectTarget(player), "departure defensive threat could not target Bot One")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_DEFENSIVE_THREAT_SPAWNED")
            return
        end
    end
    error("no adjacent tile was available for the departure defensive threat")
end

local function prepareLevelEightCombat(player)
    local targetExperience = Game.getExperienceForLevel(8) - 1
    assert(player:getExperience() < targetExperience, "level-eight combat fixture expected a low-level player")
    player:addExperience(targetExperience - player:getExperience())
    assert(player:getLevel() == 7, "level-eight combat fixture could not prepare level 7")
    assert(player:teleportTo(depotPosition), "level-eight combat fixture could not leave the temple")
    removeNearbyMonsters(player)
    spawnCorpseMonster(player:getId(), levelEightMonsterName)
    addEvent(spawnDepartureDefensiveThreat, 2500, player:getId())
    addEvent(verifyOracleDeparture, 500, player:getId(), 360)
    print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_INTERRUPT_START")
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

local function verifyPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the pickup progression test")
    local equipped = player:getSlotItem(CONST_SLOT_LEFT)
    local complete = player:getStorageValue(pickupRewardStorage) == 1 and equipped and equipped:getId() == pickupRewardId
    if not complete and attempts > 0 then
        addEvent(verifyPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(pickupRewardStorage) == 1, "pickup reward storage was not persisted")
    assert(equipped and equipped:getId() == pickupRewardId, "pickup reward was not equipped")
    assert(player:getItemCount(starterWeaponId) == 1, "displaced starter weapon was not preserved")
    assert(player:getSpeed() > 220, "playerbot testing speed boost was not preserved")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_PASS")
end

local function verifyNestedPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during nested pickup progression")
    local shield = player:getSlotItem(CONST_SLOT_RIGHT)
    local complete = player:getStorageValue(nestedRewardStorage) == 1 and shield and
        shield:getId() == nestedRewardShieldId
    if not complete and attempts > 0 then
        addEvent(verifyNestedPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(nestedRewardStorage) == 1, "nested reward storage was not persisted")
    assert(shield and shield:getId() == nestedRewardShieldId, "nested wooden shield was not equipped")
    assert(player:getItemCount(nestedRewardRootId) == 1, "nested reward root bag was not preserved")
    assert(player:getItemCount(2380) == 1, "nested reward hand axe was not preserved")
    assert(player:getItemCount(2175) == 1, "nested reward spellbook was not preserved")
    assert(player:getItemCount(starterWeaponId) == 1, "starter weapon was lost during nested equipment handling")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_PASS")
end

local function verifyEconomicPickupProgression(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during economic pickup progression")
    local complete = player:getStorageValue(economicRewardStorage) == 1 and
        player:getItemCount(2050) >= 1 and player:getItemCount(2152) >= 15
    if not complete and attempts > 0 then
        addEvent(verifyEconomicPickupProgression, 500, playerId, attempts - 1)
        return
    end
    assert(player:getStorageValue(economicRewardStorage) == 1, "economic reward storage was not persisted")
    assert(player:getItemCount(2050) >= 1, "economic reward torch was not preserved")
    assert(player:getItemCount(2152) >= 15, "economic reward platinum coins were not merged and preserved")
    print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_BUNDLE_PASS")
end

local function triggerArbitrationInterrupt(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before the arbitration interrupt")
    player:addHealth(-math.floor(player:getMaxHealth() / 2))
    print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_INTERRUPT_TRIGGERED")
end

local function verifyService(playerId, initialDepotBagCount, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared during the gameplay test")

    local depotTile = Tile(depotTilePosition)
    assert(depotTile, "fake depot tile is unavailable")
    local depositedBags = depotTile:getItemCountById(ITEM_BAG) - initialDepotBagCount
    local complete = depositedBags == 1 and player:getItemCount(saleItemId) == 0 and
        player:getItemCount(potionItemId) >= 5 and player:getItemCount(meatItemId) >= 1 and
        player:getMoney() == 100 and player:getBankBalance() == 134
    if not complete and attempts > 0 then
        addEvent(verifyService, 500, playerId, initialDepotBagCount, attempts - 1)
        return
    end

    assert(depositedBags == 1, "unexpected deposited loot-bag count: " .. depositedBags)
    assert(player:getItemCount(saleItemId) == 0, "policy-approved loot was not sold")
    assert(player:getItemCount(potionItemId) >= 5, "small health potion reserve was not purchased")
    assert(player:getItemCount(meatItemId) >= 1, "meat reserve was not purchased")
    assert(player:getMoney() == 100, "banker did not leave the carried gold reserve")
	assert(player:getBankBalance() == 134, "shop purchases and bank transactions produced the wrong balance")
	assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == starterArmorId, "starter jacket was deposited")
	local right = player:getSlotItem(CONST_SLOT_RIGHT)
	assert(not right or right:getId() == meatItemId, "unexpected item appeared in the starter shield slot")
	assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == starterWeaponId, "starter club was deposited")
	local feet = player:getSlotItem(CONST_SLOT_FEET)
	assert(not feet or feet:getId() == potionItemId, "unexpected item appeared in the starter feet slot")
    assert(player:getItemCount(2120) == 1, "rope was deposited")
    assert(player:getItemCount(2554) == 1, "shovel was deposited")
    print("PLAYERBOT_GAMEPLAY_TEST SERVICE_PASS")
end

local function verifyDepot(playerId, attempts)
	local player = Player(playerId)
	if not player or player:isRemoved() then
		if attempts > 0 then
			addEvent(verifyDepot, 500, playerId, attempts - 1)
		end
		return
	end
	local chest = player:getDepotChest(thaisTownId, false)
	local restartPhase = os.getenv("PLAYERBOT_DEPOT_RESTART_PHASE") or ""
	local expectedLoot = restartPhase ~= "" and 2 or player:getStorageValue(depotFixtureStorage) == 2 and 3 or 2
	local priorGoldPersists = restartPhase == "" or restartPhase == "deposit" or restartPhase == "depart"
	local expectedPriorGold = priorGoldPersists and 1 or 0
	local complete = chest and chest:getItemCountById(depotLootItemId) == expectedLoot and
		chest:getItemCountById(ITEM_GOLD_COIN) == expectedPriorGold
    if not complete and attempts > 0 then
        addEvent(verifyDepot, 500, playerId, attempts - 1)
        return
    end
    assert(complete, "real Thais depot did not retain prior contents and nested loot")
    assert(player:getItemCount(depotLootItemId) == 0, "nested policy-approved loot remained carried")
    assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == starterArmorId, "equipped armor was deposited")
    assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == starterWeaponId, "equipped weapon was deposited")
    assert(player:getItemCount(2120) == 1 and player:getItemCount(2554) == 1, "navigation tools were deposited")
    assert(player:getItemCount(potionItemId) >= 5 and player:getItemCount(meatItemId) >= 1, "supply reserves were deposited")
    print("PLAYERBOT_GAMEPLAY_TEST DEPOT_PASS")
end

local login = CreatureEvent("zzPlayerbotGameplayRegression")

function login.onLogin(player)
    if player:getName() ~= botName then
        return true
    end

    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "cycle"
	assert(mode == "mainland" or mode == "cycle" or mode == "depot" or mode == "navigation" or mode == "target_pursuit" or mode == "target_pursuit_abandon" or mode == "corpse" or mode == "death" or mode == "healing" or
		mode == "healing_resupply" or mode == "value" or mode == "progression" or mode == "progression_bundle" or
		mode == "progression_nested" or
        mode == "progression_resume" or mode == "progression_nested_resume" or mode == "progression_space" or
        mode == "arbitration" or
        mode == "arbitration_interrupt" or mode == "departure" or mode == "departure_interrupt" or
        mode == "departure_recovery" or mode == "spell_training" or mode == "stamina_bonus" or mode == "stamina_boundary" or
        mode == "stamina_normal" or mode == "hunt_planning" or mode == "readiness_ready" or
        mode == "readiness_upgrade" or mode == "readiness_missing_weapon" or mode == "readiness_supplies" or
        mode == "readiness_retention",
        "unknown PLAYERBOT_GAMEPLAY_MODE: " .. mode)
	if mode == "mainland" then
		local town = player:getTown()
		assert(player:getLevel() == 8 and player:getVocation():getId() == 4 and town and town:getId() == thaisTownId,
			"mainland fixture did not load the level-8 Thais Knight")
		assert(player:getSlotItem(CONST_SLOT_ARMOR) and player:getSlotItem(CONST_SLOT_ARMOR):getId() == 2463,
			"mainland fixture did not load plate armor")
		local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
		assert(backpack and backpack:addItem(2696, 1), "mainland fixture could not seed local-service loot")
		print("PLAYERBOT_GAMEPLAY_TEST MAINLAND_START")
		return true
	end
	restoreRookgaardBaseline(player)
	if mode == "readiness_ready" or mode == "readiness_upgrade" or mode == "readiness_missing_weapon" or
		mode == "readiness_supplies" or mode == "readiness_retention" then
		assert(player:setVocation(4), "readiness fixture could not select Knight vocation")
		assert(player:teleportTo(depotPosition), "readiness fixture could not reach the depot")
		suppressNearbyMonsters(player:getId())
		if mode == "readiness_upgrade" then
			local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
			assert(backpack and backpack:addItem(pickupRewardId, 1), "readiness upgrade fixture could not add carried weapon")
			addEvent(verifyReadiness, 500, player:getId(), "upgrade", 120)
		elseif mode == "readiness_missing_weapon" then
			removeAll(player, starterWeaponId)
			removeAll(player, pickupRewardId)
			print("PLAYERBOT_GAMEPLAY_TEST READINESS_MISSING_WEAPON_START")
		elseif mode == "readiness_supplies" then
			removeAll(player, potionItemId)
			removeAll(player, meatItemId)
			addEvent(verifyReadiness, 500, player:getId(), "supplies", 240)
		elseif mode == "readiness_retention" then
			removeAll(player, meatItemId)
			local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
			assert(backpack and backpack:addItem(2050, 1), "retention fixture could not add unknown item")
			assert(backpack:addItem(saleItemId, 1), "retention fixture could not add sale item")
			addEvent(verifyReadiness, 500, player:getId(), "retention", 240)
		else
			addEvent(verifyReadiness, 500, player:getId(), "ready", 120)
		end
		print("PLAYERBOT_GAMEPLAY_TEST READINESS_" .. string.upper(mode:gsub("readiness_", "")) .. "_START")
		return true
	end
	if mode == "hunt_planning" then
		print("PLAYERBOT_GAMEPLAY_TEST HUNT_PLANNING_START")
		return true
	end
	if mode == "depot" then
		local fixtureState = player:getStorageValue(depotFixtureStorage)
		if fixtureState == -1 then
			local thais = Town(thaisTownId)
			assert(thais and player:setTown(thais), "depot fixture could not select Thais")
			assert(player:teleportTo(thais:getTemplePosition()), "depot fixture could not reach the Thais temple")
			local chest = player:getDepotChest(thaisTownId, true)
			assert(chest and chest:addItem(ITEM_GOLD_COIN, 1), "depot fixture could not seed prior depot contents")
			local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
			local outer = backpack and backpack:addItem(ITEM_BAG, 1)
			local nested = outer and outer:addItem(ITEM_BAG, 1)
			assert(nested and nested:addItem(depotLootItemId, 1) and nested:addItem(depotLootItemId, 1),
				"depot fixture could not seed nested loot")
			assert(backpack:addItem(2050, 1), "depot fixture could not seed an unknown retained item")
			local restartPhase = os.getenv("PLAYERBOT_DEPOT_RESTART_PHASE") or ""
			assert(player:setStorageValue(depotFixtureStorage, restartPhase == "" and 0 or 2),
				"depot fixture state could not persist")
		elseif fixtureState == 0 then
			local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
			assert(backpack and backpack:addItem(depotLootItemId, 1), "depot fixture could not seed second-cycle loot")
			assert(player:setStorageValue(depotFixtureStorage, 2), "depot second-cycle state could not persist")
		end
		addEvent(verifyDepot, 500, player:getId(), 360)
		print("PLAYERBOT_GAMEPLAY_TEST DEPOT_START")
		return true
	end
	if mode == "stamina_bonus" or mode == "stamina_boundary" or mode == "stamina_normal" then
		local staminaMinutes = mode == "stamina_bonus" and 2520 or mode == "stamina_boundary" and 2401 or 2400
		assert(player:setStamina(staminaMinutes), "stamina projection fixture could not set stamina")
		suppressNearbyMonsters(player:getId())
		print("PLAYERBOT_GAMEPLAY_TEST STAMINA_PROJECTION_START " .. staminaMinutes)
		return true
	end
    if mode == "death" then
        deathLoginCount = deathLoginCount + 1
        removeNearbyMonsters(player)
        local position = player:getPosition()
        assert(position.x == 32097 and position.y == 32219 and position.z == 7,
            "Bot One did not log in at the Rookgaard temple")
        if deathLoginCount <= 2 then
            addEvent(prepareDeath, 100, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_START " .. deathLoginCount)
        else
            addEvent(prepareDeath, 1500, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_RECOVERY_STATE_PASS")
        end
        return true
    end
    if mode == "departure" then
        if player:getVocation():getId() ~= 0 then
            local town = player:getTown()
            assert(player:getVocation():getId() == 4 and town and town:getId() == 2,
                "Oracle departure restart restored the wrong vocation or town")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_RESTART_PASS")
            return true
        end
        local requiredExperience = Game.getExperienceForLevel(8) - player:getExperience()
        if requiredExperience > 0 then
            player:addExperience(requiredExperience)
        end
        assert(player:getLevel() == 8, "Oracle departure fixture could not advance Bot One to level 8")
        suppressNearbyMonsters(player:getId())
        addEvent(verifyOracleDeparture, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_START")
        return true
    end
    if mode == "departure_interrupt" then
        prepareLevelEightCombat(player)
        return true
    end
    if mode == "departure_recovery" then
        if player:getStorageValue(departureRecoveryStorage) ~= 1 then
            local targetExperience = Game.getExperienceForLevel(9) - 1
            assert(player:getExperience() < targetExperience,
                "Oracle recovery fixture expected a low-level player")
            player:addExperience(targetExperience - player:getExperience())
            assert(player:getLevel() == 8, "Oracle recovery fixture could not prepare level 8")
            assert(player:setStorageValue(departureRecoveryStorage, 1),
                "Oracle recovery fixture could not persist its restart marker")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_PREPARED")
            return true
        end
        assert(player:getVocation():getId() == 0 and player:getLevel() == 8,
            "Oracle recovery fixture did not restore a vocationless level-8 player")
        assert(player:getExperience() == Game.getExperienceForLevel(9) - 1,
            "Oracle recovery fixture did not restore the level-9 experience boundary")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)),
            "Oracle recovery fixture could not lower Bot One's health")
        suppressNearbyMonsters(player:getId())
        addEvent(verifyOracleDeparture, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_RECOVERY_START")
        return true
    end
    if mode == "arbitration" or mode == "arbitration_interrupt" then
        local position = player:getPosition()
        assert(position.x == 32097 and position.y == 32219 and position.z == 7,
            "goal arbitration fixture did not start at the Rookgaard temple")
        assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == starterArmorId,
            "goal arbitration fixture expected starter armor")
        assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == starterWeaponId,
            "goal arbitration fixture expected starter weapon")
        suppressNearbyMonsters(player:getId())
        if mode == "arbitration_interrupt" then
            addEvent(triggerArbitrationInterrupt, 2000, player:getId())
            print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_INTERRUPT_START")
        else
            print("PLAYERBOT_GAMEPLAY_TEST GOAL_ARBITRATION_START")
        end
        return true
    end

	if mode == "progression_bundle" then
		assert(player:addItem(2152, 5), "economic progression fixture could not seed an existing platinum stack")
		assert(player:teleportTo(Position(32034, 32277, 8)), "economic progression fixture could not approach the reward")
		suppressNearbyMonsters(player:getId())
		addEvent(verifyEconomicPickupProgression, 500, player:getId(), 360)
		print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_BUNDLE_START")
		return true
	end
    if mode == "spell_training" then
        if player:getStorageValue(spellTrainingStorage) == 1 then
            assert(player:hasLearnedSpell("Find Person"), "spell training did not persist across restart")
            print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_RESTART_PASS")
            return true
        end
        local thais = Town(thaisTownId)
        assert(thais and player:setTown(thais), "spell training fixture could not select Thais")
        assert(player:setVocation(4), "spell training fixture could not select Knight")
        local requiredExperience = Game.getExperienceForLevel(8) - player:getExperience()
        if requiredExperience > 0 then player:addExperience(requiredExperience) end
        assert(player:getLevel() == 8, "spell training fixture could not select level 8")
        assert(player:teleportTo(thais:getTemplePosition()), "spell training fixture could not reach the Thais temple")
        for _, spellName in ipairs({"Find Person", "Light", "Light Healing", "Cure Poison", "Great Light"}) do
            player:forgetSpell(spellName)
        end
        removeAll(player, potionItemId)
        removeAll(player, meatItemId)
        assert(player:addItem(potionItemId, 5), "spell training fixture could not supply potions")
        assert(player:addItem(meatItemId, 1), "spell training fixture could not supply food")
        local totalMoney = player:getMoney() + player:getBankBalance()
        if totalMoney < 300 then assert(player:addMoney(300 - totalMoney), "spell training fixture could not fund Bot One") end
        if totalMoney > 300 then assert(player:removeTotalMoney(totalMoney - 300), "spell training fixture could not normalize funding") end
        assert(player:getMoney() + player:getBankBalance() == 300, "spell training fixture has the wrong starting money")
        suppressNearbyMonsters(player:getId())
        addEvent(verifySpellTraining, 500, player:getId(), 360)
        print("PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_START")
        return true
    end

	if mode == "progression_nested" then
		assert(player:setStorageValue(50082, 1), "nested progression fixture could not suppress the nearby currency reward")
		local rewardChest = Container(nestedRewardStorage)
		local rewardBag = nil
		for _, item in ipairs(rewardChest and rewardChest:getItems() or {}) do
			if item:getId() == nestedRewardRootId then
				rewardBag = item
				break
			end
		end
		assert(rewardBag and rewardBag:addItem(meatItemId, 2),
			"nested progression fixture could not add mutable food siblings")
		assert(player:teleportTo(Position(32034, 32275, 7)), "nested progression fixture could not approach the reward")
		suppressNearbyMonsters(player:getId())
		addEvent(verifyNestedPickupProgression, 500, player:getId(), 360)
		print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_START")
		return true
	end

    if mode == "progression" or mode == "progression_resume" or mode == "progression_nested_resume" or
        mode == "progression_space" then
        local position = player:getPosition()
        if mode == "progression_resume" then
            assert(player:getStorageValue(pickupRewardStorage) == -1,
                "resume fixture expected an unclaimed pickup reward")
            assert(player:setStorageValue(pickupRewardStorage, 1), "resume fixture could not set claimed storage")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack and backpack:addItem(pickupRewardId, 1), "resume fixture could not add claimed reward")
        elseif mode == "progression_nested_resume" then
            assert(player:getStorageValue(nestedRewardStorage) == -1,
                "nested resume fixture expected an unclaimed reward")
            assert(player:setStorageValue(nestedRewardStorage, 1), "nested resume fixture could not set claimed storage")
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            local rewardBag = backpack and backpack:addItem(nestedRewardRootId, 1)
            assert(rewardBag, "nested resume fixture could not add the reward bag")
            assert(rewardBag:addItem(2175, 1), "nested resume fixture could not add the spellbook")
            assert(rewardBag:addItem(2380, 1), "nested resume fixture could not add the hand axe")
            assert(rewardBag:addItem(nestedRewardShieldId, 1), "nested resume fixture could not add the wooden shield")
        elseif mode == "progression_space" then
            local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
            assert(backpack, "inventory-space fixture expected a backpack")
            while backpack:getSize() < backpack:getCapacity() do
                assert(backpack:addItem(ITEM_BAG, 1), "inventory-space fixture could not fill the backpack")
            end
            print("PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_SPACE_START")
            return true
        elseif player:getStorageValue(pickupRewardStorage) == -1 then
            assert(position.x == 32097 and position.y == 32219 and position.z == 7,
                "pickup progression fixture did not start at the Rookgaard temple")
            assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == starterArmorId,
                "pickup progression fixture expected starter armor")
            assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == starterWeaponId,
                "pickup progression fixture expected starter weapon")
            assert(not player:getSlotItem(CONST_SLOT_RIGHT), "pickup progression fixture expected an empty shield slot")
            assert(not player:getSlotItem(CONST_SLOT_FEET), "pickup progression fixture expected an empty feet slot")
        else
            assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == pickupRewardId,
                "pickup progression fixture did not restore persisted equipment")
        end
        suppressNearbyMonsters(player:getId())
		local progressionRestart = mode == "progression" and player:getStorageValue(pickupRewardStorage) == 1
		if not progressionRestart then
			addEvent(mode == "progression_nested_resume" and verifyNestedPickupProgression or
				verifyPickupProgression, 500, player:getId(), 360)
		end
        local startMarker = mode == "progression_resume" and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_RESUME_START" or
            mode == "progression_nested_resume" and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_NESTED_RESUME_START" or
			progressionRestart and "PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_RESTART_START" or
			"PLAYERBOT_GAMEPLAY_TEST PICKUP_PROGRESSION_START"
        print(startMarker)
        return true
    end

    assert(player:teleportTo(depotPosition), "fake depot position could not be restored")
    if mode == "target_pursuit" or mode == "target_pursuit_abandon" then
        suppressNearbyMonsters(player:getId())
        local position = player:getPosition()
        local monster = Game.createMonster(emptyMonsterName, Position(position.x + 4, position.y, position.z), true, true)
        assert(monster, "target pursuit fixture could not create a visible monster")
        addEvent(hidePursuitMonster, 1100, player:getId(), monster:getId(), mode == "target_pursuit" and 12 or 30)
        print("PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_START " .. monster:getId())
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
		local potions = player:getItemCount(potionItemId)
		if potions > 0 then assert(player:removeItem(potionItemId, potions), "healing fixture could not remove starter potions") end
        assert(player:getItemCount(potionItemId) == 0, "healing fixture expected no seeded potions")
        assert(player:addItem(potionItemId, healingPotionCount), "healing fixture could not add small health potions")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)), "healing fixture could not lower Bot One's health")
        addEvent(verifyHealing, 250, player:getId(), 40)
        print("PLAYERBOT_GAMEPLAY_TEST HEALING_START")
        return true
    end
    if mode == "healing_resupply" then
        suppressNearbyMonsters(player:getId())
		local potions = player:getItemCount(potionItemId)
		if potions > 0 then assert(player:removeItem(potionItemId, potions), "healing resupply fixture could not remove starter potions") end
		removeAll(player, meatItemId)
        assert(player:getItemCount(potionItemId) == 0, "healing resupply fixture expected no seeded potions")
		local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
		assert(backpack and backpack:addItem(7636, 1), "healing resupply fixture could not add an empty potion flask")
        assert(player:setHealth(math.floor(player:getMaxHealth() * 0.5)), "healing resupply fixture could not lower Bot One's health")
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
    assert(player:getBankBalance() == 100, "Bot One initial bank balance was not 100 gp")
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
	local potions = player:getItemCount(potionItemId)
	if potions > 0 then assert(player:removeItem(potionItemId, potions), "cycle fixture could not remove starter potions") end
	local meat = player:getItemCount(meatItemId)
	if meat > 0 then assert(player:removeItem(meatItemId, meat), "cycle fixture could not remove starter meat") end
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
