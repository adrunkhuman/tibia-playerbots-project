function Invoke-RawCompose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    & docker @composeArguments @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed: $($Arguments -join ' ')"
    }
}

function Stop-ServerLogFollower {
    if ($script:serverLogProcess -and -not $script:serverLogProcess.HasExited) {
        $script:serverLogProcess.Kill($true)
        $script:serverLogProcess.WaitForExit()
    }
    $script:serverLogProcess = $null
    $script:serverLogOutputTask = $null
    $script:serverLogErrorTask = $null
}

function Start-ServerLogFollower {
    Stop-ServerLogFollower

    $arguments = @($composeArguments) + @("logs", "--follow", "--no-log-prefix", "server")
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "docker"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $script:serverLogBuffer.Clear() | Out-Null
    $script:serverLogLines.Clear()
    $script:serverPlayerbotEvents.Clear()
    $script:serverLogProcess = [System.Diagnostics.Process]::new()
    $script:serverLogProcess.StartInfo = $startInfo
    if (-not $script:serverLogProcess.Start()) {
        throw "Could not start the server log follower."
    }
    $script:serverLogOutputTask = $script:serverLogProcess.StandardOutput.ReadLineAsync()
    $script:serverLogErrorTask = $script:serverLogProcess.StandardError.ReadLineAsync()
}

function Add-ServerLogLine {
    param([string]$Line)

    [void]$script:serverLogBuffer.AppendLine($Line)
    [void]$script:serverLogLines.Add($Line)
    if (-not $Line.StartsWith('{')) {
        return
    }
    try {
        $event = $Line | ConvertFrom-Json
        if ($event.component -eq "playerbot") {
            [void]$script:serverPlayerbotEvents.Add($event)
        }
    }
    catch {
        # Non-JSON output is retained in the raw diagnostic buffer.
    }
}

function Update-ServerLogs {
    while ($script:serverLogOutputTask -and $script:serverLogOutputTask.IsCompleted) {
        $line = $script:serverLogOutputTask.GetAwaiter().GetResult()
        if ($null -eq $line) {
            $script:serverLogOutputTask = $null
            break
        }
        Add-ServerLogLine -Line $line
        $script:serverLogOutputTask = $script:serverLogProcess.StandardOutput.ReadLineAsync()
    }
    while ($script:serverLogErrorTask -and $script:serverLogErrorTask.IsCompleted) {
        $line = $script:serverLogErrorTask.GetAwaiter().GetResult()
        if ($null -eq $line) {
            $script:serverLogErrorTask = $null
            break
        }
        Add-ServerLogLine -Line $line
        $script:serverLogErrorTask = $script:serverLogProcess.StandardError.ReadLineAsync()
    }
}

function Reset-ScenarioStack {
    Stop-ServerLogFollower
    $script:minimumServerOnlineEvents = 0

    if (-not $script:testStackInitialized) {
        Invoke-RawCompose down --volumes --remove-orphans
        Invoke-RawCompose up --detach --wait --wait-timeout 120 database
        $script:testStackInitialized = $true
    }
    else {
        Invoke-RawCompose rm --stop --force server
    }

    $resetCommand = @'
set -eu
export MYSQL_PWD="angelion-root"
mariadb --host=localhost --user=root -e "DROP DATABASE IF EXISTS angelion; CREATE DATABASE angelion CHARACTER SET utf8;"
mariadb --host=localhost --user=root angelion < /docker-entrypoint-initdb.d/01-schema.sql
mariadb --host=localhost --user=root angelion < /docker-entrypoint-initdb.d/02-god.sql
mariadb --host=localhost --user=root angelion < /docker-entrypoint-initdb.d/03-local-characters.sql
mariadb --host=localhost --user=root angelion < /test-schema/04-playerbots.sql
'@
    & docker @composeArguments exec -T database sh -c $resetCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Could not restore the gameplay test database baseline."
    }

    $botCountQuery = @'
SELECT COUNT(*) FROM player_bots
JOIN players ON players.id = player_bots.player_id
JOIN accounts ON accounts.id = players.account_id
WHERE players.name = 'Bot One' AND accounts.name = 'bot-one' AND players.deletion = 0;
'@
    $botCount = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $botCountQuery
    if ($LASTEXITCODE -ne 0 -or $botCount -ne "1") {
        throw "The gameplay baseline did not restore exactly one valid Bot One registration."
    }

    if (-not (& docker @composeArguments ps --all --quiet playerbot-setup)) {
        Invoke-RawCompose up --detach playerbot-setup
        $setupContainer = & docker @composeArguments ps --all --quiet playerbot-setup
        $setupExitCode = & docker wait $setupContainer
        if ($LASTEXITCODE -ne 0 -or $setupExitCode -ne "0") {
            throw "Playerbot provisioning validation failed."
        }
    }
}

function Invoke-Compose {
    param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)

    if (($Arguments -join ' ') -eq "down --volumes --remove-orphans") {
        Reset-ScenarioStack
        return
    }

    if ($Arguments.Count -ge 2 -and $Arguments[0] -in @("stop", "rm") -and $Arguments -contains "server") {
        Stop-ServerLogFollower
    }
    if (($Arguments -join ' ') -eq "up --detach") {
        Invoke-RawCompose up --no-deps --detach server
        return
    }
    if (($Arguments -join ' ') -eq "up --detach server") {
        $script:minimumServerOnlineEvents = @($script:serverPlayerbotEvents | Where-Object {
            $_.event -eq "lifecycle" -and $_.status -eq "online"
        }).Count + 1
        Invoke-RawCompose up --no-deps --detach server
        return
    }
    Invoke-RawCompose @Arguments
}

