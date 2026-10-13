#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
. $PSScriptRoot/../../scripts/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/../../scripts/playerbot-gameplay/assertions-recovery.ps1

function New-RoundTrip([string]$Mode) {
    $melee = $Mode -eq 'recovery_melee'
    $weapon = if ($melee) { 2376 } else { 0 }
    @(
        @{event='recovery_fixture'; scenario=$Mode; phase='start'; left_item_id=2389; right_item_id=2525; spears=1; melee_count=[int]$melee; carried_gold=0; bank_gold=0; health=185; health_max=185}
        @{event='hunt_region_candidate'; candidate_phase='route_validation'; planning_pass=2; scoring_revision=3; region_id=4; recovery_loadout_id=2; route_validated=$true; suitable=$true; supply_recovery=$true; recovery_net_gold_per_minute=100}
        @{event='recovery_loadout'; result='selected'; planning_pass=2; scoring_revision=3; region_id=4; loadout_id=2; left_item_id=$weapon; right_item_id=2525; expected_net_gold_per_minute=100}
        @{event='recovery_loadout'; result='move_requested'; item_id=2389; slot=6; inventory_count=1; stowed=$true}
        @{event='recovery_loadout'; result='move_verified'; item_id=2389; slot=6; inventory_count=1; stowed=$true}
        if ($melee) { @{event='recovery_loadout'; result='move_verified'; item_id=2376; inventory_count=1; stowed=$false} }
        @{event='recovery_loadout'; result='equipped'; left_item_id=$weapon; right_item_id=2525}
        @{event='navigation_progress'; result='recovery_departure_validated'; attack=$(if ($melee) {14} else {7}); attack_skill=40; health=185; outbound_danger=0; return_danger=0; route_health_loss=0; expected_net_gold_per_minute=100}
        @{event='hunt_area_entered'; region_id=4}
        @{event='recovery_fixture'; scenario=$Mode; phase='hit'; left_item_id=$weapon; right_item_id=2525; spears=1; target_id=99}
        @{event='recovery_fixture'; scenario=$Mode; phase='death'; target_id=99; corpse_gold=2000}
        @{event='action_result'; action='loot'; result='success'; coin_gold_acquired=2000}
        @{event='objective_transition'; to='deposit_loot'; reason='depot_reached'}
        if ($melee) { @{event='recovery_loadout'; result='move_verified'; item_id=2376; inventory_count=1; stowed=$true} }
        @{event='recovery_loadout'; result='move_verified'; item_id=2389; inventory_count=1; stowed=$false}
        @{event='recovery_loadout'; result='restored'; left_item_id=2389; right_item_id=2525}
        @{event='recovery_fixture'; scenario=$Mode; phase='restored'; left_item_id=2389; right_item_id=2525; spears=1; melee_count=[int]$melee; carried_gold=2000; bank_gold=0}
        @{event='npc_reply'; npc_name='Galuna'}
        @{event='action_result'; action='buy_throwing_weapons'; result='success'; item_id=2389; count=2; carried_before=2000; bank_before=0; carried_after=1982; bank_after=0}
    )
}
function Test-RoundTrip([array]$Events, [string]$Mode, [bool]$Reject=$false) {
    $logs = ($Events | ForEach-Object {
        $_.component='playerbot'; $_.bot='Bot One'; $_ | ConvertTo-Json -Depth 8 -Compress
    }) -join "`n"
    try { Assert-RecoveryLoadoutRoundTrip -Logs $logs -Mode $Mode }
    catch { if ($Reject) { return }; throw }
    if ($Reject) { throw 'Incomplete/invalid positive recovery evidence passed.' }
}
foreach ($mode in @('recovery_fists','recovery_melee')) {
    $events = New-RoundTrip $mode
    Test-RoundTrip $events $mode
    for ($omit=0; $omit -lt $events.Count; $omit++) {
        $broken = @($events | Where-Object { $_ -ne $events[$omit] })
        Test-RoundTrip $broken $mode $true
    }
    foreach ($field in @('route_validated','suitable','supply_recovery')) {
        $events = New-RoundTrip $mode; $events[1][$field]=$false
        Test-RoundTrip $events $mode $true
    }
    foreach ($field in @('planning_pass','scoring_revision','region_id','recovery_loadout_id')) {
        $events = New-RoundTrip $mode; $events[1][$field]=99
        Test-RoundTrip $events $mode $true
    }
    $events = New-RoundTrip $mode
    ($events | Where-Object { $_.result -eq 'recovery_departure_validated' }).route_health_loss=92
    Test-RoundTrip $events $mode $true
    $events = New-RoundTrip $mode
    ($events | Where-Object { $_.result -eq 'recovery_departure_validated' }).expected_net_gold_per_minute=0
    Test-RoundTrip $events $mode $true
    $events = New-RoundTrip $mode; $events[4].inventory_count=0
    Test-RoundTrip $events $mode $true
    $events = New-RoundTrip $mode
    ($events | Where-Object { $_.phase -eq 'hit' }).left_item_id=2389
    Test-RoundTrip $events $mode $true
    $events = New-RoundTrip $mode
    ($events | Where-Object { $_.action -eq 'loot' }).coin_gold_acquired=0
    Test-RoundTrip $events $mode $true
    Test-RoundTrip (@(New-RoundTrip $mode) + @{event='terminal';reason='recovery_no_safe_income_outing'}) $mode $true
    Test-RoundTrip (@(@{event='action_result';action='buy_throwing_weapons';result='success'}) + @(New-RoundTrip $mode)) $mode $true
    # The first unfunded service must not liquidate the carried recovery weapon.
    $events = New-RoundTrip $mode
    Test-RoundTrip (@($events[0], @{event='action_result';action='sell';result='success';item_id=2376;count=1}) + $events[1..($events.Count-1)]) $mode $true
    # The ordinary service may choose either shop, at its exact loaded price.
    $events = New-RoundTrip $mode
    $events[-2].npc_name='Turvy'; $events[-1].carried_after=1980
    Test-RoundTrip $events $mode
    foreach ($paid in @(0, 18, 19, 21)) {
        $events = New-RoundTrip $mode
        $events[-2].npc_name='Turvy'; $events[-1].carried_after=2000-$paid
        Test-RoundTrip $events $mode $true
    }
    $events = New-RoundTrip $mode; $events[-2].npc_name='Sam'
    Test-RoundTrip $events $mode $true
    # The independent inventory observer may run just before the move receipt.
    $events = New-RoundTrip $mode
    $sample = $events[-3]
    $reordered = @($events[0..($events.Count-6)]) + $sample + @($events[($events.Count-5)..($events.Count-4)]) + @($events[-2..-1])
    Test-RoundTrip $reordered $mode
}
# Keep the exact payment expectations aligned with the loaded shop offers.
$root = (Resolve-Path "$PSScriptRoot/../..").Path
foreach ($shop in @(@{Name='Galuna';Price=9}, @{Name='Turvy';Price=10})) {
    [xml]$npc = Get-Content -Raw "$root/server/data/npc/$($shop.Name).xml"
    $offers = ($npc.npc.parameters.parameter | Where-Object { $_.key -eq 'shop_buyable' }).value
    if ($offers -notmatch ('spear,2389,' + $shop.Price + ';')) { throw 'Recovery payment expectation differs from the loaded shop.' }
}
# Execute only selection/enumeration, never a scenario body or Docker command.
$driver = Get-Content -Raw "$root/scripts/test-playerbot-gameplay.ps1"
$begin = $driver.IndexOf('$scenarioCatalog = @(')
$end = $driver.IndexOf('if ($exactScenarioSelection -and ($Focused -or $MagicTrainingCase))')
if ($begin -lt 0 -or $end -le $begin -or $end -ge $driver.IndexOf('& docker info')) { throw 'Unsafe selection preflight boundary.' }
$Scenario = @('recovery_fists,recovery_melee')
. ([scriptblock]::Create($driver.Substring($begin, $end - $begin)))
if ($selectedScenarios.Count -ne 2) { throw 'Recovery exact selection is not isolated.' }
$names = [System.Collections.Generic.List[string]]::new()
$bodies = [System.Collections.Generic.List[scriptblock]]::new()
function Invoke-Scenario {
    param([string]$Name, [int]$DefaultTimeoutSeconds, [scriptblock]$Body)
    if ($selectedScenarios.Contains($Name)) { $names.Add($Name); $bodies.Add($Body) }
}
$CombatReadiness = $true
. "$root/scripts/playerbot-gameplay/scenarios-recovery.ps1"
if (($names -join ',') -ne 'recovery_fists,recovery_melee') { throw 'Recovery scenario enumeration differs from selection.' }
foreach ($path in @('scripts/test-playerbot-gameplay.ps1', 'scripts/playerbot-gameplay/scenarios-recovery.ps1')) {
    $tokens = $null; $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $path), [ref]$tokens, [ref]$errors)
    if ($errors) { throw ($errors -join "`n") }
}
# Run the scenario's wait wiring with inert command stubs. A terminal record
# must abort the first wait, not sit until the outer 900-second deadline.
& {
    $projectRoot = $root
    function Invoke-Compose { }
    function Wait-ForLog {
        param([string]$Pattern)
        if ($Pattern -notmatch 'terminal') { throw 'Recovery first wait does not watch terminal.' }
        '{"component":"playerbot","event":"terminal","reason":"recovery_no_safe_income_outing"}'
    }
    function Wait-ForPlayerbotEvent { throw 'Recovery continued waiting after terminal.' }
    $failed = $false
    try { & $bodies[0] }
    catch {
        if ($_.Exception.Message -notmatch '^Positive recovery stopped: recovery_no_safe_income_outing') { throw }
        $failed = $true
    }
    if (-not $failed) { throw 'Positive recovery accepted terminal.' }
}
'Recovery round-trip assertions, negative controls, fail-fast waits, exact scenario enumeration and syntax passed.'
