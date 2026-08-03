local botName = "Bot One"
local storageKey = 45016
local modeValues = {
    interactions = 86016,
    death = 86017,
    remove = 86018,
    spellReset = 86022,
    spellLearning = 86019,
    spellPersistence = 86020,
    spellTrainer = 86021,
    spellFailures = 86023,
}

local function pass(mode)
    print("PLAYERBOT_CONNECTIONLESS_TEST PASS mode=" .. mode)
end

local function getBot(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One is unavailable")
    return player
end

local function setExperience(player, experience)
    local current = player:getExperience()
    if current < experience then
        player:addExperience(experience - current)
    elseif current > experience then
        player:removeExperience(current - experience)
    end
    assert(player:getExperience() == experience, "test experience could not be set")
end

local function setLevel(player, level)
    setExperience(player, Game.getExperienceForLevel(level))
    assert(player:getLevel() == level, "test level could not be set")
end

local function getTotalMoney(player)
    return player:getMoney() + player:getBankBalance()
end

local function clearMoney(player)
    local total = getTotalMoney(player)
    if total > 0 then
        assert(player:removeTotalMoney(total), "test money could not be cleared")
    end
    assert(getTotalMoney(player) == 0, "test money remained after clearing")
end

local function hasCastableSpell(player, spellName)
    for _, spell in ipairs(player:getInstantSpells()) do
        if spell.name == spellName then
            return true
        end
    end
    return false
end

local login = CreatureEvent("zzPlayerbotConnectionlessRegression")

function login.onLogin(player)
    if player:getName() ~= botName then
        return true
    end

    local mode = os.getenv("PLAYERBOT_REGRESSION_MODE") or "interactions"
    if mode == "reject" then
        pass(mode)
        return false
    end

    assert(modeValues[mode], "unknown PLAYERBOT_REGRESSION_MODE: " .. mode)
    assert(player:getClient().version == 0, "Bot One unexpectedly has a protocol client")
    assert(player:setStorageValue(storageKey, modeValues[mode]))

    if mode == "interactions" then
        assert(player:teleportTo(Position(32097, 32219, 7)), "test position could not be restored")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack and backpack:getId() == ITEM_BACKPACK, "seeded backpack is missing")
        assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == 2650, "seeded starter jacket is missing")
        assert(not player:getSlotItem(CONST_SLOT_RIGHT), "starter shield slot is not empty")
        assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == 2382, "seeded starter club is missing")
        assert(not player:getSlotItem(CONST_SLOT_FEET), "starter feet slot is not empty")
        assert(player:getItemCount(2120) == 1, "seeded rope is missing")
        assert(player:getItemCount(2554) == 1, "seeded shovel is missing")
        player:removeCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        local cheeseCount = player:getItemCount(2696)
        if cheeseCount > 0 then
            assert(player:removeItem(2696, cheeseCount), "existing cheese could not be removed")
        end
        local foodContainer = backpack:addItem(ITEM_BACKPACK, 1)
        assert(foodContainer, "test food container could not be added")
        for _ = 1, 20 do
            assert(foodContainer:addItem(2696, 1), "test cheese could not be added")
        end
        assert(player:sendTextMessage(MESSAGE_STATUS_DEFAULT, "connectionless regression"))
        assert(player:sendOutfitWindow())
        assert(player:showTextDialog(1950, "connectionless regression"))
        assert(player:sendTutorial(1))
        assert(player:addMapMark(player:getPosition(), 0, "connectionless regression"))
        assert(player:popupFYI("connectionless regression"))

        local message = NetworkMessage()
        message:addByte(0)
        assert(message:sendToPlayer(player))

        local bag = player:addItem(ITEM_BAG, 1, false)
        assert(bag, "could not add temporary bag")
        assert(bag:addItem(2148, 1), "could not add temporary bag content")
        assert(bag:getSize() == 1, "temporary bag content is missing")
        assert(bag:remove(), "could not remove temporary bag")

        pass(mode)
        return true
    end

    if mode == "spellReset" then
        local playerId = player:getId()
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:setVocation(4), "spell reset could not select Knight vocation")
            setLevel(bot, 8)
            bot:forgetSpell("Light")
            assert(not bot:hasLearnedSpell("Light"), "Light remained learned after reset")
            assert(not hasCastableSpell(bot, "Light"), "unlearned Light remained castable")
            pass(mode)
        end, 100)
        return true
    elseif mode == "spellLearning" then
        assert(not player:hasLearnedSpell("Light"), "Light was already learned before the learning phase")
        assert(player:canLearnSpell("Light"), "level 8 Knight cannot learn Light")
        assert(not hasCastableSpell(player, "Light"), "Light was castable before training")
        assert(player:addMoney(100), "Light training money could not be added")
        local moneyBefore = getTotalMoney(player)
        local gregor = Npc("Gregor")
        assert(gregor, "Gregor is unavailable")
        local gregorPosition = gregor:getPosition()
        local testPosition = Position(gregorPosition.x - 1, gregorPosition.y, gregorPosition.z)
        local playerId = player:getId()
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:teleportTo(testPosition), "Gregor learning position could not be reached")
            assert(bot:say("hi", TALKTYPE_PRIVATE_PN, false, gregor), "Gregor learning greeting failed")
        end, 100)
        addEvent(function()
            assert(getBot(playerId):say("light", TALKTYPE_PRIVATE_PN, false, gregor), "Light learning request failed")
        end, 200)
        addEvent(function()
            assert(getBot(playerId):say("yes", TALKTYPE_PRIVATE_PN, false, gregor), "Light learning confirmation failed")
        end, 300)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Light"), "Gregor did not teach Light through normal dialogue")
            assert(hasCastableSpell(bot, "Light"), "Light remained uncastable after training")
            assert(getTotalMoney(bot) == moneyBefore - 100, "Gregor did not charge exactly 100 gold for Light")
            pass(mode)
        end, 400)
        return true
    elseif mode == "spellPersistence" then
        assert(player:hasLearnedSpell("Light"), "learned Light did not persist")
        assert(hasCastableSpell(player, "Light"), "persisted Light is not castable")
        assert(player:addMoney(100), "duplicate test money could not be added")
        local moneyBefore = getTotalMoney(player)
        local gregor = Npc("Gregor")
        assert(gregor, "Gregor is unavailable")
        local gregorPosition = gregor:getPosition()
        local testPosition = Position(gregorPosition.x - 1, gregorPosition.y, gregorPosition.z)
        local playerId = player:getId()
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:teleportTo(testPosition), "Gregor duplicate position could not be reached")
            assert(bot:say("hi", TALKTYPE_PRIVATE_PN, false, gregor), "Gregor duplicate greeting failed")
        end, 100)
        addEvent(function()
            assert(getBot(playerId):say("light", TALKTYPE_PRIVATE_PN, false, gregor), "duplicate Light request failed")
        end, 200)
        addEvent(function()
            assert(getBot(playerId):say("yes", TALKTYPE_PRIVATE_PN, false, gregor), "duplicate Light confirmation failed")
        end, 300)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Light"), "duplicate purchase removed learned Light")
            assert(getTotalMoney(bot) == moneyBefore, "duplicate purchase changed money")
            assert(bot:removeMoney(100), "duplicate test money could not be removed")
            pass(mode)
        end, 400)
        return true
    elseif mode == "spellTrainer" then
        player:forgetSpell("Light")
        player:forgetSpell("Light Healing")
        assert(player:addMoney(270), "test spell money could not be added")
        local moneyBefore = player:getMoney()
        local gregor = Npc("Gregor")
        assert(gregor, "Gregor is unavailable")
        local gregorPosition = gregor:getPosition()
        local testPosition = Position(gregorPosition.x - 1, gregorPosition.y, gregorPosition.z)
        local playerId = player:getId()
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:setVocation(4), "Gregor trainer test could not select Knight vocation")
            setLevel(bot, 9)
            assert(bot:teleportTo(testPosition), "Gregor test position could not be reached")
        end, 50)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:teleportTo(testPosition), "Gregor test position could not be restored")
            assert(bot:say("hi", TALKTYPE_PRIVATE_PN, false, gregor), "Gregor greeting failed")
        end, 100)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:teleportTo(testPosition), "Gregor test position could not be restored")
            assert(bot:say("light healing", TALKTYPE_PRIVATE_PN, false, gregor), "Light Healing request failed")
        end, 200)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:teleportTo(testPosition), "Gregor test position could not be restored")
            assert(bot:say("yes", TALKTYPE_PRIVATE_PN, false, gregor), "Light Healing confirmation failed")
        end, 300)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Light Healing"), "Gregor did not teach Light Healing")
            assert(not bot:hasLearnedSpell("Light"), "Light Healing request matched Light")
            assert(bot:getMoney() == moneyBefore - 170, "Gregor did not charge exactly 170 gold for Light Healing")
            assert(bot:say("hi", TALKTYPE_PRIVATE_PN, false, gregor), "second Gregor greeting failed")
        end, 400)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:say("light", TALKTYPE_PRIVATE_PN, false, gregor), "Light request failed")
        end, 500)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:say("yes", TALKTYPE_PRIVATE_PN, false, gregor), "Light confirmation failed")
        end, 600)
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Light"), "Gregor did not teach Light")
            assert(bot:getMoney() == moneyBefore - 270, "Gregor did not charge exactly 100 gold for Light")
            pass(mode)
        end, 700)
        return true
    elseif mode == "spellFailures" then
        local originalExperience = player:getExperience()
        local originalVocation = player:getVocation():getId()
        local originalPremiumEndsAt = player:getPremiumEndsAt()
        local originalInventoryMoney = player:getMoney()
        local originalBankMoney = player:getBankBalance()
        local testedSpells = {"Light", "Light Healing", "Whirlwind Throw", "Challenge"}
        local originallyLearned = {}
        for _, spellName in ipairs(testedSpells) do
            originallyLearned[spellName] = player:hasLearnedSpell(spellName)
            player:forgetSpell(spellName)
        end
        clearMoney(player)

        local gregor = Npc("Gregor")
        local puffels = Npc("Puffels")
        local eremo = Npc("Eremo")
        assert(gregor and puffels and eremo, "spell failure trainer is unavailable")
        local positions = {
            Gregor = Position(gregor:getPosition().x - 1, gregor:getPosition().y, gregor:getPosition().z),
            Puffels = Position(puffels:getPosition().x - 1, puffels:getPosition().y, puffels:getPosition().z),
            Eremo = Position(eremo:getPosition().x - 1, eremo:getPosition().y, eremo:getPosition().z),
        }
        local playerId = player:getId()
        local delay = 100
        local function later(callback)
            addEvent(callback, delay)
            delay = delay + 100
        end
        local function sayTo(npc, position, message)
            later(function()
                local bot = getBot(playerId)
                assert(bot:teleportTo(position), "spell failure trainer position could not be reached")
                assert(bot:say(message, TALKTYPE_PRIVATE_PN, false, npc), "spell failure dialogue failed: " .. message)
            end)
        end

        later(function()
            local bot = getBot(playerId)
            assert(bot:setVocation(2), "vocation failure could not select Druid")
            setLevel(bot, 8)
            assert(bot:setPremiumEndsAt(0), "vocation failure could not remove premium")
            assert(bot:addMoney(100), "vocation failure money could not be added")
        end)
        sayTo(gregor, positions.Gregor, "hi")
        sayTo(gregor, positions.Gregor, "light")
        sayTo(gregor, positions.Gregor, "yes")
        later(function()
            local bot = getBot(playerId)
            assert(not bot:hasLearnedSpell("Light"), "wrong vocation learned Light from Gregor")
            assert(getTotalMoney(bot) == 100, "vocation rejection changed money")
            assert(bot:say("bye", TALKTYPE_PRIVATE_PN, false, gregor), "Gregor vocation test farewell failed")
            clearMoney(bot)
        end)

        later(function()
            local bot = getBot(playerId)
            assert(bot:setVocation(4), "level failure could not select Knight")
            setLevel(bot, 8)
            assert(bot:addMoney(170), "level failure money could not be added")
        end)
        sayTo(gregor, positions.Gregor, "hi")
        sayTo(gregor, positions.Gregor, "light healing")
        sayTo(gregor, positions.Gregor, "yes")
        later(function()
            local bot = getBot(playerId)
            assert(not bot:hasLearnedSpell("Light Healing"), "under-level Knight learned Light Healing")
            assert(getTotalMoney(bot) == 170, "level rejection changed money")
            clearMoney(bot)
        end)

        later(function()
            local bot = getBot(playerId)
            setLevel(bot, 15)
            assert(bot:setPremiumEndsAt(0), "premium failure could not remove premium")
            assert(bot:addMoney(800), "premium failure money could not be added")
        end)
        sayTo(puffels, positions.Puffels, "hi")
        sayTo(puffels, positions.Puffels, "whirlwind throw")
        sayTo(puffels, positions.Puffels, "yes")
        later(function()
            local bot = getBot(playerId)
            assert(not bot:hasLearnedSpell("Whirlwind Throw"), "free-account Knight learned Whirlwind Throw")
            assert(getTotalMoney(bot) == 800, "premium rejection changed money")
            clearMoney(bot)
        end)

        later(function()
            local bot = getBot(playerId)
            setLevel(bot, 20)
            assert(bot:setPremiumEndsAt(os.time() + 86400), "promotion failure could not add premium")
            assert(bot:addMoney(2000), "promotion failure money could not be added")
        end)
        sayTo(eremo, positions.Eremo, "hi")
        sayTo(eremo, positions.Eremo, "challenge")
        sayTo(eremo, positions.Eremo, "yes")
        later(function()
            local bot = getBot(playerId)
            assert(not bot:hasLearnedSpell("Challenge"), "unpromoted Knight learned Challenge")
            assert(getTotalMoney(bot) == 2000, "promotion rejection changed money")
            clearMoney(bot)
        end)

        later(function()
            local bot = getBot(playerId)
            setLevel(bot, 8)
            assert(bot:setPremiumEndsAt(0), "money failure could not remove premium")
        end)
        sayTo(gregor, positions.Gregor, "hi")
        sayTo(gregor, positions.Gregor, "light")
        sayTo(gregor, positions.Gregor, "yes")
        later(function()
            local bot = getBot(playerId)
            assert(not bot:hasLearnedSpell("Light"), "moneyless Knight learned Light")
            assert(getTotalMoney(bot) == 0, "money rejection changed money")

            assert(bot:setVocation(originalVocation), "original vocation could not be restored")
            setExperience(bot, originalExperience)
            assert(bot:setPremiumEndsAt(originalPremiumEndsAt), "original premium could not be restored")
            if originalInventoryMoney > 0 then
                assert(bot:addMoney(originalInventoryMoney), "original inventory money could not be restored")
            end
            assert(bot:setBankBalance(originalBankMoney), "original bank money could not be restored")
            for _, spellName in ipairs(testedSpells) do
                if originallyLearned[spellName] then
                    bot:learnSpell(spellName)
                end
            end
            assert(bot:getMoney() == originalInventoryMoney, "restored inventory money is incorrect")
            assert(bot:getBankBalance() == originalBankMoney, "restored bank money is incorrect")
            pass(mode)
        end)
        return true
    end

    local playerId = player:getId()
    if mode == "death" then
        local foodCondition = player:getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        assert(foodCondition and foodCondition:getTicks() > 0, "food condition did not persist")
        assert(player:getItemCount(2696) > 0, "nested cheese did not persist")
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:addHealth(-bot:getHealth()), "could not execute player death")
            pass(mode)
        end, 1000)
    elseif mode == "remove" then
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:remove(), "could not remove player")
            pass(mode)
        end, 1000)
    end
    return true
end

login:register()
