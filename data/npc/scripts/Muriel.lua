local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

local function releasePlayer(cid)
	if not Player(cid) then
		return
	end

	npcHandler:releaseFocus(cid)
	npcHandler:resetNpc(cid)
end

local function creatureSayCallback(cid, type, msg)
	if not npcHandler:isFocused(cid) then
		return false
	end

	local player = Player(cid)

	if msgcontains(msg, 'mission') then
		if player:getLevel() < 35 then
			npcHandler:say('Indeed there is something to be done, but I need someone more experienced. Come back later if you want to.', cid)
			addEvent(releasePlayer, 1000, cid)
			return true
		end

		if player:getStorageValue(Storage.TibiaTales.IntoTheBonePit) == -1 then
			npcHandler:say({
				'Indeed, there is something you can do for me. You must know I am researching for a new spell against the undead. ...',
				'To achieve that I need a desecrated bone. There is a cursed bone pit somewhere in the dungeons north of Thais where the dead never rest. ...',
				'Find that pit, dig for a well-preserved human skeleton and conserve a sample in a special container which you receive from me. Are you going to help me?'
			}, cid)
			npcHandler.topic[cid] = 1
		elseif player:getStorageValue(Storage.TibiaTales.IntoTheBonePit) == 1 then
			npcHandler:say({
				'The rotworms dug deep into the soil north of Thais. Rumours say that you can access a place of endless moaning from there. ...',
				'No one knows how old that common grave is but the people who died there are cursed and never come to rest. A bone from that pit would be perfect for my studies.'
			}, cid)
			addEvent(releasePlayer, 1000, cid)
		elseif player:getStorageValue(Storage.TibiaTales.IntoTheBonePit) == 2 then
			player:setStorageValue(Storage.TibiaTales.IntoTheBonePit, 3)
			if player:removeItem(4864, 1) then
				player:addItem(6300, 1)
				npcHandler:say('Excellent! Now I can try to put my theoretical thoughts into practice and find a cure for the symptoms of undead. Here, take this for your efforts.', cid)
			else
				npcHandler:say({
					'I am so glad you are still alive. Benjamin found the container with the bone sample inside. Fortunately, I inscribe everything with my name, so he knew it was mine. ...',
					'I thought you have been haunted and killed by the undead. I\'m glad that this is not the case. Thank you for your help.'
				}, cid)
			end
			addEvent(releasePlayer, 1000, cid)
		else
			npcHandler:say('I am very glad you helped me, but I am very busy at the moment.', cid)
			addEvent(releasePlayer, 1000, cid)
		end
	elseif msgcontains(msg, 'yes') then
		if npcHandler.topic[cid] == 1 then
			player:addItem(4863, 1)
			player:setStorageValue(Storage.TibiaTales.IntoTheBonePit, 1)
			npcHandler:say({
				'Great! Here is the container for the bone. Once, I used it to collect ectoplasma of ghosts, but it will work here as well. ...',
				'If you lose it, you can buy a new one from the explorer\'s society in North Port or Port Hope. Ask me about the mission when you come back.'
			}, cid)
			addEvent(releasePlayer, 1000, cid)
		end
	elseif msgcontains(msg, 'no') then
		if npcHandler.topic[cid] == 1 then
			npcHandler:say('Ohh, then I need to find another adventurer who wants to earn a great reward. Bye!', cid)
			addEvent(releasePlayer, 1000, cid)
		end
	end
	return true
end

npcHandler:setMessage(MESSAGE_GREET, 'Greetings, |PLAYERNAME|! Looking for wisdom and power, eh?')
npcHandler:setMessage(MESSAGE_FAREWELL, 'Farewell.')
npcHandler:setMessage(MESSAGE_WALKAWAY, 'Farewell.')

