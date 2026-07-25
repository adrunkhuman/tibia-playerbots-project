INSERT INTO `players` (
    `name`, `group_id`, `account_id`, `level`, `vocation`, `health`,
    `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`,
    `looklegs`, `looktype`, `lookaddons`, `direction`, `maglevel`, `mana`,
    `manamax`, `manaspent`, `soul`, `town_id`, `posx`, `posy`, `posz`,
    `cap`, `sex`, `stamina`
)
SELECT
    'Rook Tester', 1, `id`, 1, 0, 150,
    150, 0, 68, 76, 78,
    39, 128, 0, 2, 0, 0,
    0, 0, 100, 6, 32097, 32219, 7,
    400, 1, 2520
FROM `accounts`
WHERE `name` = 'admin'
  AND NOT EXISTS (SELECT 1 FROM `players` WHERE `name` = 'Rook Tester');
