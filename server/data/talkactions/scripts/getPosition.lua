function onSay(player, words, param)
    if player:getAccountType() < ACCOUNT_TYPE_TUTOR then
		return false
	end

	local position = player:getPosition()
	player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "You current position is [X: " .. position.x .. " | Y: " .. position.y .. " | Z: " .. position.z .. "]")
	return TRUE
end
