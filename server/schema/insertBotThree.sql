INSERT INTO `accounts` (`name`, `password`, `type`, `premium_ends_at`, `email`, `creation`)
VALUES ('bot-three', SHA1('bot-three'), 1, 0, '', 0)
ON DUPLICATE KEY UPDATE `id` = `id`;

-- A post-training level-8 Paladin at the Carlin temple. Seed skill only on creation.
INSERT INTO `players` (
    `name`, `group_id`, `account_id`, `level`, `vocation`, `health`,
    `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`,
    `looklegs`, `looktype`, `lookaddons`, `direction`, `maglevel`, `mana`,
    `manamax`, `manaspent`, `soul`, `town_id`, `posx`, `posy`, `posz`,
    `cap`, `sex`, `stamina`, `skill_dist`, `skill_shielding`, `balance`
)
SELECT
    'Bot Three', 1, `id`, 8, 3, 185,
    185, 4200, 68, 76, 78,
    39, 128, 0, 2, 0, 35,
    35, 0, 100, 4, 32360, 31782, 7,
    470, 1, 2520, 40, 20, 1000
FROM `accounts`
WHERE `name` = 'bot-three'
  AND NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Bot Three');

INSERT INTO `player_bots` (`player_id`)
SELECT `players`.`id`
FROM `players`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`name` = 'Bot Three' AND `accounts`.`name` = 'bot-three' AND `players`.`deletion` = 0
ON DUPLICATE KEY UPDATE `player_id` = `player_id`;

SET @bot_player_id = (
    SELECT `players`.`id`
    FROM `players`
    JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
    WHERE `players`.`name` = 'Bot Three' AND `accounts`.`name` = 'bot-three' AND `players`.`deletion` = 0
    LIMIT 1
);

-- Assign SIDs after existing items; refill empty slots without replacing saved gear.
SET @bot_next_sid = (
    SELECT COALESCE(MAX(`sid`), 100) FROM `player_items` WHERE `player_id` = @bot_player_id
);
INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT @bot_player_id, `loadout`.`pid`, @bot_next_sid + `loadout`.`offset`, `loadout`.`itemtype`, `loadout`.`count`, ''
FROM (
    SELECT 3 AS `pid`, 1 AS `offset`, 1988 AS `itemtype`, 1 AS `count`
    UNION ALL SELECT 1, 2, 2480, 1
    UNION ALL SELECT 4, 3, 2464, 1
    UNION ALL SELECT 5, 4, 2530, 1
    UNION ALL SELECT 6, 5, 2389, 3
    UNION ALL SELECT 7, 6, 2468, 1
    UNION ALL SELECT 8, 7, 2643, 1
) AS `loadout`
WHERE @bot_player_id IS NOT NULL
  AND NOT EXISTS (
    SELECT 1 FROM `player_items` WHERE `player_id` = @bot_player_id AND `pid` = `loadout`.`pid`
);

SET @bot_backpack_sid = (
    SELECT `sid` FROM `player_items` WHERE `player_id` = @bot_player_id AND `pid` = 3 LIMIT 1
);
SET @bot_next_sid = (
    SELECT COALESCE(MAX(`sid`), 100) FROM `player_items` WHERE `player_id` = @bot_player_id
);
INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT @bot_player_id, @bot_backpack_sid, @bot_next_sid + `tools`.`offset`, `tools`.`itemtype`, 1, ''
FROM (
    SELECT 1 AS `offset`, 2120 AS `itemtype`
    UNION ALL SELECT 2, 2554
) AS `tools`
WHERE @bot_backpack_sid IS NOT NULL
  AND NOT EXISTS (
    SELECT 1 FROM `player_items`
    WHERE `player_id` = @bot_player_id AND `itemtype` = `tools`.`itemtype`
  );
