local function registerCorpseTestMonster(name, loot, corpseId)
    local monsterType = Game.createMonsterType(name)
    local monster = {
        description = name:lower(),
        experience = 0,
        outfit = {lookType = 21},
        health = 1,
        maxHealth = 1,
        race = "blood",
        corpse = corpseId or 5964,
        speed = 0,
        flags = {
            summonable = false,
            attackable = true,
            hostile = false,
            challengeable = false,
            convinceable = false,
            illusionable = false,
            pushable = false,
            canPushItems = false,
            canPushCreatures = false,
            targetDistance = 1,
            staticAttackChance = 100,
        },
        loot = loot,
    }
    monsterType:register(monster)
end

registerCorpseTestMonster("Playerbot Empty Corpse", {})
registerCorpseTestMonster("Playerbot Loot Corpse", {
    {id = ITEM_GOLD_COIN, chance = 100000, maxCount = 1},
})
registerCorpseTestMonster("Playerbot Nonlootable Corpse", {}, ITEM_GOLD_COIN)
registerCorpseTestMonster("Playerbot Container Death Item", {}, ITEM_BAG)
