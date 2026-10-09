#Requires -Version 7.0
<#
.SYNOPSIS
Checks playerbot SQL provisioning in an isolated, disposable database-only Compose project.
#>
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$root = Split-Path -Parent $PSScriptRoot
$project = 'angelion-provision-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
$compose = @('compose', '--project-name', $project, '-f', (Join-Path $root 'server/compose.yaml'))
$temp = Join-Path ([IO.Path]::GetTempPath()) "playerbot-provision-$([guid]::NewGuid().ToString('N'))"
$started = $false
$normalRoster = @('Bot One', 'Bot Two', 'Bot Three', 'Bot Four')

function Invoke-Docker {
    param([string[]]$Arguments, [switch]$ExpectFailure, [string]$ErrorPattern)
    $output = @(& docker @Arguments 2>&1) -join "`n"
    $code = $LASTEXITCODE
    if ($ExpectFailure) {
        if ($code -eq 0 -or ($ErrorPattern -and $output -notmatch $ErrorPattern)) {
            throw "Expected Docker failure ($ErrorPattern), got exit $code`: $output"
        }
    } elseif ($code -ne 0) {
        throw "Docker exited $code`: $output"
    }
    return $output.Trim()
}

function Invoke-Compose {
    param([string[]]$Arguments, [string]$Overlay, [switch]$ExpectFailure, [string]$ErrorPattern)
    $composeArgs = @($compose)
    if ($Overlay) { $composeArgs += @('-f', $Overlay) }
    $composeArgs += $Arguments
    Invoke-Docker -Arguments $composeArgs -ExpectFailure:$ExpectFailure -ErrorPattern $ErrorPattern
}

function Invoke-Sql {
    param([string]$Statement)
    Invoke-Compose -Arguments @('exec', '-T', '-e', 'MYSQL_PWD=angelion', 'database',
        'mariadb', '--user=angelion', '--batch', '--raw', '--skip-column-names', 'angelion', "--execute=$Statement")
}

function Assert-Sql {
    param([string]$Statement, [string]$Expected)
    $actual = Invoke-Sql $Statement
    if ($actual -ne $Expected) { throw "SQL result expected '$Expected', got '$actual'." }
}

function Start-Database {
    Invoke-Compose -Arguments @('up', '--detach', 'database') | Out-Null
    $id = Invoke-Compose -Arguments @('ps', '--quiet', 'database')
    if (-not $id) { throw 'Database container was not created.' }
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $health = Invoke-Docker -Arguments @('inspect', '--format', '{{.State.Health.Status}}', $id)
        if ($health -eq 'healthy') { return }
        if ($health -eq 'unhealthy') { throw 'Database healthcheck failed.' }
        Start-Sleep -Seconds 2
    }
    throw 'Database did not become healthy within 120 seconds.'
}

function Reset-Database {
    Invoke-Compose -Arguments @('down', '--volumes', '--remove-orphans') | Out-Null
    Start-Database
}

function Assert-Roster {
    param([string[]]$Names)
    Assert-Sql 'SELECT COUNT(*) FROM player_bots;' "$($Names.Count)"
    Assert-Sql @'
SELECT GROUP_CONCAT(players.name ORDER BY players.name SEPARATOR '|')
FROM player_bots JOIN players ON players.id = player_bots.player_id;
'@ (($Names | Sort-Object) -join '|')
}

