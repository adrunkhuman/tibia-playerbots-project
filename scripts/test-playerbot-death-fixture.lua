-- Run from the repository root: lua scripts/test-playerbot-death-fixture.lua
PlayerbotGameplayFixture = {fixtureReadyStorage = 50099}
dofile("server/tests/playerbot-gameplay/includes/combat.inc")
local F = PlayerbotGameplayFixture
local marker, queued, kills, freed, queries
local present = true
Player = function(id)
    assert(id == 7)
    if not present then return nil end
    return {isRemoved = function() return false end, getGuid = function() return 3 end}
end
db = {storeQuery = function(sql)
    assert(sql == "SELECT `value` FROM `player_storage` WHERE `player_id` = 3 AND `key` = 50099")
    queries = queries + 1
    return marker ~= nil and 42 or false
end}
result = {
    getNumber = function(id, key) assert(id == 42 and key == "value"); return marker end,
    free = function(id) assert(id == 42); freed = freed + 1 end,
}
addEvent = function(fn, delay, id, attempts)
    assert(fn == F.waitForDeathService and delay == 250 and id == 7)
    queued = function() queued = nil; fn(id, attempts) end
end
F.prepareDeath = function(id) assert(id == 7); kills = kills + 1 end
local function reset(value)
    marker, queued, kills, freed, queries = value, nil, 0, 0, 0
end
reset(nil)
F.waitForDeathService(7, 120)
assert(kills == 0 and queued)
marker = 0
queued()
assert(kills == 0 and freed == 1 and queued)
marker = 1
queued()
assert(kills == 1 and freed == 2 and not queued)
reset(nil)
F.waitForDeathService(7, 120)
local ok, err = pcall(function() while queued do queued() end end)
assert(not ok and err:find("timed out waiting for recovered service evidence", 1, true))
assert(queries == 120 and kills == 0 and not queued)
reset(1)
present = false
assert(not pcall(F.waitForDeathService, 7, 120))
assert(queries == 0 and kills == 0 and not queued)
print("PASS death fixture: milestone gate, marker cleanup, 30-second polling bound, missing player")

-- Exercise the real login handler: the killer is at the old depot, not within
-- the recovered player's temple cleanup. Only the recorded creature may go.
Position = function(x, y, z) return {x = x, y = y, z = z} end
dofile("server/tests/playerbot-gameplay/includes/constants.inc")
CreatureEvent = function() return {register = function() end} end
dofile("server/tests/playerbot-gameplay/includes/login.inc")
os.getenv = function(key) assert(key == "PLAYERBOT_GAMEPLAY_MODE"); return "death" end
F.selectHealthPotion = function() end
F.restoreRookgaardBaseline = function() end
local player = {
    getId = function() return 7 end,
    getName = function() return F.botName end,
    isRemoved = function() return false end,
    getPosition = function() return Position(32097, 32219, 7) end,
}
Player = function(id) assert(id == 7); return player end
Tile = function() return {isWalkable = function() return true end} end
local monsters, removed, nextId = {}, {}, 100
monsters[999] = {remove = function() error("removed unrelated world monster") end}
Monster = function(id) assert(id ~= 0); return monsters[id] end
Game = {createMonster = function(name)
    assert(name == F.deathMonsterName)
    nextId = nextId + 1
    local id = nextId
    local monster = {
        getId = function() return id end,
        selectTarget = function(_, target) assert(target == player); return true end,
        isRemoved = function() return false end,
        remove = function() removed[id] = true; monsters[id] = nil; return true end,
    }
    monsters[id] = monster
    return monster
end}
F.removeNearbyMonsters = function()
    assert(F.deathMonsterId == 0, "owned killer not cleared before temple cleanup")
end
local scheduled = 0
addEvent = function(fn, delay, id, attempts)
    assert(F.deathMonsterId == 0, "owned killer survived until next death/service timer")
    assert(id == 7)
    scheduled = scheduled + 1
    if scheduled <= 2 then
        assert(fn == F.prepareDeath and delay == 100)
    else
        assert(fn == F.waitForDeathService and delay == 250 and attempts == 120)
    end
end
assert(F.login.onLogin(player))
for recovery = 1, 2 do
    F.spawnDeathMonster(7)
    local id = F.deathMonsterId
    assert(id == 100 + recovery and monsters[id])
    assert(F.login.onLogin(player))
    assert(removed[id] and not monsters[id] and F.deathMonsterId == 0)
end
assert(scheduled == 3 and monsters[999])
F.cleanupDeathMonster() -- repeated cleanup is harmless
F.deathMonsterId = 12345 -- engine already removed the owned creature
F.cleanupDeathMonster()
assert(F.deathMonsterId == 0 and monsters[999])
F.deathMonsterId = 12346
monsters[12346] = {isRemoved = function() return true end,
    remove = function() error("removed creature was removed twice") end}
F.cleanupDeathMonster()
assert(F.deathMonsterId == 0)
print("PASS death killer lifecycle: spawn ID, both relog cleanups, cleared/missing/removed IDs, unrelated monster retained")
