-- Run from repository root: lua server/tests/playerbot_raid_fixture_contracts.lua
local path = "server/tests/playerbot-gameplay/includes/raid-travel.inc"
local nativeGetenv, nativeMtime, nativePrint = os.getenv, os.mtime, print
local mode, now, queued, output, player, wolf, definition, raidCalls, spawnAt, tiles
os.getenv = function(key) if key == "PLAYERBOT_GAMEPLAY_MODE" then return mode end return nativeGetenv(key) end
os.mtime = function() return now end
Position = function(x, y, z) return {x = x, y = y, z = z} end
COMBAT_LIFEDRAIN, CONDITION_MANASHIELD, CONST_ME_NONE, RETURNVALUE_NOERROR = 1, 2, 3, 0

-- Inert outside the exact modes, without any world/monster/event APIs present.
for _, inactive in ipairs({"cycle", "ranged_position", "raid_travel_typo", ""}) do
    mode, PlayerbotGameplayFixture = inactive, {}
    dofile(path)
    assert(next(PlayerbotGameplayFixture) == nil, "inactive raid fixture touched the world")
end

local events, players = {}, {}
CreatureEvent = function(name)
    local event = {}
    function event:register() events[name] = self end
    return event
end
local function key(p) return p.x .. "," .. p.y .. "," .. p.z end
Tile = function(p) return tiles[key(p)] end
Game = {
    createMonsterType = function(name)
        assert(name == "Raid Safety Wolf", "fixture must not override a native XML species")
        return {register = function(_, value) definition = value end}
    end,
    createTile = function(p)
        local tile = {isWalkable = function(self) return self.ground == 4526 end}
        tiles[key(p)] = tile
        return tile
    end,
    createItem = function(id, count, p)
        assert(id == 4526 and count == 1)
        Tile(p).ground = id
        return {}
    end,
    startRaid = function(name)
        assert(name == "Playerbot Travel Safety")
        raidCalls = raidCalls + 1
        spawnAt = now + 1000
        return RETURNVALUE_NOERROR
    end,
    getSpectators = function(p, multifloor, onlyPlayers, west, east, north, south)
        assert(p.x == 60169 and p.y == 60200 and p.z == 7 and not multifloor and not onlyPlayers)
        assert(west == 48 and east == 48 and north == 1 and south == 1)
        return wolf and {wolf} or {}
    end,
}
-- Deliberately no Game.createMonster/selectTarget/movement API: only the raid
-- engine boundary may introduce the wolf; only the test moves player samples.
Player = function(id) return players[id] end
Monster = function(id) if wolf and id == wolf.id then return wolf end end
addEvent = function(callback, delay)
    assert(delay == 100 and queued == nil, "unbounded/duplicate observation scheduling")
    queued = callback
