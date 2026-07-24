INSERT INTO accounts (id, name, password, secret, type, premium_ends_at, email, creation)
VALUES ('1', 'admin', SHA1('admin'), NULL, '3', '0', '', '0')
ON DUPLICATE KEY UPDATE id = id;

INSERT INTO players (name, group_id, account_id, level)
VALUES ('GOD Admin', '6', '1', '8')
ON DUPLICATE KEY UPDATE id = id;