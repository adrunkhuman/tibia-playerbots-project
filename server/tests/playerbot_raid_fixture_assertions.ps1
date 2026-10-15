#Requires -Version 7.0
# Run from repository root: pwsh -NoProfile -File server/tests/playerbot_raid_fixture_assertions.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root/scripts/playerbot-gameplay/log-parsing.ps1"
. "$root/scripts/playerbot-gameplay/assertions-raid.ps1"

function New-RaidEvents([string]$Mode) {
    @(
        @{event='raid_fixture';phase='start';x=60160;y=60200;z=7;health=185;health_max=185;goal_x=60208;goal_y=60200;goal_z=7;elapsed_ms=0}
        @{event='lifecycle';status='online';recovered=$false}
        @{event='raid_fixture';phase='activated';x=60168;y=60200;z=7;health=185;spawn_x=60169;spawn_y=60200;spawn_z=7;elapsed_ms=3000}
        @{event='travel_safety';action='withdraw';danger=0.108;reason='unsafe_travel'}
        @{event='raid_fixture';phase='spawned';x=60168;y=60200;z=7;health=185;target_id=99;count=1;elapsed_ms=4000}
        @{event='raid_fixture';phase='withdrew';x=60166;y=60200;z=7;health=185;target_id=99;from_x=60168;elapsed_ms=5000}
        if ($Mode -eq 'raid_recovery') {
            @{event='raid_fixture';phase='kill_injected';x=60166;y=60200;z=7;health=185;elapsed_ms=5000}
            @{event='raid_fixture';phase='death';x=60166;y=60200;z=7;health=0;intentional=$true;elapsed_ms=5000}
            @{event='lifecycle';status='dead'}
            @{event='terminal';reason='controlled_player_dead'}
            @{event='lifecycle';status='recovery_scheduled'}
            @{event='raid_fixture';phase='recovered';x=60160;y=60200;z=7;health=175;target_id=99;elapsed_ms=6000}
            @{event='lifecycle';status='online';recovered=$true}
            @{event='travel_safety';action='defer';danger=0.108;reason='route_unsafe'}
        }
        @{event='raid_fixture';phase='guarded';x=60162;y=60200;z=7;health=175;target_id=99;guard_ms=5000;elapsed_ms=11000}
        @{event='raid_fixture';phase='removed';x=60162;y=60200;z=7;health=175;target_id=99;elapsed_ms=11000}
        @{event='travel_safety';action='resume';danger=0;reason='observed_clearance'}
        @{event='raid_fixture';phase='arrived';x=60208;y=60200;z=7;health=175;deaths=[int]($Mode -eq 'raid_recovery');elapsed_ms=30000}
    )
}
function Test-RaidEvents([array]$Events, [string]$Mode, [bool]$Reject=$false, [bool]$Pass=$true) {
    $logs = ($Events | ForEach-Object {
        $_.component='playerbot'; if (-not $_.bot) { $_.bot='Bot One' }
        if ($_.event -eq 'raid_fixture') { $_.scenario=$Mode; $_.source='lua_position_verifier' }
        $_ | ConvertTo-Json -Compress -Depth 8
    }) -join "`n"
    if ($Pass) { $logs += "`nPLAYERBOT_GAMEPLAY_TEST $($Mode.ToUpperInvariant())_PASS" }
    try { Assert-RaidTravelEvents -Logs $logs -Mode $Mode }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Incomplete/invalid raid evidence passed.' }
}
foreach ($mode in @('raid_travel','raid_recovery')) {
    $events = New-RaidEvents $mode
    Test-RaidEvents $events $mode
    Test-RaidEvents $events $mode $true $false
    # All independent observations and all production safety decisions are needed.
    $required = @($events | Where-Object { $_.event -in @('raid_fixture','travel_safety') -or
        ($_.event -eq 'lifecycle' -and ($_.status -eq 'dead' -or $_.recovered -eq $true)) })
    foreach ($event in $required) {
        Test-RaidEvents @($events | Where-Object { $_ -ne $event }) $mode $true
    }
    foreach ($field in @('reason','danger')) {
        $broken = New-RaidEvents $mode
        ($broken | Where-Object { $_.action -eq 'withdraw' }).Remove($field)
        Test-RaidEvents $broken $mode $true
    }
    foreach ($danger in @(0, 0.08)) {
        $broken = New-RaidEvents $mode
        ($broken | Where-Object { $_.action -eq 'withdraw' }).danger = $danger
        Test-RaidEvents $broken $mode $true
    }
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'withdrew' }).x = 60168
    # Even a production "completed" step cannot replace observed displacement.
    Test-RaidEvents @($broken + @{event='travel_safety_step';result='completed'}) $mode $true
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'guarded' }).guard_ms = 4999
    Test-RaidEvents $broken $mode $true
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'removed' }).target_id = 100
    Test-RaidEvents $broken $mode $true
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'arrived' }).elapsed_ms = 10999
    Test-RaidEvents $broken $mode $true
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'activated' }).x = 60160
    Test-RaidEvents $broken $mode $true
    $broken = New-RaidEvents $mode
    ($broken | Where-Object { $_.phase -eq 'spawned' }).count = 2
    Test-RaidEvents $broken $mode $true
    Test-RaidEvents @($events + @{event='lifecycle';status='dead'}) $mode $true
    Test-RaidEvents @($events + @{event='terminal';reason='navigation_failed'}) $mode $true
    Test-RaidEvents @($events + @{event='lifecycle';status='online';bot='Bot Two'}) $mode $true
    if ($mode -eq 'raid_recovery') {
        $broken = New-RaidEvents $mode
        ($broken | Where-Object { $_.phase -eq 'death' }).intentional = $false
        Test-RaidEvents $broken $mode $true
        $broken = New-RaidEvents $mode
        ($broken | Where-Object { $_.phase -eq 'recovered' }).target_id = 100
        Test-RaidEvents $broken $mode $true
    }
}

