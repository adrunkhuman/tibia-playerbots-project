INSERT INTO `accounts` (`name`, `password`, `type`, `premium_ends_at`, `email`, `creation`)
VALUES ('bot-one', SHA1('bot-one'), 1, 0, '', 0)
ON DUPLICATE KEY UPDATE `id` = `id`;

-- These local-development defaults support the Rookgaard hunt and service loop.

CREATE TABLE IF NOT EXISTS `player_bots` (
    `player_id` int NOT NULL,
    PRIMARY KEY (`player_id`),
    CONSTRAINT `player_bots_ibfk_1` FOREIGN KEY (`player_id`) REFERENCES `players` (`id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARACTER SET=utf8;

SET @bot_player_created = NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Bot One');

INSERT INTO `players` (
    `name`, `group_id`, `account_id`, `level`, `vocation`, `health`,
    `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`,
    `looklegs`, `looktype`, `lookaddons`, `direction`, `maglevel`, `mana`,
    `manamax`, `manaspent`, `soul`, `town_id`, `posx`, `posy`, `posz`,
    `cap`, `sex`, `stamina`, `skill_sword`, `skill_shielding`, `balance`
)
SELECT
    'Bot One', 1, `id`, 1, 0, 150,
    150, 0, 68, 76, 78,
    39, 128, 0, 2, 0, 0,
    0, 0, 100, 6, 32097, 32219, 7,
    400, 1, 2520, 70, 60, 10000
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
SELECT @bot_player_id, `loadout`.`pid`, @bot_next_sid + `loadout`.`offset`, `loadout`.`itemtype`, 1, ''
FROM (
    SELECT 4 AS `pid`, 1 AS `offset`, 2463 AS `itemtype`
    UNION ALL SELECT 5, 2, 2521
    UNION ALL SELECT 6, 3, 2392
    UNION ALL SELECT 8, 4, 2195
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

-- Seed the initial purse once; later provisioning runs preserve spent money.
SET @bot_next_sid = (
    SELECT COALESCE(MAX(`sid`), 100) FROM `player_items` WHERE `player_id` = @bot_player_id
);

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT @bot_player_id, @bot_backpack_sid, @bot_next_sid + `coins`.`offset`, 2148, 100, ''
FROM (
    SELECT 1 AS `offset`
    UNION ALL SELECT 2
) AS `coins`
WHERE @bot_backpack_sid IS NOT NULL
  AND @bot_player_created;
