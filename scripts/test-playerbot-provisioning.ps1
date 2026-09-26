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
    param([int]$Count)
    Assert-Sql 'SELECT COUNT(*) FROM player_bots;' "$Count"
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
    Set-Content -Path $badOverlay -Encoding utf8 -Value @"
services:
  playerbot-setup:
    volumes:
      - type: bind
        source: $source
        target: /schema/insertBotTwo.sql
        read_only: true
"@
    $fixtureOverlay = Join-Path $temp 'single-bot.yaml'
    Set-Content -Path $fixtureOverlay -Encoding utf8 -Value @'
services:
  playerbot-setup:
    environment:
      PLAYERBOT_SEED_BOT_TWO: "false"
'@
    Invoke-Compose -Arguments @('config', '--quiet') -Overlay $badOverlay | Out-Null
    $mount = Invoke-Compose -Arguments @('config', '--format', 'json') -Overlay $badOverlay | ConvertFrom-Json
    if (-not @($mount.services.'playerbot-setup'.volumes | Where-Object target -eq '/schema/insertBotTwo.sql' |
        Where-Object source -eq $badSql).Count) {
        throw 'Invalid SQL override did not replace the Bot Two mount.'
    }

    $started = $true
    Start-Database
    Invoke-Compose -Overlay $badOverlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
        -ExpectFailure -ErrorPattern 'playerbot_provisioning_missing_table' | Out-Null
    Assert-Roster 1
    Invoke-Compose -Overlay $fixtureOverlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    Assert-Roster 1

    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    Assert-Roster 2
    $rosterQuery = @'
SELECT CONCAT_WS('|', players.name, accounts.name, players.id, players.deletion,
    players.level, players.vocation, players.town_id, players.posx, players.posy, players.posz,
    players.lookbody, players.lookfeet, players.lookhead, players.looklegs, players.looktype,
    players.balance, players.skill_sword, players.skill_shielding)
FROM player_bots JOIN players ON players.id = player_bots.player_id
JOIN accounts ON accounts.id = players.account_id ORDER BY players.name;
'@
    $inventoryQuery = @'
SELECT CONCAT_WS('|', players.name, player_items.pid, player_items.sid, player_items.itemtype, player_items.count)
FROM player_items JOIN players ON players.id = player_items.player_id
WHERE players.name IN ('Bot One', 'Bot Two') ORDER BY players.name, player_items.sid;
'@
    $roster = Invoke-Sql $rosterQuery
    $lines = @($roster -split "`n")
    if ($lines.Count -ne 2) { throw "Expected two registered bots: $roster" }
    $one = $lines[0] -split '\|'
    $two = $lines[1] -split '\|'
    if ($one[0] -ne 'Bot One' -or $two[0] -ne 'Bot Two' -or
        $one[1] -ne 'bot-one' -or $two[1] -ne 'bot-two' -or $one[2] -eq $two[2] -or
        $one[3] -ne '0' -or $two[3] -ne '0' -or
        ($one[4..5] -join ',') -ne '8,4' -or ($two[4..5] -join ',') -ne '8,4' -or
        ($one[6..9] -join ',') -ne '4,32360,31782,7' -or
        ($two[6..9] -join ',') -ne '2,32369,32241,7' -or
        ($one[10..17] -join ',') -ne ($two[10..17] -join ',') -or
        ($one[15..17] -join ',') -ne '1000,20,20') {
        throw "New bot identities or starting attributes differ: $roster"
    }
    $gearQuery = @'
SELECT CONCAT_WS('|', players.name,
    GROUP_CONCAT(CONCAT(IF(player_items.pid BETWEEN 1 AND 10, player_items.pid, 99), ':',
        player_items.itemtype, ':', player_items.count)
        ORDER BY IF(player_items.pid BETWEEN 1 AND 10, player_items.pid, 99), player_items.itemtype SEPARATOR ','))
FROM player_items JOIN players ON players.id = player_items.player_id
WHERE players.name IN ('Bot One', 'Bot Two') GROUP BY players.id ORDER BY players.name;
'@
    $gear = @((Invoke-Sql $gearQuery) -split "`n")
    if ($gear.Count -ne 2 -or ($gear[0] -split '\|', 2)[1] -ne ($gear[1] -split '\|', 2)[1]) {
        throw "Starting gear differs: $gear"
    }
    $inventory = Invoke-Sql $inventoryQuery
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') | Out-Null
    if ((Invoke-Sql $rosterQuery) -ne $roster -or (Invoke-Sql $inventoryQuery) -ne $inventory) {
        throw 'Provisioning was not idempotent.'
    }
    Invoke-Compose -Overlay $fixtureOverlay -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
        -ExpectFailure -ErrorPattern 'Single-bot fixture requires only Bot One registered' | Out-Null
    Assert-Roster 2

    Reset-Database
    Invoke-Sql @'
INSERT INTO accounts (name, password) VALUES ('unrelated', SHA1('unrelated'));
INSERT INTO players (name, account_id) SELECT 'Bot Two', id FROM accounts WHERE name = 'unrelated';
'@ | Out-Null
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
        -ExpectFailure -ErrorPattern 'Bot Two identity collision or deletion' | Out-Null
    Assert-Roster 1
    Assert-Sql "SELECT accounts.name FROM players JOIN accounts ON accounts.id = players.account_id WHERE players.name = 'Bot Two';" 'unrelated'

    Reset-Database
    Invoke-Sql @'
INSERT INTO accounts (name, password) VALUES ('bot-two', SHA1('bot-two'));
INSERT INTO players (name, account_id, deletion) SELECT 'Bot Two', id, 1 FROM accounts WHERE name = 'bot-two';
'@ | Out-Null
    Invoke-Compose -Arguments @('run', '--rm', '--no-deps', 'playerbot-setup') `
        -ExpectFailure -ErrorPattern 'Bot Two identity collision or deletion' | Out-Null
    Assert-Roster 1
    Assert-Sql "SELECT deletion FROM players WHERE name = 'Bot Two';" '1'
    Write-Host 'Playerbot provisioning: import failure, fixture guard, two identities, idempotence, wrong account and deleted character passed.'
} finally {
    if ($started) {
        Invoke-Compose -Arguments @('down', '--volumes', '--remove-orphans') | Out-Null
    }
    if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
}