# Exercise catalog wiring without Docker. The raid-specific mount and speed
# override must not leak on success OR failure; generic runtime owns other envs.
$script:RaidTravel = $true
$script:projectRoot = $root
$script:ran = @()
$script:failWait = $false
$script:waitPattern = $null
$originalCatalog = $env:PLAYERBOT_RAID_CATALOG
$originalSpeed = $env:PLAYERBOT_SPEED_BONUS
function Invoke-Scenario { param($Name, $DefaultTimeoutSeconds, [scriptblock]$Body)
    if ($DefaultTimeoutSeconds -ne 180) { throw 'Raid deadline wiring changed.' }
    $script:ran += $Name
    & $Body
}
function Invoke-Compose {
    if ($args[0] -eq 'up' -and ($env:PLAYERBOT_RAID_CATALOG -ne './tests/playerbot-gameplay/raids/raids.xml' -or
        $env:PLAYERBOT_SPEED_BONUS -ne '0' -or $env:PLAYERBOT_HUNT_DURATION_SECONDS -ne '900')) {
        throw 'Raid scenario did not isolate its catalog/speed/duration.'
    }
}
function Wait-ForLog { param($Pattern)
    $script:waitPattern = $Pattern
    if ($script:failWait) { throw 'deliberate waiter failure' }
    return 'stub logs'
}
function Assert-RaidTravelEvents { param($Logs, $Mode)
    if ($Mode -ne $env:PLAYERBOT_GAMEPLAY_MODE -or $Logs -ne 'stub logs') { throw 'Wrong raid assertion wiring.' }
}
$restore = @{}
foreach ($name in @('PLAYERBOT_GAMEPLAY_MODE','PLAYERBOT_HUNT_DURATION_SECONDS','PLAYERBOT_RELOG_DELAY_SECONDS','PLAYERBOT_MAX_CONSECUTIVE_DEATHS')) {
    $restore[$name] = [Environment]::GetEnvironmentVariable($name)
}
try {
    $env:PLAYERBOT_RAID_CATALOG='incoming-catalog'; $env:PLAYERBOT_SPEED_BONUS='123'
    . "$root/scripts/playerbot-gameplay/scenarios-raid.ps1"
    if (($ran -join ',') -ne 'raid_travel,raid_recovery' -or $env:PLAYERBOT_RAID_CATALOG -ne 'incoming-catalog' -or
        $env:PLAYERBOT_SPEED_BONUS -ne '123') { throw 'Raid scenario selection/environment leaked.' }
    if ('{"event":"terminal","reason":"controlled_player_dead"}' -match $waitPattern -or
        '{"event":"terminal","reason":"navigation_failed"}' -notmatch $waitPattern) {
        throw 'Raid waiter mishandles intentional controller replacement.'
    }
    $script:failWait=$true
    $failed=$false
    try { . "$root/scripts/playerbot-gameplay/scenarios-raid.ps1" }
    catch { if ($_.Exception.Message -ne 'deliberate waiter failure') { throw }; $failed=$true }
    if (-not $failed -or $env:PLAYERBOT_RAID_CATALOG -ne 'incoming-catalog' -or $env:PLAYERBOT_SPEED_BONUS -ne '123') {
        throw 'Failed raid scenario leaked its environment.'
    }
}
finally {
    $env:PLAYERBOT_RAID_CATALOG=$originalCatalog; $env:PLAYERBOT_SPEED_BONUS=$originalSpeed
    foreach ($name in $restore.Keys) { [Environment]::SetEnvironmentVariable($name, $restore[$name]) }
}
Write-Host 'Playerbot raid fixture assertions and scenario wiring passed.'
