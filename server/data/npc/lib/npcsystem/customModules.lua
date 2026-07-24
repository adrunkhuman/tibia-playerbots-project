function StdModule.kick(cid, message, keywords, parameters, node)
	local npcHandler = parameters.npcHandler
	if npcHandler == nil then
		error("StdModule.travel called without any npcHandler instance.")
	end

	if not npcHandler:isFocused(cid) then
		return false
	end

	npcHandler:releaseFocus(cid)
	npcHandler:say(parameters.text or "Off with you!", cid)

	local destination = parameters.destination
	if type(destination) == 'table' then
		destination = destination[math.random(#destination)]
	end

	Player(cid):teleportTo(destination, true)

	npcHandler:resetNpc(cid)
	return true
end

local GreetModule = {}
function GreetModule.greet(cid, message, keywords, parameters)
	if not parameters.npcHandler:isInRange(cid) then
		return true
	end

	if parameters.npcHandler:isFocused(cid) then
		return true
	end

	local parseInfo = { [TAG_PLAYERNAME] = Player(cid):getName() }
	parameters.npcHandler:say(parameters.npcHandler:parseMessage(parameters.text, parseInfo), cid, true)
	parameters.npcHandler:addFocus(cid)
	return true
end

function GreetModule.farewell(cid, message, keywords, parameters)
	if not parameters.npcHandler:isFocused(cid) then
		return false
	end

	local parseInfo = { [TAG_PLAYERNAME] = Player(cid):getName() }
	parameters.npcHandler:say(parameters.npcHandler:parseMessage(parameters.text, parseInfo), cid, true)
	parameters.npcHandler:resetNpc(cid)
	parameters.npcHandler:releaseFocus(cid)
	return true
end

-- Adds a keyword which acts as a greeting word
function KeywordHandler:addGreetKeyword(keys, parameters, condition, action)
	local keys = keys
	keys.callback = FocusModule.messageMatcherDefault
	return self:addKeyword(keys, GreetModule.greet, parameters, condition, action)
end

-- Adds a keyword which acts as a farewell word
function KeywordHandler:addFarewellKeyword(keys, parameters, condition, action)
	local keys = keys
	keys.callback = FocusModule.messageMatcherDefault
	return self:addKeyword(keys, GreetModule.farewell, parameters, condition, action)
end

-- Set custom greeting messages
function FocusModule:addGreetMessage(message)
	if not self.greetWords then
		self.greetWords = {}
	end


	if type(message) == 'string' then
		table.insert(self.greetWords, message)
	else
		for i = 1, #message do
			table.insert(self.greetWords, message[i])
		end
	end
end

-- Set custom farewell messages
function FocusModule:addFarewellMessage(message)
	if not self.farewellWords then
		self.farewellWords = {}
	end

	if type(message) == 'string' then
		table.insert(self.farewellWords, message)
	else
		for i = 1, #message do
			table.insert(self.farewellWords, message[i])
		end
	end
end

-- Set custom greeting callback
function FocusModule:setGreetCallback(callback)
	self.greetCallback = callback
end

-- Set custom farewell callback
function FocusModule:setFarewellCallback(callback)
	self.farewellCallback = callback
end