param(
    [ValidateRange(60, 3600)]
    [int]$TimeoutSeconds = 300,
    [switch]$FullNavigation,
    [switch]$CorpseLoot,
    [switch]$DeathTelemetry,
    [switch]$Healing,
    [switch]$ValueLoot,
    [switch]$KeepStack
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectRoot "server\compose.yaml"
$gameplayComposeFile = Join-Path $projectRoot "server\compose.playerbot-gameplay.yaml"
$composeArguments = @("compose", "-f", $composeFile, "-f", $gameplayComposeFile)
$previousDuration = $env:PLAYERBOT_HUNT_DURATION_SECONDS
$previousMode = $env:PLAYERBOT_GAMEPLAY_MODE
$previousRelogDelay = $env:PLAYERBOT_RELOG_DELAY_SECONDS
$previousMaximumDeaths = $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    & docker @composeArguments @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Get-ServerLogs {
    $output = & docker @composeArguments logs --no-log-prefix server 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Could not read server logs."
    }
    return $output -join "`n"
}

function Get-OnlineBotCount {
    $query = "SELECT COUNT(*) FROM players_online JOIN players ON players.id = players_online.player_id WHERE players.name = 'Bot One';"
    $output = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $query
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query Bot One's online state."
    }
    return [int]($output | Select-Object -Last 1)
}

function Wait-ForLog {
    param([string]$Pattern)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        if ($logs -match $Pattern) {
            return $logs
        }
        Start-Sleep -Seconds 2
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for server log pattern: $Pattern"
}

function Wait-ForPlayerbotEvent {
    param([scriptblock]$Predicate)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        if (@(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object $Predicate).Count -gt 0) {
            return $logs
        }
        Start-Sleep -Seconds 1
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for a playerbot event."
}

function Wait-ForPlayerbotEventCount {
    param(
        [string]$Action,
        [int]$Count
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $logs = Get-ServerLogs
        $events = @(ConvertFrom-PlayerbotLogs -Logs $logs | Where-Object {
            $_.event -eq "action_result" -and $_.action -eq $Action -and $_.result -eq "reached"
        })
        if ($events.Count -ge $Count) {
            return $logs
        }
        Start-Sleep -Seconds 2
    }
    throw "Timed out after $TimeoutSeconds seconds waiting for $Count '$Action' events."
}

function ConvertFrom-PlayerbotLogs {
    param([string]$Logs)

    foreach ($line in $Logs -split "`r?`n") {
        if (-not $line.StartsWith('{')) {
            continue
        }
        try {
            $event = $line | ConvertFrom-Json
            if ($event.component -eq "playerbot") {
                $event
            }
        }
        catch {
            continue
        }
    }
}

