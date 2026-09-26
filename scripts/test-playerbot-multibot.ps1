#Requires -Version 7.0
<#
.SYNOPSIS
Runs bounded, disposable two-bot integration checks on Compose project angelion.
.DESCRIPTION
Refuses an already-running angelion stack. Resets the database for each isolated
case, but recreates only the server (with --no-deps) for the persistence check.
The caller must grant Docker access; this script never uses sudo.
Use -Case shared_npc -SkipBuild to rerun just the Gregor fixture with an existing image.
-Case persist_verify always runs both prime and server-only restart phases.
#>
[CmdletBinding()]
param(
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 180,
    [switch]$SkipBuild,
    [ValidateSet('startup', 'death_one', 'cancel_recovery', 'remove_one', 'remove_manager',
        'cancel_pending', 'reject_two', 'shared_npc', 'persist_verify', 'queued_death', 'pvp_remove')]
    [string[]]$Case,
    [switch]$KeepStack,
    [string]$ArtifactsPath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ArtifactsPath) { $ArtifactsPath = Join-Path $root 'artifacts/playerbot-multibot' }
$compose = @('compose', '-f', (Join-Path $root 'server/compose.yaml'), '-f', (Join-Path $root 'server/compose.playerbot-multibot.yaml'))
$oldMode = $env:PLAYERBOT_MULTIBOT_CASE
$oldGameplayMode = $env:PLAYERBOT_GAMEPLAY_MODE
$oldRelogDelay = $env:PLAYERBOT_RELOG_DELAY_SECONDS
$loginHelper = Join-Path $PSScriptRoot 'test-playerbot-login.py'
$owned = $false
$currentCase = 'preflight'
$logs = ''
$allCases = @('startup', 'death_one', 'cancel_recovery', 'remove_one', 'remove_manager',
    'cancel_pending', 'reject_two', 'shared_npc', 'persist_verify', 'queued_death', 'pvp_remove')
