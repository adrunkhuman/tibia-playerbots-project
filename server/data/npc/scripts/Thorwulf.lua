local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

keywordHandler:addSpellKeyword({'berserk'}, {npcHandler = npcHandler, spellName = 'Berserk', price = 2500, level = 35, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'cure','poison'}, {npcHandler = npcHandler, spellName = 'Cure Poison', price = 150, level = 10, vocation ={4}})
keywordHandler:addSpellKeyword({'fierce','berserk'}, {npcHandler = npcHandler, spellName = 'Fierce Berserk', price = 5000, level = 70, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'find','person'}, {npcHandler = npcHandler, spellName = 'Find Person', price = 80, level = 8, vocation ={4}})
keywordHandler:addSpellKeyword({'great','light'}, {npcHandler = npcHandler, spellName = 'Great Light', price = 500, level = 13, vocation ={4}})
keywordHandler:addSpellKeyword({'groundshaker'}, {npcHandler = npcHandler, spellName = 'Groundshaker', price = 1500, level = 33, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'haste'}, {npcHandler = npcHandler, spellName = 'Haste', price = 600, level = 14, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'levitate'}, {npcHandler = npcHandler, spellName = 'Levitate', price = 500, level = 12, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'light'}, {npcHandler = npcHandler, spellName = 'Light', price = 100, level = 8, vocation ={4}})
keywordHandler:addSpellKeyword({'light','healing'}, {npcHandler = npcHandler, spellName = 'Light Healing', price = 170, level = 9, vocation ={4}})
keywordHandler:addSpellKeyword({'magic','rope'}, {npcHandler = npcHandler, spellName = 'Magic Rope', price = 200, level = 9, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'whirlwind','throw'}, {npcHandler = npcHandler, spellName = 'Whirlwind Throw', price = 800, level = 15, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'wound','cleansing'}, {npcHandler = npcHandler, spellName = 'Wound Cleansing', price = 300, level = 30, premium = true, vocation ={4}})
keywordHandler:addKeyword({'attack', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Berserk}', '{Fierce Berserk}', '{Groundshaker}' and '{Whirlwind Throw}'."})
keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Cure Poison}', '{Light Healing}' and '{Wound Cleansing}'."})
keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Find Person}', '{Great Light}', '{Haste}', '{Levitate}', '{Light}' and '{Magic Rope}'."})
keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Attack spells}, {Healing spells} and {Support spells}.'})

npcHandler:setMessage(MESSAGE_GREET, 'Hi |PLAYERNAME|.')
npcHandler:setMessage(MESSAGE_FAREWELL, 'Bye |PLAYERNAME|.')
npcHandler:setMessage(MESSAGE_WALKAWAY, 'Bye.')

npcHandler:addModule(FocusModule:new())
