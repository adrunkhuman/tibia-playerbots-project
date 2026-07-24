local worldTypes = {
	[WORLD_TYPE_NO_PVP] = "No-PVP",
	[WORLD_TYPE_PVP] = "PVP",
	[WORLD_TYPE_PVP_ENFORCED] = "PVP-Enforced"
}


function onSay(player, words, param)
	local currentWorldType = worldTypes[Game.getWorldType()]

	if (not currentWorldType) then
		return true
	end

	player:sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, "World is set to " .. currentWorldType .. ".")
	return false
end
