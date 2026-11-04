function Assert-RaidTravelEvents {
    param([string]$Logs, [ValidateSet('raid_travel', 'raid_recovery')][string]$Mode)

    if ($Logs -match 'RAID_FIXTURE_FAILED' -or
        $Logs -notmatch ('PLAYERBOT_GAMEPLAY_TEST ' + $Mode.ToUpperInvariant() + '_PASS')) {
        throw 'Raid travel lacks a successful independent position verifier.'
    }
    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $phases = @{}
    $phaseOrder = @('start', 'activated', 'spawned', 'withdrew')
    if ($Mode -eq 'raid_recovery') { $phaseOrder += @('kill_injected', 'death', 'recovered') }
    $phaseOrder += @('guarded', 'removed', 'arrived')
    $nextPhase = 0
    $withdrawal = $false
    $recoverySafety = $false
    $resume = $false
    $dead = 0
    $recovered = $false
    for ($i = 0; $i -lt $events.Count; ++$i) {
        $e = $events[$i]
        if ($e.bot -and $e.bot -ne 'Bot One') { throw 'Raid fixture must seed only Bot One.' }
        if ($e.event -eq 'raid_fixture') {
            if ($e.scenario -ne $Mode -or $e.source -ne 'lua_position_verifier' -or
                $nextPhase -ge $phaseOrder.Count -or $e.phase -ne $phaseOrder[$nextPhase]) {
                throw 'Raid position evidence is missing, duplicated, or out of order.'
            }
            if ($e.z -ne 7 -or $e.x -lt 60128 -or $e.x -gt 60216 -or $e.y -ne 60200 -or
                ($e.phase -ne 'death' -and $e.health -le 0)) {
                throw 'Raid position evidence left the live isolated runway.'
            }
            $phases[$e.phase] = $e
            ++$nextPhase
        }
        if ($e.event -eq 'lifecycle' -and $e.status -eq 'dead') {
            if ($Mode -ne 'raid_recovery' -or -not $phases.ContainsKey('kill_injected') -or
                $phases.ContainsKey('recovered') -or ++$dead -ne 1) {
                throw 'Raid caused an unplanned production death.'
            }
        }
        if ($e.event -eq 'terminal' -and -not ($Mode -eq 'raid_recovery' -and
            $e.reason -eq 'controlled_player_dead' -and $phases.ContainsKey('kill_injected') -and
            -not $phases.ContainsKey('recovered'))) {
            throw "Raid controller stopped unexpectedly: $($e.reason)"
        }
        if ($e.event -eq 'lifecycle' -and $e.status -eq 'online' -and $e.recovered -eq $true) {
            if ($Mode -ne 'raid_recovery' -or -not $phases.ContainsKey('recovered') -or
                $phases.ContainsKey('removed') -or $recovered) {
                throw 'Raid recovery was missing, repeated, or too late.'
            }
            $recovered = $true
        }
        if ($e.event -eq 'travel_safety') {
            if ($e.action -notin @('withdraw', 'defer', 'resume', 'step_rejected') -or
                -not $e.reason -or $null -eq $e.danger -or [double]$e.danger -lt 0) {
                throw 'Raid safety telemetry lacks its action/danger/reason contract.'
            }
            if ($phases.ContainsKey('activated') -and -not $phases.ContainsKey('removed')) {
                if ($e.action -eq 'withdraw' -and [double]$e.danger -gt 0.08) { $withdrawal = $true }
                if ($recovered -and $e.action -in @('withdraw', 'defer', 'step_rejected')) { $recoverySafety = $true }
            }
            if ($phases.ContainsKey('removed') -and -not $phases.ContainsKey('arrived') -and
                $e.action -eq 'resume') { $resume = $true }
        }
    }
    if ($nextPhase -ne $phaseOrder.Count -or -not $withdrawal -or -not $resume) {
        throw 'Raid travel needs a real spawn, observed withdrawal, guarded clearance, resume, and arrival.'
    }
    if ($phases.start.x -ne 60160 -or $phases.start.y -ne 60200 -or $phases.start.health_max -ne 185 -or
        $phases.start.goal_x -ne 60208 -or $phases.start.goal_y -ne 60200 -or $phases.start.goal_z -ne 7 -or
        $phases.activated.x -lt $phases.start.x + 8 -or $phases.activated.spawn_x -ne 60169 -or
        $phases.activated.spawn_y -ne 60200 -or $phases.activated.spawn_z -ne 7 -or
        $phases.spawned.count -ne 1 -or $phases.spawned.target_id -le 0 -or
        $phases.withdrew.from_x -lt $phases.activated.x -or
        $phases.withdrew.from_x - $phases.withdrew.x -lt 2 -or $phases.guarded.guard_ms -lt 5000 -or
        $phases.arrived.x -ne $phases.start.goal_x -or $phases.arrived.y -ne $phases.start.goal_y -or
        $phases.arrived.elapsed_ms -le $phases.removed.elapsed_ms) {
        throw 'Raid fixture did not prove ordinary outbound, retreat, live-threat guard, and post-removal displacement.'
    }
    foreach ($phase in @('withdrew', 'guarded', 'removed')) {
        if ($phases[$phase].target_id -ne $phases.spawned.target_id) { throw 'Raid fixture changed its threat identity.' }
    }
    if ($Mode -eq 'raid_recovery') {
        if ($dead -ne 1 -or -not $recovered -or -not $recoverySafety -or
            $phases.death.intentional -ne $true -or $phases.recovered.target_id -ne $phases.spawned.target_id -or
            $phases.recovered.x -ne $phases.start.x -or $phases.recovered.y -ne $phases.start.y -or
            $phases.arrived.deaths -ne 1) {
            throw 'Raid recovery needs one intentional normal death and renewed safety against the same live threat.'
        }
    } elseif ($dead -ne 0 -or $phases.arrived.deaths -ne 0) {
        throw 'Raid travel did not remain death-free.'
    }
}

