-- Mounted only by compose.playerbot-multibot.yaml. Test intervention is confined to login/timers.
local names = { ["Bot One"] = true, ["Bot Two"] = true }
local markerKey = 50190
local markers = { ["Bot One"] = {value = 101, item = 1950}, ["Bot Two"] = {value = 202, item = 1955} }
local mode = os.getenv("PLAYERBOT_MULTIBOT_CASE") or "startup"

local function requireBot(id, name)
    local player = Player(id)
    assert(player and not player:isRemoved() and player:getName() == name,
        "multibot fixture lost " .. name)
    return player
end

local function verifySurvivor(name, label)
    local bot = Player(name)
    assert(bot and not bot:isRemoved() and bot:getName() == name,
        "multibot fixture lost surviving " .. name)
    print("PLAYERBOT_MULTIBOT SURVIVOR_PASS " .. label)
end

local function removeOne(id)
    local bot = requireBot(id, "Bot One")
    assert(bot:remove(), "could not externally remove Bot One")
    print("PLAYERBOT_MULTIBOT REMOVE_ONE_PASS")
end

local function killOne(id)
    local bot = requireBot(id, "Bot One")
    -- The temple is protected; this is a fixture-only fatal health change, not a production action.
    assert(bot:addHealth(-bot:getHealth()), "could not trigger Bot One death")
    print("PLAYERBOT_MULTIBOT KILL_ONE_PASS")
end

