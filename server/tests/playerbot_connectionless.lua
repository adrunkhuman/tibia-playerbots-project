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
    paladinSpellReset = 86024,
    paladinSpellLearning = 86025,
    paladinSpellPersistence = 86026,
    paladinSpellFailures = 86027,
}

local function pass(mode)
    print("PLAYERBOT_CONNECTIONLESS_TEST PASS mode=" .. mode)
    -- Removal can leave no controller to flush subsequent server output.
    io.flush()
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

-- Each step runs 100 ms after the previous one so NPC dialogue is processed in order.
local function newTimeline()
    local delay = 0
    return function(callback)
        delay = delay + 100
        addEvent(callback, delay)
    end
end

local function trainer(name)
    local npc = Npc(name)
    assert(npc, name .. " is unavailable")
    local position = npc:getPosition()
    return {npc = npc, position = Position(position.x - 1, position.y, position.z)}
end

local function sayTo(later, playerId, target, message)
    later(function()
        local bot = getBot(playerId)
        assert(bot:teleportTo(target.position), "trainer position could not be reached")
        assert(bot:say(message, TALKTYPE_PRIVATE_PN, false, target.npc), "trainer dialogue failed: " .. message)
    end)
end

local function buySpell(later, playerId, target, keyword)
    sayTo(later, playerId, target, "hi")
    sayTo(later, playerId, target, keyword)
    sayTo(later, playerId, target, "yes")
end