$selectedCases = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $(if ($Case) { $Case } else { $allCases })) { [void]$selectedCases.Add($name) }

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
    $output = @(& docker @compose @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "docker compose $($Arguments -join ' ') failed: $($output -join [Environment]::NewLine)" }
    return $output
}
function Read-ServerLogs {
    $lines = @(Invoke-Compose logs --no-log-prefix server)
    $text = $lines -join "`n"
    if ($text.Length -gt 8MB) { throw 'Server logs exceeded the 8 MiB test budget.' }
    return $text
}
function Events {
    param([string]$Text)
    foreach ($line in ($Text -split "`n")) {
        if (-not $line.StartsWith('{')) { continue }
        try {
            $entry = $line | ConvertFrom-Json -ErrorAction Stop
            if ($entry.component -eq 'playerbot') { $entry }
        } catch { throw "Invalid playerbot JSONL event: $line" }
    }
}
function Wait-For {
    param([scriptblock]$Condition, [string]$Description)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $script:logs = Read-ServerLogs
        if (& $Condition $script:logs) { return }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description ($currentCase, ${TimeoutSeconds}s)."
}
function Check {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Assert-Events {
    param([string]$Text, [string]$Case)
    $events = @(Events $Text)
    $online = @($events | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'online' })
    $one = @($online | Where-Object { $_.bot -eq 'Bot One' })
    $two = @($online | Where-Object { $_.bot -eq 'Bot Two' })
    Check ($one.Count -ge 1) "${Case}: Bot One never became online."
    Check (@($events | Where-Object { $_.bot -notin @('Bot One','Bot Two') -and $_.player_id -ne 0 }).Count -eq 0) "${Case}: unexpected roster identity."
    Check (@($events | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'startup_failed' -and $_.bot -eq 'Bot One' }).Count -eq 0) "${Case}: Bot One failed to start."
    if ($Case -eq 'reject_two') {
        Check ($two.Count -eq 0 -and $Text.Contains('PLAYERBOT_MULTIBOT REJECT_TWO_PASS')) 'Rejected Bot Two became online or login hook did not run.'
        Check (@($events | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'lifecycle' -and $_.status -eq 'startup_failed' }).Count -ge 1) 'Rejected Bot Two did not report startup failure.'
    } elseif ($Case -eq 'cancel_pending') {
        Check ($two.Count -eq 0 -and -not $Text.Contains('PLAYERBOT_MULTIBOT LOGIN_PASS Bot Two')) 'Cancelled pending Bot Two still activated.'
    } else {
        Check ($two.Count -ge 1) "${Case}: Bot Two never became online."
        $first = $one[0]; $second = $two[0]
        Check ($first.player_id -gt 0 -and $second.player_id -gt 0 -and $first.player_id -ne $second.player_id) "${Case}: player GUIDs collide."
        Check ($first.controller_id -and $second.controller_id -and $first.controller_id -ne $second.controller_id) "${Case}: controller IDs collide."
        Check ($first.server_run_id -eq $second.server_run_id) "${Case}: startup spanned different server runs."
        $gap = ([DateTimeOffset]::Parse($second.ts) - [DateTimeOffset]::Parse($first.ts)).TotalMilliseconds
        Check ($gap -ge 1250) "${Case}: second activation was not staggered (gap ${gap}ms)."
    }
    foreach ($group in @($events | Where-Object { $_.controller_id } | Group-Object server_run_id, controller_id)) {
        $ordered = @($group.Group | Sort-Object sequence)
        $terminals = @($ordered | Where-Object { $_.event -eq 'terminal' })
        Check ($terminals.Count -le 1) "${Case}: duplicate terminal event for $($group.Name)."
        if ($terminals.Count -eq 1) {
            Check ($ordered[-1].sequence -eq $terminals[0].sequence) "${Case}: event emitted after terminal for $($group.Name)."
        }
    }
    return ,$events
}
function Assert-SurvivorTurn {
    param([string]$Text, [string]$Marker, [string]$Bot)
    $offset = $Text.IndexOf($Marker, [StringComparison]::Ordinal)
    Check ($offset -ge 0) "Missing intervention marker: $Marker"
    # Timer-based existence checks can run before the survivor's next turn.
    Wait-For {
        param($s)
        $position = $s.IndexOf($Marker, [StringComparison]::Ordinal)
        if ($position -lt 0) { return $false }
        return @(Events $s.Substring($position + $Marker.Length) | Where-Object {
            $_.bot -eq $Bot -and $_.event -in @('action_result', 'hunt_planning_slice')
        }).Count -gt 0
    } "$Bot executing a turn after $Marker"
}
function Invoke-LoginProbe {
    param([string]$Character, [string]$Account, [string]$Expectation)
    $output = @(& python3 $loginHelper --character $Character --account $Account --password $Account --expect $Expectation 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Network login probe failed for $Character ($Expectation): $($output -join '; ')" }
    "PLAYERBOT_MULTIBOT_TEST LOGIN_PASS character=$Character expect=$Expectation"
}
function Fresh-Case {
    param([string]$Name)
    $script:currentCase = $Name
    $env:PLAYERBOT_MULTIBOT_CASE = $Name
    Invoke-Compose down --volumes --remove-orphans | Out-Null
    Invoke-Compose up --detach | Out-Null
}
function Save-Case {
    param([string]$Name)
    New-Item -ItemType Directory -Force -Path $ArtifactsPath | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $ArtifactsPath "$Name.log"), $script:logs)
}