function Assert-CycleEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $deposit = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "deposit" -and
        $_.result -eq "success" -and $_.item_id -eq 1987 -and $_.count -eq 1
    })
    $serviceDiscovery = @($events | Where-Object {
        $_.event -eq "service_discovered" -and $_.capability -in @("shop", "banker")
    })
    $shopCatalog = @($serviceDiscovery | Where-Object { $_.capability -eq "shop" -and $_.offers -gt 0 })
    $bankerDiscovery = @($serviceDiscovery | Where-Object { $_.capability -eq "banker" })
    $npcReplies = @($events | Where-Object { $_.event -eq "npc_reply" -and $_.npc_name })
    $bankDeposit = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "bank_deposit" -and $_.result -eq "success"
    })
    $bankWithdraw = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "bank_withdraw" -and $_.result -eq "success"
    })
    $dynamicSale = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "sell" -and $_.result -eq "success" -and
        $_.item_id -eq 2992 -and $_.count -eq 1
    })
    $cycles = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_cycle" -and $_.result -eq "started"
    })
    $huntPlan = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "plan" -and $_.result -eq "success" -and
        $_.destination.x -eq 32084 -and $_.destination.y -eq 32144 -and $_.destination.z -eq 5
    })
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })
    $defensiveStart = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
        $_.result -eq "started" -and $_.chase -eq $false
    })
    $defensiveComplete = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "defensive_combat" -and
        $_.result -eq "success" -and $_.reason -eq "target_defeated"
    })

    if ($deposit.Count -ne 1) {
        throw "Expected exactly one injected-loot deposit event, found $($deposit.Count)."
    }
    if ($shopCatalog.Count -lt 1 -or $bankerDiscovery.Count -lt 1) {
        throw "The bot did not discover the tagged live service NPC catalogues."
    }
    if ($npcReplies.Count -lt 3) {
        throw "The bot did not acknowledge the selected NPCs before requesting services."
    }
    if (@($events | Where-Object { $_.event -eq "service_catalog" }).Count -ne 0) {
        throw "The bot probed a shop window instead of using the live NPC offer catalog."
    }
    if ($bankDeposit.Count -lt 1 -or $bankWithdraw.Count -lt 1) {
        throw "The bot did not produce the expected purchase, sale, and bank balance result."
    }
    if ($dynamicSale.Count -ne 1) {
        throw "The bot did not sell the dead rabbit discovered from the live NPC offer catalog."
    }
    if ($cycles.Count -lt 2) {
        throw "The bot did not begin a second hunt cycle."
    }
    if ($huntPlan.Count -lt 1) {
        throw "The bot did not plan a map-derived route to hunting point A."
    }
    if ($defensiveStart.Count -lt 1 -or $defensiveComplete.Count -lt 1) {
        throw "The bot did not complete a non-chasing defensive combat interruption."
    }
    if ($defensiveStart[0].route_critical -ne $true) {
        throw "The bot did not prioritize the attacker occupying its failed navigation step."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during the gameplay cycle."
    }
}

function Assert-DeathEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $deaths = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "dead" -and
        $_.level -gt 0 -and $_.objective -and $_.state -and $_.health -eq 0 -and
        $_.killer_id -gt 0 -and $_.killer_name -eq "Playerbot Death Threat" -and
        $_.killer_type -eq "monster" -and $_.most_damage_id -eq $_.killer_id -and
        $_.most_damage_name -eq $_.killer_name
    })
    $terminals = @($events | Where-Object {
        $_.event -eq "terminal" -and $_.reason -eq "controlled_player_dead"
    })
    $scheduled = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "recovery_scheduled" -and
        $_.reason -eq "death" -and $_.relog_attempt -eq 1
    })
    $recovered = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online" -and $_.recovered -eq $true -and
        $_.objective -eq "service"
    })
    $abandoned = @($events | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "recovery_abandoned" -and
        $_.reason -eq "death_loop_limit"
    })
    if ($deaths.Count -ne 3) {
        throw "Expected exactly three contextual playerbot death events, found $($deaths.Count)."
    }
    if ($terminals.Count -ne 3) {
        throw "Expected exactly three controlled_player_dead terminal events, found $($terminals.Count)."
    }
    if ($scheduled.Count -ne 2 -or $scheduled[0].death_count -ne 1 -or $scheduled[0].delay_ms -ne 1000 -or
        $scheduled[1].death_count -ne 2 -or $scheduled[1].delay_ms -ne 2000) {
        throw "Death recovery did not schedule the expected bounded exponential backoff."
    }
    if ($recovered.Count -ne 2 -or $recovered[0].recovery_count -ne 1 -or $recovered[1].recovery_count -ne 2) {
        throw "Expected two fresh service-first recovered controllers."
    }
    if ($abandoned.Count -ne 1 -or $abandoned[0].death_count -ne 3 -or $abandoned[0].maximum_deaths -ne 2) {
        throw "Death recovery did not stop at the configured consecutive-death limit."
    }
    if (@($events | Where-Object { $_.event -eq "lifecycle" -and $_.status -eq "recovery_failed" }).Count -ne 0) {
        throw "The focused death recovery scenario produced a failed relog."
    }

    for ($deathIndex = 0; $deathIndex -lt $recovered.Count; $deathIndex++) {
        $start = [Array]::IndexOf($events, $deaths[$deathIndex])
        $scheduledIndex = [Array]::IndexOf($events, $scheduled[$deathIndex])
        $terminalIndex = [Array]::IndexOf($events, $terminals[$deathIndex])
        $end = [Array]::IndexOf($events, $recovered[$deathIndex])
        if ($start -lt 0 -or $scheduledIndex -le $start -or $terminalIndex -le $scheduledIndex -or $end -le $terminalIndex) {
            throw "Death, scheduling, terminal, and recovered events were not causally ordered."
        }
        $actionsDuringRelog = @($events[($start + 1)..($end - 1)] | Where-Object { $_.event -eq "action_result" })
        if ($actionsDuringRelog.Count -ne 0) {
            throw "The dead controller continued acting during its relog delay."
        }
        $scheduledAt = [DateTimeOffset]::Parse($scheduled[$deathIndex].ts).ToUnixTimeMilliseconds()
        $recoveredAt = [DateTimeOffset]::Parse($recovered[$deathIndex].ts).ToUnixTimeMilliseconds()
        if ($recoveredAt - $scheduledAt -lt $scheduled[$deathIndex].delay_ms - 100) {
            throw "The bot recovered before its requested relog delay elapsed."
        }
    }

    $secondRecoveryIndex = [Array]::IndexOf($events, $recovered[1])
    $thirdDeathIndex = [Array]::IndexOf($events, $deaths[2])
    $serviceAfterRecovery = @($events[($secondRecoveryIndex + 1)..($thirdDeathIndex - 1)] | Where-Object {
        $_.event -eq "service_discovered"
    })
    $huntAfterRecovery = @($events[($secondRecoveryIndex + 1)..($thirdDeathIndex - 1)] | Where-Object {
        $_.event -eq "action_result" -and $_.action -in @("attack", "hunt_cycle", "hunt_waypoint")
    })
    if ($serviceAfterRecovery.Count -lt 1 -or $huntAfterRecovery.Count -ne 0) {
        throw "The final recovered controller did not resume service before hunting."
    }
    if (@($events[($thirdDeathIndex + 1)..($events.Count - 1)] | Where-Object {
        $_.event -eq "lifecycle" -and $_.status -eq "online"
    }).Count -ne 0) {
        throw "The bot relogged after reaching the configured death-loop limit."
    }
}

