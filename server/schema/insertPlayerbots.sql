INSERT INTO `accounts` (`name`, `password`, `type`, `premium_ends_at`, `email`, `creation`)
VALUES ('bot-one', SHA1('bot-one'), 1, 0, '', 0)
ON DUPLICATE KEY UPDATE `id` = `id`;

-- Bot One starts as a freshly promoted mainland Knight at the Carlin temple.
-- Focused Rookgaard fixtures replace this state for earlier progression stages.

CREATE TABLE IF NOT EXISTS `player_bots` (
    `player_id` int NOT NULL,
    PRIMARY KEY (`player_id`),
    CONSTRAINT `player_bots_ibfk_1` FOREIGN KEY (`player_id`) REFERENCES `players` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8;

INSERT INTO `players` (
    `name`, `group_id`, `account_id`, `level`, `vocation`, `health`,
    `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`,
    `looklegs`, `looktype`, `lookaddons`, `direction`, `maglevel`, `mana`,
    `manamax`, `manaspent`, `soul`, `town_id`, `posx`, `posy`, `posz`,
    `cap`, `sex`, `stamina`, `skill_sword`, `skill_shielding`, `balance`
)
SELECT
    'Bot One', 1, `id`, 8, 4, 185,
    185, 4200, 68, 76, 78,
    39, 128, 0, 2, 0, 35,
    35, 0, 100, 4, 32360, 31782, 7,
    470, 1, 2520, 20, 20, 1000
FROM `accounts`
WHERE `name` = 'bot-one'
  AND NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Bot One');

INSERT INTO `player_bots` (`player_id`)
SELECT `players`.`id`
FROM `players`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`name` = 'Bot One' AND `accounts`.`name` = 'bot-one' AND `players`.`deletion` = 0
ON DUPLICATE KEY UPDATE `player_id` = `player_id`;

SET @bot_player_id = (
    SELECT `players`.`id`
    FROM `players`
    JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
    WHERE `players`.`name` = 'Bot One' AND `accounts`.`name` = 'bot-one' AND `players`.`deletion` = 0
    LIMIT 1
);

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT @bot_player_id, 3,
       (SELECT COALESCE(MAX(`sid`), 100) + 1 FROM `player_items` WHERE `player_id` = @bot_player_id),
       1988, 1, ''
WHERE @bot_player_id IS NOT NULL
  AND NOT EXISTS (SELECT 1 FROM `player_items` WHERE `player_id` = @bot_player_id AND `pid` = 3);

SET @bot_backpack_sid = (
    SELECT `sid` FROM `player_items` WHERE `player_id` = @bot_player_id AND `pid` = 3 LIMIT 1
);
SET @bot_next_sid = (
    SELECT COALESCE(MAX(`sid`), 100) FROM `player_items` WHERE `player_id` = @bot_player_id
);

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT @bot_player_id, `loadout`.`pid`, @bot_next_sid + `loadout`.`offset`, `loadout`.`itemtype`, `loadout`.`count`, ''
FROM (
    SELECT 1 AS `pid`, 1 AS `offset`, 2480 AS `itemtype`, 1 AS `count`
    UNION ALL SELECT 4, 2, 2464, 1
    UNION ALL SELECT 5, 3, 2530, 1
    UNION ALL SELECT 6, 4, 2395, 1
    UNION ALL SELECT 7, 5, 2468, 1
    UNION ALL SELECT 8, 6, 2643, 1
) AS `loadout`
WHERE NOT EXISTS (
    SELECT 1 FROM `player_items`
    WHERE `player_id` = @bot_player_id AND `pid` = `loadout`.`pid`
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
