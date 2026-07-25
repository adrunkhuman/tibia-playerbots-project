INSERT INTO `accounts` (`name`, `password`, `type`, `premium_ends_at`, `email`, `creation`)
VALUES ('bot-one', SHA1('bot-one'), 1, 0, '', 0)
ON DUPLICATE KEY UPDATE `id` = `id`;

-- These local-development defaults support the hardcoded Rookgaard sewer scenario.

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
    `cap`, `sex`, `stamina`
)
SELECT
    'Bot One', 1, `id`, 1, 0, 150,
    150, 0, 68, 76, 78,
    39, 128, 0, 2, 0, 0,
    0, 0, 100, 6, 32097, 32219, 7,
    400, 1, 2520
FROM `accounts`
WHERE `name` = 'bot-one'
  AND NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Bot One');

INSERT INTO `player_bots` (`player_id`)
SELECT `players`.`id`
FROM `players`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`name` = 'Bot One' AND `accounts`.`name` = 'bot-one' AND `players`.`deletion` = 0
ON DUPLICATE KEY UPDATE `player_id` = `player_id`;

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT `player_bots`.`player_id`, 5,
       (SELECT COALESCE(MAX(`sid`), 100) + 1 FROM `player_items` WHERE `player_id` = `player_bots`.`player_id`),
       2395, 1, ''
FROM `player_bots`
JOIN `players` ON `players`.`id` = `player_bots`.`player_id`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`deletion` = 0 AND `accounts`.`name` = 'bot-one'
  AND NOT EXISTS (
    SELECT 1 FROM `player_items`
    WHERE `player_id` = `player_bots`.`player_id` AND `pid` = 5
);

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT `player_bots`.`player_id`, 3,
       (SELECT COALESCE(MAX(`sid`), 100) + 1 FROM `player_items` WHERE `player_id` = `player_bots`.`player_id`),
       1988, 1, ''
FROM `player_bots`
JOIN `players` ON `players`.`id` = `player_bots`.`player_id`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`deletion` = 0 AND `accounts`.`name` = 'bot-one'
  AND NOT EXISTS (
    SELECT 1 FROM `player_items`
    WHERE `player_id` = `player_bots`.`player_id` AND `pid` = 3
);

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT `player_bots`.`player_id`, 6,
       (SELECT COALESCE(MAX(`sid`), 100) + 1 FROM `player_items` WHERE `player_id` = `player_bots`.`player_id`),
       2526, 1, ''
FROM `player_bots`
JOIN `players` ON `players`.`id` = `player_bots`.`player_id`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
WHERE `players`.`deletion` = 0 AND `accounts`.`name` = 'bot-one'
  AND NOT EXISTS (
    SELECT 1 FROM `player_items`
    WHERE `player_id` = `player_bots`.`player_id` AND `pid` = 6
);
