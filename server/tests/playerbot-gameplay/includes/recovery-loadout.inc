local F = PlayerbotGameplayFixture
local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE")
if mode ~= "recovery_fists" and mode ~= "recovery_melee" then return end

local meleeId = mode == "recovery_melee" and 2376 or 0
local quarry = "Playerbot Recovery Quarry"
local observation

-- One held spear cannot fund a complete productive outing at this deliberately
-- expensive break rate. The atlas reads the same registered rate as the engine.
-- Do not alter hit/damage rolls or any readiness/routing policy.
local spear = Weapon(WEAPON_DISTANCE)
spear:id(F.spearItemId)
spear:breakChance(100)
assert(spear:register(), "recovery fixture could not set spear consumption")

local function hand(player, slot)
    local item = player:getSlotItem(slot)
    return item and item:getId() or 0
end

local function evidence(player, phase, extra)
    print(string.format('{"component":"playerbot","event":"recovery_fixture","bot":"Bot One",' ..
        '"source":"lua_setup_verifier","scenario":"%s","phase":"%s","left_item_id":%d,' ..
        '"right_item_id":%d,"spears":%d,"melee_count":%d,"carried_gold":%d,"bank_gold":%d,' ..
        '"health":%d,"health_max":%d,"x":%d,"y":%d,"z":%d%s}',
        mode, phase, hand(player, CONST_SLOT_LEFT), hand(player, CONST_SLOT_RIGHT),
        player:getItemCount(F.spearItemId), player:getItemCount(2376),
        player:getMoney(), player:getBankBalance(), player:getHealth(), player:getMaxHealth(),
        player:getPosition().x, player:getPosition().y, player:getPosition().z, extra or ""))
end

F.recoveryHealth = CreatureEvent("PlayerbotRecoveryHealth")
function F.recoveryHealth.onHealthChange(creature, attacker, damage, damageType, secondary, secondaryType)
    if observation and not observation.done and creature:getName() == quarry and attacker and attacker:getName() == F.botName and
        damageType ~= COMBAT_HEALING and math.abs(damage) + math.abs(secondary) > 0 then
        -- Fail on the very first wrong-weapon hit, even if a later hit is correct.
        assert(hand(attacker, CONST_SLOT_LEFT) == meleeId and hand(attacker, CONST_SLOT_RIGHT) == 2525,
            "recovery quarry attacked before the chosen hands were physically equipped")
        assert(attacker:getItemCount(F.spearItemId) == 1, "recovery lost the protected spear")
        observation.hits = observation.hits + 1
        evidence(attacker, "hit", ',"target_id":' .. creature:getId())
    end
    return damage, damageType, secondary, secondaryType
end
F.recoveryHealth:register()

F.recoveryDeath = CreatureEvent("PlayerbotRecoveryDeath")
function F.recoveryDeath.onDeath(creature, corpse, killer)
    if observation and not observation.done and creature:getName() == quarry then
        assert(killer and killer:getName() == F.botName and observation.hits > 0 and corpse,
            "recovery quarry did not die through normal player combat")
        -- Normalize only the drop RNG, after the normal corpse is created.
        assert(creature:setDropLoot(false), "recovery fixture could not suppress random drops")
        for _, item in ipairs(corpse:getItems() or {}) do assert(item:remove()) end
        assert(corpse:addItem(ITEM_PLATINUM_COIN, 20), "recovery fixture could not seed corpse coins")
        observation.kills = observation.kills + 1
        evidence(killer, "death", ',"target_id":' .. creature:getId() .. ',"corpse_gold":2000')
    end
    return true
end
F.recoveryDeath:register()

