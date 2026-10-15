local F = PlayerbotGameplayFixture
local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE")
if mode ~= "raid_travel" and mode ~= "raid_recovery" then return end

-- Use a unique fixture species: Lua scripts load before XML monsters, so a
-- native name would let later XML loading overwrite this normalization. One
-- guaranteed 40-HP life-drain hit / 2 seconds = 0.108 maxHP/s at level 8
-- (185 HP), above the 0.08 route ceiling even with the seeded armor/shield.
-- Slow pursuit bounds exposure; combat, movement and raid spawning stay native.
Game.createMonsterType("Raid Safety Wolf"):register({
    description = "a raid safety wolf", experience = 0, outfit = {lookType = 3},
    health = 1000, maxHealth = 1000, speed = 160, race = "blood", corpse = 6009,
    flags = {attackable = true, hostile = true, pushable = false,
        canPushItems = false, canPushCreatures = false, targetDistance = 1, staticAttackChance = 100},
    attacks = {{name = "combat", type = COMBAT_LIFEDRAIN, interval = 2000, chance = 100,
        minDamage = -40, maxDamage = -40, target = true, range = 1}},
    defenses = {armor = 0, defense = 0}, loot = {},
})

local start = Position(60160, 60200, 7)
local spawn = Position(60169, 60200, 7) -- Must match the real raid XML.
local goal = Position(60208, 60200, 7) -- Must match travelSafetyGoal().
local observation
local guardDuration = 5000

local function distance(a, b)
    if a.z ~= b.z then return math.huge end
    return math.max(math.abs(a.x - b.x), math.abs(a.y - b.y))
end

local function evidence(player, phase, extra)
    local p = player:getPosition()
    print(string.format('{"component":"playerbot","event":"raid_fixture","bot":"Bot One",' ..
        '"source":"lua_position_verifier","scenario":"%s","phase":"%s",' ..
        '"x":%d,"y":%d,"z":%d,"health":%d,"health_max":%d,"elapsed_ms":%d%s}',
        mode, phase, p.x, p.y, p.z, player:getHealth(), player:getMaxHealth(),
        os.mtime() - observation.startedAt, extra or ""))
end

local function check(condition, reason)
    if condition then return end
    print('PLAYERBOT_GAMEPLAY_TEST RAID_FIXTURE_FAILED ' .. mode .. ' ' .. reason)
    error(reason)
end

F.raidDeath = CreatureEvent("PlayerbotRaidFixtureDeath")
function F.raidDeath.onDeath(player)
    if observation and player:getName() == F.botName then
        observation.deaths = observation.deaths + 1
        evidence(player, "death", ',"intentional":' .. tostring(observation.injectingDeath == true))
        check(observation.injectingDeath and observation.deaths == 1,
            "raid caused an unplanned death")
    end
    return true
end
F.raidDeath:register()

function F.setupRaidTravel(player)
    check(player:getName() == F.botName, "raid fixture touched another player")
    if observation then
        -- Re-enter the SAME runway from outside the threat's viewport. Do not
        -- reload raids, reset controller/manager memory, move the wolf, or grant
        -- levels/health/equipment on recovery. The normal recovery path owns that.
        check(mode == "raid_recovery" and observation.injectingDeath and observation.deaths == 1 and
            not observation.recovered, "unexpected raid fixture relog")
        check(Monster(observation.monsterId), "raid threat disappeared during recovery")
        check(player:teleportTo(start), "recovered bot could not enter its runway")
        check(player:registerEvent("PlayerbotRaidFixtureDeath"), "recovery death observer registration failed")
        observation.playerId = player:getId()
        observation.recovered = true
        observation.guardStartedAt = os.mtime()
        observation.guardPreviousPosition = player:getPosition()
        evidence(player, "recovered", ',"target_id":' .. observation.monsterId)
        check(player:setStorageValue(F.fixtureReadyStorage, 1), "recovery readiness failed")
        return
    end

    check(player:getLevel() == 8 and player:getVocation():getId() == 4 and player:getMaxHealth() == 185,
        "raid fixture requires the fresh level-eight Knight")
    -- A straight open-ground runway leaves only the ordinary backward escape.
    -- No alternate lane may bypass the threat before clearance is observed.
    for x = 60128, 60216 do
        local p = Position(x, start.y, start.z)
        check(not Tile(p), "raid runway overlaps mapped terrain")
        local tile = Game.createTile(p)
        check(tile and Game.createItem(4526, 1, p) and tile:isWalkable(), "raid runway ground failed")
    end
    check(player:teleportTo(start) and player:setHealth(player:getMaxHealth()), "raid runway entry failed")
    player:removeCondition(CONDITION_MANASHIELD)
    check(player:registerEvent("PlayerbotRaidFixtureDeath"), "raid death observer registration failed")
    observation = {playerId = player:getId(), startedAt = os.mtime(), deaths = 0}
    evidence(player, "start", ',"goal_x":' .. goal.x .. ',"goal_y":' .. goal.y .. ',"goal_z":' .. goal.z)
    check(player:setStorageValue(F.fixtureReadyStorage, 1), "raid fixture readiness failed")
    addEvent(F.measureRaidTravel, 100)
