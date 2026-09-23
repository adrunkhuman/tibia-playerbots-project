	foreach ($preflightCase in @(
		@{ Name = "navigation_fare_rejection"; Marker = "NAVIGATION_FARE_REJECTION"; Reason = "fare_breaks_restock_reserve" },
		@{ Name = "navigation_risk_rejection"; Marker = "NAVIGATION_RISK_REJECTION"; Reason = "route_danger_above_tolerance" }
	)) {
		if (-not $FullNavigation -and -not $selectedScenarios.Contains($preflightCase.Name)) { continue }
		Invoke-Scenario -Name $preflightCase.Name -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = $preflightCase.Name
			Invoke-Compose up --detach
			Wait-ForLog -Pattern ("PLAYERBOT_GAMEPLAY_TEST " + $preflightCase.Marker + "_START") | Out-Null
			Wait-ForLog -Pattern '"event":"terminal".*"reason":"navigation_route_unavailable"' | Out-Null
			Start-Sleep -Seconds 2
			Assert-NavigationPreflightRejectionEvents -Logs (Get-ServerLogs) -Reason $preflightCase.Reason
		}
	}

	if ($FullNavigation -or $selectedScenarios.Contains("door_passages")) {
		Invoke-Scenario -Name "door_passages" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "door_passages"
			Invoke-Compose up --detach
			$doorLogs = Wait-ForPlayerbotEvent -Predicate { $_.event -eq "door_passages_contract" }
			Assert-DoorPassageEvents -Logs $doorLogs
		}
	}

	if ($FullNavigation -or $selectedScenarios.Contains("transit_return")) {
		Invoke-Scenario -Name "transit_return" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "transit_return"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TRANSIT_RETURN_STATE_PASS' | Out-Null
			$transitLogs = Wait-ForPlayerbotEvent -Predicate {
				$_.event -eq "objective_transition" -and $_.to -eq "deposit_loot"
			}
			Assert-TransitReturnEvents -Logs $transitLogs
		}
	}


	if ($FullNavigation -or $selectedScenarios.Contains("carlin_service_route") -or
		$selectedScenarios.Contains("mutable_portal_route") -or
		$selectedScenarios.Contains("mutable_portal_open_no_shovel")) {
		Invoke-Scenario -Name "navigation" -DefaultTimeoutSeconds 240 -Body {
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
		Invoke-Scenario -Name "svargrond_local_route_recovery" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "svargrond_local_route_recovery"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST SVARGROND_LOCAL_ROUTE_RECOVERY_START' | Out-Null
			$routeLogs = Wait-ForLog -Pattern '"event":"local_route_recovery",".*"result":"reached"'
			Assert-SvargrondLocalRouteRecoveryEvents -Logs $routeLogs
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
			$env:PLAYERBOT_MUTABLE_PORTAL_VARIANT = "closed_with_shovel"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MUTABLE_PORTAL_ROUTE_START CLOSED_WITH_SHOVEL' | Out-Null
			$routeLogs = Wait-ForPlayerbotEventCount -Action "hunt_waypoint" -Count 1
			Assert-MutablePortalRouteEvents -Logs $routeLogs
		}
		Invoke-Scenario -Name "mutable_portal_open_no_shovel" -DefaultTimeoutSeconds 60 -Body {
			Invoke-Compose down --volumes --remove-orphans
			$env:PLAYERBOT_GAMEPLAY_MODE = "mutable_portal_route"
			$env:PLAYERBOT_MUTABLE_PORTAL_VARIANT = "open_without_shovel"
			$env:PLAYERBOT_HUNT_DURATION_SECONDS = "900"
			Invoke-Compose up --detach
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST MUTABLE_PORTAL_ROUTE_START OPEN_WITHOUT_SHOVEL' | Out-Null
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
			Wait-ForLog -Pattern 'PLAYERBOT_GAMEPLAY_TEST TARGET_PRIORITY_ATTACKERS' | Out-Null
			$priorityLogs = Wait-ForLog -Pattern '"reason":"target_defeated"'
			Assert-TargetAttackerPriorityEvents -Logs $priorityLogs
		}
	}