local function sharedNpc(oneId, twoId)
    local one, two = requireBot(oneId, "Bot One"), requireBot(twoId, "Bot Two")
    local gregor = Npc("Gregor")
    assert(gregor and not gregor:isRemoved(), "shared NPC Gregor is unavailable")
    local center = gregor:getPosition()
    local approaches = {}
    for _, offset in ipairs({{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) do
        local position = Position(center.x + offset[1], center.y + offset[2], center.z)
        local tile = Tile(position)
        if tile and tile:isWalkable() then approaches[#approaches + 1] = position end
    end
    assert(#approaches >= 2, "Gregor has fewer than two distinct walkable adjacent tiles")
    assert(one:getLevel() == 8 and two:getLevel() == 8 and
        one:getVocation():getId() == 4 and two:getVocation():getId() == 4,
        "shared NPC fixture requires two level-8 Knights")
    for index, bot in ipairs({one, two}) do
        bot:forgetSpell("Light")
        assert(bot:canLearnSpell("Light"), "Knight cannot learn Light")
        local money = bot:getMoney() + bot:getBankBalance()
        if money > 0 then assert(bot:removeTotalMoney(money), "could not clear prior money") end
        assert(bot:addMoney(100), "could not seed exactly 100 gold")
        assert(bot:getMoney() + bot:getBankBalance() == 100, "wrong shared NPC test funds")
        assert(bot:teleportTo(approaches[index]), "could not place Knight next to Gregor")
    end
    local function speak(bot, message)
        assert(bot:say(message, TALKTYPE_PRIVATE_PN, false, gregor),
            bot:getName() .. " could not tell Gregor " .. message)
    end
    -- One dispatcher callback keeps both independent NPC conversations in flight
    -- without autonomous movement or scheduler turns between these normal says.
    speak(one, "hi")
    speak(two, "hi")
    speak(one, "light")
    speak(two, "light")
    speak(one, "yes")
    assert(one:hasLearnedSpell("Light") and not two:hasLearnedSpell("Light") and
        one:getMoney() + one:getBankBalance() == 0 and two:getMoney() + two:getBankBalance() == 100,
        "first Gregor confirmation crossed the other bot's spell or funds")
    print("PLAYERBOT_MULTIBOT SHARED_NPC_FIRST_PASS")
    speak(two, "yes")
    assert(one:hasLearnedSpell("Light") and two:hasLearnedSpell("Light") and
        one:getMoney() + one:getBankBalance() == 0 and two:getMoney() + two:getBankBalance() == 0,
        "two Gregor confirmations did not teach and charge each Knight independently")
    print("PLAYERBOT_MULTIBOT SHARED_NPC_PASS")
end

local login = CreatureEvent("zzPlayerbotMultibot")
function login.onLogin(player)
    local name = player:getName()
    if not names[name] then return true end
    -- Keep fixture mutations off human login attempts. The network probe must
    -- reject even an offline identity while the manager reserves its name.
    if player:getClient().version ~= 0 then return true end
    if mode == "reject_two" and name == "Bot Two" then
        print("PLAYERBOT_MULTIBOT REJECT_TWO_PASS")
        return false
    end
    if name == "Bot One" and (mode == "reject_two" or mode == "cancel_pending") then
        addEvent(verifySurvivor, 4500, name, mode)
    elseif name == "Bot Two" and (mode == "death_one" or mode == "remove_one" or mode == "remove_manager" or
        mode == "cancel_recovery" or mode == "queued_death" or mode == "pvp_remove") then
        addEvent(verifySurvivor, 7000, name, mode)
    end
    if mode == "shared_npc" and name == "Bot Two" then
        local first = Player("Bot One")
        assert(first and not first:isRemoved(), "Bot One is unavailable for shared Gregor fixture")
        addEvent(sharedNpc, 500, first:getId(), player:getId())
    end
    if mode == "persist_prime" then
        local marker = markers[name]
        assert(player:getStorageValue(markerKey) == -1, name .. " already has a marker")
        local bag = player:getSlotItem(CONST_SLOT_BACKPACK)
        assert(bag and bag:addItem(marker.item, 1), name .. " could not add its inventory marker")
        assert(player:setStorageValue(markerKey, marker.value), name .. " could not save its storage marker")
        print("PLAYERBOT_MULTIBOT PRIME_PASS " .. name)
    elseif mode == "persist_verify" then
        local marker = markers[name]
        local other = markers[name == "Bot One" and "Bot Two" or "Bot One"]
        assert(player:getStorageValue(markerKey) == marker.value, name .. " storage did not persist")
        assert(player:getItemCount(marker.item) == 1 and player:getItemCount(other.item) == 0,
            name .. " inventory marker is missing or crossed identities")
        print("PLAYERBOT_MULTIBOT PERSIST_PASS " .. name)
    elseif mode == "queued_death" and name == "Bot One" then
        local count = player:getStorageValue(markerKey)
        if count == -1 then
            assert(player:setStorageValue(markerKey, 1), "could not mark first queued death")
        else
            assert(count == 1, "unexpected repeated queued death recovery")
            print("PLAYERBOT_MULTIBOT QUEUED_RECOVER_LOGIN_PASS")
        end
    elseif (mode == "death_one" or mode == "cancel_recovery") and name == "Bot One" then
        local count = player:getStorageValue(markerKey)
        if count == -1 then
            assert(player:getLevel() == 8, "Bot One is not level 8 before death")
            assert(player:setStorageValue(markerKey, 1), "could not mark first death")
            addEvent(killOne, mode == "cancel_recovery" and 3000 or 3500, player:getId())
            print("PLAYERBOT_MULTIBOT DEATH_ARMED")
        else
            assert(mode == "death_one" and count == 1, "unexpected repeated Bot One recovery")
            print("PLAYERBOT_MULTIBOT RECOVER_LOGIN_PASS")
        end
    elseif mode == "remove_one" and name == "Bot One" then
        addEvent(removeOne, 3500, player:getId())
        print("PLAYERBOT_MULTIBOT REMOVE_ARMED")
    else
        assert(mode == "startup" or mode == "reject_two" or mode == "death_one" or mode == "remove_one" or
            mode == "remove_manager" or mode == "cancel_pending" or mode == "cancel_recovery" or
            mode == "shared_npc" or mode == "queued_death" or mode == "pvp_remove",
            "unknown PLAYERBOT_MULTIBOT_CASE: " .. mode)
    end
    assert(player:getVocation():getId() == 4, name .. " is not a Knight")
    if (mode ~= "death_one" and mode ~= "cancel_recovery" and mode ~= "queued_death") or name ~= "Bot One" then
        assert(player:getLevel() == 8, name .. " is not level 8")
    end
    print("PLAYERBOT_MULTIBOT LOGIN_PASS " .. name)
    return true
end
login:register()