function Assert-BridgeRouteEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $start = @($events | Where-Object { $_.event -eq 'bridge_fixture' -and $_.phase -eq 'start' })
    $result = @($events | Where-Object { $_.event -eq 'bridge_fixture' -and $_.phase -eq 'result' })
    if ($start.Count -ne 1 -or $result.Count -ne 1) { throw 'Bridge route lacks one start and one result sample.' }
    if ($start[0].shovel -lt 1 -or $start[0].rope -lt 1) { throw 'Bridge route must start with a shovel and a rope.' }
    $plans = @($events | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'action_result' -and $_.action -eq 'plan' })
    $floors = @($events | Where-Object { $_.event -eq 'bridge_fixture' -and $_.phase -eq 'floor_change' })
    $safety = @($events | Where-Object { $_.bot -eq 'Bot One' -and $_.event -eq 'travel_safety' })
    $r = $result[0]
    Write-Host ("PLAYERBOT_GAMEPLAY_TEST BRIDGE_ROUTE_SUMMARY result={0} plateau_samples={1} z8_samples={2} floor_changes={3} minimum_health={4} travel_safety={5}" -f
        $r.result, $r.plateau_samples, $r.z8_samples, $r.floor_changes, $r.minimum_health, $safety.Count)
    foreach ($f in $floors) { Write-Host ("PLAYERBOT_GAMEPLAY_TEST BRIDGE_ROUTE_FLOOR {0},{1},{2} at {3}ms" -f $f.x, $f.y, $f.z, $f.elapsed_ms) }
    foreach ($plan in ($plans | Select-Object -First 6)) {
        Write-Host ("PLAYERBOT_GAMEPLAY_TEST BRIDGE_ROUTE_PLAN from={0},{1},{2} waypoint={3},{4},{5} steps={6} danger_cost={7} peak={8} shovel={9}" -f
            $plan.position.x, $plan.position.y, $plan.position.z, $plan.waypoint.x, $plan.waypoint.y, $plan.waypoint.z,
            $plan.steps, $plan.danger_cost, $plan.maximum_health_loss_per_second, $plan.shovel_available)
    }
    if ($r.result -ne 'arrived' -or $r.plateau_samples -gt 0) {
        throw "Bridge route must reach the north side without crossing the Dwarven Bridge plateau (result=$($r.result), plateau_samples=$($r.plateau_samples))."
    }
}
