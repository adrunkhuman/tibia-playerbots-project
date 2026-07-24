function onSay(player, words, param)
	if not player:getGroup():getAccess() then
		return true
	end

	if player:getAccountType() < ACCOUNT_TYPE_GOD then
		return false
	end

    if(param == "") then
        player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "Need world type.")
        return false
    end

	param = param:lower()
    if param == "nopvp" then
        Game.setWorldType(WORLD_TYPE_NO_PVP)
		world = "No-PVP"
	elseif param == "pvp" then
		Game.setWorldType(WORLD_TYPE_PVP)
		world = "PVP"
	elseif param == "pvpe" then
		Game.setWorldType(WORLD_TYPE_PVP_ENFORCED)
		world = "PVP-Enforced"
	else
        player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "Bad world type.")
		return true
	end

    Game.broadcastMessage("Gameworld type set to: " .. world .. ".")
    print("> " .. player:getName() .. " changed word type to: " .. world .. ".")
	return true
end
