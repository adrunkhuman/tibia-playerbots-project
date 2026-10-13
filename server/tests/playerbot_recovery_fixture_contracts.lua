-- Run from the repository root. These mocks test fixture assertions, not combat.
local path = "server/tests/playerbot-gameplay/includes/recovery-loadout.inc"
local getenv, output = os.getenv, print
local mode
os.getenv = function() return mode end
PlayerbotGameplayFixture = {}
dofile(path)
assert(next(PlayerbotGameplayFixture) == nil, "inactive fixture touched the world")

CONST_SLOT_HEAD, CONST_SLOT_NECKLACE, CONST_SLOT_BACKPACK, CONST_SLOT_ARMOR = 1, 2, 3, 4
CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_RING, CONST_SLOT_AMMO = 5, 6, 7, 8, 9, 10
SKILL_FIST, SKILL_CLUB, SKILL_SWORD, SKILL_AXE, SKILL_DISTANCE, SKILL_SHIELD = 0, 1, 2, 3, 4, 5
WEAPON_DISTANCE, COMBAT_HEALING, ITEM_PLATINUM_COIN = 5, 128, 2152
CONDITION_REGENERATION, CONDITIONID_DEFAULT, CONDITION_PARAM_TICKS = 1, 0, 2
Position = function(x, y, z) return {x = x, y = y, z = z, getDistance = function() return 0 end} end
Town = function(id) assert(id == 2); return {} end
Condition = function() return {setParameter = function(_, _, ticks) assert(ticks == 1200000) end} end
local events, messages, scheduled, player
CreatureEvent = function(name)
    local event = {register = function() return true end}
    events[name] = event
    return event
end
Weapon = function(kind)
    assert(kind == WEAPON_DISTANCE)
    return {id = function(_, id) assert(id == 2389) end,
        breakChance = function(_, value) assert(value == 100) end, register = function() return true end}