try {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'Docker is required.' }
    & docker info *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Docker daemon is unavailable or access is denied; grant access in the caller shell.' }
    $existing = @(& docker ps -a --filter 'label=com.docker.compose.project=angelion' --format '{{.ID}}')
    if ($LASTEXITCODE -ne 0) { throw 'Could not check whether Compose project angelion is already in use.' }
    if ($existing.Count -gt 0 -and @($existing | Where-Object { $_ }).Count -gt 0) {
        throw 'Compose project angelion already has containers. Stop it yourself before running this destructive disposable test.'
    }
    foreach ($command in @('python3', 'openssl')) {
        if (-not (Get-Command $command -ErrorAction SilentlyContinue)) { throw "$command is required for the network takeover probe." }
    }
    if (-not (Test-Path $loginHelper) -or -not (Test-Path (Join-Path $root 'server/key.pem'))) {
        throw 'The network login helper and the server RSA key are required.'
    }
    $codec = @(& python3 $loginHelper --self-test 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Network login codec self-test failed: $($codec -join '; ')" }
    $env:PLAYERBOT_GAMEPLAY_MODE = ''
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = '1'
    Invoke-Compose config --quiet | Out-Null
    if ($SkipBuild) {
        & docker image inspect angelion-server:latest *> $null
        if ($LASTEXITCODE -ne 0) { throw '-SkipBuild requires angelion-server:latest.' }
    } else {
        Invoke-Compose build server | Out-Null
    }
    $owned = $true

    if ($selectedCases.Contains('startup')) {
    Fresh-Case startup
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT LOGIN_PASS Bot One') -and $s.Contains('PLAYERBOT_MULTIBOT LOGIN_PASS Bot Two') -and @(Events $s | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -ge 2 } 'both online bots'
    $startup = Assert-Events $logs startup
    Invoke-LoginProbe 'GOD Admin' admin login
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Invoke-Compose stop server | Out-Null
    $logs = Read-ServerLogs
    $shutdown = Assert-Events $logs startup
    foreach ($name in @('Bot One', 'Bot Two')) {
        $controller = @($startup | Where-Object { $_.bot -eq $name -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' })[0].controller_id
        Check (@($shutdown | Where-Object { $_.bot -eq $name -and $_.controller_id -eq $controller -and $_.event -eq 'terminal' }).Count -eq 1) "$name did not terminate exactly once at shutdown."
    }
    Save-Case startup
    'PLAYERBOT_MULTIBOT_TEST PASS startup'
    }

    if ($selectedCases.Contains('death_one')) {
    Fresh-Case death_one
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT RECOVER_LOGIN_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS death_one') -and @(Events $s | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' -and $_.recovered -eq $true }).Count -ge 1 } 'Bot One recovery'
    $death = Assert-Events $logs death_one
    $first = @($death | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' })
    $other = @($death | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' })
    Check ($first.Count -eq 2 -and $first[0].controller_id -ne $first[1].controller_id -and $first[1].recovered -eq $true) 'Bot One did not get an independent recovered controller.'
    Check ($other.Count -eq 1 -and @($death | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'Bot Two was interrupted by Bot One death.'
    Check (@($death | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'dead' }).Count -eq 1) 'Bot One death was not recorded once.'
    Check (@($death | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' -and $_.reason -eq 'controlled_player_dead' -and $_.controller_id -eq $first[0].controller_id }).Count -eq 1) 'Dead controller did not terminate once.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT KILL_ONE_PASS' 'Bot Two'
    Save-Case death_one
    'PLAYERBOT_MULTIBOT_TEST PASS death_one'
    }

    if ($selectedCases.Contains('queued_death')) {
    Fresh-Case queued_death
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT DEAD_TURN_PASS') -and
        $s.Contains('PLAYERBOT_MULTIBOT QUEUED_RECOVER_LOGIN_PASS') -and
        $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS queued_death') -and
        @(Events $s | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' -and $_.recovered -eq $true }).Count -eq 1
    } 'engine death after an earlier queued controller turn'
    $queued = Assert-Events $logs queued_death
    $dead = @($queued | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'dead' })
    Check ($dead.Count -eq 1 -and $dead[0].killer_name -eq 'Bot Two') 'Queued turn stole the authoritative killer or lost the death event.'
    Check (@($queued | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'recovery_scheduled' }).Count -eq 1) 'Queued turn prevented Bot One recovery.'
    Check (@($queued | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' -and $_.reason -eq 'controlled_player_dead' }).Count -eq 1) 'Queued turn caused duplicate or missing terminal events.'
    Check (@($queued | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'Queued death interrupted Bot Two.'
    Save-Case queued_death
    'PLAYERBOT_MULTIBOT_TEST PASS queued_death'
    }

    if ($selectedCases.Contains('pvp_remove')) {
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = '5'
    Fresh-Case pvp_remove
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT DEAD_TURN_PASS') -and
        $s.Contains('PLAYERBOT_MULTIBOT PVP_RESTORED_PASS') -and
        $s.Contains('PLAYERBOT_MULTIBOT PVP_REMOVE_PASS') -and
        $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS pvp_remove')
    } 'non-corpse PVP death and later external removal'
    Start-Sleep -Seconds 6 # Past the relog deadline measured from the arena death.
    $logs = Read-ServerLogs
    $pvp = Assert-Events $logs pvp_remove
    Check (@($pvp | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'dead' -and $_.killer_name -eq 'Bot Two' }).Count -eq 1) 'PVP death lost killer telemetry.'
    $scheduled = @($pvp | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'recovery_scheduled' -and $_.reason -eq 'death' })
    Check ($scheduled.Count -eq 1 -and $scheduled[0].delay_ms -eq 5000) 'PVP death did not schedule five-second recovery.'
    Check (@($pvp | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'removed' }).Count -eq 1) 'External removal did not cancel PVP recovery.'
    Check (@($pvp | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -eq 1) 'Externally removed PVP bot relogged.'
    Check (@($pvp | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'PVP removal stopped Bot Two.'
    $afterRemoval = @(Events $logs.Substring($logs.IndexOf('PLAYERBOT_MULTIBOT PVP_REMOVE_PASS')))
    Check (@($afterRemoval | Where-Object { $_.bot -eq 'Bot One' }).Count -eq 0) 'PVP bot emitted after removal.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT PVP_REMOVE_PASS' 'Bot Two'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Save-Case pvp_remove
    'PLAYERBOT_MULTIBOT_TEST PASS pvp_remove'
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = '1'
    }

    if ($selectedCases.Contains('cancel_recovery')) {
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = '5'
    Fresh-Case cancel_recovery
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT CANCEL_RECOVERY_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS cancel_recovery') } 'pending recovery removal and survivor'
    Start-Sleep -Seconds 8 # Past the five-second relog deadline measured from death.
    $logs = Read-ServerLogs
    $cancelledRecovery = Assert-Events $logs cancel_recovery
    $scheduled = @($cancelledRecovery | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'recovery_scheduled' -and $_.reason -eq 'death' })
    Check ($scheduled.Count -eq 1 -and $scheduled[0].delay_ms -eq 5000) 'Bot One did not enter a five-second pending recovery.'
    Check ($logs.IndexOf('"status":"recovery_scheduled"') -lt $logs.IndexOf('PLAYERBOT_MULTIBOT CANCEL_RECOVERY_PASS')) 'Manager removal did not follow recovery scheduling.'
    Check (@($cancelledRecovery | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -eq 1) 'Removed bot relogged after its recovery deadline.'
    $afterRemoval = @(Events $logs.Substring($logs.IndexOf('PLAYERBOT_MULTIBOT CANCEL_RECOVERY_PASS')))
    Check (@($afterRemoval | Where-Object { $_.bot -eq 'Bot One' }).Count -eq 0) 'Removed bot emitted an event after cancellation.'
    Check (@($cancelledRecovery | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' -and $_.reason -eq 'controlled_player_dead' }).Count -eq 1) 'Dying controller did not terminate once.'
    Check (@($cancelledRecovery | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'Pending recovery removal stopped Bot Two.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT CANCEL_RECOVERY_PASS' 'Bot Two'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Save-Case cancel_recovery
    'PLAYERBOT_MULTIBOT_TEST PASS cancel_recovery'
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = '1'
    }

    if ($selectedCases.Contains('remove_one')) {
    Fresh-Case remove_one
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT REMOVE_ONE_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS remove_one') -and @(Events $s | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' }).Count -ge 1 } 'external removal and terminal'
    Start-Sleep -Seconds 3 # Catch cancelled callbacks already queued on the dispatcher.
    $logs = Read-ServerLogs
    $removed = Assert-Events $logs remove_one
    Check (@($removed | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' -and $_.reason -eq 'controlled_player_removed' }).Count -eq 1) 'External removal did not terminate Bot One once.'
    Check (@($removed | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'removed' }).Count -eq 1) 'External removal was not isolated.'
    Check (@($removed | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'External removal terminated Bot Two.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT REMOVE_ONE_PASS' 'Bot Two'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Save-Case remove_one
    'PLAYERBOT_MULTIBOT_TEST PASS remove_one'
    }

    if ($selectedCases.Contains('remove_manager')) {
    Fresh-Case remove_manager
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT REMOVE_MANAGER_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS remove_manager') -and @(Events $s | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' }).Count -ge 1 } 'manager removal'
    Start-Sleep -Seconds 3
    $logs = Read-ServerLogs
    $managerRemoved = Assert-Events $logs remove_manager
    Check (@($managerRemoved | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' -and $_.reason -eq 'manager_removed' }).Count -eq 1) 'Manager remove did not terminate Bot One.'
    Check (@($managerRemoved | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'terminal' }).Count -eq 0) 'Manager remove terminated Bot Two.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT REMOVE_MANAGER_PASS' 'Bot Two'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Save-Case remove_manager
    'PLAYERBOT_MULTIBOT_TEST PASS remove_manager'
    }

    if ($selectedCases.Contains('cancel_pending')) {
    Fresh-Case cancel_pending
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT CANCEL_PENDING_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS cancel_pending') } 'pending activation cancellation'
    Start-Sleep -Seconds 3 # Past the second activation deadline (1.5s).
    $logs = Read-ServerLogs
    $pending = Assert-Events $logs cancel_pending
    Check (@($pending | Where-Object { $_.bot -eq 'Bot Two' }).Count -eq 0) 'Cancelled pending Bot Two emitted telemetry.'
    Check (@($pending | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'terminal' }).Count -eq 0) 'Pending cancellation stopped Bot One.'
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT CANCEL_PENDING_PASS' 'Bot One'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Save-Case cancel_pending
    'PLAYERBOT_MULTIBOT_TEST PASS cancel_pending'
    }

    if ($selectedCases.Contains('reject_two')) {
    Fresh-Case reject_two
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT REJECT_TWO_PASS') -and $s.Contains('PLAYERBOT_MULTIBOT SURVIVOR_PASS reject_two') -and @(Events $s | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -ge 1 -and @(Events $s | Where-Object { $_.bot -eq 'Bot Two' -and $_.event -eq 'lifecycle' -and $_.status -eq 'startup_failed' }).Count -ge 1 } 'isolated login rejection'
    $rejected = Assert-Events $logs reject_two
    Assert-SurvivorTurn $logs 'PLAYERBOT_MULTIBOT REJECT_TWO_PASS' 'Bot One'
    Invoke-LoginProbe 'Bot One' bot-one reject
    Invoke-LoginProbe 'Bot Two' bot-two reject
    Save-Case reject_two
    'PLAYERBOT_MULTIBOT_TEST PASS reject_two'
    }

    if ($selectedCases.Contains('shared_npc')) {
        Fresh-Case shared_npc
        Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT SHARED_NPC_FIRST_PASS') -and
            $s.Contains('PLAYERBOT_MULTIBOT SHARED_NPC_PASS') -and
            @(Events $s | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -ge 2
        } 'independent spell purchases through Gregor'
        $shared = Assert-Events $logs shared_npc
        Check (@($shared | Where-Object { $_.event -eq 'terminal' }).Count -eq 0) 'Shared Gregor fixture stopped a bot.'
        Save-Case shared_npc
        'PLAYERBOT_MULTIBOT_TEST PASS shared_npc'
    }

    if ($selectedCases.Contains('persist_verify')) {
    Fresh-Case persist_prime
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT PRIME_PASS Bot One') -and $s.Contains('PLAYERBOT_MULTIBOT PRIME_PASS Bot Two') -and @(Events $s | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -ge 2 } 'both persistence markers'
    $prime = Assert-Events $logs persist_prime
    Save-Case persist_prime
    # Recreate only server: provisioning can refill inventory and mask save/reload defects.
    $currentCase = 'persist_verify'
    $env:PLAYERBOT_MULTIBOT_CASE = 'persist_verify'
    Invoke-Compose up --detach --no-deps --force-recreate server | Out-Null
    Wait-For { param($s) $s.Contains('PLAYERBOT_MULTIBOT PERSIST_PASS Bot One') -and $s.Contains('PLAYERBOT_MULTIBOT PERSIST_PASS Bot Two') -and @(Events $s | Where-Object { $_.event -eq 'lifecycle' -and $_.status -eq 'online' }).Count -ge 2 } 'independent saved inventories and storage'
    $verified = Assert-Events $logs persist_verify
    Check ($verified[0].server_run_id -ne $prime[0].server_run_id) 'Server was not recreated for the persistence test.'
    foreach ($name in @('Bot One','Bot Two')) {
        $before = @($prime | Where-Object { $_.bot -eq $name -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' })[0]
        $after = @($verified | Where-Object { $_.bot -eq $name -and $_.event -eq 'lifecycle' -and $_.status -eq 'online' })[0]
        Check ($before.player_id -eq $after.player_id) "$name changed persistent GUID on restart."
    }
    Save-Case persist_verify
    'PLAYERBOT_MULTIBOT_TEST PASS persist_verify'
    }
}
catch {
    try { if ($owned) { $logs = Read-ServerLogs; Save-Case "failure-$currentCase" } } catch { Write-Warning "Could not capture failure logs: $_" }
    throw
}
finally {
    if ($owned -and -not $KeepStack) {
        try { Invoke-Compose down --volumes --remove-orphans | Out-Null } catch { Write-Warning "Could not clean disposable stack: $_" }
    }
    $env:PLAYERBOT_MULTIBOT_CASE = $oldMode
    $env:PLAYERBOT_GAMEPLAY_MODE = $oldGameplayMode
    $env:PLAYERBOT_RELOG_DELAY_SECONDS = $oldRelogDelay
}