npcHandler:setCallback(CALLBACK_MESSAGE_DEFAULT, creatureSayCallback)
npcHandler:addModule(FocusModule:new())



	keywordHandler:addSpellKeyword({'creature','illusion'}, {npcHandler = npcHandler, spellName = 'Creature Illusion', price = 1000, level = 23, vocation ={1}})
	keywordHandler:addSpellKeyword({'cure','poison'}, {npcHandler = npcHandler, spellName = 'Cure Poison', price = 150, level = 10, vocation ={1}})
	keywordHandler:addSpellKeyword({'destroy','field'}, {npcHandler = npcHandler, spellName = 'Destroy Field', price = 700, level = 17, vocation ={1}})
	keywordHandler:addSpellKeyword({'energy','beam'}, {npcHandler = npcHandler, spellName = 'Energy Beam', price = 1000, level = 23, vocation ={1}})
	keywordHandler:addSpellKeyword({'energy','field'}, {npcHandler = npcHandler, spellName = 'Energy Field', price = 700, level = 18, vocation ={1}})
	keywordHandler:addSpellKeyword({'energy','wall'}, {npcHandler = npcHandler, spellName = 'Energy Wall', price = 2500, level = 41, vocation ={1}})
	keywordHandler:addSpellKeyword({'energy','wave'}, {npcHandler = npcHandler, spellName = 'Energy Wave', price = 2500, level = 38, vocation ={1}})
	keywordHandler:addSpellKeyword({'explosion'}, {npcHandler = npcHandler, spellName = 'Explosion', price = 1800, level = 31, vocation ={1}})
	keywordHandler:addSpellKeyword({'find','person'}, {npcHandler = npcHandler, spellName = 'Find Person', price = 80, level = 8, vocation ={1}})
	keywordHandler:addSpellKeyword({'fire','bomb'}, {npcHandler = npcHandler, spellName = 'Fire Bomb', price = 1500, level = 27, vocation ={1}})
	keywordHandler:addSpellKeyword({'fire','field'}, {npcHandler = npcHandler, spellName = 'Fire Field', price = 500, level = 15, vocation ={1}})
	keywordHandler:addSpellKeyword({'fire','wall'}, {npcHandler = npcHandler, spellName = 'Fire Wall', price = 2000, level = 33, vocation ={1}})
	keywordHandler:addSpellKeyword({'fire','wave'}, {npcHandler = npcHandler, spellName = 'Fire Wave', price = 850, level = 18, vocation ={1}})
	keywordHandler:addSpellKeyword({'great','energy','beam'}, {npcHandler = npcHandler, spellName = 'Great Energy Beam', price = 1800, level = 29, vocation ={1}})
	keywordHandler:addSpellKeyword({'great','fireball'}, {npcHandler = npcHandler, spellName = 'Great Fireball', price = 1200, level = 30, vocation ={1}})
	keywordHandler:addSpellKeyword({'great','light'}, {npcHandler = npcHandler, spellName = 'Great Light', price = 500, level = 13, vocation ={1}})
	keywordHandler:addSpellKeyword({'heavy','magic','missile'}, {npcHandler = npcHandler, spellName = 'Heavy Magic Missile', price = 1500, level = 25, vocation ={1}})
	keywordHandler:addSpellKeyword({'intense','healing'}, {npcHandler = npcHandler, spellName = 'Intense Healing', price = 350, level = 20, vocation ={1}})
	keywordHandler:addSpellKeyword({'invisible'}, {npcHandler = npcHandler, spellName = 'Invisible', price = 2000, level = 35, vocation ={1}})
	keywordHandler:addSpellKeyword({'light'}, {npcHandler = npcHandler, spellName = 'Light', price = 0, level = 8, vocation ={1}})
	keywordHandler:addSpellKeyword({'light','healing'}, {npcHandler = npcHandler, spellName = 'Light Healing', price = 0, level = 8, vocation ={1}})
	keywordHandler:addSpellKeyword({'light','magic','missile'}, {npcHandler = npcHandler, spellName = 'Light Magic Missile', price = 500, level = 15, vocation ={1}})
	keywordHandler:addSpellKeyword({'magic','shield'}, {npcHandler = npcHandler, spellName = 'Magic Shield', price = 450, level = 14, vocation ={1}})
	keywordHandler:addSpellKeyword({'poison','field'}, {npcHandler = npcHandler, spellName = 'Poison Field', price = 300, level = 14, vocation ={1}})
	keywordHandler:addSpellKeyword({'poison','wall'}, {npcHandler = npcHandler, spellName = 'Poison Wall', price = 1600, level = 29, vocation ={1}})
	keywordHandler:addSpellKeyword({'stalagmite'}, {npcHandler = npcHandler, spellName = 'Stalagmite', price = 1400, level = 24, vocation ={1}})
	keywordHandler:addSpellKeyword({'sudden','death'}, {npcHandler = npcHandler, spellName = 'Sudden Death', price = 3000, level = 45, vocation ={1}})
	keywordHandler:addSpellKeyword({'summon','creature'}, {npcHandler = npcHandler, spellName = 'Summon Creature', price = 2000, level = 25, vocation ={1}})
	keywordHandler:addSpellKeyword({'ultimate','healing'}, {npcHandler = npcHandler, spellName = 'Ultimate Healing', price = 1000, level = 30, vocation ={1}})
	keywordHandler:addKeyword({'attack', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Energy Beam}', '{Energy Wave}', '{Fire Wave}' and '{Great Energy Beam}'."})
	keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Cure Poison}', '{Intense Healing}', '{Light Healing}' and '{Ultimate Healing}'."})
	keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Creature Illusion}', '{Destroy Field}', '{Energy Field}', '{Energy Wall}', '{Explosion}', '{Find Person}', '{Fire Bomb}', '{Fire Field}', '{Fire Wall}', '{Great Fireball}', '{Great Light}', '{Heavy Magic Missile}', '{Invisible}', '{Light}', '{Light Magic Missile}', '{Magic Shield}', '{Poison Field}', '{Poison Wall}', '{Stalagmite}', '{Sudden Death}' and '{Summon Creature}'."})
	keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Attack spells}, {Healing spells} and {Support spells}.'})
	
	
