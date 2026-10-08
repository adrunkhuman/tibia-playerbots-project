local F = PlayerbotGameplayFixture

local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or ""
if mode ~= "ranged_position" and mode ~= "ranged_position_control" and mode ~= "ranged_position_corner" then
    return
end

function F.createRangedArena()
    -- A 30-second fight needs more than a viewport of retreat space. Keep this
    -- synthetic runway away from mapped terrain and static spawn danger; the
    -- blocked-retreat variant adds its wall to the identical arena.
    local origin = Position(60064, 60005, 7)
    for x = -64, 8 do
        for y = -5, 5 do
            local position = Position(origin.x + x, origin.y + y, origin.z)
            assert(not Tile(position), "ranged fixture arena overlaps an existing tile")
            local tile = Game.createTile(position)
            assert(tile and Game.createItem(4526, 1, position) and tile:isWalkable(),
                "ranged fixture could not create open ground")
        end
    end
    return origin
end

-- Isolate positioning from hit rolls, damage rolls, armor and spear breakage.
-- This override exists only in the disposable ranged fixtures. Attacks still use
-- the normal weapon range, line of sight, action delay and combat/death path.
local combat = Combat()
combat:setParameter(COMBAT_PARAM_TYPE, COMBAT_PHYSICALDAMAGE)
combat:setOrigin(ORIGIN_RANGED)
combat:setFormula(COMBAT_FORMULA_DAMAGE, -10, 0, -10, 0)
local spear = Weapon(WEAPON_DISTANCE)
spear:id(F.spearItemId)
spear:hitChance(100)
spear:breakChance(0)
spear.onUseWeapon = function(player, variant)
    return combat:execute(player, variant)
end
assert(spear:register(), "ranged position fixture could not normalize spear combat")

F.rangedHealthChange = CreatureEvent("PlayerbotRangedHealthChange")
function F.rangedHealthChange.onHealthChange(creature, attacker, primaryDamage, primaryType, secondaryDamage, secondaryType)
    local observation = F.rangedPositionObservation
    if observation and not observation.done and attacker and primaryType ~= COMBAT_HEALING then
        local damage = (primaryType ~= COMBAT_NONE and math.abs(primaryDamage) or 0) +
            (secondaryType ~= COMBAT_NONE and secondaryType ~= COMBAT_HEALING and math.abs(secondaryDamage) or 0)
        -- The event precedes the final health cap in combatChangeHealth. There is
        -- no mana shield or damage-veto event in this fixture; clamp overkill.
        damage = math.min(creature:getHealth(), damage)
        if creature:getId() == observation.playerId and attacker:getId() == observation.monsterId then
            observation.damage = observation.damage + damage
        elseif creature:getId() == observation.monsterId and attacker:getId() == observation.playerId then
            observation.dealt = observation.dealt + damage
            if damage > 0 then observation.hits = observation.hits + 1 end
        end
    end
    -- Observe, never replace healing or damage.
    return primaryDamage, primaryType, secondaryDamage, secondaryType
end
F.rangedHealthChange:register()

F.rangedDeath = CreatureEvent("PlayerbotRangedDeath")
function F.rangedDeath.onDeath(creature, corpse)
    local observation = F.rangedPositionObservation
    if observation and creature:getId() == observation.monsterId then
        observation.defeated = corpse ~= nil
        observation.killedAt = os.mtime()
        local player = Player(observation.playerId)
        if player then observation.deathPosition = player:getPosition() end
    end
    return true
end
F.rangedDeath:register()

local function distance(a, b)
    if a.z ~= b.z then return math.huge end
    return math.max(math.abs(a.x - b.x), math.abs(a.y - b.y))
end

function F.startRangedPositionObservation(player, monster, fixtureMode)
    local now = os.mtime()
    local position = player:getPosition()
    F.rangedPositionObservation = {
        playerId = player:getId(), monsterId = monster:getId(), mode = fixtureMode,
        origin = position, previousPosition = position, startedAt = now, sampledAt = now,
        previousDistance = distance(position, monster:getPosition()),
        range = ItemType(F.spearItemId):getShootRange(), lootBefore = player:getItemCount(ITEM_GOLD_COIN),
        damage = 0, dealt = 0, hits = 0, fightMs = 0, rangedMs = 0, steps = 0, maxDisplacement = 0,
    }
    assert(player:registerEvent("PlayerbotRangedHealthChange") and
        monster:registerEvent("PlayerbotRangedHealthChange") and monster:registerEvent("PlayerbotRangedDeath"),
        "ranged position fixture could not register combat observations")
    addEvent(F.measureRangedPosition, 100)
end

function F.measureRangedPosition()
    local observation = F.rangedPositionObservation
    if not observation or observation.done then return end
    local player = Player(observation.playerId)
    if not player or player:isRemoved() then return end
    local now = os.mtime()
    local sampleEnd = observation.killedAt or now
    local elapsed = sampleEnd - observation.sampledAt
    observation.fightMs = observation.fightMs + elapsed
    if observation.previousDistance >= 2 and observation.previousDistance <= observation.range then
        observation.rangedMs = observation.rangedMs + elapsed
    end
    -- Do not count movement toward the corpse after the fight as combat displacement.
    local position = observation.deathPosition or player:getPosition()
    if distance(position, observation.previousPosition) > 0 then observation.steps = observation.steps + 1 end
    observation.maxDisplacement = math.max(observation.maxDisplacement, distance(position, observation.origin))
    local monster = Monster(observation.monsterId)
    if observation.killedAt or not monster or now - observation.startedAt >= 90000 then
        observation.done = true
        player:unregisterEvent("PlayerbotRangedHealthChange")
        if monster then
            monster:unregisterEvent("PlayerbotRangedHealthChange")
            monster:unregisterEvent("PlayerbotRangedDeath")
        end
        print(string.format("PLAYERBOT_GAMEPLAY_TEST RANGED_POSITION_RESULT %s damage=%d defeated=%s seconds=%.1f target=%d dealt=%d hits=%d fight_ms=%d ranged_ms=%d steps=%d displacement=%d loot_before=%d",
            observation.mode, observation.damage, tostring(observation.defeated == true), observation.fightMs / 1000,
            observation.monsterId, observation.dealt, observation.hits, observation.fightMs, observation.rangedMs,
            observation.steps, observation.maxDisplacement, observation.lootBefore))
        return
    end
    observation.sampledAt = now
    observation.previousPosition = position
    observation.previousDistance = distance(position, monster:getPosition())
    addEvent(F.measureRangedPosition, 100)
end
