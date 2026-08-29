	if ($FullNavigation -or $selectedScenarios.Contains("carlin_service_route") -or $selectedScenarios.Contains("mutable_portal_route")) {
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
		Invoke-Scenario -Name "carlin_service_route" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "carlin_service_route"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST CARLIN_SERVICE_ROUTE_START' | Out-Null
			$routeLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 1
			Assert-CarlinServiceRouteEvents -Logs $routeLogs
		}
		Invoke-Scenario -Name "mutable_portal_route" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mutable_portal_route"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MUTABLE_PORTAL_ROUTE_START' | Out-Null
			$routeLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 1
			Assert-MutablePortalRouteEvents -Logs $routeLogs
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

	if ($TargetApproach) {
		Invoke-Scenario -Name "target_approach" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_approach"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			$approachLogs = Wait-ForLog -Pattern '"reason":"target_defeated"'
			Assert-TargetApproachEvents -Logs $approachLogs
		}
		Invoke-Scenario -Name "target_approach_unreachable" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_approach_unreachable"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern '"action":"target_approach","result":"skipped","reason":"route_unavailable"' | Out-Null
			$approachLogs = Wait-ForLog -Pattern '"action":"plan","result":"success".*"same_floor":false'
			Assert-UnreachableTargetApproachEvents -Logs $approachLogs
		}
		Invoke-Scenario -Name "target_attacker_priority" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "target_attacker_priority"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PRIORITY_ATTACKER' | Out-Null
			$priorityLogs = Wait-ForLog -Pattern '"reason":"target_defeated"'
			Assert-TargetAttackerPriorityEvents -Logs $priorityLogs
		}
	}
