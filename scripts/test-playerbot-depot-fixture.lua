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
local paused, stored, carried, equipment, pending
local originalPrint, originalFlush = print, io.flush
local buffered, visible
print = function(message) buffered = message end
io.flush = function() visible, buffered = buffered, nil end
os.getenv = function() error("delayed verifier must not read mutable environment") end
addEvent = function(callback, delay, ...)
    assert(delay == 500)
    local args = {...}
    pending = function() callback((table.unpack or unpack)(args)) end
end
Player = function()
    return {
        isRemoved = function() return false end,
        getDepotChest = function() return {getItemCountById = function(_, id) return stored[id] or 0 end} end,
        getStorageValue = function(_, key) assert(key == 50096); return paused and 1 or 0 end,
        getItemCount = function(_, id) return carried[id] or 0 end,
        getSlotItem = function(_, slot) return {getId = function() return equipment[slot] end} end,
    }
end
local function setup(loot, gold)
    paused, pending, visible, buffered = true, nil, nil, nil
    equipment = {[4] = F.starterArmorId, [6] = F.seedWeaponId, [1] = 2461, [8] = 2643}
    stored = {[F.depotLootItemId] = loot, [2148] = gold,
        [2380] = 1, [F.starterWeaponId] = 1, [2120] = 2, [2554] = 2}
    carried = {[F.depotLootItemId] = 2, [F.potionItemId] = 10, [2120] = 1, [2554] = 1}
end
for _, gold in ipairs({0, 1}) do
    setup(2, gold)
    F.verifyDepot(3, 0, 2, gold)
    assert(visible == "PLAYERBOT_GAMEPLAY_TEST DEPOT_PASS" and buffered == nil,
        "paused verifier left its success marker buffered")
end
setup(3, 1)
F.verifyDepot(3, 0, 3, 1)
-- Matching food/gold is not proof that all deposits have finished.
paused = false
stored[2120] = 0
F.verifyDepot(3, 1, 3, 1)
assert(pending, "verifier did not wait for Depart")
assert(not pcall(F.verifyDepot, 3, 0, 3, 1), "accepted inventory before pause")
paused, stored[2120] = true, 2
pending()
for _, id in ipairs({F.depotLootItemId, 2148, 2380, F.starterWeaponId, 2120, 2554}) do
    setup(3, 1)
    stored[id] = stored[id] - 1
    assert(not pcall(F.verifyDepot, 3, 0, 3, 1), "accepted lost depot inventory: " .. id)
end
for _, id in ipairs({F.depotLootItemId, F.potionItemId, 2120, 2554}) do
    setup(3, 1)
    carried[id] = 0
    assert(not pcall(F.verifyDepot, 3, 0, 3, 1), "accepted lost reserve: " .. id)
end
for slot in pairs(equipment) do
    setup(3, 1)
    equipment[slot] = 0
    assert(not pcall(F.verifyDepot, 3, 0, 3, 1), "accepted lost equipment: " .. slot)
end
setup(3, 1)
stored[F.potionItemId] = 1
assert(not pcall(F.verifyDepot, 3, 0, 3, 1), "accepted deposited potion reserve")
print, io.flush = originalPrint, originalFlush
-- Exercise the actual login branch: snapshot before second-cycle storage mutation.
local file = assert(io.open("server/tests/playerbot-gameplay/includes/login.inc"))
local source = file:read("*a")
file:close()
local branch = assert(source:match('(    if mode == "depot" then.-)    if mode == "slotted_loot_seller"'))
local login = assert((loadstring or load)("return function(F, player, mode)\n" .. branch .. "end"))()
Condition = function() return {setParameter = function() end} end
local state, seeded, scheduled
local player = {
    removeCondition = function() end,
    addCondition = function() return true end,
    getStorageValue = function() return state end,
    setStorageValue = function(_, key, value) assert(key == F.depotFixtureStorage); state = value; return true end,
    getSlotItem = function() return {addItem = function() seeded = true; return true end} end,
    getId = function() return 3 end,
}
local env = {}
os.getenv = function(key) return env[key] end
addEvent = function(callback, delay, id, attempts, loot, gold)
    assert(callback == F.verifyDepot and delay == 500 and id == 3 and attempts == 360)
    scheduled = {loot, gold}
end
for _, phase in ipairs({"approach", "locker", "chest", "deposit", "depart"}) do
    state, seeded, scheduled = 2, false, nil
    env = {PLAYERBOT_DEPOT_RESTART_PHASE = "depart", PLAYERBOT_DEPOT_VERIFIER_PHASE = phase}
    login(F, player, "depot")
    assert(not seeded and scheduled[1] == 2)
    assert(scheduled[2] == ((phase == "deposit" or phase == "depart") and 1 or 0))
end
state, seeded, scheduled = 0, false, nil
env = {PLAYERBOT_DEPOT_RESTART_PHASE = "depart"}
login(F, player, "depot")
assert(state == 2 and seeded and scheduled[1] == 3 and scheduled[2] == 1)
for _, phase in ipairs({"approach", "locker", "chest", "deposit", ""}) do
    state, scheduled = 2, nil
    env = {PLAYERBOT_DEPOT_RESTART_PHASE = phase}
    login(F, player, "depot")
    assert(not scheduled, "successful verifier scheduled outside Depart")
end
state, scheduled = 2, nil
env = {PLAYERBOT_DEPOT_RESTART_PHASE = "depart", PLAYERBOT_DEPOT_MOVE_CASE = "rejected"}
login(F, player, "depot")
assert(not scheduled, "rejected moves invoked successful-depot verifier")
print("PASS depot fixture: login snapshots, bounded pause wait, explicit expectations, lost inventory and reserve protection")