function Assert-HealingEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $heals = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
        $_.method -eq "small_health_potion" -and $_.item_id -eq 8704 -and
        $_.trigger -eq "health_threshold" -and $_.objective -eq "hunt" -and
        $_.health_after -gt $_.health_before -and $_.resource_after -eq ($_.resource_before - 1)
    })
    $failures = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -ne "success"
    })
    $lastHealIndex = -1
    $planAfterHealing = $false
    for ($index = 0; $index -lt $events.Count; $index++) {
        $event = $events[$index]
        if ($event.event -eq "action_result" -and $event.action -eq "heal" -and $event.result -eq "success") {
            $lastHealIndex = $index
        }
        elseif ($lastHealIndex -ge 0 -and $event.event -eq "action_result" -and $event.action -eq "plan" -and
            $event.result -eq "success" -and $event.destination.x -eq 32084 -and
            $event.destination.y -eq 32144 -and $event.destination.z -eq 5) {
            $planAfterHealing = $true
        }
    }
    if ($heals.Count -lt 1 -or $heals.Count -gt 3) {
        throw "Expected one to three verified small-health-potion actions, found $($heals.Count)."
    }
    if ($heals[-1].health_after * 100 -le $heals[-1].health_max * 60) {
        throw "The bot did not heal above its configured health threshold."
    }
    if ($failures.Count -ne 0) {
        throw "The focused healing scenario produced a failed or skipped healing outcome."
    }
    if (-not $planAfterHealing) {
        throw "The bot did not resume its interrupted hunt objective after healing."
    }
    if (@($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "The playerbot emitted a terminal event during healing."
    }
}