end
Player = function(id) assert(id == 1); return player end
addEvent = function(callback, delay, ...) scheduled = {callback, delay, ...} end
print = function(message) messages[#messages + 1] = message end

for _, scenario in ipairs({"recovery_fists", "recovery_melee"}) do
    mode, events, messages = scenario, {}, {}
    PlayerbotGameplayFixture = {}
    dofile("server/tests/playerbot-gameplay/includes/constants.inc")
    local F = PlayerbotGameplayFixture
    local inventory, slots, children, skills, storage = {}, {}, {}, {}, {}
    local gold, bank, health, mana = 500, 500, 100, 0
    local function item(id, count, slot)
        inventory[id] = (inventory[id] or 0) + count
        local value = {getId = function() return id end, getCount = function() return count end}
        function value:remove()
            inventory[id] = inventory[id] - count
            if slot then slots[slot] = nil end
            count = 0
            return true
        end
        if slot then slots[slot] = value else children[#children + 1] = value end
        return value
    end
    local backpack = {addItem = function(_, id, count) return item(id, count) end}
    slots[3] = backpack
    item(2383, 1, 6); item(2152, 5); item(2389, 10)
    F.clearBackpack = function(bag)
        assert(bag == backpack)
        for _, child in ipairs(children) do child:remove() end
        children, gold = {}, 0
    end
    F.selectHealthPotion = function() F.healthPotionItemId = 7618 end
    player = {
        getId = function() return 1 end, getName = function() return F.botName end,
        getLevel = function() return 8 end, setVocation = function(_, v) assert(v == 3); return true end,
        setTown = function() return true end, teleportTo = function(_, p) assert(p == F.thaisDepotPosition); return true end,
        getPosition = function() return F.thaisDepotPosition end,
        setCapacity = function(_, n) assert(n == 100000); return true end,
        setBankBalance = function(_, n) bank = n; return true end,
        getBankBalance = function() return bank end, getMoney = function() return gold end,
        getSlotItem = function(_, slot) return slots[slot] end,
        getItemCount = function(_, id) return inventory[id] or 0 end,
        addItem = function(_, id, count, _, _, slot) return item(id, count, slot) end,
        getSkillLevel = function(_, id) return skills[id] or 10 end,
        addSkillLevel = function(_, id, delta) skills[id] = (skills[id] or 10) + delta; return true end,
        getHealth = function() return health end, getMaxHealth = function() return 185 end,
        setHealth = function(_, n) health = n; return true end,
        getMaxMana = function() return 35 end, getMana = function() return mana end,
        addMana = function(_, n) mana = mana + n; return true end,
        removeCondition = function() end, addCondition = function() return true end,
        setStorageValue = function(_, k, v) storage[k] = v; return true end,
        isRemoved = function() return false end,
    }
    dofile(path)
    F.setupRecoveryLoadout(player)
    assert(gold == 0 and bank == 0 and inventory[2389] == 1 and inventory[2152] == 0)
    assert(inventory[7618] == 20 and inventory[7620] == 20 and health == 185 and mana == 35)
    assert(skills[SKILL_DISTANCE] == 40 and skills[SKILL_SHIELD] == 40)
    assert(skills[SKILL_FIST] == (mode == 'recovery_fists' and 40 or 10))
    assert(skills[SKILL_SWORD] == (mode == 'recovery_melee' and 40 or 10))
    for _, key in ipairs({50076, 50082, 50083, 64120, 50099}) do assert(storage[key] == 1) end
    assert(scheduled[1] == F.verifyRecoveryLoadout and scheduled[2] == 100 and scheduled[4] == 9000)
    assert(not pcall(F.verifyRecoveryLoadout, 1, 0), 'fixture accepted timeout without combat')
    local monster = {getName = function() return 'Playerbot Recovery Quarry' end, getId = function() return 99 end,
        setDropLoot = function(_, enabled) assert(not enabled); return true end}
    local healthEvent = events.PlayerbotRecoveryHealth.onHealthChange
    assert(not pcall(healthEvent, monster, player, 5, 1, 0, 0), 'old spear attack passed')
    local spear = slots[6]
    slots[6] = nil
    if mode == 'recovery_melee' then
        for _, value in ipairs(children) do if value:getId() == 2376 then slots[6] = value end end
    end
    local a, b, c, d = healthEvent(monster, player, 5, 1, 0, 0)
    assert(a == 5 and b == 1 and c == 0 and d == 0, 'fixture modified combat damage')
    local loot = 0
    local corpse = {getItems = function() return {} end, addItem = function(_, id, count)
        assert(id == ITEM_PLATINUM_COIN and count == 20); loot = count * 100; return {}
    end}
    assert(events.PlayerbotRecoveryDeath.onDeath(monster, corpse, player))
    assert(loot == 2000 and gold == 0, 'fixture credited hidden corpse coins directly to player')
    slots[6] = spear
    assert(not pcall(F.verifyRecoveryLoadout, 1, 0), 'fixture accepted restoration without earned funds')
    gold = loot
    F.verifyRecoveryLoadout(1, 0)
    assert(messages[#messages] == 'PLAYERBOT_GAMEPLAY_TEST ' .. mode:upper() .. '_PASS')
    -- Once complete, the fixture must not police a later normal spear hunt.
    assert(pcall(healthEvent, monster, player, 5, 1, 0, 0))
end

local registered
Game = {createMonsterType = function(name)
    assert(name == 'Playerbot Recovery Quarry')
    return {register = function(_, definition) registered = definition end}
end}
dofile('server/tests/playerbot-gameplay/recovery-monster.inc')
assert(registered.health == 20 and registered.maxHealth == 20 and registered.loot[1].maxCount == 20 and #registered.events == 2)
assert(registered.flags.ignoreSpawnBlock, 'quarry renewal differs from the atlas model')
local spawnsFile = assert(io.open('server/tests/playerbot-gameplay/recovery-spawn.xml'))
local spawns = spawnsFile:read('*a'); spawnsFile:close()
local _, count = spawns:gsub('<monster name="Playerbot Recovery Quarry"', '')
assert(count == 4, 'atlas tiny-pocket admission requires at least four renewing spawns')
mode = 'cycle'
assert(not pcall(dofile, 'server/tests/playerbot-gameplay/recovery-monster.inc'), 'overlay accepted unrelated mode')
os.getenv, print = getenv, output
print('Recovery fixture isolation, setup, real-hit/death observers, corpse-only funding, timeout and restoration checks passed.')