end

function F.measureRaidTravel()
    local o = observation
    if not o or o.done then return end
    local now = os.mtime()
    check(now - o.startedAt < 120000, "raid fixture deadline exceeded")
    local player = Player(o.playerId)
    if o.injectingDeath and not o.recovered then
        -- The dispatcher may still own the dead Player during recovery. Never
        -- treat it as an alive sample, even if Player(id) still resolves.
        addEvent(F.measureRaidTravel, 100)
        return
    end
    check(player and not player:isRemoved() and player:getHealth() > 0, "raid player disappeared")
    local p = player:getPosition()
    check(p.z == start.z and p.y == start.y and p.x >= 60128 and p.x <= 60216,
        "raid player left the isolated runway")
    if not o.activated and p.x >= start.x + 8 then
        -- Trigger only after observed ordinary travel, not at login. The engine
        -- loads this named raid from fixture XML; Lua never creates the wolf.
        o.activated = true
        o.activationX, o.activationAt = p.x, now
        check(Game.startRaid("Playerbot Travel Safety") == RETURNVALUE_NOERROR, "real raid activation failed")
        evidence(player, "activated", ',"spawn_x":' .. spawn.x .. ',"spawn_y":' .. spawn.y .. ',"spawn_z":' .. spawn.z)
    end
    if o.activated and not o.withdrew then o.peakX = math.max(o.peakX or p.x, p.x) end
    if o.activated and not o.monsterId then
        local wolves = {}
        for _, creature in ipairs(Game.getSpectators(spawn, false, false, 48, 48, 1, 1)) do
            if creature:isMonster() and creature:getName() == "Raid Safety Wolf" then wolves[#wolves + 1] = creature end
        end
        check(#wolves <= 1, "raid fixture spawned more than one wolf")
        if #wolves == 1 then
            o.monsterId = wolves[1]:getId()
            evidence(player, "spawned", ',"target_id":' .. o.monsterId .. ',"count":1')
        else
            check(now - o.activationAt < 5000, "real raid did not spawn its wolf")
        end
    end
    if o.monsterId and not o.removed then
        local wolf = Monster(o.monsterId)
        check(wolf and wolf:getHealth() > 0, "raid threat died/disappeared before controlled removal")
        if not o.withdrew and o.peakX - p.x >= 2 then
            o.withdrew = true
            evidence(player, "withdrew", ',"from_x":' .. o.peakX .. ',"target_id":' .. o.monsterId)
            if mode == "raid_recovery" then
                -- Inject exactly one combat death AFTER observed withdrawal.
                -- The wolf is left alive; this does not claim a naturally caused
                -- death is safe or acceptable.
                o.injectingDeath = true
                evidence(player, "kill_injected")
                check(doTargetCombatHealth(0, player:getId(), COMBAT_LIFEDRAIN,
                    -player:getHealth(), -player:getHealth(), CONST_ME_NONE), "intentional combat death failed")
                check(player:getHealth() == 0, "intentional combat damage did not kill the bot")
                addEvent(F.measureRaidTravel, 100)
                return
            end
            o.guardStartedAt = now
            o.guardPreviousPosition = p
        end
        if o.guardStartedAt then
            check(p.x < spawn.x - 1, "bot retried lethal contact while the raid remained alive")
            -- Compare both player samples against the SAME current wolf tile.
            -- Closing pursuit while a normal walk step is pending is not a
            -- forward retry. Safe approach after recovery remains eligible;
            -- moving toward the wolf within its unsafe two-tile band does not.
            local wolfPosition = wolf:getPosition()
            local separation = distance(p, wolfPosition)
            check(separation > 2 or separation >= distance(o.guardPreviousPosition, wolfPosition),
                "bot moved toward the live threat while unsafe")
            o.guardPreviousPosition = p
            if now - o.guardStartedAt >= guardDuration then
                evidence(player, "guarded", ',"guard_ms":' .. (now - o.guardStartedAt) ..
                    ',"target_id":' .. o.monsterId)
                check(wolf:remove() and not Monster(o.monsterId), "controlled wolf removal failed")
                o.removed = true
                evidence(player, "removed", ',"target_id":' .. o.monsterId)
            end
        end
    end
    if o.removed and distance(p, goal) == 0 then
        check(o.withdrew and o.deaths == (mode == "raid_recovery" and 1 or 0), "raid completion lost safety evidence")
        o.done = true
        evidence(player, "arrived", ',"deaths":' .. o.deaths)
        print("PLAYERBOT_GAMEPLAY_TEST " .. mode:upper() .. "_PASS")
        -- This fixture then idles without ordinary C++ telemetry. Lua's stdout
        -- is block-buffered in Docker; publish the final marker immediately.
        io.stdout:flush()
        return
    end
    addEvent(F.measureRaidTravel, 100)
end
