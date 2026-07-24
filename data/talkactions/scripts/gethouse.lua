function onSay(player, words, param)
    if player:getAccountType() < ACCOUNT_TYPE_TUTOR then
		return false
	end

	if(param == "") then
		player:sendCancelMessage("Command param required.")
		return true
	end

	local target = Player(param)
	local house = target:getHouse()
	if(not house) then
		player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "Player " .. param .. " does not own house or doesn't exists.")
		return true
	end

	player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, param .. " owns house: " .. house:getName() .. ".")
	return true
end
