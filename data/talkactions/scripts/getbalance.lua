function onSay(player, words, param)
	player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "You have " .. player:getMoney() .. " gold.")
	return TRUE
end
