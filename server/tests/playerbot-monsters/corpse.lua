local function registerCorpseTestMonster(name, loot, corpseId, hostile, health, attacks, experience)
    local monsterType = Game.createMonsterType(name)
    local monster = {
        description = name:lower(),
        experience = experience or 0,
        outfit = {lookType = 21},
        health = health or 1,
        maxHealth = health or 1,
        race = "blood",
        corpse = corpseId or 5964,
        speed = 0,
        flags = {
            summonable = false,
            attackable = true,
            hostile = hostile or false,
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
        attacks = attacks,
    }
    monsterType:register(monster)
end

registerCorpseTestMonster("Playerbot Empty Corpse", {})
registerCorpseTestMonster("Playerbot Loot Corpse", {
    {id = ITEM_GOLD_COIN, chance = 100000, maxCount = 1},
})
registerCorpseTestMonster("Playerbot Nonlootable Corpse", {}, ITEM_GOLD_COIN)
registerCorpseTestMonster("Playerbot Container Death Item", {}, ITEM_BAG)
registerCorpseTestMonster("Playerbot Defensive Threat", {}, nil, false, 1)
registerCorpseTestMonster("Playerbot Level Eight Target", {}, nil, false, 1, nil, 1)
registerCorpseTestMonster("Playerbot Value Corpse", {
    {id = 2826, chance = 100000, maxCount = 1},
})
registerCorpseTestMonster("Playerbot Spell Target", {}, nil, false, 1000)
registerCorpseTestMonster("Playerbot Death Threat", {}, nil, true, 100000, {
    {name = "combat", type = COMBAT_PHYSICALDAMAGE, interval = 100, chance = 100,
        minDamage = -10000, maxDamage = -10000, target = true, range = 1},
})
