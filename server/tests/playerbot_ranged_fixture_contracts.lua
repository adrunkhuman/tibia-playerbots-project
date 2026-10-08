-- Run from repository root: lua server/tests/playerbot_ranged_fixture_contracts.lua
local fixture = "server/tests/playerbot-gameplay/includes/ranged-position.inc"
local nativeGetenv, nativeMtime, nativePrint = os.getenv, os.mtime, print
local mode, now = "cycle", 0
os.getenv = function(name) if name == "PLAYERBOT_GAMEPLAY_MODE" then return mode end return nativeGetenv(name) end
os.mtime = function() return now end
PlayerbotGameplayFixture = {}
-- Inactive fixtures must not touch weapon or event APIs.
dofile(fixture)
assert(next(PlayerbotGameplayFixture) == nil)

COMBAT_NONE, COMBAT_HEALING, COMBAT_PHYSICALDAMAGE, COMBAT_LIFEDRAIN = 0, 1, 2, 3
COMBAT_PARAM_TYPE, COMBAT_FORMULA_DAMAGE, ORIGIN_RANGED, WEAPON_DISTANCE, ITEM_GOLD_COIN = 1, 2, 3, 4, 2148
local weapon, combat, events, queued, output, player, monster
Combat = function()
    combat = {parameters = {}}
    function combat:setParameter(key, value) self.parameters[key] = value end
    function combat:setOrigin(value) self.origin = value end
    function combat:setFormula(...) self.formula = {...} end
    function combat:execute(p, variant) assert(p == player and variant == 42) return true end
    return combat
end
Weapon = function(kind)
    assert(kind == WEAPON_DISTANCE)
    weapon = {}
    function weapon:id(value) self.itemId = value end
    function weapon:hitChance(value) self.chance = value end
    function weapon:breakChance(value) self.breakage = value end
    function weapon:register() self.registered = true return true end
    return weapon
end
CreatureEvent = function(name)
    local event = {}
    function event:register() events[name] = self end
    return event
