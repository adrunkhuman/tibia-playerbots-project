#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-magic-training.ps1

function New-RestartFixture([string]$Reason = 'spell_cooldown') {
    @(
        @{ event = 'magic_training_fixture'; source = 'authoritative_forecast'; case = 'active_default'; active = $true; gain = 5; interval = 6000; remaining = 6000 }
        @{ event = 'goal_candidate'; goal = 'magic_training'; decision_id = 1; decision_reason = 'startup'; feasible = $false; reason = $Reason; mana = 980; mana_gain = 5; mana_max = 1000; mana_tick_remaining = 6000; mana_tick_interval = 6000 }
        @{ event = 'goal_selection'; decision_id = 1; decision_reason = 'startup'; to_goal = 'hunt' }
        @{ event = 'lifecycle'; status = 'online' }
    )
}
function Test-RestartFixture([string]$Name, [array]$Events, [bool]$Reject = $false) {
    $logs = ($Events | ForEach-Object {
        $_.component = 'playerbot'; $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Compress
    }) -join "`n"
    try { Assert-MagicTrainingEvents -Logs $logs -Mode restart }
    catch { if ($Reject) { return }; throw "${Name}: $_" }
    if ($Reject) { throw "${Name}: invalid restart unexpectedly passed." }
}
foreach ($reason in @('spell_cooldown', 'next_tick_not_overflow')) {
    Test-RestartFixture $reason (New-RestartFixture $reason)
}
$cast = @{ event = 'action_result'; action = 'magic_training'; result = 'requested' }
$events = @(New-RestartFixture)
Test-RestartFixture 'cast before initial decision' (@($events[0], $cast) + $events[1..3]) $true
Test-RestartFixture 'cast between candidate and selection' ($events[0..1] + $cast + $events[2..3]) $true
$later = @{ event = 'goal_selection'; decision_id = 2; decision_reason = 'hunt_deadline'; to_goal = 'magic_training' }
Test-RestartFixture 'later cast outside startup window' ($events + $later + $cast)
Test-RestartFixture 'duplicate startup candidate' ($events[0..1] + $events[1] + $events[2..3]) $true
Test-RestartFixture 'missing forecast fixture' $events[1..3] $true
Test-RestartFixture 'forecast after startup selection' @($events[1], $events[2], $events[0], $events[3]) $true
Test-RestartFixture 'missing online' $events[0..2] $true
Test-RestartFixture 'missing selection' ($events[0..1] + $events[3]) $true
Test-RestartFixture 'selection before candidate' @($events[0], $events[2], $events[1], $events[3]) $true
Test-RestartFixture 'duplicate startup selection' ($events + $events[2]) $true
foreach ($change in @(@{ to_goal = 'magic_training' }, @{ to_goal = '' }, @{ decision_id = 2 }, @{ decision_reason = 'hunt_deadline' })) {
    $events = @(New-RestartFixture)
    foreach ($key in $change.Keys) { $events[2][$key] = $change[$key] }
    Test-RestartFixture "invalid selection $($change.Keys)" $events $true
}
foreach ($change in @(
    @{ mana = 1000 }, @{ feasible = $true }, @{ reason = 'cooldown' },
    @{ decision_reason = 'magic_training_complete' }, @{ decision_id = 2 },
    @{ mana_tick_remaining = 0 }, @{ mana_tick_remaining = 6001 }
)) {
    $events = @(New-RestartFixture)
    foreach ($key in $change.Keys) { $events[1][$key] = $change[$key] }
    Test-RestartFixture "invalid candidate $($change.Keys)" $events $true
}
foreach ($field in @('mana', 'mana_gain', 'mana_max', 'mana_tick_remaining', 'mana_tick_interval', 'feasible')) {
    $events = @(New-RestartFixture); $events[1].Remove($field)
    Test-RestartFixture "missing $field" $events $true
}
foreach ($change in @(@{ active = $false }, @{ remaining = 0 }, @{ remaining = 5999 }, @{ source = 'other' })) {
    $events = @(New-RestartFixture)
    foreach ($key in $change.Keys) { $events[0][$key] = $change[$key] }
    Test-RestartFixture "invalid forecast $($change.Keys)" $events $true
}
$events = @(New-RestartFixture)
$earlier = (New-RestartFixture)[1]; $earlier.decision_reason = 'hunt_deadline'
Test-RestartFixture 'startup must be first candidate' (@($events[0], $earlier) + $events[1..3]) $true
Write-Host 'Magic-training restart assertion regressions passed.'
