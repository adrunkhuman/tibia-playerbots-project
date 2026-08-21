	if ($FullNavigation) {
		Invoke-Scenario -Name "navigation" -DefaultTimeoutSeconds 180 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "navigation"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$navigationLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 5
			Assert-NavigationEvents -Logs $navigationLogs
		}
		Invoke-Scenario -Name "navigation_recovery" -DefaultTimeoutSeconds 150 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "navigation_recovery"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST NAVIGATION_RECOVERY_START' | Out-Null
			$recoveryLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 1
			Assert-NavigationRecoveryEvents -Logs $recoveryLogs
		}
		Invoke-Scenario -Name "patrol_recovery" -DefaultTimeoutSeconds 120 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "patrol_recovery"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST PATROL_RECOVERY_START' | Out-Null
			$recoveryLogs = Wait-ForLog -Pattern '"action":"plan","result":"success"'
			Assert-PatrolRecoveryEvents -Logs $recoveryLogs
		}
	}

	if ($TargetPursuit) {
		Invoke-Scenario -Name "target_pursuit" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_pursuit"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_HIDDEN' | Out-Null
			Wait-ForLog -Pattern '"action":"target_pursuit","result":"reacquired"' | Out-Null
			$pursuitLogs = Wait-ForLog -Pattern '"reason":"target_defeated"'
			Assert-TargetPursuitEvents -Logs $pursuitLogs
		}
		Invoke-Scenario -Name "target_pursuit_abandon" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_pursuit_abandon"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PURSUIT_HIDDEN' | Out-Null
			$pursuitLogs = Wait-ForLog -Pattern '"action":"target_pursuit","result":"abandoned"'
			Assert-TargetPursuitAbandonEvents -Logs $pursuitLogs
		}
	}
