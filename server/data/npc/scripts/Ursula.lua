local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)			npcHandler:onCreatureAppear(cid)			end
function onCreatureDisappear(cid)		npcHandler:onCreatureDisappear(cid)			end
function onCreatureSay(cid, type, msg)		npcHandler:onCreatureSay(cid, type, msg)		end
function onThink()				npcHandler:onThink()					end

npcHandler:addModule(FocusModule:new())

keywordHandler:addSpellKeyword({'conjure','bolt'}, {npcHandler = npcHandler, spellName = 'Conjure Bolt', price = 750, level = 17, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'conjure','piercing','bolt'}, {npcHandler = npcHandler, spellName = 'Conjure Piercing Bolt', price = 850, level = 33, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'conjure','sniper','arrow'}, {npcHandler = npcHandler, spellName = 'Conjure Sniper Arrow', price = 800, level = 24, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'disintegrate'}, {npcHandler = npcHandler, spellName = 'Disintegrate Rune', price = 900, level = 21, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'divine','caldera'}, {npcHandler = npcHandler, spellName = 'Divine Caldera', price = 3000, level = 50, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'divine','healing'}, {npcHandler = npcHandler, spellName = 'Divine Healing', price = 2100, level = 35, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'divine','missile'}, {npcHandler = npcHandler, spellName = 'Divine Missile', price = 1800, level = 40, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'enchant','spear'}, {npcHandler = npcHandler, spellName = 'Enchant Spear', price = 2000, level = 45, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'ethereal','spear'}, {npcHandler = npcHandler, spellName = 'Ethereal Spear', price = 1100, level = 23, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'groundshaker'}, {npcHandler = npcHandler, spellName = 'Groundshaker', price = 1500, level = 33, premium = true, vocation ={4}})
keywordHandler:addSpellKeyword({'haste'}, {npcHandler = npcHandler, spellName = 'Haste', price = 600, level = 14, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'holy','missile'}, {npcHandler = npcHandler, spellName = 'Holy Missile Rune', price = 1600, level = 27, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'levitate'}, {npcHandler = npcHandler, spellName = 'Levitate', price = 500, level = 12, premium = true, vocation ={3}})
keywordHandler:addSpellKeyword({'magic','rope'}, {npcHandler = npcHandler, spellName = 'Magic Rope', price = 200, level = 9, premium = true, vocation ={3}})
keywordHandler:addKeyword({'attack', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Divine Caldera}', '{Divine Missile}', '{Ethereal Spear}' and '{Groundshaker}'."})
keywordHandler:addKeyword({'healing', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Divine Healing}'."})
keywordHandler:addKeyword({'support', 'spells'}, StdModule.say, {npcHandler = npcHandler, text = "In this category I have '{Conjure Bolt}', '{Conjure Piercing Bolt}', '{Conjure Sniper Arrow}', '{Disintegrate}', '{Enchant Spear}', '{Haste}', '{Holy Missile}', '{Levitate}' and '{Magic Rope}'."})
keywordHandler:addKeyword({'spells'}, StdModule.say, {npcHandler = npcHandler, text = 'I can teach you {Attack spells}, {Healing spells} and {Support spells}.'})
