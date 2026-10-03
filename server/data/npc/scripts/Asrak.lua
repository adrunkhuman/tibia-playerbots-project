local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

npcHandler:addModule(FocusModule:new())


keywordHandler:addSpellKeyword({'conjure','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Arrow', price = 450, level = 13, vocation ={3}})
	keywordHandler:addSpellKeyword({'conjure','explosive','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Explosive Arrow', price = 1000, level = 25, vocation ={3}})
	keywordHandler:addSpellKeyword({'conjure','poisoned','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Poisoned Arrow', price = 700, level = 16, vocation ={3}})
	keywordHandler:addSpellKeyword({'cure','poison'}, {npcHandler = npcHandler, spellName = 'Cure Poison', price = 150, level = 10, vocation ={3,4}})
	keywordHandler:addSpellKeyword({'destroy','field'}, {npcHandler = npcHandler, spellName = 'Destroy Field Rune', price = 700, level = 17, vocation ={3}})
	keywordHandler:addSpellKeyword({'find','person'}, {npcHandler = npcHandler, spellName = 'Find Person', price = 80, level = 8, vocation ={3,4}})
	keywordHandler:addSpellKeyword({'great','light'}, {npcHandler = npcHandler, spellName = 'Great Light', price = 500, level = 13, vocation ={3,4}})
	keywordHandler:addSpellKeyword({'intense','healing'}, {npcHandler = npcHandler, spellName = 'Intense Healing', price = 350, level = 11, vocation ={3}})
keywordHandler:addSpellKeyword({'invisible'}, {npcHandler = npcHandler, spellName = 'Invisibility', price = 2000, level = 35, vocation ={3}})
	keywordHandler:addSpellKeyword({'light'}, {npcHandler = npcHandler, spellName = 'Light', price = 100, level = 8, vocation ={3,4}})
	keywordHandler:addSpellKeyword({'light','healing'}, {npcHandler = npcHandler, spellName = 'Light Healing', price = 170, level = 9, vocation ={3,4}})
	keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Cure Poison}', '{Intense Healing}' and '{Light Healing}'."})
	keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Conjure Arrow}', '{Conjure Explosive Arrow}', '{Conjure Poisoned Arrow}', '{Destroy Field}', '{Find Person}', '{Great Light}', '{Invisible}' and '{Light}'."})
	keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Healing spells} and {Support spells}.'})
