local F = PlayerbotGameplayFixture

F.inaccessibleCorpseDeath = CreatureEvent("PlayerbotInaccessibleCorpseDeath")

function F.inaccessibleCorpseDeath.onDeath(creature, corpse)
    if creature:getName() ~= F.lootMonsterName or (os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "") ~= "corpse_inaccessible" then
        return true
    end
    local player = Player(F.botName)
    assert(player and corpse, "inaccessible corpse fixture lost Bot One or the corpse")
    local origin = corpse:getPosition()
    for _, position in ipairs({
        Position(origin.x - 3, origin.y, origin.z),
        Position(origin.x + 3, origin.y, origin.z),
        Position(origin.x, origin.y - 3, origin.z),
        Position(origin.x, origin.y + 3, origin.z),
    }) do
        local tile = Tile(position)
        if tile and tile:isWalkable() and player:teleportTo(position) then
            local stepX = origin.x == position.x and 0 or origin.x > position.x and 1 or -1
            local stepY = origin.y == position.y and 0 or origin.y > position.y and 1 or -1
            local blockerPosition = Position(position.x + stepX, position.y + stepY, position.z)
            local blocker = Game.createMonster(F.corpseBlockerMonsterName, blockerPosition, true, true)
            assert(blocker and blocker:selectTarget(player),
                "inaccessible corpse fixture could not create its attacking blocker")
            print("PLAYERBOT_GAMEPLAY_TEST CORPSE_INACCESSIBLE_DISPLACED")
            return true
        end
    end
    error("inaccessible corpse fixture could not displace Bot One")
end

F.inaccessibleCorpseDeath:register()
