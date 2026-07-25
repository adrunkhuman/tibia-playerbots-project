local botName = "Bot One"
local checkpointStorage = 45017
local tripStorage = 45018
local modes = {
    traversal = true,
    transition_recovery = true,
}

local login = CreatureEvent("zzPlayerbotGameplayRegression")

function login.onLogin(player)
    if player:getName() ~= botName then
        return true
    end

    local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE") or "traversal"
    assert(modes[mode], "unknown PLAYERBOT_GAMEPLAY_MODE: " .. mode)

    if mode == "traversal" then
        assert(player:setStorageValue(checkpointStorage, 0), "traversal checkpoint could not be reset")
        assert(player:setStorageValue(tripStorage, 0), "traversal trip count could not be reset")
        assert(player:teleportTo(Position(32097, 32219, 7)), "traversal start could not be restored")
    else
        -- Simulate shutdown after the final ladder changed floors but before the controller persisted progress.
        assert(player:setStorageValue(checkpointStorage, 6), "recovery checkpoint could not be set")
        assert(player:setStorageValue(tripStorage, 0), "recovery trip count could not be reset")
        assert(player:teleportTo(Position(32101, 32129, 4)), "recovery position could not be set")
    end

    print("PLAYERBOT_GAMEPLAY_TEST START mode=" .. mode)
    return true
end

login:register()
