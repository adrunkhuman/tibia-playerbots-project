function onSay(player, words, param)
    if not player:getGroup():getAccess() then
		return true
	end

	if player:getAccountType() < ACCOUNT_TYPE_GAMEMASTER then
		return false
	end

    for var = 1,3 do
        if var == 1 then
            Game.broadcastMessage("Saving server...") --- this appears before server freeze...
        end
        if var == 2 then
            Game.saveGameState() --- freeezee....
        end
        if var == 3 then
            Game.broadcastMessage("Done.") --- no more lag!
        end
    end

    return false
end