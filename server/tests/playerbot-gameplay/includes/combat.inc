local F = PlayerbotGameplayFixture

function F.spawnDefensiveMonsters(playerId)
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
            local monster = Game.createMonster(F.defensiveMonsterName, position, true, true)
            assert(monster, "defensive blocker could not be created")
            addEvent(F.activateDefensiveMonster, 2000, playerId, monster:getId())
            spawned = spawned + 1
        end
    end
    assert(spawned >= 1, "no adjacent defensive blocker could be created")
    print("PLAYERBOT_GAMEPLAY_TEST DEFENSIVE_BLOCKERS_SPAWNED " .. spawned)
end

function F.spawnDeathMonster(playerId)
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
            local monster = Game.createMonster(F.deathMonsterName, position, true, true)
            assert(monster and monster:selectTarget(player), "death test monster could not target Bot One")
            print("PLAYERBOT_GAMEPLAY_TEST DEATH_THREAT_SPAWNED")
            return
        end
    end
    error("no adjacent tile was available for death telemetry test monster")
end

function F.spawnSpellTarget(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before spell target spawn")
    local origin = player:getPosition()
    for _, position in ipairs({
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(F.spellTargetMonsterName, position, true, true)
            assert(monster, "spell-use fixture could not create a target")
            F.spellTargetId = monster:getId()
            print("PLAYERBOT_GAMEPLAY_TEST SPELL_TARGET_SPAWNED " .. F.spellTargetId)
            return
        end
    end
    error("no adjacent tile was available for the spell-use target")
end

function F.triggerSpellRecoveryPreemption(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before spell recovery preemption")
    assert(player:addMana(200 - player:getMana()), "spell-use fixture could not restore recovery mana")
    assert(player:setHealth(110), "spell-use fixture could not trigger emergency recovery")
    if F.spellCalibrationFixture then
        addEvent(F.verifyBerserkSingleTarget, 500, playerId, 80)
    end
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_RECOVERY_PREEMPTION_TRIGGERED")
end

function F.placeSpellTarget(player, target, positions)
    if not target or target:isRemoved() then
        return false
    end
    local origin = player:getPosition()
    for _, offset in ipairs(positions) do
        local position = Position(origin.x + offset[1], origin.y + offset[2], origin.z)
        local tile = Tile(position)
        if tile and tile:isWalkable() and target:teleportTo(position, true) then
            return true
        end
    end
    return false
end

function F.keepSpellTargetAdjacent(player, target)
    return F.placeSpellTarget(player, target, {{1, 0}, {0, 1}, {-1, 0}, {0, -1}})
end

function F.waitForSpellAttackTarget(playerId, attempts)
    local player = Player(playerId)
    local target = F.spellTargetId ~= 0 and Monster(F.spellTargetId) or nil
    local attacked = player and player:getTarget() or nil
    assert(player and not player:isRemoved(), "Bot One disappeared before spell target acquisition")
    if target and attacked and attacked:getId() == target:getId() then
        addEvent(F.triggerSpellRecoveryPreemption, 1800, playerId)
        return
    end
    assert(target and not target:isRemoved(), "spell-use fixture target was removed before acquisition")
    assert(F.keepSpellTargetAdjacent(player, target), "spell-use fixture could not keep its target adjacent")
    if attempts > 0 then
        addEvent(F.waitForSpellAttackTarget, 250, playerId, attempts - 1)
        return
    end
    error("spell-use fixture did not establish an attacked target")
end

function F.prepareSpellRecoveryPreemption(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before spell target preparation")
    assert(player:addMana(50 - player:getMana()), "spell-use fixture could not reserve too little mana for offense")
    F.spawnSpellTarget(playerId)
    addEvent(F.waitForSpellAttackTarget, 250, playerId, 120)
end

function F.waitForSpellSupport(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before spell support")
    if not player:hasCondition(CONDITION_HASTE) and attempts > 0 then
        addEvent(F.waitForSpellSupport, 250, playerId, attempts - 1)
        return
    end
    assert(player:hasCondition(CONDITION_HASTE), "spell-use fixture did not apply Haste before low-mana combat")
    F.spellSupportObserved = true
    F.prepareSpellRecoveryPreemption(playerId)
end

function F.restoreLightHealing(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before Light Healing restoration")
    assert(player:learnSpell("Light Healing"), "spell-use fixture could not restore Light Healing")
end

function F.triggerSpellFallback(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before spell fallback")
    assert(player:forgetSpell("Light Healing"), "spell-use fixture could not forget Light Healing")
    assert(player:addItem(F.potionItemId, 1), "spell-use fixture could not preserve the potion reserve")
    assert(player:setHealth(110), "spell-use fixture could not trigger spell fallback")
    print("PLAYERBOT_GAMEPLAY_TEST SPELL_FALLBACK_TRIGGERED")
end

function F.cleanupSpellTargets()
    for _, targetId in ipairs({F.spellTargetId, F.spellSecondaryTargetId}) do
        local target = targetId ~= 0 and Monster(targetId) or nil
        if target and not target:isRemoved() then
            target:remove()
        end
    end
    F.spellTargetId = 0
    F.spellSecondaryTargetId = 0
end

function F.prepareDeath(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before death recovery setup")
    assert(player:teleportTo(F.depotPosition), "death recovery fixture could not leave the temple protection zone")
    F.removeNearbyMonsters(player)
    F.spawnDeathMonster(playerId)
end

function F.spawnCorpseMonster(playerId, monsterName)
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

function F.PlayerbotSpawnInaccessibleCorpseMonster(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One disappeared before inaccessible corpse spawn")
    local origin = player:getPosition()
    for _, position in ipairs({
        Position(origin.x + 1, origin.y, origin.z),
        Position(origin.x, origin.y + 1, origin.z),
        Position(origin.x - 1, origin.y, origin.z),
        Position(origin.x, origin.y - 1, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() then
            local monster = Game.createMonster(F.lootMonsterName, position, true, true)
            assert(monster and monster:registerEvent("PlayerbotInaccessibleCorpseDeath"),
                "inaccessible corpse monster could not register its death event")
            print("PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_START")
            return
        end
    end
    error("no adjacent tile was available for inaccessible corpse test monster")
end

function F.spawnDepartureDefensiveThreat(playerId)
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
            local monster = Game.createMonster(F.defensiveMonsterName, position, true, true)
            assert(monster and monster:selectTarget(player), "departure defensive threat could not target Bot One")
            print("PLAYERBOT_GAMEPLAY_TEST ORACLE_DEPARTURE_DEFENSIVE_THREAT_SPAWNED")
            return
        end
    end
    error("no adjacent tile was available for the departure defensive threat")
end

function F.prepareLevelEightCombat(player)
    local targetExperience = Game.getExperienceForLevel(8) - 1
    assert(player:getExperience() < targetExperience, "level-eight combat fixture expected a low-level player")
    player:addExperience(targetExperience - player:getExperience())
    assert(player:getLevel() == 7, "level-eight combat fixture could not prepare level 7")
    assert(player:teleportTo(F.depotPosition), "level-eight combat fixture could not leave the temple")
    F.removeNearbyMonsters(player)
    F.spawnCorpseMonster(player:getId(), F.levelEightMonsterName)
    addEvent(F.spawnDepartureDefensiveThreat, 2500, player:getId())
    addEvent(F.verifyOracleDeparture, 500, player:getId(), 360)
    print("PLAYERBOT_GAMEPLAY_TEST ORACLE_LEVEL_EIGHT_INTERRUPT_START")
end
