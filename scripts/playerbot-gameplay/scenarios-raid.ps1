if ($RaidTravel) {
    foreach ($raidMode in @('raid_travel', 'raid_recovery')) {
        Invoke-Scenario -Name $raidMode -DefaultTimeoutSeconds 180 -Body {
            $previousRaidCatalog = $env:PLAYERBOT_RAID_CATALOG
            $previousSpeedBonus = $env:PLAYERBOT_SPEED_BONUS
            try {
                Invoke-Compose down --volumes --remove-orphans
                $env:PLAYERBOT_RAID_CATALOG = './tests/playerbot-gameplay/raids/raids.xml'
                $env:PLAYERBOT_SPEED_BONUS = '0'
                $env:PLAYERBOT_GAMEPLAY_MODE = $raidMode
                $env:PLAYERBOT_HUNT_DURATION_SECONDS = '900'
                $env:PLAYERBOT_RELOG_DELAY_SECONDS = '1'
                $env:PLAYERBOT_MAX_CONSECUTIVE_DEATHS = '2'
                Invoke-Compose up --detach
                $logs = Wait-ForLog -Pattern ('PLAYERBOT_GAMEPLAY_TEST ' + $raidMode.ToUpperInvariant() +
                    '_PASS|PLAYERBOT_GAMEPLAY_TEST RAID_FIXTURE_FAILED|"event":"terminal"(?![^\r\n]*"reason":"controlled_player_dead")')
                Assert-RaidTravelEvents -Logs $logs -Mode $raidMode
            }
            finally {
                $env:PLAYERBOT_RAID_CATALOG = $previousRaidCatalog
                $env:PLAYERBOT_SPEED_BONUS = $previousSpeedBonus
            }
        }
    }
}
