local botName = "Bot One"
local storageKey = 45016
local modeValues = {
    interactions = 86016,
    death = 86017,
    remove = 86018,
}

local function pass(mode)
    print("PLAYERBOT_CONNECTIONLESS_TEST PASS mode=" .. mode)
end

local function getBot(playerId)
    local player = Player(playerId)
    assert(player and not player:isRemoved(), "Bot One is unavailable")
    return player
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
        assert(player:teleportTo(Position(32099, 32211, 7)), "test position could not be restored")
        local backpack = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(backpack and backpack:getId() == ITEM_BACKPACK, "seeded backpack is missing")
        player:removeCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        local cheeseCount = player:getItemCount(2696)
        if cheeseCount > 0 then
            assert(player:removeItem(2696, cheeseCount), "existing cheese could not be removed")
        end
        for _ = 1, 12 do
            assert(backpack:addItem(2696, 1), "test cheese could not be added")
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

    local playerId = player:getId()
    if mode == "death" then
        local foodCondition = player:getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT)
        assert(foodCondition and foodCondition:getTicks() > 0, "food condition did not persist")
        assert(player:getItemCount(2696) == 1, "remaining cheese did not persist")
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