function F.verifyRecoveryLoadout(playerId, attempts)
    local player = Player(playerId)
    assert(player and not player:isRemoved() and player:getHealth() > 0, "recovery player disappeared")
    if observation.kills > 0 and hand(player, CONST_SLOT_LEFT) == F.spearItemId then
        assert(player:getItemCount(F.spearItemId) >= 1 and hand(player, CONST_SLOT_RIGHT) == 2525 and
            player:getItemCount(2376) == (meleeId ~= 0 and 1 or 0), "restoration lost original or temporary gear")
        assert(player:getMoney() + player:getBankBalance() > 0, "restoration has no earned funds")
        assert(player:getPosition():getDistance(F.thaisDepotPosition) <= 12,
            "normal equipment restored before returning to the depot service boundary")
        observation.done = true
        evidence(player, "restored")
        print("PLAYERBOT_GAMEPLAY_TEST " .. mode:upper() .. "_PASS")
        return
    end
    assert(attempts > 0, "recovery failed to combat/loot/return/restore before fixture deadline")
    addEvent(F.verifyRecoveryLoadout, 100, playerId, attempts - 1)
end

function F.setupRecoveryLoadout(player)
    assert(player:getLevel() == 8 and player:setVocation(3) and player:setTown(Town(F.thaisTownId)) and
        player:teleportTo(F.thaisDepotPosition), "recovery fixture could not place level-eight Paladin")
    assert(player:setCapacity(100000) and player:setBankBalance(0))
    for _, slot in ipairs({CONST_SLOT_HEAD, CONST_SLOT_ARMOR, CONST_SLOT_LEFT, CONST_SLOT_RIGHT,
        CONST_SLOT_LEGS, CONST_SLOT_FEET, CONST_SLOT_AMMO, CONST_SLOT_NECKLACE, CONST_SLOT_RING}) do
        local item = player:getSlotItem(slot)
        if item then assert(item:remove()) end
    end
    local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
    assert(backpack, "recovery fixture needs the seeded backpack")
    F.clearBackpack(backpack)
    for _, equipment in ipairs({{2457, CONST_SLOT_HEAD}, {2463, CONST_SLOT_ARMOR},
        {2525, CONST_SLOT_RIGHT}, {F.spearItemId, CONST_SLOT_LEFT},
        {2647, CONST_SLOT_LEGS}, {2643, CONST_SLOT_FEET}}) do
        assert(player:addItem(equipment[1], 1, false, 1, equipment[2]))
    end
    F.selectHealthPotion(player)
    for _, item in ipairs({{F.ropeItemId, 1}, {F.shovelItemId, 1}, {F.healthPotionItemId, 20},
        {F.manaPotionItemId, 20}, {F.meatItemId, 2}}) do assert(backpack:addItem(item[1], item[2])) end
    if meleeId ~= 0 then assert(backpack:addItem(meleeId, 1)) end
    for _, skill in ipairs({SKILL_FIST, SKILL_CLUB, SKILL_SWORD, SKILL_AXE, SKILL_DISTANCE, SKILL_SHIELD}) do
        local level = skill == SKILL_DISTANCE or skill == SKILL_SHIELD or skill == SKILL_SWORD and meleeId ~= 0
            or skill == SKILL_FIST and meleeId == 0
        assert(player:addSkillLevel(skill, (level and 40 or 10) - player:getSkillLevel(skill)))
    end
    assert(player:setHealth(player:getMaxHealth()) and player:addMana(player:getMaxMana() - player:getMana()))
    player:removeCondition(CONDITION_REGENERATION)
    local food = Condition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
    food:setParameter(CONDITION_PARAM_TICKS, 1200000)
    assert(player:addCondition(food))
    for _, storage in ipairs({F.mainlandRewardStorage, F.economicRewardStorage,
        F.nestedRewardStorage, F.pickupRewardStorage}) do assert(player:setStorageValue(storage, 1)) end
    assert(player:getMoney() == 0 and player:getBankBalance() == 0 and player:getItemCount(F.spearItemId) == 1)
    assert(player:setStorageValue(F.fixtureReadyStorage, 1))
    observation = {hits = 0, kills = 0}
    evidence(player, "start")
    addEvent(F.verifyRecoveryLoadout, 100, player:getId(), 9000)
end