function Assert-HealingResupplyEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $missingSupply = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and
        $_.result -eq "skipped" -and $_.reason -eq "missing_supply" -and $_.objective -eq "hunt"
    })
    $purchases = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_potions" -and $_.result -eq "success"
    })
    $heals = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "heal" -and $_.result -eq "success" -and
        $_.objective -eq "service" -and $_.resource_after -eq ($_.resource_before - 1)
    })
    $serviceResumed = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_meat" -and $_.result -eq "success"
    })
    $transactionFailures = @($events | Where-Object {
        $_.reason -eq "transaction_delta_mismatch" -or $_.reason -eq "shop_transaction_delta_mismatch"
    })
    if ($missingSupply.Count -ne 1 -or $purchases.Count -lt 1 -or $heals.Count -lt 1) {
        throw "The bot did not refill and consume potions after the missing-supply healing outcome."
    }
    if ($serviceResumed.Count -lt 1) {
        throw "The bot did not resume service after healing with newly purchased potions."
    }
    if ($transactionFailures.Count -ne 0 -or @($events | Where-Object { $_.event -eq "terminal" }).Count -ne 0) {
        throw "Healing interfered with service transaction verification."
    }
}

function Assert-ValueLootEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $replacement = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot_replace" -and $_.result -eq "success" -and
        $_.discarded_item_id -eq 2992 -and $_.discarded_count -eq 1 -and $_.discarded_value -eq 2 -and
        $_.incoming_item_id -eq 2826
    })
    $incomingLoot = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.result -eq "success" -and
        $_.item_id -eq 2826 -and $_.count -eq 1 -and $_.unit_value -eq 5 -and $_.total_value -eq 5
    })
    $capacitySkips = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and $_.reason -eq "no_capacity"
    })
    $bankFundedPurchase = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "buy_potions" -and $_.result -eq "success" -and
        $_.bank_after -lt $_.bank_before
    })
    if ($replacement.Count -ne 1 -or $incomingLoot.Count -ne 1 -or $capacitySkips.Count -ne 0 -or
        $bankFundedPurchase.Count -ne 1) {
        throw "The bot did not replace lower-value cargo with the more profitable corpse item."
    }
}

function Assert-NavigationEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $waypoints = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "hunt_waypoint" -and $_.result -eq "reached"
    })
    $actual = @($waypoints[0..4] | ForEach-Object { "$($_.position.x),$($_.position.y),$($_.position.z)" })
    $expected = @("32084,32144,5", "32103,32124,8", "32117,32090,9", "32103,32124,8", "32084,32144,5")
    if (($actual -join '|') -ne ($expected -join '|')) {
        throw "Unexpected hunting waypoint sequence: $($actual -join ' -> ')"
    }
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })
    $blockedRecovery = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "navigate" -and
        $_.result -eq "failed" -and $_.reason -eq "step_result_mismatch"
    })
    if ($blockedRecovery.Count -lt 1) {
        throw "The full navigation test did not exercise temporary blockage recovery."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during full navigation."
    }
}

function Assert-CorpseEvents {
    param([string]$Logs)

    $events = @(ConvertFrom-PlayerbotLogs -Logs $Logs)
    $nonlootableTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Nonlootable Corpse"
    })
    $nonlootableResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_not_lootable" -and
        $_.expected_corpse_item_id -eq 2148
    })
    $containerDeathItemTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Container Death Item"
    })
    $containerDeathItemResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_not_lootable" -and
        $_.expected_corpse_item_id -eq 1987
    })
    $emptyTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Empty Corpse"
    })
    $emptyResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "skipped" -and $_.reason -eq "corpse_empty"
    })
    $lootTarget = @($events | Where-Object {
        $_.event -eq "target_changed" -and $_.target_name -eq "Playerbot Loot Corpse"
    })
    $lootResult = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "success" -and $_.item_id -eq 2148
    })
    $falseUnavailable = @($events | Where-Object {
        $_.event -eq "action_result" -and $_.action -eq "loot" -and
        $_.result -eq "failed" -and $_.reason -eq "owned_corpse_unavailable"
    })
    $terminal = @($events | Where-Object { $_.event -eq "terminal" })

    if ($nonlootableTarget.Count -lt 1 -or $nonlootableResult.Count -ne 1) {
        throw "The non-lootable corpse was not skipped exactly once."
    }
    if ($containerDeathItemTarget.Count -lt 1 -or $containerDeathItemResult.Count -ne 1) {
        throw "The container-only death item was not skipped exactly once."
    }
    if ($emptyTarget.Count -lt 1 -or $emptyResult.Count -ne 1) {
        throw "The empty corpse was not opened and classified exactly once."
    }
    if ($lootTarget.Count -lt 1 -or $lootResult.Count -lt 1) {
        throw "The guaranteed-loot corpse did not preserve normal looting."
    }
    if ($falseUnavailable.Count -ne 0) {
        throw "An available test corpse was reported unavailable."
    }
    if ($terminal.Count -ne 0) {
        throw "The playerbot emitted a terminal event during corpse classification."
    }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker is required to run the playerbot gameplay suite."
}

