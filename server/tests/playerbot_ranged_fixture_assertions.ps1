#Requires -Version 7.0
# Run from repository root: pwsh -NoProfile -File server/tests/playerbot_ranged_fixture_assertions.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. "$root/scripts/playerbot-gameplay/log-parsing.ps1"
. "$root/scripts/playerbot-gameplay/assertions-navigation.ps1"

function New-FixtureLogs {
    param([string]$Case = 'open', [int]$Damage = 6, [int]$RangedMs = 16000, [int]$Steps = 5, [int]$Displacement = 2)
    $mode = @{ control = 'ranged_position_control'; open = 'ranged_position'; corner = 'ranged_position_corner' }[$Case]
    $decision = switch ($Case) {
        'open' { '{"component":"playerbot","event":"ranged_position","action":"retreat","target_id":2}' }
        'corner' { '{"component":"playerbot","event":"ranged_position","action":"fight_in_place","reason":"retreat_blocked","target_id":2}' }
        default { '' }
    }
    @(
        $decision
        '{"component":"playerbot","event":"target_changed","reason":"target_defeated","previous_target_id":2,"sequence":30}'
        "PLAYERBOT_GAMEPLAY_TEST RANGED_POSITION_RESULT $mode damage=$Damage defeated=true seconds=30.0 target=2 dealt=150 hits=15 fight_ms=30000 ranged_ms=$RangedMs steps=$Steps displacement=$Displacement loot_before=20"
        '{"component":"playerbot","event":"action_result","action":"loot","result":"success","item_id":2148,"count":1,"inventory_count":21,"coin_gold_acquired":1,"sequence":31}'
    ) -join "`n"
}
function Assert-Rejected {
    param([string]$Logs, [string]$Case = 'open', [string]$Reason)
    try { Assert-RangedPositionEvents -Logs $Logs -Case $Case }
    catch {
        if ($_.Exception.Message -notlike "*$Reason*") { throw "Wrong rejection: $($_.Exception.Message); expected $Reason" }
        return
    }
    throw "Fixture accepted invalid evidence: $Reason"
}
$script:rangedPositionControlDamage = $null
Assert-Rejected -Logs (New-FixtureLogs) -Reason 'needs the control result'
Assert-RangedPositionEvents -Logs (New-FixtureLogs -Case control -Damage 60) -Case control
Assert-RangedPositionEvents -Logs (New-FixtureLogs) -Case open
Assert-RangedPositionEvents -Logs (New-FixtureLogs -Case corner) -Case corner
Assert-Rejected -Logs (New-FixtureLogs -RangedMs 15000) -Reason 'no majority'
Assert-Rejected -Logs (New-FixtureLogs -RangedMs 14999) -Reason 'no majority'
Assert-Rejected -Logs (New-FixtureLogs -Steps 0) -Reason 'executed displacement'
Assert-Rejected -Logs (New-FixtureLogs -Displacement 0) -Reason 'executed displacement'
Assert-Rejected -Logs (New-FixtureLogs -Damage 60) -Reason 'control took'
Assert-Rejected -Logs (New-FixtureLogs -Case control -Damage 0) -Case control -Reason 'must take chaser damage'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'hits=15', 'hits=14') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'dealt=150', 'dealt=149') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'defeated=true', 'defeated=false') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'previous_target_id":2', 'previous_target_id":99') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'ranged_ms=16000', 'ranged_ms=30001') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'inventory_count":21', 'inventory_count":20') -Reason 'book the chaser'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'sequence":31', 'sequence":29') -Reason 'book the chaser'
Assert-Rejected -Logs ((New-FixtureLogs) -replace 'coin_gold_acquired":1', 'coin_gold_acquired":0') -Reason 'book the chaser'
Assert-Rejected -Logs ((New-FixtureLogs) + "`n" + '{"component":"playerbot","event":"death"}') -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs) + "`n" + (New-FixtureLogs)) -Reason 'one complete observation'
Assert-Rejected -Logs (New-FixtureLogs -Case corner) -Case open -Reason 'normalized chaser combat'
Assert-Rejected -Logs ((New-FixtureLogs -Case corner) -replace 'retreat_blocked', 'wall_sliding') -Case corner -Reason 'shallow wall'

# Execute the driver's real selection preflight, never its Docker/runtime code.
$driver = Get-Content -Raw "$root/scripts/test-playerbot-gameplay.ps1"
$begin = $driver.IndexOf('$scenarioCatalog = @(')
$end = $driver.IndexOf('if ($exactScenarioSelection -and ($Focused -or $MagicTrainingCase))')
if ($begin -lt 0 -or $end -le $begin -or $end -ge $driver.IndexOf('& docker info')) { throw 'Selection preflight is missing or follows Docker access.' }
$selectionPreflight = [scriptblock]::Create($driver.Substring($begin, $end - $begin))
foreach ($standalone in @('ranged_position_control', 'ranged_position_corner')) {
    $Scenario = @($standalone)
    . $selectionPreflight
    if ($selectedScenarios.Count -ne 1 -or -not $selectedScenarios.Contains($standalone)) { throw "Unexpected dependency for $standalone" }
}
$Scenario = @('RANGED_POSITION,ranged_position_control', 'ranged_position')
. $selectionPreflight
if ($selectedScenarios.Count -ne 2 -or -not $selectedScenarios.Contains('ranged_position_control')) { throw 'Exact open-room selection lacks its control dependency.' }
$names = [System.Collections.Generic.List[string]]::new()
function Invoke-Scenario {
    param([string]$Name, [int]$DefaultTimeoutSeconds, [scriptblock]$Body)
    if (-not $exactScenarioSelection -or $selectedScenarios.Contains($Name)) { $names.Add($Name) }
    # Do not execute Body: this is a non-Docker enumeration contract.
}
$TargetApproach = $CorpseLoot = $DeathTelemetry = $Healing = $ValueLoot = $false
$RangedPosition = $true
. "$root/scripts/playerbot-gameplay/scenarios-combat-loot.ps1"
if (($names -join ',') -ne 'ranged_position_control,ranged_position') { throw "Wrong dependency execution order: $names" }
$names.Clear()
$exactScenarioSelection = $false
. "$root/scripts/playerbot-gameplay/scenarios-combat-loot.ps1"
if (($names -join ',') -ne 'ranged_position_control,ranged_position,ranged_position_corner') { throw "Wrong focused ranged scenarios: $names" }

# Parse all changed PowerShell entry points, without running the driver.
foreach ($path in @('scripts/test-playerbot-gameplay.ps1', 'scripts/playerbot-gameplay/scenarios-combat-loot.ps1', 'scripts/playerbot-gameplay/assertions-navigation.ps1', 'server/tests/playerbot_ranged_fixture_assertions.ps1')) {
    $tokens = $parseErrors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $path), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "PowerShell parse errors in ${path}: $parseErrors" }
}
'playerbot ranged fixture assertions passed'