end
ItemType = function(id) assert(id == 2389) return {getShootRange = function() return 3 end} end
Player = function(id) if player and id == player.id then return player end end
Monster = function(id) if monster and id == monster.id then return monster end end
addEvent = function(fn, delay, ...) assert(delay == 100) queued = {fn = fn, args = {...}} end
print = function(line) output[#output + 1] = line end

local function creature(id, health, x)
    local c = {id = id, health = health, position = {x = x, y = 0, z = 7}, registrations = {}}
    function c:getId() return self.id end
    function c:getHealth() return self.health end
    function c:getPosition() return {x = self.position.x, y = self.position.y, z = self.position.z} end
    function c:getItemCount(itemId) assert(itemId == ITEM_GOLD_COIN) return 20 end
    function c:isRemoved() return false end
    function c:registerEvent(name) assert(events[name]) self.registrations[name] = true return true end
    function c:unregisterEvent(name) self.registrations[name] = nil return true end
    return c
end
local function start(fixtureMode)
    mode, now, events, queued, output = fixtureMode, 0, {}, nil, {}
    PlayerbotGameplayFixture = {spearItemId = 2389}
    dofile(fixture)
    player, monster = creature(1, 200, 0), creature(2, 150, 3)
    PlayerbotGameplayFixture.startRangedPositionObservation(player, monster, mode)
    return PlayerbotGameplayFixture, PlayerbotGameplayFixture.rangedPositionObservation
end
local function health(c, attacker, amount, kind, secondary, secondaryKind)
    local values = {events.PlayerbotRangedHealthChange.onHealthChange(c, attacker, amount, kind, secondary or 0, secondaryKind or COMBAT_NONE)}
    assert(values[1] == amount and values[2] == kind and values[3] == (secondary or 0) and values[4] == (secondaryKind or COMBAT_NONE), "observer mutated combat")
end
local function tick(time)
    now = time
    local callback = queued
    queued = nil
    assert(callback)
    callback.fn((table.unpack or unpack)(callback.args))
end

local F, observation = start("ranged_position")
local tiles, tileCount = {}, 0
Position = function(x, y, z) return {x = x, y = y, z = z} end
local function tileKey(position) return position.x .. "," .. position.y .. "," .. position.z end
Tile = function(position) return tiles[tileKey(position)] end
Game = {
    createTile = function(position)
        local tile = {isWalkable = function(self) return self.ground == 4526 end}
        tiles[tileKey(position)] = tile
        tileCount = tileCount + 1
        return tile
    end,
    createItem = function(id, count, position)
        assert(id == 4526 and count == 1)
        Tile(position).ground = id
        return {}
    end,
}
local arena = F.createRangedArena()
assert(tileCount == 73 * 11 and Tile(Position(arena.x - 64, arena.y, arena.z)):isWalkable())
assert(not Tile(Position(arena.x - 65, arena.y, arena.z)))
assert(not pcall(F.createRangedArena), "arena overwrote mapped terrain")
assert(weapon.registered and weapon.itemId == 2389 and weapon.chance == 100 and weapon.breakage == 0)
assert(combat.origin == ORIGIN_RANGED and combat.parameters[COMBAT_PARAM_TYPE] == COMBAT_PHYSICALDAMAGE)
assert(combat.formula[1] == COMBAT_FORMULA_DAMAGE and combat.formula[2] == -10 and combat.formula[4] == -10)
assert(weapon.onUseWeapon(player, 42))
-- Two health losses and healing inside one sampling interval: net health is
-- unchanged, but actual damage must include both losses and secondary damage.
health(player, monster, -6, COMBAT_PHYSICALDAMAGE)
health(player, player, 6, COMBAT_HEALING)
health(player, monster, 6, COMBAT_PHYSICALDAMAGE, 2, COMBAT_PHYSICALDAMAGE)
health(player, monster, 100, COMBAT_HEALING)
health(player, creature(99, 100, 0), 100, COMBAT_PHYSICALDAMAGE)
assert(observation.damage == 14 and player:getHealth() == 200)
player.position.x = -1
tick(125) -- first interval at distance 3, next at distance 4
player.position.x = 0
tick(300)
assert(observation.fightMs == 300 and observation.rangedMs == 125 and observation.steps == 2 and observation.maxDisplacement == 1)
for _ = 1, 15 do
    health(monster, player, -10, COMBAT_PHYSICALDAMAGE)
    monster.health = monster.health - 10
end
assert(observation.dealt == 150 and observation.hits == 15)
now = 375
assert(events.PlayerbotRangedDeath.onDeath(monster, {}))
monster = nil
player.position.x = 10 -- post-kill corpse approach must not inflate combat displacement
tick(400)
assert(observation.done and observation.fightMs == 375 and observation.rangedMs == 200 and observation.maxDisplacement == 1)
assert(not player.registrations.PlayerbotRangedHealthChange and queued == nil)
assert(#output == 1 and output[1]:find("damage=14 defeated=true", 1, true) and output[1]:find("dealt=150 hits=15", 1, true))
F.measureRangedPosition()
assert(#output == 1)
health(player, creature(2, 150, 3), 6, COMBAT_PHYSICALDAMAGE)
assert(observation.damage == 14, "finished observer kept recording")

F, observation = start("ranged_position_control")
health(player, monster, 999, COMBAT_PHYSICALDAMAGE)
assert(observation.damage == 200, "overkill counted beyond actual health")
monster = nil
tick(100)
assert(output[1]:find("defeated=false", 1, true), "removal was mistaken for a kill")
F, observation = start("ranged_position_corner")
tick(90000)
assert(observation.done and observation.fightMs == 90000 and queued == nil)
assert(output[1]:find("defeated=false", 1, true) and not monster.registrations.PlayerbotRangedHealthChange)

-- Declaration contract: one guaranteed coin and fixed, slower melee chaser.
local definitions = {}
ITEM_BAG, ITEM_PLATINUM_COIN = 1987, 2152
Game = {createMonsterType = function(name)
    return {register = function(_, definition) definitions[name] = definition end}
end}
dofile("server/tests/playerbot-monsters/corpse.lua")
local chaser = definitions["Playerbot Melee Chaser"]
assert(chaser.health == 150 and chaser.speed == 200 and chaser.flags.targetDistance == 1)
assert(chaser.attacks[1].type == COMBAT_LIFEDRAIN and chaser.attacks[1].range == 1)
assert(chaser.attacks[1].minDamage == -6 and chaser.attacks[1].maxDamage == -6 and chaser.attacks[1].chance == 100)
assert(#chaser.loot == 1 and chaser.loot[1].id == ITEM_GOLD_COIN and chaser.loot[1].chance == 100000 and chaser.loot[1].maxCount == 1)
os.getenv, os.mtime, print = nativeGetenv, nativeMtime, nativePrint
print("playerbot ranged fixture contracts passed")