function Get-ServerLogs {
    if (-not $script:serverLogProcess) {
        Start-ServerLogFollower
    }
    Update-ServerLogs
    return $script:serverLogBuffer.ToString().TrimEnd("`r", "`n")
}

function Get-OnlineBotCount {
    $query = "SELECT COUNT(*) FROM players_online JOIN players ON players.id = players_online.player_id WHERE players.name = 'Bot One';"
    $output = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $query
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query Bot One's online state."
    }
    return [int]($output | Select-Object -Last 1)
}

function Invoke-DatabaseScalar {
	param([string]$Query)

	$output = & docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion --skip-column-names angelion -e $Query
	if ($LASTEXITCODE -ne 0) {
		throw "Database query failed."
	}
	return [int]($output | Select-Object -Last 1)
}

function Invoke-DatabaseCommand {
	param([string]$Query)

	& docker @composeArguments exec -T database mariadb --host=database --user=angelion --password=angelion angelion -e $Query
	if ($LASTEXITCODE -ne 0) {
		throw "Database command failed."
	}
}

function Throw-WaitTimeout {
	param([string]$Message)

	$status = & docker @composeArguments ps --all 2>&1
	$logs = try { Get-ServerLogs } catch { "Server logs unavailable: $($_.Exception.Message)" }
	$tail = (($logs -split "`r?`n") | Select-Object -Last 80) -join "`n"
	throw "$Message`nScenario: $currentScenario`n--- compose status ---`n$($status -join "`n")`n--- server log tail ---`n$tail"
}

function Wait-ForLog {
    param([string]$Pattern)

	$deadline = $currentScenarioDeadline
    $nextLine = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $script:serverLogProcess) {
            Start-ServerLogFollower
        }
        Update-ServerLogs
        $onlineEvents = @($script:serverPlayerbotEvents | Where-Object {
            $_.event -eq "lifecycle" -and $_.status -eq "online"
        }).Count
        if ($onlineEvents -lt $script:minimumServerOnlineEvents) {
            Start-Sleep -Milliseconds 100
            continue
        }
        while ($nextLine -lt $script:serverLogLines.Count) {
            if ($script:serverLogLines[$nextLine++] -match $Pattern) {
                return Get-ServerLogs
            }
        }
        Start-Sleep -Milliseconds 100
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for server log pattern: $Pattern"
}

function Wait-ForLatestServerGenerationLog {
    param([string]$Pattern)

    while ([DateTime]::UtcNow -lt $currentScenarioDeadline) {
        $logs = Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)
        if ($logs -match $Pattern) {
            return $logs
        }
        Start-Sleep -Milliseconds 100
    }
    Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for latest server generation pattern: $Pattern"
}

function Wait-ForPlayerbotEvent {
    param([scriptblock]$Predicate)

	$deadline = $currentScenarioDeadline
    $nextEvent = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $script:serverLogProcess) {
            Start-ServerLogFollower
        }
        Update-ServerLogs
        while ($nextEvent -lt $script:serverPlayerbotEvents.Count) {
            $event = $script:serverPlayerbotEvents[$nextEvent++]
            if (@($event | Where-Object $Predicate).Count -gt 0) {
                return Get-ServerLogs
            }
        }
        Start-Sleep -Milliseconds 100
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for a playerbot event."
}

function Wait-ForPlayerbotEventCount {
    param(
        [string]$Action,
        [int]$Count
    )

	$deadline = $currentScenarioDeadline
    $nextEvent = 0
    $matchingEvents = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $script:serverLogProcess) {
            Start-ServerLogFollower
        }
        Update-ServerLogs
        while ($nextEvent -lt $script:serverPlayerbotEvents.Count) {
            $event = $script:serverPlayerbotEvents[$nextEvent++]
            if ($event.event -eq "action_result" -and $event.action -eq $Action -and $event.result -eq "reached") {
                ++$matchingEvents
            }
        }
        if ($matchingEvents -ge $Count) {
            return Get-ServerLogs
        }
        Start-Sleep -Milliseconds 100
    }
	Throw-WaitTimeout "Timed out after $currentWaitTimeoutSeconds seconds waiting for $Count '$Action' events."
}

function Invoke-TimedStep {
	param(
		[string]$Name,
		[scriptblock]$Body
	)

	$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	try {
		& $Body
	}
	finally {
		$stopwatch.Stop()
		$timings[$Name] = $stopwatch.Elapsed
	}
}

function Invoke-Scenario {
	param(
		[string]$Name,
		[int]$DefaultTimeoutSeconds,
		[scriptblock]$Body
	)

	$script:currentScenario = $Name
	$script:currentWaitTimeoutSeconds = if ($timeoutOverridden) { $TimeoutSeconds } else { $DefaultTimeoutSeconds }
	$script:currentScenarioDeadline = [DateTime]::UtcNow.AddSeconds($currentWaitTimeoutSeconds)
	Invoke-TimedStep -Name $Name -Body $Body
}
