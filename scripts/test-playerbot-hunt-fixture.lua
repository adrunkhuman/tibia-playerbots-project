-- Run from the repository root. Real fixture login/helpers; mocked player/world boundary.
local function read(path)
    local file = assert(io.open(path))
    local text = file:read('*a')
    file:close()
    return text
end
local seed = read('server/schema/insertPlayerbots.sql')
local columns, values = seed:match('INSERT INTO `players` %((.-)%)%s*SELECT%s*(.-)%s*FROM `accounts`')
assert(columns and values)
local names, seeded = {}, {}
for name in columns:gmatch('`([%w_]+)`') do names[#names + 1] = name end
local index = 0
for value in values:gmatch('([^,]+)') do
    index = index + 1
    seeded[names[index]] = tonumber(value:match('^%s*(.-)%s*$'))
end
local loadout = assert(seed:match('FROM %(%s*(SELECT 1 AS `pid`.-)%) AS `loadout`'))
local equipment = {}
local slot, id = loadout:match('SELECT (%d+) AS `pid`, %d+ AS `offset`, (%d+) AS `itemtype`')
equipment[tonumber(slot)] = tonumber(id)
for s, item in loadout:gmatch('UNION ALL SELECT (%d+), %d+, (%d+), 1') do equipment[tonumber(s)] = tonumber(item) end
assert(seeded.level == 8 and seeded.skill_sword == 20 and seeded.skill_shielding == 20)

Position = function(x, y, z) return {x = x, y = y, z = z} end
CONST_SLOT_HEAD, CONST_SLOT_NECKLACE, CONST_SLOT_BACKPACK, CONST_SLOT_ARMOR = 1, 2, 3, 4
CONST_SLOT_RIGHT, CONST_SLOT_LEFT, CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_RING, CONST_SLOT_AMMO = 5, 6, 7, 8, 9, 10
SKILL_FIST, SKILL_CLUB, SKILL_SWORD, SKILL_AXE, SKILL_DISTANCE, SKILL_SHIELD, SKILL_FISHING = 0, 1, 2, 3, 4, 5, 6
ITEM_GOLD_COIN = 2148
CreatureEvent = function() return {register = function() end} end
Town = function(id) assert(id == 4); return {} end
local experienceByLevel = {[8] = seeded.experience, [15] = 37800}
Game = {getExperienceForLevel = function(level) return assert(experienceByLevel[level]) end}
PlayerbotGameplayFixture = {}
local root = 'server/tests/playerbot-gameplay/includes/'
dofile(root .. 'constants.inc')
dofile(root .. 'helpers.inc')
dofile(root .. 'login.inc')
local F = PlayerbotGameplayFixture
local mode
os.getenv = function(name) assert(name == 'PLAYERBOT_GAMEPLAY_MODE'); return mode end
local output, suppressed
local originalPrint = print
print = function(line) output[#output + 1] = line end
F.suppressNearbyMonsters = function(id) assert(id == 3); suppressed = suppressed + 1 end

local function login(selectedMode, fault, starter)
    mode, output, suppressed = selectedMode, {}, 0
    local slots, inventory, skills, tries = {}, {[2120] = 1, [2554] = 1, [7618] = 3, [8704] = 1}, {}, {}
    for skill = 0, 6 do skills[skill], tries[skill] = 10, 0 end
    -- Normalize both undertrained and overtrained characters to the seed, not better.
    skills[SKILL_SWORD], skills[SKILL_SHIELD], tries[SKILL_SHIELD] = 30, 11, 42
    local function equip(s, item)
        slots[s] = {getId = function() return item end, remove = function() slots[s] = nil; return true end}
    end
    for s, item in pairs(equipment) do equip(s, item) end
    if starter then equip(4, F.starterArmorId); equip(6, F.starterWeaponId) end
    equip(3, 1988)
    equip(CONST_SLOT_AMMO, 2050) -- firstitems.lua runs before the fixture
    if fault == 'tool' then inventory[2120] = 0 end
    local vocation, health, position = 4, 1, nil
    local level, experience = seeded.level, seeded.experience
    local player = {
        getName = function() return F.botName end,
        getId = function() return 3 end,
        getLevel = function() return level end,
        getExperience = function() return experience end,
        addExperience = function(_, amount)
            experience = experience + amount
            if experience >= experienceByLevel[15] then level = 15 end
        end,
        getVocation = function() return {getId = function() return vocation end} end,
        setVocation = function(_, value) assert(value == 4, 'hunt fixture restored Rookgaard'); vocation = value; return true end,
        setTown = function() return true end,
        setMaxHealth = function(_, value) assert(value == seeded.healthmax); return true end,
        setHealth = function(_, value) health = value; return true end,
        teleportTo = function(_, p) position = p; return true end,
        getSlotItem = function(_, s) return slots[s] end,
        getItemCount = function(_, item) return inventory[item] or 0 end,
        removeItem = function(_, item, count) inventory[item] = inventory[item] - count; return true end,
        addItem = function(_, item, count, drop, subtype, s)
            if s then
                assert(count == 1 and drop == false and subtype == 1)
                if fault ~= 'unequipped' then equip(s, item) end
            else
                assert(item == 7618 and count == 10, 'hunt fixture added extra supplies')
                inventory[item] = (inventory[item] or 0) + count
            end
            return true
        end,
        getSkillLevel = function(_, skill) return skills[skill] end,
        getSkillTries = function(_, skill) return tries[skill] end,
        addSkillLevel = function(_, skill, delta)
            if fault == 'skill_write' then return false end
            skills[skill] = skills[skill] + delta + (fault == 'skill_verification' and 1 or 0)
            tries[skill] = 0
            return true
        end,
        getMagicLevel = function() return seeded.maglevel end,
        getMaxMana = function() return seeded.manamax end,
        getCapacity = function() return seeded.cap * 100 end,
        getMoney = function() return 17 end, -- no money mutation API: fixture must preserve funds
        getBankBalance = function() return seeded.balance end,
        setStamina = function(_, minutes)
            assert(minutes == ({stamina_bonus = 2520, stamina_boundary = 2401, stamina_normal = 2400})[mode])
            if fault == 'stamina_write' then return false end
            return true
        end,
    }
    local ok, reason = pcall(F.login.onLogin, player)
    if fault then assert(not ok, fault .. ' was accepted'); return tostring(reason) end
    assert(ok and reason == true, tostring(reason))
    for s, item in pairs(equipment) do assert(slots[s] and slots[s]:getId() == item, 'fixture differs from SQL loadout') end
    for skill = 0, 6 do
        local wanted = skill == SKILL_SWORD and seeded.skill_sword or skill == SKILL_SHIELD and seeded.skill_shielding or 10
        assert(skills[skill] == wanted and tries[skill] == 0)
    end
    assert(health == seeded.health and position.x == seeded.posx and position.y == seeded.posy and position.z == seeded.posz)
    assert(level == (mode == 'remote_hunt' and 15 or 8))
    assert(inventory[7618] == 10 and inventory[8704] == 0)
    local stamina = ({stamina_bonus = 2520, stamina_boundary = 2401, stamina_normal = 2400})[mode]
    assert(suppressed == ((mode == 'hunt_planning' or stamina) and 1 or 0))
    assert(output[1]:find('HUNT_MAINLAND_LOADOUT_PASS ' .. mode, 1, true))
    local start = stamina and 'STAMINA_PROJECTION_START ' .. stamina or
        mode == 'hunt_planning' and 'HUNT_PLANNING_START' or
        mode == 'hunt_area_arrival' and 'HUNT_AREA_ARRIVAL_START' or 'REMOTE_HUNT_START'
    assert(output[2] == 'PLAYERBOT_GAMEPLAY_TEST ' .. start)
end
for _, selectedMode in ipairs({'hunt_planning', 'hunt_area_arrival', 'remote_hunt',
    'stamina_bonus', 'stamina_boundary', 'stamina_normal'}) do
    login(selectedMode, nil, false)
    login(selectedMode, nil, true)
    assert(login(selectedMode, 'unequipped'):find('did not equip seeded item', 1, true))
    assert(login(selectedMode, 'skill_write'):find('could not restore seeded skill', 1, true))
    assert(login(selectedMode, 'skill_verification'):find('wrong seeded skill', 1, true))
    assert(login(selectedMode, 'tool'):find('seeded backpack or tools', 1, true))
    if selectedMode:find('stamina_', 1, true) then
        assert(login(selectedMode, 'stamina_write'):find('could not set stamina', 1, true))
    end
end
print = originalPrint
print('Hunt and stamina fixtures seeded mainland loadout/skills, funds/supplies, suppression and failure contracts passed.')