try {
    & docker info *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "Docker is not running."
    }

    Invoke-Compose down --volumes --remove-orphans
    $env:PLAYERBOT_GAMEPLAY_MODE = "cycle"
    $env:PLAYERBOT_HUNT_DURATION_SECONDS = "10"
    Invoke-Compose up --build --detach

    Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SERVICE_PASS' | Out-Null
    $cycleLogs = Wait-ForLog -Pattern '"action":"hunt_cycle","result":"started","cycle":2'
    Assert-CycleEvents -Logs $cycleLogs

    if ($FullNavigation) {
        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "navigation"
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
        Invoke-Compose up --detach
        $navigationLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 5
        Assert-NavigationEvents -Logs $navigationLogs
    }

    if ($CorpseLoot) {
        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "corpse"
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
        Invoke-Compose up --detach
        $corpseLogs = Wait-ForLog -Pattern '"reason":"corpse_not_lootable","expected_corpse_item_id":1987'
        Assert-CorpseEvents -Logs $corpseLogs
    }

    if ($DeathTelemetry) {
        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "death"
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
        $env:PLAYERBOT_RELOG_DELAY_SECONDS = "1"
        $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = "2"
        Invoke-Compose up --detach
        Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST DEATH_RECOVERY_STATE_PASS' | Out-Null
        $deathLogs = Wait-ForPlayerbotEvent -Predicate {
            $_.event -eq "lifecycle" -and $_.status -eq "recovery_abandoned" -and $_.reason -eq "death_loop_limit"
        }
        Start-Sleep -Seconds 1
        if ((Get-OnlineBotCount) -ne 0) {
            throw "Bot One remained online after death recovery was abandoned."
        }
        $deathLogs = Get-ServerLogs
        Assert-DeathEvents -Logs $deathLogs
    }

    if ($Healing) {
        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "healing"
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
        Invoke-Compose up --detach
        Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HEALING_STATE_PASS' | Out-Null
        $healingLogs = Wait-ForLog -Pattern '"action":"plan","result":"success".*"destination":\{"x":32084,"y":32144,"z":5\}'
        Assert-HealingEvents -Logs $healingLogs

        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "healing_resupply"
        Invoke-Compose up --detach
        Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST HEALING_RESUPPLY_STATE_PASS' | Out-Null
        $resupplyLogs = Wait-ForLog -Pattern '"action":"buy_meat".*"result":"success"'
        Assert-HealingResupplyEvents -Logs $resupplyLogs
    }

    if ($ValueLoot) {
        Invoke-Compose down --volumes --remove-orphans
        $env:PLAYERBOT_GAMEPLAY_MODE = "value"
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
        Invoke-Compose up --detach
        $valueLogs = Wait-ForLog -Pattern '"action":"buy_potions".*"result":"success"'
        Assert-ValueLootEvents -Logs $valueLogs
    }
    "PLAYERBOT_GAMEPLAY_TEST PASS"
}
finally {
    try {
        if (-not $KeepStack -and (Get-Command docker -ErrorAction SilentlyContinue)) {
            & docker @composeArguments down --volumes --remove-orphans
            if ($LASTEXITCODE -ne 0) {
                throw "Could not clean up the playerbot gameplay stack."
            }
        }
    }
    finally {
        $env:PLAYERBOT_HUNT_DURATION_SECONDS = $previousDuration
        $env:PLAYERBOT_GAMEPLAY_MODE = $previousMode
        $env:PLAYERBOT_RELOG_DELAY_SECONDS = $previousRelogDelay
        $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = $previousMaximumDeaths
    }
}
