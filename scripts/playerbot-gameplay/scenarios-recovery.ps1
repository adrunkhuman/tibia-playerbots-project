if ($CombatReadiness) {
    foreach ($recoveryMode in @('recovery_fists', 'recovery_melee')) {
        Invoke-Scenario -Name $recoveryMode -DefaultTimeoutSeconds 900 -Body {
            Invoke-Compose down --volumes --remove-orphans
            $previousCompose = $script:composeArguments
            try {
                $script:composeArguments = $previousCompose + @('-f', (Join-Path $projectRoot 'server/compose.playerbot-recovery.yaml'))
                $env:PLAYERBOT_GAMEPLAY_MODE = $recoveryMode
                $env:PLAYERBOT_HUNT_DURATION_SECONDS = '60'
                Invoke-Compose up --detach
                $logs = Wait-ForLog -Pattern ('PLAYERBOT_GAMEPLAY_TEST ' + $recoveryMode.ToUpperInvariant() + '_PASS|"event":"terminal"')
                Assert-RecoveryNotTerminal -Logs $logs
                # The independent sample can precede the next-turn move receipt.
                Wait-ForPlayerbotEvent -Predicate {
                    if ($_.event -eq 'terminal') { throw "Recovery stopped: $($_.reason)" }
                    $_.event -eq 'recovery_loadout' -and $_.result -eq 'restored'
                } | Out-Null
                $logs = Wait-ForPlayerbotEvent -Predicate {
                    if ($_.event -eq 'terminal') { throw "Recovery stopped: $($_.reason)" }
                    $_.event -eq 'action_result' -and $_.action -eq 'buy_throwing_weapons' -and $_.result -eq 'success'
                }
                Assert-RecoveryLoadoutRoundTrip -Logs $logs -Mode $recoveryMode
            }
            finally {
                $script:composeArguments = $previousCompose
            }
        }
    }
}
