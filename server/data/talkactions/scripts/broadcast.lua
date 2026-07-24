function onSay(player, words, param)
	if not player:hasFlag(PlayerFlag_CanBroadcast) then
		return true
	end

	if player:getAccountType() < ACCOUNT_TYPE_COMMUNITYMANAGER then
		return false
	end

	print("> " .. player:getName() .. " broadcasted: \"" .. param .. "\".")
	for _, targetPlayer in ipairs(Game.getPlayers()) do
		targetPlayer:sendPrivateMessage(player, param, TALKTYPE_BROADCAST)
	end
	return false
end
