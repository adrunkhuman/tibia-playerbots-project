local mode = os.getenv("PLAYERBOT_GAMEPLAY_MODE")
assert(mode == "recovery_fists" or mode == "recovery_melee",
    "recovery spawn overlay must not be used outside its focused scenarios")

-- Ordinary combat rolls and engine loot ownership/open/move behavior. The
-- high-value weak quarry isolates the recovery pipeline, not natural economics.
Game.createMonsterType("Playerbot Recovery Quarry"):register({
    description = "a recovery fixture quarry",
    experience = 1,
    outfit = {lookType = 21},
    health = 20, maxHealth = 20,
    race = "blood", corpse = 5964, speed = 0,
    flags = {
        summonable = false, attackable = true, hostile = true,
        challengeable = false, convinceable = false, illusionable = false,
        pushable = false, canPushItems = false, canPushCreatures = false,
        targetDistance = 1, staticAttackChance = 100,
        -- The atlas and engine both read this; renewal must be real, not a
        -- fixture override of tiny-pocket or productivity admission.
        ignoreSpawnBlock = true,
    },
    -- Small nonzero unmitigated damage also gives the atlas a real minimum
    -- fight window; fully armor-blocked damage would be a poor productivity control.
    attacks = {{name = "combat", type = COMBAT_LIFEDRAIN, interval = 10000, chance = 100,
        minDamage = -1, maxDamage = -1, target = true, range = 1}},
    loot = {{id = ITEM_PLATINUM_COIN, chance = 100000, maxCount = 20}},
    events = {"PlayerbotRecoveryHealth", "PlayerbotRecoveryDeath"},
})