end
print = function(line) output[#output + 1] = line end

local function makePlayer(id)
    local p = {id = id, health = 100, position = Position(10, 10, 7), writes = {}, registrations = {}}
    function p:getId() return self.id end
    function p:getName() return "Bot One" end
    function p:getLevel() return 8 end
    function p:getVocation() return {getId = function() return 4 end} end
    function p:getMaxHealth() return 185 end
    function p:getHealth() return self.health end
    function p:setHealth(value) self.health = value; return true end
    function p:getPosition() return Position(self.position.x, self.position.y, self.position.z) end
    function p:teleportTo(value) self.position = value; self.teleports = (self.teleports or 0) + 1; return true end
    function p:isRemoved() return false end
    function p:removeCondition(kind) assert(kind == CONDITION_MANASHIELD); return true end
    function p:registerEvent(name) assert(events[name]); self.registrations[name] = true; return true end
    function p:setStorageValue(storage, value) assert(storage == 50099 and value == 1); self.writes[storage] = value; return true end
    players[id] = p
    return p
end
local function makeWolf()
    wolf = {id = 99, position = Position(60169, 60200, 7)}
    function wolf:getId() return self.id end
    function wolf:getName() return "Raid Safety Wolf" end
    function wolf:isMonster() return true end
    function wolf:getPosition() return self.position end
    function wolf:getHealth() return 1000 end
    function wolf:remove() wolf = nil; return true end
end
local function start(fixtureMode)
    mode, now, queued, output, player, wolf, definition, raidCalls, spawnAt, tiles = fixtureMode, 0, nil, {}, nil, nil, nil, 0, nil, {}
    events, players = {}, {}
    PlayerbotGameplayFixture = {botName = "Bot One", fixtureReadyStorage = 50099}
    dofile(path)
    player = makePlayer(1)
    PlayerbotGameplayFixture.selectHealthPotion = function(p) assert(p:getName() == "Bot One") end
    dofile("server/tests/playerbot-gameplay/includes/login.inc")
    assert(PlayerbotGameplayFixture.login.onLogin({getName = function() return "Rook Tester" end}),
        "raid login touched a human player")
    assert(PlayerbotGameplayFixture.login.onLogin(player))
    assert(player.position.x == 60160 and player.position.y == 60200 and player.health == 185 and raidCalls == 0)
    assert(player.writes[50099] == 1 and player.teleports == 1)
    assert(Tile(Position(60128, 60200, 7)):isWalkable() and Tile(Position(60216, 60200, 7)):isWalkable())
    assert(not Tile(Position(60169, 60199, 7)), "runway permits bypassing the threat")
    assert(not Tile(Position(60127, 60200, 7)))
    local attack = definition.attacks[1]
    assert(definition.maxHealth == 1000 and definition.speed == 160 and definition.flags.hostile)
    assert(attack.type == COMBAT_LIFEDRAIN and attack.range == 1 and attack.chance == 100 and attack.interval == 2000)
    assert(attack.minDamage == -40 and attack.maxDamage == -40 and 40 / (attack.interval / 1000) / 185 > 0.08)
    return PlayerbotGameplayFixture
end
local function tick(time, x)
    now = time
    if x then player.position = Position(x, 60200, 7) end
    if spawnAt and now >= spawnAt and not wolf then makeWolf(); spawnAt = nil end
    local callback = assert(queued)
    queued = nil
    callback()
end
local function has(fragment)
    for _, line in ipairs(output) do if line:find(fragment, 1, true) then return true end end
    return false
end
local function rejected(callback, fragment)
    local ok, err = pcall(callback)
    assert(not ok and tostring(err):find(fragment, 1, true), "expected rejection: " .. fragment)
    assert(has("RAID_FIXTURE_FAILED"), "fixture failure was not observable by the scenario waiter")
end

local F = start("raid_travel")
tick(100, 60167)
assert(raidCalls == 0, "raid triggered before ordinary outbound movement")
tick(200, 60168)
assert(raidCalls == 1 and not wolf)
tick(1200, 60168)
assert(wolf and has('"phase":"spawned"') and not has('"phase":"withdrew"'))
-- A request/event alone cannot complete it. Only sampled westward displacement.
tick(1300, 60167)
assert(not has('"phase":"withdrew"'))
tick(1400, 60166)
assert(has('"phase":"withdrew"') and wolf)
-- The pursuer closes during an ordinary step delay. This is not a bot retry.
wolf.position.x = 60167
player.health = 145
tick(1500)
tick(1600, 60165) -- Moving away from close pursuit remains legal.
tick(6300)
assert(wolf and not has('"phase":"removed"'))
tick(6400)
assert(not wolf and has('"phase":"guarded"') and has('"phase":"removed"'))
tick(6500, 60208)
assert(has("RAID_TRAVEL_PASS") and has('"deaths":0') and not queued and player.teleports == 1)
F.measureRaidTravel()
assert(not queued, "finished observer scheduled again")

F = start("raid_recovery")
doTargetCombatHealth = function(attacker, id, kind, minimum, maximum, effect)
    assert(attacker == 0 and id == player.id and kind == COMBAT_LIFEDRAIN and minimum == -185 and maximum == -185 and effect == CONST_ME_NONE)
    player.health = 0
    assert(events.PlayerbotRaidFixtureDeath.onDeath(player))
    players[player.id] = nil
    return true
end
tick(100, 60168)
tick(1100, 60168)
tick(1200, 60166)
assert(has('"phase":"kill_injected"') and has('"intentional":true') and wolf)
tick(1300)
player = makePlayer(2) -- normal engine recovery creates another Player
player.health = 175
assert(F.login.onLogin(player))
assert(player.health == 175 and player.teleports == 1 and raidCalls == 1 and wolf.id == 99,
    "recovery reset the raid or granted health")
assert(has('"phase":"recovered"'))
tick(1400, 60164) -- Recovery may approach while still outside the unsafe band.
wolf.position.x = 60165
player.health = 135
tick(1500) -- Stationary player, closing wolf: do not fail before a walk can finish.
tick(1600, 60163)
tick(6300)
assert(not wolf and has('"phase":"removed"'))
tick(6400, 60208)
assert(has("RAID_RECOVERY_PASS") and has('"deaths":1') and not queued)

for _, fixtureMode in ipairs({"raid_travel", "raid_recovery"}) do
    F = start(fixtureMode)
    tick(100, 60168)
    tick(1100, 60168)
    tick(1200, 60166)
    if fixtureMode == "raid_recovery" then
        tick(1300)
        player = makePlayer(2)
        player.health = 175
        assert(F.login.onLogin(player))
        tick(1400, 60164)
    end
    wolf.position.x = player.position.x + 2
    tick(1500) -- Pursuit alone cannot fail the guard, even at distance two.
    rejected(function() tick(1600, player.position.x + 1) end, "moved toward the live threat while unsafe")
end

F = start("raid_travel")
rejected(function() events.PlayerbotRaidFixtureDeath.onDeath(player) end, "unplanned death")
F = start("raid_travel")
rejected(function() tick(120000) end, "deadline exceeded")
F = start("raid_travel")
tick(100, 60168)
tick(1100, 60168)
tick(1200, 60166)
rejected(function() tick(1300, 60173) end, "retried lethal contact")
F = start("raid_travel")
tick(100, 60168)
tick(1100, 60168)
wolf = nil
rejected(function() tick(1200) end, "threat died/disappeared")
F = start("raid_travel")
rejected(function() tick(100, 10) end, "left the isolated runway")

-- The real XML and C++ fixture goal must stay aligned with the Lua runway.
local function source(file)
    local handle = assert(io.open(file)); local content = handle:read("*a"); handle:close(); return content
end
local raid = source("server/tests/playerbot-gameplay/raids/travel.xml")
assert(raid:find('<singlespawn delay="1000" name="Raid Safety Wolf" x="60169" y="60200" z="7" />', 1, true))
assert(not raid:find("areaspawn", 1, true), "raid fixture introduced spawn RNG")
assert(not source("server/data/monster/monsters.xml"):find('name="Raid Safety Wolf"', 1, true),
    "native XML loading could overwrite the unique fixture species")
local catalog = source("server/tests/playerbot-gameplay/raids/raids.xml")
assert(catalog:find('name="Playerbot Travel Safety" file="playerbot-fixture/travel.xml"', 1, true))
assert(catalog:find('margin="1440"', 1, true), "fixture raid could activate randomly")
assert(source("server/src/playerbotfixturedriver.cpp"):find("if (policy.raidTravelFixture) return Position(60208, 60200, 7);", 1, true))
os.getenv, os.mtime, print = nativeGetenv, nativeMtime, nativePrint
print("playerbot raid fixture contracts passed")
