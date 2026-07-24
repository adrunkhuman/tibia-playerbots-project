local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

-- Female Summoner and Male Mage Hat Addon (needs to be rewritten)
local hatKeyword = keywordHandler:addKeyword({'proof'}, StdModule.say, {npcHandler = npcHandler, text = '... I cannot believe my eyes. You retrieved this hat from Ferumbras\' remains? That is incredible. If you give it to me, I will grant you the right to wear this hat as addon. What do you say?'},
		function(player) return not player:hasOutfit(player:getSex() == PLAYERSEX_FEMALE and 141 or 130, 2) end
	)
	hatKeyword:addChildKeyword({'yes'}, StdModule.say, {npcHandler = npcHandler, text = 'Sorry you don\'t have the Ferumbras\' hat.'}, function(player) return player:getItemCount(5903) == 0 end)
	hatKeyword:addChildKeyword({'yes'}, StdModule.say, {npcHandler = npcHandler, text = 'I bow to you, player, and hereby grant you the right to wear Ferumbras\' hat as accessory. Congratulations!'}, nil,
		function(player)
			player:removeItem(5903, 1)
			player:addOutfitAddon(141, 2)
			player:addOutfitAddon(130, 2)
			player:getPosition():sendMagicEffect(CONST_ME_MAGIC_RED)
		end
	)
	-- hatKeyword:addChildKeyword({'no'}, StdModule.say, {npcHandler = npcHandler, text = ''})

keywordHandler:addKeyword({'myra'}, StdModule.say, {npcHandler = npcHandler,
	text = {
		'Bah, I know. I received some sort of \'nomination\' from our outpost in Port Hope. ...',
		'Usually it takes a little more than that for an award though. However, I honour Myra\'s word. ...',
		'I hereby grant you the right to wear a special sign of honour, acknowledged by the academy of Edron. Since you are a man, I guess you don\'t want girlish stuff. There you go.'
	}},
	function(player) return player:getStorageValue(Storage.OutfitQuest.MageSummoner.AddonHatCloak) == 10 end,
	function(player)
		player:addOutfitAddon(138, 2)
		player:addOutfitAddon(133, 2)
		player:getPosition():sendMagicEffect(CONST_ME_MAGIC_BLUE)
		player:setStorageValue(Storage.OutfitQuest.MageSummoner.AddonHatCloak, 11)
		player:setStorageValue(Storage.OutfitQuest.MageSummoner.MissionHatCloak, 0)
		player:setStorageValue(Storage.OutfitQuest.Ref, math.min(0, player:getStorageValue(Storage.OutfitQuest.Ref) - 1))
	end
)

keywordHandler:addKeyword({'myra'}, StdModule.say, {npcHandler = npcHandler, text = 'Stop bothering me. I am a far too busy man to be constantly giving out awards.'}, function(player) return player:getStorageValue(Storage.OutfitQuest.MageSummoner.AddonHatCloak) == 11 end)
keywordHandler:addKeyword({'myra'}, StdModule.say, {npcHandler = npcHandler, text = 'What the hell are you talking about?'})

npcHandler:setMessage(MESSAGE_GREET, 'Welcome |PLAYERNAME|, student of the arcane arts.')
npcHandler:setMessage(MESSAGE_FAREWELL, 'Good bye, and don\'t come back too soon.')
npcHandler:setMessage(MESSAGE_WALKAWAY, 'Good bye, and don\'t come back too soon.')

npcHandler:addModule(FocusModule:new())


keywordHandler:addSpellKeyword({'blood','rage'}, {npcHandler = npcHandler, spellName = 'Blood Rage', price = 8000, level = 60, vocation ={4}})
keywordHandler:addSpellKeyword({'conjure','bolt'}, {npcHandler = npcHandler, spellName = 'Conjure Bolt', price = 750, level = 17, vocation ={3}})
keywordHandler:addSpellKeyword({'conjure','piercing','bolt'}, {npcHandler = npcHandler, spellName = 'Conjure Piercing Bolt', price = 850, level = 33, vocation ={3}})
keywordHandler:addSpellKeyword({'conjure','poisoned','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Poisoned Arrow', price = 700, level = 16, vocation ={3}})
keywordHandler:addSpellKeyword({'conjure','sniper','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Sniper Arrow', price = 800, level = 24, vocation ={3}})
keywordHandler:addSpellKeyword({'divine','caldera'}, {npcHandler = npcHandler, spellName = 'Divine Caldera', price = 3000, level = 50, vocation ={3}})
keywordHandler:addSpellKeyword({'energy','bomb'}, {npcHandler = npcHandler, spellName = 'Energy Bomb', price = 2300, level = 37, vocation ={1}})
keywordHandler:addSpellKeyword({'eternal','winter'}, {npcHandler = npcHandler, spellName = 'Eternal Winter', price = 8000, level = 60, vocation ={2}})
keywordHandler:addSpellKeyword({'fierce','berserk'}, {npcHandler = npcHandler, spellName = 'Fierce Berserk', price = 7500, level = 90, vocation ={4}})
keywordHandler:addSpellKeyword({'hell\'s','core'}, {npcHandler = npcHandler, spellName = 'Hell\'s Core', price = 8000, level = 60, vocation ={1}})
keywordHandler:addSpellKeyword({'mass','healing'}, {npcHandler = npcHandler, spellName = 'Mass Healing', price = 2200, level = 36, vocation ={2}})
keywordHandler:addSpellKeyword({'paralyse'}, {npcHandler = npcHandler, spellName = 'Paralyse', price = 1900, level = 54, vocation ={2}})
keywordHandler:addSpellKeyword({'protector'}, {npcHandler = npcHandler, spellName = 'Protector', price = 6000, level = 55, vocation ={4}})
keywordHandler:addSpellKeyword({'rage','of','the','skies'}, {npcHandler = npcHandler, spellName = 'Rage of the Skies', price = 6000, level = 55, vocation ={1}})
keywordHandler:addSpellKeyword({'sharpshooter'}, {npcHandler = npcHandler, spellName = 'Sharpshooter', price = 8000, level = 60, vocation ={3}})
keywordHandler:addSpellKeyword({'swift','foot'}, {npcHandler = npcHandler, spellName = 'Swift Foot', price = 6000, level = 55, vocation ={3}})
keywordHandler:addSpellKeyword({'wrath','of','nature'}, {npcHandler = npcHandler, spellName = 'Wrath of Nature', price = 6000, level = 55, vocation ={2}})
keywordHandler:addKeyword({'attack', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Divine Caldera}', '{Eternal Winter}', '{Fierce Berserk}', '{Hell's Core}', '{Rage of the Skies}' and '{Wrath of Nature}'."})
keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Mass Healing}'."})
keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Blood Rage}', '{Conjure Bolt}', '{Conjure Piercing Bolt}', '{Conjure Poisoned Arrow}', '{Conjure Sniper Arrow}', '{Energy Bomb}', '{Paralyse}', '{Protector}', '{Sharpshooter}' and '{Swift Foot}'."})
keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Attack spells}, {Healing spells} and {Support spells}.'})