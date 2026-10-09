-- Run from the repository root: lua scripts/test-playerbot-spear-fixture.lua
PlayerbotGameplayFixture = {}
CONST_SLOT_HEAD, CONST_SLOT_BACKPACK, CONST_SLOT_ARMOR = 1, 3, 4
CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_AMMO = 5, 6, 7, 8, 10
ITEM_GOLD_COIN = 2148
SKILL_FIST, SKILL_CLUB, SKILL_SWORD, SKILL_AXE, SKILL_DISTANCE, SKILL_SHIELD, SKILL_FISHING = 0, 1, 2, 3, 4, 5, 6
CONDITION_REGENERATION, CONDITIONID_DEFAULT, CONDITION_PARAM_TICKS = 1, 0, 2
Condition = function() return {setParameter = function(_, _, ticks) assert(ticks == 1200000) end} end
Position = function(x, y, z) return {x = x, y = y, z = z, getDistance = function() return 4 end} end
CreatureEvent = function() return {register = function() end} end
Town = function(id) assert(id == 2); return {} end
Game = {getExperienceForLevel = function(level) return level * 100 end}
for _, name in ipairs({'constants', 'helpers', 'verifiers', 'events', 'login'}) do
    dofile('server/tests/playerbot-gameplay/includes/' .. name .. '.inc')
