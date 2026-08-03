local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

keywordHandler:addSpellKeyword({'berserk'}, {npcHandler = npcHandler, spellName = 'Berserk', price = 2500, level = 35, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'charge'}, {npcHandler = npcHandler, spellName = 'Charge', price = 1300, level = 25, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'haste'}, {npcHandler = npcHandler, spellName = 'Haste', price = 600, level = 14, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'levitate'}, {npcHandler = npcHandler, spellName = 'Levitate', price = 500, level = 12, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'magic','rope'}, {npcHandler = npcHandler, spellName = 'Magic Rope', price = 200, level = 9, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'whirlwind','throw'}, {npcHandler = npcHandler, spellName = 'Whirlwind Throw', price = 800, level = 15, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'wound','cleansing'}, {npcHandler = npcHandler, spellName = 'Wound Cleansing', price = 300, level = 30, premium = true, vocation ={4}})
keywordHandler:addKeyword({'attack', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Berserk}' and '{Whirlwind Throw}'."})
keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Wound Cleansing}'."})
keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Charge}', '{Haste}', '{Levitate}' and '{Magic Rope}'."})
keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Attack spells}, {Healing spells} and {Support spells}.'})

npcHandler:setMessage(MESSAGE_GREET, 'Yeah, another fool {disturbing} me, what a joy.')
npcHandler:setMessage(MESSAGE_FAREWELL, 'Whatever.')
npcHandler:setMessage(MESSAGE_WALKAWAY, 'Whatever.')

npcHandler:addModule(FocusModule:new())
