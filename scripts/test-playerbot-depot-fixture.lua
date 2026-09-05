-- Run from the repository root: lua scripts/test-playerbot-depot-fixture.lua
Position = function(x, y, z) return {x = x, y = y, z = z} end
PlayerbotGameplayFixture = {}
dofile("server/tests/playerbot-gameplay/includes/constants.inc")
dofile("server/tests/playerbot-gameplay/includes/verifiers.inc")
local F = PlayerbotGameplayFixture
CONST_SLOT_ARMOR, CONST_SLOT_LEFT, CONST_SLOT_HEAD, CONST_SLOT_FEET = 4, 6, 1, 8
ITEM_GOLD_COIN = 2148
F.potionItemId = F.smallHealthPotionItemId
F.PlayerbotThaisLockerDepotId = function() return 2 end
local phase, state, stored, carried
os.getenv = function(key)
    assert(key == "PLAYERBOT_DEPOT_RESTART_PHASE")
    return phase
end
local equipment = {[4] = F.starterArmorId, [6] = F.seedWeaponId, [1] = 2461, [8] = 2643}
Player = function()
    return {
        isRemoved = function() return false end,
        getDepotChest = function() return {getItemCountById = function(_, id) return stored[id] or 0 end} end,
        getStorageValue = function(_, key) assert(key == F.depotFixtureStorage); return state end,
        getItemCount = function(_, id) return carried[id] or 0 end,
        getSlotItem = function(_, slot) return {getId = function() return equipment[slot] end} end,
    }
end
local function setup(checkpoint, secondCycle)
    phase, state = checkpoint, secondCycle and 2 or 0
    stored = {[F.depotLootItemId] = secondCycle and 3 or 2, [2148] = 1,
        [2380] = 1, [F.starterWeaponId] = 1, [2120] = 2, [2554] = 2}
    carried = {[F.depotLootItemId] = 2, [F.potionItemId] = 10, [2120] = 1, [2554] = 1}
end
setup("depart", false)
state = 2 -- checkpoint-only restart does not seed another cheese
F.verifyDepot(3, 0)
setup("", true)
F.verifyDepot(3, 0) -- two persisted cheeses plus one new cargo; no meat was seeded
stored[F.depotLootItemId] = 2
assert(not pcall(F.verifyDepot, 3, 0), "second cycle accepted missing new cargo")
setup("", true)
stored[F.potionItemId] = 1
assert(not pcall(F.verifyDepot, 3, 0), "accepted deposited potion reserve")
setup("", true)
carried[F.potionItemId] = 4
assert(not pcall(F.verifyDepot, 3, 0), "accepted depleted potion reserve")
setup("", true)
carried[F.depotLootItemId] = 1
assert(not pcall(F.verifyDepot, 3, 0), "accepted missing retained cheese")
print("PASS depot fixture: checkpoint/two-cycle counts, unseeded meat, potion and cheese protection")
