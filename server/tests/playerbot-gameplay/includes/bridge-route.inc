local F = PlayerbotGameplayFixture
local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE")
if mode ~= "bridge_route" then return end

-- Real-map regression for the Dwarven Bridge deaths: Bot Four died twice on
-- the z6 plateau while walking north across it toward a hunt region. The
-- shovel detour digs the loose stone pile at (32680,31944,7) and climbs back
-- from z8; the bridge path crosses every Dwarf/Dwarf Soldier spawn on z6.
-- Monsters, combat, travel safety and routing all stay native.
local start = Position(32639, 31984, 7)
local goal = Position(32631, 31956, 7) -- Must match travelSafetyGoal().
do
    local gx, gy, gz = (os.getenv("PLAYERBOT_BRIDGE_GOAL") or ""):match("^(%d+),(%d+),(%d+)$")
    if gx then goal = Position(tonumber(gx), tonumber(gy), tonumber(gz)) end
end
local deadline = 300000
local observation

local function onPlateau(p)
    return p.z == 6 and p.x >= 32600 and p.x <= 32640 and p.y >= 31950 and p.y <= 31980
end

local function evidence(player, phase, extra)
    local p = player:getPosition()
    print(string.format('{"component":"playerbot","event":"bridge_fixture","bot":"Bot One",' ..
        '"source":"lua_position_verifier","scenario":"%s","phase":"%s",' ..
        '"x":%d,"y":%d,"z":%d,"health":%d,"health_max":%d,"elapsed_ms":%d%s}',
        mode, phase, p.x, p.y, p.z, player:getHealth(), player:getMaxHealth(),
        os.mtime() - observation.startedAt, extra or ""))
end

local function finish(player, result)
    local o = observation
    if o.done then return end
    o.done = true
    evidence(player, "result", string.format(',"result":"%s","plateau_samples":%d,"z8_samples":%d,' ..
        '"floor_changes":%d,"minimum_health":%d', result, o.plateauSamples, o.z8Samples, o.floorChanges, o.minimumHealth))
    print(string.format("PLAYERBOT_GAMEPLAY_TEST BRIDGE_ROUTE_RESULT result=%s plateau_samples=%d z8_samples=%d",
        result, o.plateauSamples, o.z8Samples))
    -- An arrived bot stands idle, so no later telemetry flushes this pipe-backed stream.
    io.flush()
end

F.bridgeDeath = CreatureEvent("PlayerbotBridgeFixtureDeath")
function F.bridgeDeath.onDeath(player)
    if observation and player:getName() == F.botName then finish(player, "died") end
    return true
end
F.bridgeDeath:register()

function F.setupBridgeRoute(player)
    assert(player:getName() == F.botName, "bridge fixture touched another player")
    if observation then
        -- Normal death recovery relogs after the verdict; it is not a new leg.
        assert(observation.done, "bridge fixture relogged before its verdict")
        assert(player:setStorageValue(F.fixtureReadyStorage, 1), "bridge fixture relog readiness failed")
        return
    end
    local thais = Town(F.thaisTownId)
    assert(thais and player:setTown(thais) and player:setVocation(3), "bridge fixture could not select a Thais Paladin")
    local experience = Game.getExperienceForLevel(9) - player:getExperience()
    if experience > 0 then player:addExperience(experience) end
    assert(player:getLevel() == 9, "bridge fixture seeded the wrong level")
    for _, slot in ipairs({CONST_SLOT_LEFT, CONST_SLOT_RIGHT}) do
        local item = player:getSlotItem(slot)
        if item then assert(item:remove(), "bridge fixture could not clear a hand") end
    end
    F.removeAll(player, F.spearItemId)
    assert(player:addItem(F.spearItemId, 4, false, 1, CONST_SLOT_LEFT) and
        player:addItem(F.seedShieldId, 1, false, 1, CONST_SLOT_RIGHT), "bridge fixture could not equip spear/shield")
    F.removeAllHealthPotions(player)
    assert(player:addItem(F.healthPotionItemId, 20), "bridge fixture could not seed health potions")
    for _, tool in ipairs({F.shovelItemId, F.ropeItemId}) do
        if player:getItemCount(tool) == 0 then assert(player:addItem(tool, 1), "bridge fixture could not seed tool " .. tool) end
    end
    if not player:hasLearnedSpell("Light Healing") then assert(player:learnSpell("Light Healing"), "bridge fixture could not learn Light Healing") end
    assert(player:setHealth(player:getMaxHealth()) and player:addMana(player:getMaxMana() - player:getMana()),
        "bridge fixture could not fill health/mana")
    assert(player:teleportTo(start), "bridge fixture could not reach the leg start")
    assert(player:registerEvent("PlayerbotBridgeFixtureDeath"), "bridge death observer registration failed")
    observation = {playerId = player:getId(), startedAt = os.mtime(), plateauSamples = 0, z8Samples = 0,
        floorChanges = 0, minimumHealth = player:getHealth(), lastZ = start.z}
    evidence(player, "start", string.format(',"goal_x":%d,"goal_y":%d,"goal_z":%d,"shovel":%d,"rope":%d',
        goal.x, goal.y, goal.z, player:getItemCount(F.shovelItemId), player:getItemCount(F.ropeItemId)))
    assert(player:setStorageValue(F.fixtureReadyStorage, 1), "bridge fixture readiness failed")
    addEvent(F.measureBridgeRoute, 250)
end

function F.measureBridgeRoute()
    local o = observation
    if not o or o.done then return end
    local player = Player(o.playerId)
    if not player or player:isRemoved() or player:getHealth() <= 0 then return end -- Death observer owns the result.
    local p = player:getPosition()
    o.minimumHealth = math.min(o.minimumHealth, player:getHealth())
    if onPlateau(p) then o.plateauSamples = o.plateauSamples + 1 end
    if p.z == 8 then o.z8Samples = o.z8Samples + 1 end
    if p.z ~= o.lastZ then
        o.floorChanges = o.floorChanges + 1
        o.lastZ = p.z
        evidence(player, "floor_change")
    end
    if p.z == goal.z and math.max(math.abs(p.x - goal.x), math.abs(p.y - goal.y)) <= 1 then
        finish(player, "arrived")
        return
    end
    if os.mtime() - o.startedAt >= deadline then
        finish(player, "timeout")
        return
    end
    addEvent(F.measureBridgeRoute, 250)
end
