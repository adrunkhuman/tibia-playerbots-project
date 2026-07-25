INSERT INTO `players` (
    `name`, `group_id`, `account_id`, `level`, `vocation`, `health`,
    `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`,
    `looklegs`, `looktype`, `lookaddons`, `direction`, `maglevel`, `mana`,
    `manamax`, `manaspent`, `soul`, `town_id`, `posx`, `posy`, `posz`,
    `cap`, `sex`, `stamina`, `skill_sword`, `skill_shielding`
)
SELECT
    'Rook Tester', 1, `id`, 50, 0, 395,
    395, 1847300, 68, 76, 78,
    39, 128, 0, 2, 0, 245,
    245, 0, 100, 6, 32097, 32219, 7,
    890, 1, 2520, 70, 60
FROM `accounts`
WHERE `name` = 'admin'
  AND NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Rook Tester');

INSERT INTO `player_items` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`)
SELECT `players`.`id`, `loadout`.`pid`, `loadout`.`sid`, `loadout`.`itemtype`, 1, ''
FROM `players`
JOIN `accounts` ON `accounts`.`id` = `players`.`account_id`
JOIN (
    SELECT 3 AS `pid`, 101 AS `sid`, 1988 AS `itemtype`
    UNION ALL SELECT 4, 102, 2463
    UNION ALL SELECT 5, 103, 2521
    UNION ALL SELECT 6, 104, 2392
    UNION ALL SELECT 8, 105, 2195
    UNION ALL SELECT 101, 106, 2120
    UNION ALL SELECT 101, 107, 2554
) AS `loadout`
WHERE `accounts`.`name` = 'admin' AND `players`.`name` = 'Rook Tester'
  AND `players`.`deletion` = 0
  AND NOT EXISTS (
    SELECT 1
    FROM `player_items` AS `existing`
    WHERE `existing`.`player_id` = `players`.`id`
      AND (
        `existing`.`sid` = `loadout`.`sid`
        OR (`loadout`.`pid` BETWEEN 1 AND 10 AND `existing`.`pid` = `loadout`.`pid`)
      )
  );