local paladinSpells = {"Light Healing", "Intense Healing", "Conjure Arrow", "Food", "Magic Shield", "Ultimate Healing"}

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
        assert(player:getSlotItem(CONST_SLOT_ARMOR):getId() == 2464, "seeded chain armor is missing")
        assert(player:getSlotItem(CONST_SLOT_RIGHT):getId() == 2530, "seeded copper shield is missing")
        assert(player:getSlotItem(CONST_SLOT_LEFT):getId() == 2395, "seeded carlin sword is missing")
        assert(player:getSlotItem(CONST_SLOT_FEET):getId() == 2643, "seeded leather boots are missing")
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
    elseif mode == "paladinSpellReset" then
        local playerId = player:getId()
        addEvent(function()
            local bot = getBot(playerId)
            assert(bot:setVocation(3), "Paladin spell reset could not select Paladin vocation")
            setLevel(bot, 11)
            for _, spellName in ipairs(paladinSpells) do
                bot:forgetSpell(spellName)
                assert(not bot:hasLearnedSpell(spellName), spellName .. " remained learned after reset")
                assert(not hasCastableSpell(bot, spellName), "unlearned " .. spellName .. " remained castable")
            end
            pass(mode)
        end, 100)
        return true
    elseif mode == "paladinSpellLearning" then
        assert(player:getVocation():getId() == 3, "Paladin learning phase did not start as a Paladin")
        assert(player:getLevel() == 11, "Paladin learning phase did not start at level 11")
        for _, spellName in ipairs(paladinSpells) do
            assert(not player:hasLearnedSpell(spellName), spellName .. " was learned before the learning phase")
            assert(not hasCastableSpell(player, spellName), spellName .. " was castable before training")
        end
        assert(player:canLearnSpell("Intense Healing"), "level 11 Paladin cannot learn Intense Healing")
        clearMoney(player)
        assert(player:addMoney(520), "Paladin healing money could not be added")

        local elane = trainer("Elane")
        local playerId = player:getId()
        local later = newTimeline()
        buySpell(later, playerId, elane, "light healing")
        buySpell(later, playerId, elane, "intense healing")
        later(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Light Healing"), "Elane did not teach Light Healing")
            assert(bot:hasLearnedSpell("Intense Healing"), "Elane did not teach Intense Healing at level 11")
            assert(hasCastableSpell(bot, "Intense Healing"), "Intense Healing remained uncastable after training")
            assert(getTotalMoney(bot) == 0, "Elane did not charge exactly 170 and 350 gold")
            setLevel(bot, 13)
            assert(bot:addMoney(450), "Conjure Arrow money could not be added")
        end)
        buySpell(later, playerId, elane, "conjure arrow")
        later(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Conjure Arrow"), "Elane did not teach Conjure Arrow")
            assert(hasCastableSpell(bot, "Conjure Arrow"), "Conjure Arrow remained uncastable after training")
            assert(getTotalMoney(bot) == 0, "Elane did not charge exactly 450 gold for Conjure Arrow")
            setLevel(bot, 14)
            assert(bot:setPremiumEndsAt(0), "Paladin learning could not select a free account")
            for _, spellName in ipairs({"Food", "Magic Shield"}) do
                assert(bot:canLearnSpell(spellName), "level-14 Paladin cannot learn " .. spellName)
                assert(not hasCastableSpell(bot, spellName), "unlearned " .. spellName .. " is castable at level 14")
            end
            assert(bot:addMoney(750), "Food and Magic Shield money could not be added")
        end)
        buySpell(later, playerId, elane, "food")
        buySpell(later, playerId, elane, "magic shield")
        later(function()
            local bot = getBot(playerId)
            for _, spellName in ipairs({"Food", "Magic Shield"}) do
                assert(bot:hasLearnedSpell(spellName), "Elane did not teach " .. spellName .. " at level 14")
                assert(hasCastableSpell(bot, spellName), spellName .. " remained uncastable after training")
            end
            assert(getTotalMoney(bot) == 0, "Elane did not charge exactly 300 and 450 gold")
            setLevel(bot, 20)
            assert(bot:canLearnSpell("Ultimate Healing"), "level-20 Paladin cannot learn Ultimate Healing")
            assert(not hasCastableSpell(bot, "Ultimate Healing"), "unlearned Ultimate Healing is castable at level 20")
            assert(bot:addMoney(1000), "Ultimate Healing money could not be added")
        end)
        buySpell(later, playerId, elane, "ultimate healing")
        later(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Ultimate Healing"), "Elane did not teach Ultimate Healing at level 20")
            assert(hasCastableSpell(bot, "Ultimate Healing"), "Ultimate Healing remained uncastable after training")
            assert(getTotalMoney(bot) == 0, "Elane did not charge exactly 1000 gold for Ultimate Healing")
            pass(mode)
        end)
        return true
    elseif mode == "paladinSpellPersistence" then
        for _, spellName in ipairs(paladinSpells) do
            assert(player:hasLearnedSpell(spellName), "learned " .. spellName .. " did not persist")
            assert(hasCastableSpell(player, spellName), "persisted " .. spellName .. " is not castable")
        end
        assert(player:addMoney(350), "duplicate test money could not be added")
        local moneyBefore = getTotalMoney(player)
        local playerId = player:getId()
        local later = newTimeline()
        buySpell(later, playerId, trainer("Elane"), "intense healing")
        later(function()
            local bot = getBot(playerId)
            assert(bot:hasLearnedSpell("Intense Healing"), "duplicate purchase removed learned Intense Healing")
            assert(getTotalMoney(bot) == moneyBefore, "duplicate purchase changed money")
            assert(bot:removeMoney(350), "duplicate test money could not be removed")
            pass(mode)
        end)
        return true
    elseif mode == "paladinSpellFailures" then
        local originalExperience = player:getExperience()
        local originalVocation = player:getVocation():getId()
        local originalPremiumEndsAt = player:getPremiumEndsAt()
        local originalInventoryMoney = player:getMoney()
        local originalBankMoney = player:getBankBalance()
        local testedSpells = {"Light Healing", "Intense Healing", "Conjure Bolt", "Conjure Power Bolt", "Food", "Magic Shield", "Ultimate Healing"}
        local originallyLearned = {}
        for _, spellName in ipairs(testedSpells) do
            originallyLearned[spellName] = player:hasLearnedSpell(spellName)
            player:forgetSpell(spellName)
        end
        clearMoney(player)

        local elane, ursula, eremo = trainer("Elane"), trainer("Ursula"), trainer("Eremo")
        local playerId = player:getId()
        local later = newTimeline()
        local function rejected(spellName, money, reason)
            later(function()
                local bot = getBot(playerId)
                assert(not bot:hasLearnedSpell(spellName), reason .. " learned " .. spellName)
                assert(getTotalMoney(bot) == money, reason .. " rejection changed money")
                clearMoney(bot)
            end)
        end
        -- Proves that the preceding rejection came from the gate under test, not from a missing offer.
        local function learned(spellName, reason)
            later(function()
                local bot = getBot(playerId)
                assert(bot:hasLearnedSpell(spellName), reason .. " could not learn " .. spellName)
                assert(getTotalMoney(bot) == 0, reason .. " was not charged the exact price of " .. spellName)
            end)
        end
        local function prepare(vocation, level, premium, money)
            later(function()
                local bot = getBot(playerId)
                assert(bot:setVocation(vocation), "failure setup could not select vocation " .. vocation)
                setLevel(bot, level)
                assert(bot:setPremiumEndsAt(premium and os.time() + 86400 or 0), "failure setup could not set premium")
                if money > 0 then
                    assert(bot:addMoney(money), "failure setup money could not be added")
                end
            end)
        end

        prepare(3, 10, false, 350)
        buySpell(later, playerId, elane, "intense healing")
        rejected("Intense Healing", 350, "level-10 Paladin")

        prepare(3, 13, false, 300)
        buySpell(later, playerId, elane, "food")
        rejected("Food", 300, "level-13 Paladin")
        prepare(3, 13, false, 450)
        buySpell(later, playerId, elane, "magic shield")
        rejected("Magic Shield", 450, "level-13 Paladin")
        prepare(3, 19, false, 1000)
        buySpell(later, playerId, elane, "ultimate healing")
        rejected("Ultimate Healing", 1000, "level-19 Paladin")

        prepare(3, 17, false, 750)
        buySpell(later, playerId, ursula, "conjure bolt")
        rejected("Conjure Bolt", 750, "free-account Paladin")
        prepare(3, 17, true, 750)
        buySpell(later, playerId, ursula, "conjure bolt")
        learned("Conjure Bolt", "premium Paladin")

        prepare(4, 9, false, 170)
        buySpell(later, playerId, elane, "light healing")
        rejected("Light Healing", 170, "Knight at Elane")

        prepare(3, 59, true, 2000)
        buySpell(later, playerId, eremo, "conjure power bolt")
        rejected("Conjure Power Bolt", 2000, "unpromoted Paladin")
        prepare(7, 59, true, 2000)
        buySpell(later, playerId, eremo, "conjure power bolt")
        learned("Conjure Power Bolt", "Royal Paladin")

        prepare(3, 11, false, 0)
        buySpell(later, playerId, elane, "intense healing")
        rejected("Intense Healing", 0, "moneyless Paladin")

        later(function()
            local bot = getBot(playerId)
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
                else
                    bot:forgetSpell(spellName)
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
