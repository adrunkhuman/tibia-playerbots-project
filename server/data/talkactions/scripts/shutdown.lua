local shutdownEvent = 0

function onSay(player, words, param)
    if not player:getGroup():getAccess() then
		return true
	end

	if player:getAccountType() < ACCOUNT_TYPE_GOD then
		return false
	end

	if(param == "") then
		Game.setGameState(GAME_STATE_SHUTDOWN)
		return true
	end

	if(param:lower() == "stop") then
		stopEvent(shutdownEvent)
		shutdownEvent = 0
        Game.broadcastMessage("Server is no longer shutting down. Cancelled.")
		return true
	end

	param = tonumber(param)
	if(param == nil or param < 0) then
        player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "Numeric param may not be lower than 0.")
		return true
	end

	if(shutdownEvent ~= 0) then
		stopEvent(shutdownEvent)
	end

	prepareShutdown(math.abs(math.ceil(param)))
	return true
end

function prepareShutdown(minutes)
	if(minutes <= 0) then
		Game.setGameState(GAME_STATE_SHUTDOWN)
	else
		if(minutes == 1) then
			Game.broadcastMessage("Server is going down in " .. minutes .. " minute, please log out now!")
		elseif(minutes <= 3) then
			Game.broadcastMessage("Server is going down in " .. minutes .. " minutes, please log out.")
		else
			Game.broadcastMessage("Server is going down in " .. minutes .. " minutes.")
		end
		shutdownEvent = addEvent(prepareShutdown, 60000, minutes - 1)
	end
end