end
local F = PlayerbotGameplayFixture
local selectedMode, player, scheduled, messages, suppressed
local originalGetenv, originalPrint = os.getenv, print
os.getenv = function(key) assert(key == 'PLAYERBOT_GAMEPLAY_MODE'); return selectedMode end
print = function(value) messages[#messages + 1] = value end
Player = function(id) assert(id == 1); return player end
F.suppressNearbyMonsters = function(id) assert(id == 1); suppressed = suppressed + 1 end
addEvent = function(callback, delay, ...)
    scheduled = {callback = callback, delay = delay, args = {...}}
end

local function setup(mode)
    selectedMode, messages, scheduled, suppressed = mode, {}, nil, 0
    local inventory, slots = {}, {}
    local level, vocation, bank, capacity, health, mana, distance = 8, 4, 0, 0, 100, 5, 4
    local storage, skills = {}, {[0] = 11, 25, 20, 15, 10, 25, 12}
    local function addItem(_, id, count, _, _, slot)
        inventory[id] = (inventory[id] or 0) + count
        local item = {
            getId = function() return id end,
            getCount = function() return count end,
            remove = function(_, amount)
                amount = amount or count
                inventory[id], count = inventory[id] - amount, count - amount
                if slot and count == 0 then slots[slot] = nil end
                return true
            end,
        }
        if slot then slots[slot] = item end
        return item
    end
    slots[CONST_SLOT_BACKPACK] = {addItem = addItem, getItems = function()
        local items = {}
        for id, count in pairs(inventory) do
            local equipped = false
            for _, item in pairs(slots) do if item.getId and item:getId() == id then equipped = true end end
            if not equipped and count > 0 then
                items[#items + 1] = {getId = function() return id end, remove = function() inventory[id] = 0; return true end}
            end
        end
        return items
    end}
    -- Contamination must be cleared for reserve fixtures, not counted as funding or a spare.
    addItem(nil, 2399, 1) -- throwing star
    addItem(nil, 2152, 5) -- platinum coins
    addItem(nil, F.saleItemId, 1)
    addItem(nil, F.seedWeaponId, 1, nil, nil, CONST_SLOT_LEFT)
    addItem(nil, 2666, 1, nil, nil, CONST_SLOT_AMMO)
    player = {
        getName = function() return F.botName end,
        getId = function() return 1 end,
        isRemoved = function() return false end,
        getLevel = function() return level end,
        getExperience = function() return level * 100 end,
        getSkillLevel = function(_, skill) return skills[skill] end,
        getSkillTries = function() return 0 end,
        addSkillLevel = function(_, skill, delta) skills[skill] = skills[skill] + delta; return true end,
        addExperience = function(_, amount) level = level + amount / 100 end,
        getVocation = function() return {getId = function() return vocation end} end,
        setVocation = function(_, id) vocation = id; return true end,
        setTown = function() return true end,
        teleportTo = function(_, position) assert(position == F.thaisDepotPosition); return true end,
        getPosition = function() return {x = 32354, y = 32226, z = 7, getDistance = function() return distance end} end,
        setTestDistance = function(_, value) distance = value end,
        getSlotItem = function(_, slot) return slots[slot] end,
        getItemCount = function(_, id) return inventory[id] or 0 end,
        removeItem = function(_, id, count) inventory[id] = inventory[id] - count; return true end,
        addItem = addItem,
        setBankBalance = function(_, amount) bank = amount; return true end,
        setCapacity = function(_, amount) capacity = amount; return true end,
        getFreeCapacity = function() return capacity - 10000 end,
        getMoney = function() return (inventory[2148] or 0) + 100 * (inventory[2152] or 0) end,
        getBankBalance = function() return bank end,
        getHealth = function() return health end,
        getMaxHealth = function() return 185 end,
        setHealth = function(_, value) health = value; return true end,
        getMana = function() return mana end,
        getMaxMana = function() return 35 end,
        addMana = function(_, value) mana = mana + value; return true end,
        removeCondition = function() end,
        addCondition = function() return true end,
        setStorageValue = function(_, key, value) storage[key] = value; return true end,
    }
    assert(F.login.onLogin(player))
    assert(level == (mode == 'spear_restock_scaled' and 16 or 8) and vocation == 3)
    local startupZero = mode == 'equipment_buy_spear' or mode == 'spear_mirrored' or mode == 'spear_unfunded'
    local incomeRecovery = mode == 'spear_recovery_income'
    local reserve = startupZero or mode == 'spear_last_break' or incomeRecovery
    local money = mode == 'spear_unfunded' and 0 or incomeRecovery and 6 or reserve and 39 or 100
    assert(bank == (reserve and 0 or 100) and capacity == 100000 and (inventory[ITEM_GOLD_COIN] or 0) == money)
    if reserve then
        assert(player:getMoney() == money and health == (incomeRecovery and 128 or 185) and mana == 35)
        assert(inventory[2399] == 0 and inventory[2152] == 0 and inventory[F.saleItemId] == 0 and
            inventory[F.seedWeaponId] == 0 and not slots[CONST_SLOT_AMMO], 'alternate funding/weapon survived setup')
        assert(inventory[F.ropeItemId] == 1 and inventory[F.shovelItemId] == 1 and inventory[F.meatItemId] == 2)
        assert(storage[F.fixtureReadyStorage] == ((startupZero or incomeRecovery) and 1 or nil))
        if incomeRecovery then
            assert(skills[SKILL_DISTANCE] == 40 and skills[SKILL_SHIELD] == 20)
            for skill = SKILL_FIST, SKILL_FISHING do
                if skill ~= SKILL_DISTANCE and skill ~= SKILL_SHIELD then assert(skills[skill] == 10) end
            end
            for _, key in ipairs({F.mainlandRewardStorage, F.economicRewardStorage, F.nestedRewardStorage, F.pickupRewardStorage}) do
                assert(storage[key] == 1, 'reward income must not preempt combat')
            end
        end
    end
    assert(inventory[F.healthPotionItemId] == 20 and F.potionItemId == F.healthPotionItemId)
    assert(inventory[F.manaPotionItemId] == 20, 'mana recovery must not preempt the spear fixture')
    assert((inventory[F.spearItemId] or 0) == (startupZero and 0 or mode == 'spear_last_break' and 3 or mode == 'spear_break' and 2 or 1))
    assert(slots[mode == 'spear_mirrored' and CONST_SLOT_LEFT or CONST_SLOT_RIGHT]:getId() == 2525)
    if mode == 'spear_unfunded' then
        assert(scheduled.delay == 10000 and scheduled.args[3] == 0, 'unfunded inventory sampling must be bounded')
    end
    assert(scheduled.callback == (incomeRecovery and F.verifySpearRecoveryIncome or mode == 'spear_last_break' and F.breakLastSpear or mode == 'spear_break' and F.breakHandSpear or F.verifySpearRestock))
    assert(suppressed == (incomeRecovery and 0 or 1), 'income hunt must retain ordinary loaded-world monsters')
    if incomeRecovery then assert(scheduled.delay == 250 and scheduled.args[2] == 4800) end
    assert(messages[1]:find('"phase":"start"', 1, true) and
        messages[2] == 'PLAYERBOT_GAMEPLAY_TEST ' .. mode:upper() .. '_START')
    return inventory, slots
end

for _, mode in ipairs({'spear_restock', 'spear_break', 'spear_restock_scaled', 'equipment_buy_spear', 'spear_last_break', 'spear_mirrored'}) do
    local inventory, slots = setup(mode)
    local reserve = mode == 'equipment_buy_spear' or mode == 'spear_last_break' or mode == 'spear_mirrored'
    local weaponSlot = mode == 'spear_mirrored' and CONST_SLOT_RIGHT or CONST_SLOT_LEFT
    local shieldSlot = mode == 'spear_mirrored' and CONST_SLOT_LEFT or CONST_SLOT_RIGHT
    if mode == 'spear_last_break' then
        player:setTestDistance(0)
        assert(not pcall(F.breakLastSpear, 1, 1, 2, 7, 0), 'break accepted a hunt that never left the depot')
        F.breakLastSpear(1, 1, 2, 7, 1)
        assert(scheduled.callback == F.breakLastSpear and scheduled.args[5] == 0)
        player:setTestDistance(4)
        inventory[2148] = 100
        assert(not pcall(F.breakLastSpear, 1, 1, 2, 7, 0), 'break accepted funding above the reserve')
        inventory[2148] = 39
        inventory[F.spearItemId] = 4
        assert(not pcall(F.breakLastSpear, 1, 1, 2, 7, 0), 'break accepted a backpack spare')
        inventory[F.spearItemId] = 3
        F.breakLastSpear(1, 1, 2, 7, 0)
        assert(inventory[F.spearItemId] == 0 and slots[CONST_SLOT_LEFT] == nil)
        assert(messages[#messages - 1]:find('"phase":"last_spear"', 1, true) and
            messages[#messages - 1]:find('"spears":1,"equipped_spears":1', 1, true))
        assert(messages[#messages]:find('"phase":"broken"', 1, true) and
            messages[#messages]:find('"spears":0,"equipped_spears":0', 1, true))
        assert(scheduled.callback == F.verifySpearRestock and scheduled.args[2] == mode)
    elseif mode == 'spear_break' then
        F.breakHandSpear(1, 1, 2, 7, 0)
        assert(inventory[F.spearItemId] == 1 and slots[CONST_SLOT_LEFT] == nil)
        assert(scheduled.callback == F.verifySpearRestock and scheduled.args[2] == mode)
    end
    local target = mode == 'spear_restock_scaled' and 7 or 3
    local function verify(handCount, totalCount, attempts)
        inventory[F.spearItemId] = totalCount
        slots[weaponSlot] = handCount and {
            getId = function() return F.spearItemId end,
            getCount = function() return handCount end,
        } or nil
        return pcall(F.verifySpearRestock, 1, mode, attempts or 0)
    end
    if reserve then
        assert(not verify(target, target), 'unspent reserve passed')
        inventory[2148] = 9
    end
    assert(verify(target, target))
    assert(messages[#messages - 1]:find('"spears":' .. target, 1, true) and
        messages[#messages] == 'PLAYERBOT_GAMEPLAY_TEST ' .. mode:upper() .. '_PASS')
    assert(not verify(target - 1, target - 1), 'short stock passed')
    assert(not verify(target + 1, target + 1), 'oversized stock passed')
    assert(not verify(1, target), 'unwielded spare stack passed')
    assert(not verify(nil, target), 'missing hand stack passed')
    assert(verify(1, 1, 1) and scheduled.callback == F.verifySpearRestock and scheduled.args[3] == 0,
        'incomplete stock did not retry within its existing bound')
    if reserve then
        inventory[2148] = 12
        assert(verify(3, 3), 'normal nine-gold spear price failed')
        inventory[2148] = 10
        assert(not verify(3, 3), 'discounted/unrelated spending passed')
        inventory[2148] = 9
        player:setBankBalance(1)
        assert(not verify(3, 3), 'nonzero bank passed')
        player:setBankBalance(0)
        local shield = slots[shieldSlot]
        slots[shieldSlot], slots[weaponSlot] = slots[weaponSlot], shield
        assert(not pcall(F.verifySpearRestock, 1, mode, 0), 'swapped hand orientation passed')
        slots[shieldSlot], slots[weaponSlot] = shield, nil
        slots[shieldSlot] = nil
        assert(not verify(3, 3), 'lost shield passed')
    end
    if mode == 'spear_restock_scaled' then
        player.getLevel = function() return 8 end
        assert(not verify(7, 7), 'level-8 fixture passed as scaled stock')
    end
end
local inventory, slots = setup('spear_unfunded')
assert(pcall(F.verifySpearRestock, 1, 'spear_unfunded', 0))
assert(messages[#messages - 1]:find('"spears":0,"equipped_spears":0', 1, true) and
    messages[#messages] == 'PLAYERBOT_GAMEPLAY_TEST SPEAR_UNFUNDED_PASS')
for _, id in ipairs({ITEM_GOLD_COIN, F.spearItemId}) do
    inventory[id] = 1
    assert(not pcall(F.verifySpearRestock, 1, 'spear_unfunded', 0), 'unfunded fixture accepted stock or funds')
    inventory[id] = 0
end
player:setBankBalance(1)
assert(not pcall(F.verifySpearRestock, 1, 'spear_unfunded', 0), 'unfunded fixture accepted bank funds')
player:setBankBalance(0)
slots[CONST_SLOT_LEFT] = {getId = function() return F.spearItemId end}
assert(not pcall(F.verifySpearRestock, 1, 'spear_unfunded', 0), 'unfunded fixture accepted a wielded spear')
slots[CONST_SLOT_LEFT], slots[CONST_SLOT_RIGHT] = nil, nil
assert(not pcall(F.verifySpearRestock, 1, 'spear_unfunded', 0), 'unfunded fixture accepted a lost shield')
inventory, slots = setup('spear_recovery_income')
local registered, naturalLootDisabled = 0, false
local monster = {
    isMonster = function() return true end,
    getId = function() return 42 end,
    getName = function() return 'Rat' end,
    registerEvent = function(_, name)
        assert(name == 'PlayerbotSupplyRecoveryCorpseDeath'); registered = registered + 1; return true
    end,
    setDropLoot = function(_, enabled) assert(enabled == false); naturalLootDisabled = true; return true end,
}
Game.getSpectators = function(_, multifloor, _, x1, x2, y1, y2)
    assert(not multifloor and x1 == 8 and x2 == 8 and y1 == 8 and y2 == 8)
    return {monster, {isMonster = function() return false end}}
end
F.supplyRecoveryRegisteredMonsters = {}
F.verifySpearRecoveryIncome(1, 1)
assert(registered == 1 and not naturalLootDisabled and scheduled.callback == F.verifySpearRecoveryIncome and scheduled.args[2] == 0)
assert(not pcall(F.verifySpearRecoveryIncome, 1, 0), 'income verifier did not exhaust its bound')
assert(registered == 1, 'income event registered repeatedly')
inventory[ITEM_GOLD_COIN] = 11 -- Selling a used healing flask must not pass.
assert(not pcall(F.verifySpearRecoveryIncome, 1, 0), 'noncombat proceeds passed as hunt income')
inventory[ITEM_GOLD_COIN] = 6
local removed, coin = 0, 0
local corpse = {
    getItems = function() return {{remove = function() removed = removed + 1; return true end}} end,
    addItem = function(_, id, count) assert(id == ITEM_GOLD_COIN and count == 1); coin = count; return true end,
}
assert(F.supplyRecoveryCorpseDeath.onDeath(monster, corpse) and naturalLootDisabled and removed == 1 and coin == 1)
assert(messages[#messages]:find('"target_id":42,"gold":1', 1, true))
assert(inventory[ITEM_GOLD_COIN] == 6, 'corpse normalization granted direct player income')
assert(not pcall(F.supplyRecoveryCorpseDeath.onDeath, monster, nil), 'missing corpse passed')
inventory[ITEM_GOLD_COIN] = 7 -- Stand in for the normal loot move; parsing tests require its telemetry.
F.verifySpearRecoveryIncome(1, 0)
assert(messages[#messages - 1]:find('"carried_gold":7,"bank_gold":0', 1, true) and
    messages[#messages] == 'PLAYERBOT_GAMEPLAY_TEST SPEAR_RECOVERY_INCOME_PASS')
os.getenv, print = originalGetenv, originalPrint
print('Spear setup, spare/last break, mirrored hands, unfunded stock, paid reserve, and bounded recovery-income contracts passed.')
