-- Run from the repository root: lua scripts/test-playerbot-fixture-isolation.lua
-- Check mode boundaries first, then complete the two intended-rapier login paths.
local boundary = {}
PlayerbotGameplayFixture = {
    botName = "Bot One",
    economicRewardStorage = 50082,
    pickupRewardStorage = 64120,
    mainlandRewardStorage = 50076,
    nestedRewardStorage = 50083,
    selectHealthPotion = function() error(boundary) end,
}
CreatureEvent = function() return {register = function() end} end
local loginPath = "server/tests/playerbot-gameplay/includes/login.inc"
dofile(loginPath)
local selectedMode
os.getenv = function() return selectedMode end
local isolated = {
    progression = true, readiness_low_wealth = true, arbitration = true,
    magic_training_haste = true, magic_training_great_light = true, magic_training_light = true,
    magic_training_refresh = true, magic_training_failed = true, magic_training_restart = true,
    magic_training_hunt = true, magic_training_post_hunt = true, magic_training_post_hunt_no_overflow = true,
}
local function run(mode, storage, name, rejectWrite)
    selectedMode = mode
    local writes = 0
    local player = {
        getName = function() return name or "Bot One" end,
        getStorageValue = function(_, key) return storage[key] or -1 end,
        setStorageValue = function(_, key, value)
            local rapier = mode == "progression" or mode == "arbitration"
            assert((key == 50082 or rapier and (key == 50076 or key == 50083)) and value == 1,
                "isolation changed another reward")
            writes = writes + 1
            if rejectWrite then return false end
            storage[key] = value
            return true
        end,
    }
    local ok, result = pcall(PlayerbotGameplayFixture.login.onLogin, player)
    return ok, result, writes
end

local file = assert(io.open(loginPath))
local source = file:read("*a")
file:close()
local modes = {cycle = true}
for mode in source:gmatch('mode == "([%w_]+)"') do modes[mode] = true end
local count = 0
for mode in pairs(modes) do
    local storage = {}
    local ok, result, writes = run(mode, storage)
    assert(not ok and result == boundary, mode .. " did not reach gameplay setup")
    local rapier = mode == "progression" or mode == "arbitration"
    assert(writes == (rapier and 3 or isolated[mode] and 1 or 0), mode .. " has incorrect isolation")
    assert(storage[50076] == (rapier and 1 or nil) and storage[50083] == (rapier and 1 or nil),
        mode .. " has incorrect competing reward storage")
    assert(storage[64120] == nil, "intended pickup reward was claimed")
    local humanOk, _, humanWrites = run(mode, {}, "Rook Tester")
    assert(humanOk and humanWrites == 0, "isolation affected another player")
    count = count + 1
end
for mode, marker in pairs({progression = 64120, magic_training_restart = 50098}) do
    local ok, result, writes = run(mode, {[marker] = 1, [50082] = 1, [50076] = 1, [50083] = 1})
    assert(not ok and result == boundary and writes == 0, "restart rewrote persisted isolation")
    ok, result, writes = run(mode, {[marker] = 1})
    assert(not ok and tostring(result):find("did not persist", 1, true) and writes == 0,
        "restart silently repaired missing isolation")
