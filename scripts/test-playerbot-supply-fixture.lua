-- Run from the repository root: lua scripts/test-playerbot-supply-fixture.lua
PlayerbotGameplayFixture = {potionItemId = 7618}
dofile('server/tests/playerbot-gameplay/includes/verifiers.inc')
local F = PlayerbotGameplayFixture
local learned, money, potions, scheduled, marker
local originalPrint = print
print = function(value) marker = value end
Player = function()
    return {
        isRemoved = function() return false end,
        hasLearnedSpell = function(_, name) return name == 'Light Healing' and learned end,
        getMoney = function() return money end,
        getBankBalance = function() return 0 end,
        getItemCount = function(_, id) assert(id == 7618); return potions end,
    }
end
addEvent = function(callback, delay, id, attempts)
    assert(callback == F.verifyLowSupplySpellTraining and delay == 100 and id == 1 and attempts == 1)
    scheduled = true
end
learned, money, potions = false, 270, 2
F.verifyLowSupplySpellTraining(1, 2)
assert(scheduled and not marker)
assert(not pcall(F.verifyLowSupplySpellTraining, 1, 0), 'unlearned Exura passed')
learned, money = true, 100
F.verifyLowSupplySpellTraining(1, 0)
assert(marker == 'PLAYERBOT_GAMEPLAY_TEST SPELL_TRAINING_LOW_SUPPLIES_PASS')
for _, invalid in ipairs({{99, 2}, {101, 2}, {100, 1}, {100, 3}}) do
    money, potions = invalid[1], invalid[2]
    assert(not pcall(F.verifyLowSupplySpellTraining, 1, 0), 'incorrect payment/potion reserve passed')
end
print = originalPrint
print('Low-supply Exura live verifier contracts passed.')
