-- Run from the repository root: luajit scripts/test-maealil-healing-keywords.lua
-- Lua 5.1/5.4 also work; existing modules.lua is not compatible with Lua 5.5.
-- Load production routing/callbacks; stub only server/player/NPC boundaries.
CONDITION_FIRE, CONDITION_POISON, CONDITION_ENERGY = 1, 2, 3
CONST_ME_MAGIC_GREEN, CONST_ME_MAGIC_RED = 1, 2
MESSAGE_GREET, MESSAGE_WALKAWAY, MESSAGE_FAREWELL = 1, 2, 3
TAG_PLAYERNAME = '|PLAYERNAME|'

function table.contains(values, expected)
    for _, value in ipairs(values) do
        if value == expected then return true end
    end
    return false
end

local player = { health = 100, conditions = {}, effects = {}, money = 2000, learned = {} }
function Player(cid)
    assert(cid == 1)
    return player
end
function player:getName() return 'Test Player' end
function player:getCondition(condition) return self.conditions[condition] end
function player:removeCondition(condition) self.conditions[condition] = nil end
function player:getHealth() return self.health end
function player:addHealth(amount) self.health = self.health + amount end
function player:getPosition()
    return { sendMagicEffect = function(_, effect)
        self.effects[#self.effects + 1] = effect
    end }
end
function player:getVocation()
    return { getBase = function() return { getId = function() return 3 end } end }
end
function player:hasLearnedSpell(name) return self.learned[name] == true end
function player:canLearnSpell() return true end
function player:isPremium() return true end
function player:removeTotalMoney(amount)
    assert(self.money >= amount)
    self.money = self.money - amount
    return true
end
function player:learnSpell(name) self.learned[name] = true end

local npc = { addSpellOffer = function() end }
function Npc() return npc end
local handler
NpcHandler = {}
function NpcHandler:new(keywordHandler)
    handler = setmetatable({ keywordHandler = keywordHandler }, { __index = self })
    return handler
end
function NpcHandler:isFocused(cid) return cid == 1 end
function NpcHandler:parseMessage(text) return text end
function NpcHandler:say(text, cid)
    assert(cid == 1)
    self.response = text
end
function NpcHandler:resetNpc(cid) self.keywordHandler:reset(cid) end
function NpcHandler:setMessage() end
function NpcHandler:addModule(module) module:init(self) end
NpcSystem = { parseParameters = function() end }

dofile('server/data/npc/lib/npcsystem/keywordhandler.lua')
dofile('server/data/npc/lib/npcsystem/modules.lua')
dofile('server/data/npc/lib/npcsystem/customModules.lua')
dofile('server/data/npc/scripts/Maealil.lua')

local function reset(health, conditions)
    handler:resetNpc(1)
    handler.response = nil
    player.health, player.conditions = health, conditions or {}
    player.effects, player.learned, player.money = {}, {}, 2000
end
local function request(message)
    assert(handler.keywordHandler:processMessage(1, message), 'No keyword matched: ' .. message)
    return handler.keywordHandler:getLastNode(1)
end

-- A free-heal condition must not steal a spell request, even before the fallback.
local states = {
    { health = 100 },
    { health = 20 },
    { health = 100, condition = CONDITION_FIRE },
    { health = 100, condition = CONDITION_POISON },
    { health = 100, condition = CONDITION_ENERGY },
}
local spells = { { 'Light Healing', 170 }, { 'Intense Healing', 350 }, { 'Ultimate Healing', 1000 } }
for _, state in ipairs(states) do
    for _, spell in ipairs(spells) do
        reset(state.health, state.condition and { [state.condition] = true })
        local node = request(spell[1])
        assert(node.parameters.spellName == spell[1], 'Free healing intercepted ' .. spell[1])
        assert(node.callback == StdModule.say)
        assert(handler.response == 'Do you want to learn the spell ' .. spell[1] .. ' for ' .. spell[2] .. ' gold?')
        assert(player.health == state.health and #player.effects == 0)
        if state.condition then assert(player.conditions[state.condition]) end
        request('yes')
        assert(player.learned[spell[1]] and player.money == 2000 - spell[2])
        assert(handler.response == 'You have learned ' .. spell[1] .. '.')
    end
end

for _, phrase in ipairs({ 'heal', 'please heal me', 'HEAL!', 'please (heal) me' }) do
    reset(100)
    local node = request(phrase)
    assert(not node.parameters.spellName)
    assert(handler.response:find("You aren't looking that bad.", 1, true) == 1)
    assert(player.health == 100 and #player.effects == 0 and player.money == 2000)

    reset(20)
    request(phrase)
    assert(handler.response == 'You are hurt, my child. I will heal your wounds.')
    assert(player.health == 40 and #player.effects == 1 and player.effects[1] == CONST_ME_MAGIC_GREEN)

    for _, condition in ipairs({ CONDITION_FIRE, CONDITION_POISON, CONDITION_ENERGY }) do
        reset(20, { [condition] = true })
        request(phrase)
        assert(player.conditions[condition] == nil and player.health == 20)
        assert(#player.effects == 1)
        assert(player.effects[1] == (condition == CONDITION_POISON and CONST_ME_MAGIC_RED or CONST_ME_MAGIC_GREEN))
        local expected = {
            [CONDITION_FIRE] = 'You are burning. Let me quench those flames.',
            [CONDITION_POISON] = 'You are poisoned. Let me soothe your pain.',
            [CONDITION_ENERGY] = 'You are electrified, my child. Let me help you to stop trembling.',
        }
        assert(handler.response == expected[condition])
    end
end

-- Preserve condition priority and the strict health threshold.
reset(20, { [CONDITION_FIRE] = true, [CONDITION_POISON] = true })
request('heal')
assert(not player.conditions[CONDITION_FIRE] and player.conditions[CONDITION_POISON])
request('heal')
assert(not player.conditions[CONDITION_POISON] and player.health == 20)
request('heal')
assert(player.health == 40)
request('heal')
assert(handler.response:find("You aren't looking that bad.", 1, true) == 1)

for _, phrase in ipairs({ 'healing', 'unheal', 'healer', 'prehealpost' }) do
    reset(20, { [CONDITION_FIRE] = true })
    assert(not handler.keywordHandler:processMessage(1, phrase), 'Substring matched: ' .. phrase)
    assert(handler.response == nil and player.health == 20 and player.conditions[CONDITION_FIRE])
end
print('PASS Maealil: loaded spell routing/payment, whole-word free healing, fire/poison/energy priority and low-HP threshold')
