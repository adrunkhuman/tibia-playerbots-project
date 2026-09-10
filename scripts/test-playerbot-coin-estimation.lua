-- Compare the C++ contract expectations with the actual datapack loot generator.
-- No server, map, inventory, or unopened corpse contents are used.
Container = {}
RETURNVALUE_NOERROR = 0
ITEM_ATTRIBUTE_CHARGES = 1
local rate, roll, acquired = 1, 0, 0
local worth = { [1] = 1, [2] = 100, [3] = 10000 }
configKeys = { RATE_LOOT = 1 }
configManager = { getNumber = function() return rate end }
MAX_LOOTCHANCE = 100000
local global = assert(io.open('server/data/global.lua'))
local source = global:read('*a')
global:close()
-- Load the production function, not a second implementation of its arithmetic.
assert(load(source:match('(function getLootRandom%(%).-\nend)')))()
local random = math.random
math.random = function(low, high)
    assert(low == 0 and high == MAX_LOOTCHANCE)
    return roll
end
function ItemType(id)
    return {
        isStackable = function() return worth[id] ~= nil end,
        isFluidContainer = function() return false end,
    }
end
Game = { createItem = function(id, count)
    -- luaGameCreateItem reads uint16_t; fractional Lua counts are truncated.
    acquired = acquired + (worth[id] or 0) * math.floor(count)
    return { isContainer = function() return false end }
end }
dofile('server/data/lib/core/container.lua')
local corpse = {
    getEmptySlots = function() return 10 end,
    addItemEx = function() return RETURNVALUE_NOERROR end,
}
local function estimate(id, chance, count, lootRate)
    acquired, rate = 0, lootRate
    for value = 0, MAX_LOOTCHANCE do
        roll = value
        assert(Container.createLootItem(corpse, {
            itemId = id, chance = chance, maxCount = count,
            subType = -1, actionId = -1,
        }))
    end
    return acquired / (MAX_LOOTCHANCE + 1)
end
assert(estimate(1, 3, 10, 1) == 6 / 100001)
assert(estimate(1, 3, 10, 2) == 12 / 100001)
assert(estimate(1, 100000, 10, 1) == 550000 / 100001)
assert(estimate(2, 3, 10, 1) == 600 / 100001)
assert(estimate(3, 100000, 1, 2) == 10000)
assert(estimate(4, 100000, 1, 1) == 0) -- no resale credit
math.random = random
print('Coin estimate / loaded Lua chance-count contracts passed.')
