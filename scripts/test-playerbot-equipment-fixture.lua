-- Run from the repository root: lua scripts/test-playerbot-equipment-fixture.lua
-- Optional first argument: alternate login.inc (for checking an unfixed source).
-- Real login/constants/helpers; only player/world APIs are doubled. Events are not run.
local function read(path)
    local file = assert(io.open(path))
    local source = file:read('*a')
    file:close()
    return source
end
local potionTarget = assert(tonumber(read('server/src/playerbotinventorypolicy.h'):
    match('healthPotionAmmoTarget%s*=%s*(%d+)')), 'missing production potion stock target')
local weights = {}
for id, body in read('server/data/items/items.xml'):gmatch('<item id="(%d+)"[^/>]*>(.-)</item>') do
    weights[tonumber(id)] = tonumber(body:match('<attribute key="weight" value="(%d+)"'))
end

Position = function(x, y, z) return {x = x, y = y, z = z} end
CONST_SLOT_HEAD, CONST_SLOT_BACKPACK, CONST_SLOT_ARMOR = 1, 3, 4
CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET = 5, 6, 7, 8
ITEM_GOLD_COIN, ITEM_BACKPACK, ITEM_BAG = 2148, 1988, 1987
CreatureEvent = function() return {register = function() end} end
PlayerbotGameplayFixture = {}
local root = 'server/tests/playerbot-gameplay/includes/'
dofile(root .. 'constants.inc')
dofile(root .. 'helpers.inc')
dofile(root .. 'verifiers.inc')
dofile(arg[1] or (root .. 'login.inc'))
local F = PlayerbotGameplayFixture
local mode, player, events, marker
os.getenv = function(key) assert(key == 'PLAYERBOT_GAMEPLAY_MODE'); return mode end
Player = function(id) assert(id == 3); return player end
Game = {getSpectators = function() return {} end}
local thaisPosition = Position(32369, 32241, 7)
Town = function(id)
    assert(id == F.thaisTownId)
    return {getId = function() return id end, getTemplePosition = function() return thaisPosition end}
