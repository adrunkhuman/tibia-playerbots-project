function Assert-RecoveryNotTerminal {
    param([string]$Logs)
    $terminal = @(ConvertFrom-PlayerbotLogs -Logs $Logs | Where-Object { $_.event -eq 'terminal' })
    if ($terminal.Count) { throw "Positive recovery stopped: $($terminal[0].reason)" }
}

function Assert-RecoveryLoadoutRoundTrip {
    param([string]$Logs, [ValidateSet('recovery_fists', 'recovery_melee')][string]$Mode)
    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs | Where-Object { $_.bot -eq 'Bot One' })
    for ($i = 0; $i -lt $events.Count; $i++) { $events[$i] | Add-Member -NotePropertyName log_index -NotePropertyValue $i -Force }
    if (@($events | Where-Object { $_.event -eq 'terminal' }).Count) { throw 'Positive recovery fixture stopped; terminal is not success.' }
    $weapon = if ($Mode -eq 'recovery_melee') { 2376 } else { 0 }
    $cursor = -1
    function Require-Step([scriptblock]$Predicate, [string]$Description) {
        $found = @($events | Where-Object { $_.log_index -gt $cursor } | Where-Object $Predicate | Select-Object -First 1)
        if (-not $found.Count) { throw "Recovery round trip lacks $Description after event $cursor." }
        $found[0]
    }
    $start = Require-Step {
        $_.event -eq 'recovery_fixture' -and $_.scenario -eq $Mode -and $_.phase -eq 'start' -and
        $_.left_item_id -eq 2389 -and $_.right_item_id -eq 2525 -and $_.spears -eq 1 -and
        $_.melee_count -eq [int]($weapon -ne 0) -and $_.carried_gold -eq 0 -and $_.bank_gold -eq 0 -and
        $_.health -eq $_.health_max
    } 'the isolated unfunded, equipped-spear seed'
    $cursor = $start.log_index
    $selection = Require-Step {
        $_.event -eq 'recovery_loadout' -and $_.result -eq 'selected' -and
        $_.left_item_id -eq $weapon -and $_.right_item_id -eq 2525 -and $_.expected_net_gold_per_minute -gt 0
    } 'a positive-net alternative selection'
    $proof = @($events | Where-Object {
        $_.event -eq 'hunt_region_candidate' -and $_.candidate_phase -eq 'route_validation' -and
        $_.planning_pass -eq $selection.planning_pass -and $_.scoring_revision -eq $selection.scoring_revision -and
        $_.region_id -eq $selection.region_id -and $_.recovery_loadout_id -eq $selection.loadout_id -and
        $_.route_validated -and $_.suitable -and $_.supply_recovery -and $_.recovery_net_gold_per_minute -gt 0 -and
        $_.log_index -gt $cursor -and $_.log_index -lt $selection.log_index
    })
    if (-not $proof.Count) { throw 'Selected recovery lacks its matching atlas/sliced round-trip safety and income proof.' }
    $cursor = $selection.log_index
    $stowRequest = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'move_requested' -and
        $_.item_id -eq 2389 -and $_.slot -eq 6 -and $_.inventory_count -eq 1 -and $_.stowed } 'the engine spear stow request'
    $cursor = $stowRequest.log_index
    $stow = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'move_verified' -and
        $_.item_id -eq 2389 -and $_.slot -eq 6 -and $_.inventory_count -eq 1 -and $_.stowed } 'the verified spear stow receipt'
    $cursor = $stow.log_index
    if ($weapon) {
        $move = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'move_verified' -and
            $_.item_id -eq $weapon -and $_.inventory_count -eq 1 -and -not $_.stowed } 'the carried melee equip receipt'
        $cursor = $move.log_index
    }
    $equipped = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'equipped' -and
        $_.left_item_id -eq $weapon -and $_.right_item_id -eq 2525 } 'physically verified temporary hands'
    $cursor = $equipped.log_index
    $departure = Require-Step { $_.event -eq 'navigation_progress' -and $_.result -eq 'recovery_departure_validated' -and
        $_.attack -eq $(if ($weapon) { 14 } else { 7 }) -and $_.attack_skill -ge 40 -and
        $_.health -ge 0.8 * $start.health_max -and $_.outbound_danger -ge 0 -and $_.return_danger -ge 0 -and
        $_.route_health_loss -le $_.health - [Math]::Ceiling(0.8 * $start.health_max) -and $_.expected_net_gold_per_minute -gt 0
    } 'live-profile outbound and return preflight'
    $cursor = $departure.log_index
    $area = Require-Step { $_.event -eq 'hunt_area_entered' -and $_.region_id -eq $selection.region_id } 'arrival in the selected area'
    $cursor = $area.log_index
    $hit = Require-Step { $_.event -eq 'recovery_fixture' -and $_.scenario -eq $Mode -and $_.phase -eq 'hit' -and
        $_.left_item_id -eq $weapon -and $_.right_item_id -eq 2525 -and $_.spears -eq 1 } 'real damage with the temporary hands'
    $cursor = $hit.log_index
    $death = Require-Step { $_.event -eq 'recovery_fixture' -and $_.scenario -eq $Mode -and $_.phase -eq 'death' -and
        $_.target_id -eq $hit.target_id -and $_.corpse_gold -eq 2000 } 'a genuine quarry death and coin corpse'
    $cursor = $death.log_index
    $loot = Require-Step { $_.event -eq 'action_result' -and $_.action -eq 'loot' -and
        $_.result -eq 'success' -and $_.coin_gold_acquired -ge 2000 } 'normal corpse opening and verified coin transfer'
    $cursor = $loot.log_index
    $returned = Require-Step { $_.event -eq 'objective_transition' -and $_.to -eq 'deposit_loot' -and
        $_.reason -eq 'depot_reached' } 'safe return to the real depot'
    $cursor = $returned.log_index
    if ($weapon) {
        $stowedMelee = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'move_verified' -and
            $_.item_id -eq $weapon -and $_.inventory_count -eq 1 -and $_.stowed } 'preservation of the carried melee weapon'
        $cursor = $stowedMelee.log_index
    }
    $restoredMove = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'move_verified' -and
        $_.item_id -eq 2389 -and $_.inventory_count -eq 1 -and -not $_.stowed } 'the original spear restoration receipt'
    $cursor = $restoredMove.log_index
    $restore = Require-Step { $_.event -eq 'recovery_loadout' -and $_.result -eq 'restored' -and
        $_.left_item_id -eq 2389 -and $_.right_item_id -eq 2525 } 'normal equipment restoration at service'
    # Lua may observe the actual move before the following controller turn
    # verifies its receipt. Both must follow the depot boundary.
    $cursor = $returned.log_index
    $null = Require-Step { $_.event -eq 'recovery_fixture' -and $_.scenario -eq $Mode -and $_.phase -eq 'restored' -and
        $_.left_item_id -eq 2389 -and $_.right_item_id -eq 2525 -and $_.spears -ge 1 -and
        $_.melee_count -eq [int]($weapon -ne 0) -and $_.carried_gold + $_.bank_gold -gt 0 } 'independent final inventory/funds verification'
    $cursor = $restore.log_index
    $purchase = Require-Step { $_.event -eq 'action_result' -and $_.action -eq 'buy_throwing_weapons' -and
        $_.result -eq 'success' -and $_.item_id -eq 2389 -and $_.count -ge 2
    } 'ordinary paid restoration of the original spear stock target'
    # Either loaded spear shop may win the ordinary service route selection.
    # Match payment to the actual conversation, not the cheapest catalog price.
    $seller = @($events | Where-Object {
        $_.event -eq 'npc_reply' -and $_.log_index -gt $cursor -and $_.log_index -lt $purchase.log_index
    } | Select-Object -Last 1)
    $prices = @{ Galuna = 9; Turvy = 10 }
    if (-not $seller.Count -or -not $prices.ContainsKey([string]$seller[0].npc_name)) {
        throw 'Restock lacks a conversation with a loaded spear shop.'
    }
    $paid = $purchase.carried_before + $purchase.bank_before - $purchase.carried_after - $purchase.bank_after
    if ($paid -ne $prices[[string]$seller[0].npc_name] * $purchase.count) {
        throw 'Restock payment does not match the actual spear shop offer.'
    }
    $early = @($events | Where-Object {
        $_.log_index -lt $loot.log_index -and
        (($_.action -in @('buy_throwing_weapons', 'buy_equipment', 'claim_reward', 'sell') -and $_.result -eq 'success') -or
         ($_.event -eq 'recovery_loadout' -and $_.result -eq 'restored'))
    })
    if ($early.Count) { throw 'Purchased/reward/sale funding or early restoration contaminated recovery combat income.' }
}