end
local ok, result = run("magic_training_light", {}, nil, true)
assert(not ok and tostring(result):find("could not suppress", 1, true), "failed storage write was ignored")
-- Use real constants and baseline/potion helpers; mock only player/world APIs.
Position = function(x, y, z) return {x = x, y = y, z = z} end
CONST_SLOT_ARMOR, CONST_SLOT_LEFT, CONST_SLOT_RIGHT, CONST_SLOT_FEET, CONST_SLOT_BACKPACK = 4, 6, 5, 8, 3
ITEM_GOLD_COIN = 2148
dofile("server/tests/playerbot-gameplay/includes/constants.inc")
dofile("server/tests/playerbot-gameplay/includes/helpers.inc")
local F = PlayerbotGameplayFixture
F.suppressNearbyMonsters = function() end
F.verifyPickupProgression = function() end
local function completeLogin(mode, storage, restarting)
    selectedMode = mode
    local inventory, writes, scheduled = {}, {}, 0
    local armor = {getId = function() return F.starterArmorId end}
    local weapon = {getId = function() return restarting and F.pickupRewardId or F.starterWeaponId end}
    local backpack = {addItem = function(_, id, quantity)
        inventory[id] = (inventory[id] or 0) + quantity
        return true
    end}
    local slots = {[CONST_SLOT_ARMOR] = armor, [CONST_SLOT_LEFT] = weapon, [CONST_SLOT_BACKPACK] = backpack}
    local player = {
        getName = function() return F.botName end,
        getId = function() return 3 end,
        getLevel = function() return 1 end,
        getVocation = function() return {getId = function() return 0 end} end,
        getPosition = function() return Position(32097, 32219, 7) end,
        getSlotItem = function(_, slot) return slots[slot] end,
        getItemCount = function(_, id) return inventory[id] or 0 end,
        removeItem = function(_, id, quantity)
            inventory[id] = (inventory[id] or 0) - quantity
            return true
        end,
        addItem = backpack.addItem,
        getStorageValue = function(_, key) return storage[key] or -1 end,
        setStorageValue = function(_, key, value)
            writes[key], storage[key] = value, value
            return true
        end,
    }
    addEvent = function(callback)
        assert(callback == F.verifyPickupProgression, "unexpected scheduled verifier")
        scheduled = scheduled + 1
    end
    assert(F.login.onLogin(player), "login did not finish")
    for _, key in ipairs({50076, 50082, 50083}) do
        assert(storage[key] == 1, "competing reward remained unclaimed after setup")
        assert(writes[key] == (not restarting and 1 or nil), "incorrect isolation write after setup")
    end
    assert(writes[64120] == nil and storage[64120] == (restarting and 1 or nil),
        "setup changed intended reward eligibility")
    assert(storage[56002] == nil, "setup disabled the unrelated doublet reward")
    local freshProgression = mode == "progression" and not restarting
    assert(scheduled == (freshProgression and 1 or 0), "setup changed progression verification")
    assert(inventory[8704] == ((freshProgression or mode == "arbitration") and 10 or nil) and
        inventory[2148] == (freshProgression and 100 or nil), "setup changed service reserves")
end
completeLogin("progression", {}, false)
completeLogin("arbitration", {}, false)
completeLogin("progression", {[64120] = 1, [50076] = 1, [50082] = 1, [50083] = 1}, true)
for _, missing in ipairs({50076, 50082, 50083}) do
    local storage = {[64120] = 1, [50076] = 1, [50082] = 1, [50083] = 1}
    storage[missing] = nil
    local success, failure = pcall(completeLogin, "progression", storage, true)
    assert(not success and tostring(failure):find("did not persist", 1, true),
        "restart accepted missing competing reward isolation")
end
-- Exercise the real magic reserve setup, stopping before unrelated level/spell APIs.
Town = function() return {} end
Game = {getExperienceForLevel = function() error(boundary) end}
local function magicReserve(mode, restarting, initialCount)
    selectedMode = mode
    local vocation, potions, mutations = restarting and 4 or 0, initialCount, 0
    local storage = restarting and {[50098] = 1, [50082] = 1} or {}
    local player = {
        getName = function() return F.botName end,
        getId = function() return 3 end,
        getStorageValue = function(_, key) return storage[key] or -1 end,
        setStorageValue = function(_, key, value) storage[key] = value; return true end,
        getVocation = function() return {getId = function() return vocation end} end,
        setVocation = function(_, id) vocation = id; return true end,
        setTown = function() return true end,
        getItemCount = function(_, id) assert(id == 7618); return potions end,
        removeItem = function(_, id, quantity)
            assert(id == 7618 and quantity == potions)
            potions, mutations = 0, mutations + 1
            return true
        end,
        addItem = function(_, id, quantity)
            assert(id == 7618 and quantity == 2, "wrong selected potion reserve")
            potions, mutations = potions + quantity, mutations + 1
            return true
        end,
    }
    local success, result = pcall(F.login.onLogin, player)
    assert(restarting and success and result == true or not restarting and not success and result == boundary,
        mode .. " did not reach the expected setup boundary")
    local seed = not restarting and mode ~= "magic_training_service" and mode ~= "magic_training_progression"
    assert(potions == (seed and 2 or initialCount), mode .. " changed the wrong reserve")
    assert(seed or mutations == 0, "priority/restart fixture mutated the shared reserve")
    assert(F.potionItemId == 7618, "magic fixture retained vocationless potion selection")
end
for mode in pairs(modes) do
    if mode:sub(1, 14) == "magic_training" then
        magicReserve(mode, false, 0)
        magicReserve(mode, false, 7)
    end
end
-- Persisted counts must not be repaired, even if empty or below the return threshold.
for _, potions in ipairs({0, 1, 2, 7}) do magicReserve("magic_training_restart", true, potions) end
print("PASS fixture isolation: " .. count .. " modes, rapier setup/restart, magic reserves/restart, other-player guard, failed writes")