try {
    Invoke-Docker -Arguments @('info', '--format', '{{.ServerVersion}}') | Out-Null
    Invoke-Docker -Arguments @('compose', 'version') | Out-Null
    # Never adopt or tear down another project, including an earlier test run.
    $containers = Invoke-Docker -Arguments @('ps', '--all', '--quiet', '--filter', "label=com.docker.compose.project=$project")
    $volumes = Invoke-Docker -Arguments @('volume', 'ls', '--quiet', '--filter', "label=com.docker.compose.project=$project")
    if ($containers -or $volumes) { throw "Refusing to reuse Compose project $project." }
    New-Item -ItemType Directory -Path $temp | Out-Null
    $badSql = Join-Path $temp 'invalid.sql'
    Set-Content -Path $badSql -Value 'SELECT * FROM playerbot_provisioning_missing_table;' -Encoding utf8
    $badOverlay = Join-Path $temp 'bad-import.yaml'
    $source = ($badSql.Replace('\', '/') | ConvertTo-Json -Compress)
    $singleOverlay = Join-Path $root 'server/compose.playerbot-gameplay.yaml'

    $started = $true
    Start-Database
    # Each later import must fail closed, leaving only the preceding seeds registered.
    $imports = @('insertBotTwo.sql', 'insertBotThree.sql', 'insertBotFour.sql')
    for ($index = 0; $index -lt $imports.Count; $index++) {
        if ($index -gt 0) { Reset-Database }
        $target = '/schema/' + $imports[$index]
        Set-Content -Path $badOverlay -Encoding utf8 -Value @"
services:
  playerbot-setup:
    volumes:
      - type: bind
        source: $source
        target: $target
        read_only: true
"@
        Invoke-Compose -Arguments @('config', '--quiet') -Overlay $badOverlay | Out-Null
        $mount = Invoke-Compose -Arguments @('config', '--format', 'json') -Overlay $badOverlay | ConvertFrom-Json
        if (-not @($mount.services.'playerbot-setup'.volumes | Where-Object target -eq $target |
            Where-Object source -eq $badSql).Count) {
            throw "Invalid SQL override did not replace the $target mount."
        }
        Invoke-Compose -Overlay $badOverlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
            -ExpectFailure -ErrorPattern 'playerbot_provisioning_missing_table' | Out-Null
        Assert-Roster $normalRoster[0..$index]
    }

    Reset-Database
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    Assert-Roster $normalRoster
    Assert-Sql 'SELECT COUNT(DISTINCT player_id) FROM player_bots;' '4'
    $rosterQuery = @'
SELECT CONCAT_WS('|', players.name, accounts.name, players.id, players.deletion,
    players.level, players.vocation, players.town_id, players.posx, players.posy, players.posz,
    players.health, players.healthmax, players.experience, players.mana, players.manamax,
    players.cap, players.balance, players.skill_sword, players.skill_dist, players.skill_shielding,
    players.lookbody, players.lookfeet, players.lookhead, players.looklegs, players.looktype,
    players.lookaddons, players.direction, players.maglevel, players.manaspent, players.soul,
    players.sex, players.stamina, players.skill_dist_tries, players.skill_shielding_tries)
FROM player_bots JOIN players ON players.id = player_bots.player_id
JOIN accounts ON accounts.id = players.account_id ORDER BY players.name;
'@
    $roster = Invoke-Sql $rosterQuery
    $expectedSeeds = @{
        'Bot One' = 'bot-one|0|8|4|4|32360|31782|7|185|185|4200|35|35|470|1000|20|10|20'
        'Bot Two' = 'bot-two|0|8|4|2|32369|32241|7|185|185|4200|35|35|470|1000|20|10|20'
        'Bot Three' = 'bot-three|0|8|3|4|32360|31782|7|185|185|4200|35|35|470|1000|10|40|20'
        'Bot Four' = 'bot-four|0|8|3|2|32369|32241|7|185|185|4200|35|35|470|1000|10|40|20'
    }
    foreach ($line in ($roster -split "`n")) {
        $fields = $line -split '\|'
        $name = $fields[0]
        $state = (@($fields[1]) + $fields[3..33]) -join '|'
        $expected = $expectedSeeds[$name] + '|68|76|78|39|128|0|2|0|0|100|1|2520|0|0'
        if ($fields.Count -ne 34 -or [int]$fields[2] -le 0 -or $state -ne $expected) {
            throw "Starting attributes differ for $name`: $line"
        }
    }
    $inventoryQuery = @'
SELECT CONCAT_WS('|', players.name, player_items.pid, player_items.sid,
    player_items.itemtype, player_items.count, HEX(player_items.attributes))
FROM player_items JOIN players ON players.id = player_items.player_id
WHERE players.name IN ('Bot One', 'Bot Two', 'Bot Three', 'Bot Four')
ORDER BY players.name, player_items.sid;
'@
    $gearQuery = @'
SELECT CONCAT_WS('|', players.name,
    GROUP_CONCAT(CONCAT(IF(player_items.pid = backpack.sid, 'bag', player_items.pid), ':',
        player_items.itemtype, ':', player_items.count)
        ORDER BY IF(player_items.pid BETWEEN 1 AND 10, player_items.pid, 99), player_items.itemtype SEPARATOR ','))
FROM player_items JOIN players ON players.id = player_items.player_id
LEFT JOIN player_items AS backpack ON backpack.player_id = players.id AND backpack.pid = 3
WHERE players.name IN ('Bot One', 'Bot Two', 'Bot Three', 'Bot Four')
GROUP BY players.id ORDER BY players.name;
'@
    $gear = @((Invoke-Sql $gearQuery) -split "`n")
    if ($gear.Count -ne 4) { throw "Expected four loadouts: $gear" }
    foreach ($line in $gear) {
        $name, $items = $line -split '\|', 2
        $weapon = if ($name -in @('Bot Three', 'Bot Four')) { '2389:3' } else { '2395:1' }
        $expected = "1:2480:1,3:1988:1,4:2464:1,5:2530:1,6:$weapon,7:2468:1,8:2643:1,bag:2120:1,bag:2554:1"
        if ($items -ne $expected) { throw "Starting loadout differs for $name`: $items" }
    }
    $inventory = Invoke-Sql $inventoryQuery
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    Assert-Roster $normalRoster
    if ((Invoke-Sql $rosterQuery) -ne $roster -or (Invoke-Sql $inventoryQuery) -ne $inventory) {
        throw 'Provisioning was not idempotent.'
    }

    # Simulate saved progress, a replacement weapon, consumed spears and a bag item.
    # Keep slots occupied: provisioning intentionally fills missing equipment slots.
    Invoke-Sql @'
UPDATE players SET level = 12, experience = 17200, health = 201, healthmax = 225,
    mana = 41, manamax = 95, cap = 550, balance = 4321, skill_sword = 12,
    skill_dist = 47, skill_dist_tries = 123, skill_shielding = 25, skill_shielding_tries = 456,
    town_id = 2, posx = 32369, posy = 32241, posz = 7
WHERE name IN ('Bot Three', 'Bot Four');
UPDATE player_items JOIN players ON players.id = player_items.player_id
SET player_items.itemtype = IF(players.name = 'Bot Three', 2389, 2456),
    player_items.count = IF(players.name = 'Bot Three', 2, 1), player_items.attributes = X'010203'
WHERE players.name IN ('Bot Three', 'Bot Four') AND player_items.pid = 6;
INSERT INTO player_items (player_id, pid, sid, itemtype, count, attributes)
SELECT players.id, backpack.sid, 200, 2148, 37, '' FROM players
JOIN player_items AS backpack ON backpack.player_id = players.id AND backpack.pid = 3
WHERE players.name IN ('Bot Three', 'Bot Four');
'@ | Out-Null
    $progress = Invoke-Sql $rosterQuery
    $savedInventory = Invoke-Sql $inventoryQuery
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    Assert-Roster $normalRoster
    if ((Invoke-Sql $rosterQuery) -ne $progress -or (Invoke-Sql $inventoryQuery) -ne $savedInventory) {
        throw 'Provisioning reset progressed Paladin stats or saved inventory.'
    }
    Invoke-Compose -Overlay $singleOverlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
        -ExpectFailure -ErrorPattern 'Single-bot fixture requires only Bot One registered' | Out-Null
    Assert-Roster $normalRoster

    # Exercise the real overlays rather than a synthetic single-bot override.
    foreach ($fixture in @('gameplay', 'regression', 'multibot')) {
        Reset-Database
        $overlay = Join-Path $root "server/compose.playerbot-$fixture.yaml"
        Invoke-Compose -Overlay $overlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
        $names = if ($fixture -eq 'multibot') { @('Bot One', 'Bot Two') } else { @('Bot One') }
        Assert-Roster $names
        Assert-Sql "SELECT COUNT(*) FROM players WHERE name IN ('Bot One', 'Bot Two', 'Bot Three', 'Bot Four');" "$($names.Count)"
        Assert-Sql "SELECT COUNT(*) FROM accounts WHERE name IN ('bot-one', 'bot-two', 'bot-three', 'bot-four');" "$($names.Count)"
    }

    foreach ($identity in @(
        @{ Name = 'Bot Two'; Account = 'bot-two'; Preceding = @('Bot One') },
        @{ Name = 'Bot Three'; Account = 'bot-three'; Preceding = @('Bot One', 'Bot Two') },
        @{ Name = 'Bot Four'; Account = 'bot-four'; Preceding = @('Bot One', 'Bot Two', 'Bot Three') }
    )) {
        foreach ($deleted in @($false, $true)) {
            Reset-Database
            $name = $identity.Name
            $account = if ($deleted) { $identity.Account } else { 'unrelated' }
            $deletion = [int]$deleted
            Invoke-Sql @"
INSERT INTO accounts (name, password) VALUES ('$account', SHA1('$account'));
INSERT INTO players (name, account_id, deletion)
SELECT '$name', id, $deletion FROM accounts WHERE name = '$account';
"@ | Out-Null
            $before = Invoke-Sql "SELECT CONCAT_WS('|', id, account_id, deletion) FROM players WHERE name = '$name';"
            Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
                -ExpectFailure -ErrorPattern "$name identity collision or deletion" | Out-Null
            Assert-Roster $identity.Preceding
            Assert-Sql "SELECT CONCAT_WS('|', id, account_id, deletion) FROM players WHERE name = '$name';" $before
            Assert-Sql "SELECT COUNT(*) FROM player_items JOIN players ON players.id = player_items.player_id WHERE players.name = '$name';" '0'
        }
    }
    Write-Host 'Playerbot provisioning: four seeds/loadouts, saved Paladin progress, fixture isolation, import failures and identity refusals passed.'
} finally {
    if ($started) {
        Invoke-Compose -Arguments @('down', '--volumes', '--remove-orphans') | Out-Null
    }
    if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
}