end
addEvent = function(callback, delay, ...)
    assert(type(callback) == 'function', 'fixture scheduled an unavailable callback')
    events[#events + 1] = {callback, delay, ...}
end
local originalPrint = print
print = function(line) marker = line end

local function newPlayer()
    local p = {slots = {}, inventory = {[2148] = 17, [7618] = 3, [2120] = 1, [2554] = 1,
        [F.starterWeaponId] = 2, [F.equipmentPurchaseItemId] = 1, [F.movedProviderPurchaseItemId] = 1},
        bank = 999, capacity = 47000, town = F.carlinTownId, position = F.carlinTemplePosition,
        spells = {}, storage = {}}
    local function writable() assert(not p.persisted, 'restart reseeded player state') end
    local function equip(slot, id)
        local item = {getId = function() return id end}
        item.remove = function() writable(); p.slots[slot] = nil; return true end
        p.slots[slot] = item
        return item
    end
    for slot, id in pairs({[1] = 2480, [4] = F.seedArmorId, [5] = F.seedShieldId,
        [6] = F.seedWeaponId, [7] = 2468, [8] = F.seedBootsId}) do equip(slot, id) end
    local backpack = equip(CONST_SLOT_BACKPACK, ITEM_BACKPACK)
    function backpack:getSize()
        local size = 0
        for id, count in pairs(p.inventory) do
            -- Coin/potion stacks; each filler bag occupies its own slot.
            if count > 0 then size = size + (id == ITEM_BAG and count or math.ceil(count / 100)) end
        end
        return size
    end
    function backpack:getCapacity() return 20 end
    function backpack:addItem(id, count) return p:addItem(id, count) end
    function p:getName() return F.botName end
    function p:getId() return 3 end
    function p:isRemoved() return false end
    function p:getLevel() return 8 end
    function p:getVocation() return {getId = function() return 4 end} end
    function p:setTown(town) self.town = town:getId(); return true end
    function p:getPosition() return self.position end
    function p:teleportTo(position) self.position = position; return true end
    function p:getSlotItem(slot) return self.slots[slot] end
    function p:getItemCount(id)
        local count = self.inventory[id] or 0
        for _, item in pairs(self.slots) do if item:getId() == id then count = count + 1 end end
        return count
    end
    function p:removeItem(id, count)
        writable()
        assert(count == self:getItemCount(id))
        self.inventory[id] = nil
        for slot, item in pairs(self.slots) do if item:getId() == id then self.slots[slot] = nil end end
        return true
    end
    function p:addItem(id, count, drop, subtype, slot)
        writable()
        if slot then
            assert(count == 1 and drop == false and subtype == 1 and not self.slots[slot])
            return equip(slot, id)
        end
        self.inventory[id] = (self.inventory[id] or 0) + count
        return true
    end
    function p:getMoney() return (self.inventory[2148] or 0) + 100 * (self.inventory[2152] or 0) end
    function p:getBankBalance() return self.bank end
    function p:setBankBalance(value) writable(); self.bank = value; return true end
    function p:setCapacity(value) writable(); self.capacity = value; return true end
    function p:getFreeCapacity()
        local weight = 0
        for id, count in pairs(self.inventory) do weight = weight + assert(weights[id], 'missing item weight') * count end
        for _, item in pairs(self.slots) do weight = weight + assert(weights[item:getId()], 'missing equipped weight') end
        return math.max(0, self.capacity - weight)
    end
    function p:hasLearnedSpell(name) return self.spells[name] end
    function p:learnSpell(name) writable(); self.spells[name] = true; return true end
    function p:setStorageValue(key, value) self.storage[key] = value; return true end
    return p, equip
end

local function login(selectedMode, p)
    mode, player, events, marker = selectedMode, p, {}, nil
    assert(F.login.onLogin(p) == true)
end
local function scheduled(callback, delay, ...)
    local expected = {callback, delay, ...}
    for _, event in ipairs(events) do
        if event[1] == callback then
            assert(#event == #expected, 'unexpected event arguments')
            for i, value in ipairs(expected) do assert(event[i] == value, 'incorrect event argument ' .. i) end
            return
        end
    end
    error('expected fixture event was not scheduled')
end

for _, selectedMode in ipairs({'equipment_buy', 'equipment_buy_resume', 'equipment_buy_rejected',
    'equipment_buy_space', 'equipment_buy_provider_unreachable', 'equipment_buy_provider_moved',
    'equipment_shadow', 'equipment_shadow_unaffordable', 'equipment_shadow_no_upgrade'}) do
    local p = newPlayer()
    login(selectedMode, p)
    local moved = selectedMode == 'equipment_buy_provider_moved'
    local shadow = selectedMode == 'equipment_shadow' or selectedMode == 'equipment_shadow_no_upgrade'
    local resume = selectedMode == 'equipment_buy_resume'
    local space = selectedMode == 'equipment_buy_space'
    local carried = moved and 80 or shadow and 2017 or
        (resume or selectedMode == 'equipment_shadow_unaffordable') and 0 or 5
    assert(p:getItemCount(F.healthPotionItemId) == potionTarget,
        selectedMode .. ': potions must meet the production stock target of ' .. potionTarget)
    assert(p.bank == 100 and p:getMoney() == carried, selectedMode .. ': incorrect bounded economy')
    assert(p.town == (moved and F.carlinTownId or F.thaisTownId), selectedMode .. ': wrong town')
    assert(p.position == (moved and F.carlinTemplePosition or thaisPosition), selectedMode .. ': wrong placement')
    if moved or selectedMode == 'equipment_shadow_unaffordable' then
        local upgrade = moved and F.movedProviderPurchaseItemId or 2386
        assert(p:getFreeCapacity() >= assert(weights[upgrade]) + 3000,
            selectedMode .. ': fixture lacks capacity for an upgrade plus the 3000 reserve with full potions')
    else
        assert(p.capacity == (space and 100000 or 47000), selectedMode .. ': changed unrelated capacity')
    end
    assert(p:getItemCount(F.equipmentPurchaseItemId) == (resume and 1 or 0), 'incorrect persisted purchase seed')
    assert(p:getSlotItem(CONST_SLOT_LEFT):getId() == (moved and 2376 or
        selectedMode == 'equipment_shadow_no_upgrade' and 2377 or F.starterWeaponId))
    assert(p:getSlotItem(CONST_SLOT_ARMOR):getId() == (space and F.starterArmorId or 2463))
    assert(p.storage[F.fixtureReadyStorage] == 1 and marker == 'PLAYERBOT_GAMEPLAY_TEST ' .. selectedMode:upper() .. '_START')
    scheduled(F.suppressNearbyMonsters, 100, 3)
    if moved then
        assert(p:getSlotItem(CONST_SLOT_LEGS):getId() == 2468)
        assert(p.spells['Find Person'] and p.spells['Light'])
        scheduled(F.verifyMovedProviderPurchase, 500, 3, 360)
        scheduled(F.moveEquipmentProvider, 100, 3, p.position.x, p.position.y, p.position.z, 0)
    elseif space then
        local backpack = p:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack:getSize() == backpack:getCapacity() - 1, 'space fixture must leave exactly one slot')
        scheduled(F.verifyEquipmentPurchaseSpace, 500, 3)
    elseif selectedMode == 'equipment_buy_provider_unreachable' then
        assert(#events == 1, 'unreachable provider must not schedule a successful-purchase verifier')
    elseif selectedMode:find('equipment_shadow', 1, true) then
        local right = p:getSlotItem(CONST_SLOT_RIGHT)
        scheduled(F.verifyEquipmentShadow, 250, 3, selectedMode, carried, p:getSlotItem(CONST_SLOT_LEFT):getId(),
            right and right:getId() or 0, p:getSlotItem(CONST_SLOT_ARMOR):getId(), p.position)
    else
        scheduled(F.verifyEquipmentPurchase, 500, 3, selectedMode == 'equipment_buy_rejected', resume, 360)
    end
end

-- Restart may restore location/readiness, but must not rewrite any persisted possessions.
for _, purchaseSlot in ipairs({CONST_SLOT_LEFT, CONST_SLOT_RIGHT}) do
    local p, equip = newPlayer()
    p.inventory = {[F.starterWeaponId] = 1, [2148] = 17, [7618] = 7, [2120] = 1, [2554] = 1}
    local purchased = equip(purchaseSlot, F.equipmentPurchaseItemId)
    p.bank, p.capacity, p.persisted = 83, 43210, true
    login('equipment_buy', p)
    assert(p:getSlotItem(purchaseSlot) == purchased and p:getItemCount(F.starterWeaponId) == 1)
    assert(p:getMoney() == 17 and p.bank == 83 and p:getItemCount(F.healthPotionItemId) == 7 and p.capacity == 43210)
    assert(#events == 0 and p.storage[F.fixtureReadyStorage] == 2)
    assert(marker == 'PLAYERBOT_GAMEPLAY_TEST EQUIPMENT_BUY_RESTART_PASS')
end
print = originalPrint
print('PASS equipment fixture: nine modes, full potions, exact economy, placement/capacity, verifier scheduling and both purchase-restart slots')
