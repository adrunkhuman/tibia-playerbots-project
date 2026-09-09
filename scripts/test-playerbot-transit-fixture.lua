-- Run from the repository root: lua scripts/test-playerbot-transit-fixture.lua
Position = function(x, y, z) return {x = x, y = y, z = z} end
PlayerbotGameplayFixture = {
    depotPosition = Position(32105, 32195, 8), defensiveMonsterName = "threat",
    suppressNearbyMonsters = function() end,
}
dofile("server/tests/playerbot-gameplay/includes/combat.inc")
local F = PlayerbotGameplayFixture
local position, queued, monsters, sealed, missingTile
-- Ground-only tiles extracted from World.otbm and checked against items.otb.
-- Unlike the old all-walkable mock, unlisted tiles (including the east stalls)
-- cannot satisfy fixture geometry.
local ground = {
    ["32105,32191,8"] = 8423, ["32105,32192,8"] = 8423,
    ["32105,32193,8"] = 8423, ["32105,32194,8"] = 8423,
    ["32105,32195,8"] = 8423,
    ["32104,32190,8"] = 8423, ["32105,32190,8"] = 8423,
    ["32106,32190,8"] = 8426, ["32104,32191,8"] = 8428,
}
local player = {
    getId = function() return 7 end,
    isRemoved = function() return false end,
    getPosition = function() return position end,
    teleportTo = function(_, value) position = value; return true end,
}
Player = function(id) assert(id == 7); return player end
Tile = function(at)
    local key = at.x .. "," .. at.y .. "," .. at.z
    if not ground[key] or key == missingTile then return nil end
    return {isWalkable = function() return not sealed end}
end
Monster = function(id) return monsters[id] end
Game = {createMonster = function(name, at)
    assert(name == F.defensiveMonsterName)
    assert(math.abs(at.x - position.x) <= 1 and math.abs(at.y - position.y) <= 1)
    assert(at.z == position.z and (at.y < position.y or at.x < position.x),
        "attacker must leave the southward route empty")
    assert(Tile(at) and Tile(at):isWalkable())
    local id = #monsters + 1
    local monster = {
        getId = function() return id end,
        selectTarget = function(_, target) assert(target == player); return true end,
        getHealth = function() return 1 end,
        getMaxHealth = function() return 1 end,
    }
    monsters[id] = monster
    return monster
end}
addEvent = function(fn, delay, id, ids, attempts)
    assert(fn == F.verifyTransitReturn and delay == 100 and id == 7 and #ids == 4)
    queued = function() queued = nil; fn(id, ids, attempts) end
end
local function prepare()
    monsters, queued, sealed, missingTile = {}, nil, false, nil
    F.prepareTransitReturn(player)
    assert(#monsters == 4 and position.x == 32105 and position.y == 32191 and position.z == 8 and queued)
end
prepare()
queued()
assert(queued, "stationary bot must not pass arrival")
position = F.depotPosition
queued()
assert(not queued, "arrival must end polling")
prepare()
monsters[1] = nil
local ok, err = pcall(queued)
assert(not ok and err:find("optional attacker was attacked", 1, true))
prepare()
ok, err = pcall(function() while queued do queued() end end)
assert(not ok and err:find("within 30 seconds", 1, true) and not queued)
sealed = true
ok, err = pcall(F.prepareTransitReturn, player)
assert(not ok and err:find("open southward depot route", 1, true))
sealed = false
for _, case in ipairs({
    {"32105,32193,8", "open southward depot route"},
    {"32106,32190,8", "four adjacent attacker tiles"},
}) do
    missingTile = case[1]
    monsters, queued = {}, nil
    ok, err = pcall(F.prepareTransitReturn, player)
    assert(not ok and err:find(case[2], 1, true) and #monsters == 0 and not queued)
end
print("PASS transit fixture: map-backed hall/stall geometry, four attackers, untouched arrival, lost attacker, polling bound, blocked/missing route and attacker tiles")
