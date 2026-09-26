#Requires -Version 7.0
$ErrorActionPreference = 'Stop'

$composeArguments = @()
$composeCommandLogBuffer = [System.Text.StringBuilder]::new()
$serverLogBuffer = [System.Text.StringBuilder]::new()
$serverLogLines = [System.Collections.Generic.List[string]]::new()
$serverPlayerbotEvents = [System.Collections.Generic.List[object]]::new()
$serverLogProcess = $null
$serverLogOutputTask = $null
$serverLogErrorTask = $null
$serverLogSinceUtc = $null
$minimumServerOnlineEvents = 0
$FailureArtifactsPath = Join-Path ([System.IO.Path]::GetTempPath()) "playerbot-runtime-$([guid]::NewGuid())"
$scenarioRunId = 'run'
$currentWaitTimeoutSeconds = 30
$ContinueOnFailure = $false
$KeepStack = $false
$failLogFetch = $false

. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/runtime.ps1

function docker {
    param([Parameter(ValueFromRemainingArguments)][object[]]$Arguments)

    $command = $Arguments -join ' '
    $global:LASTEXITCODE = 0
    if ($command -eq 'up success') {
        'successful lifecycle noise'
    }
    elseif ($command -eq 'up failure') {
        'actionable compose failure'
        $global:LASTEXITCODE = 17
    }
    elseif ($Arguments -contains 'logs') {
        if ($script:failLogFetch) {
            'could not fetch server logs'
            $global:LASTEXITCODE = 1
            return
        }
        'The Forgotten Server - Version test'
        '{"component":"playerbot","server_run_id":"current","sequence":1,"bot":"Bot One","event":"lifecycle","status":"online"}'
        '{"component":"playerbot","server_run_id":"current","sequence":2,"bot":"Bot One","event":"hunt_region_scan","phase":"candidate","candidate":{"name":"Final diagnostic","score":42}}'
    }
    elseif ($command -match 'ps --all --quiet server') {
        'server-container-id'
    }
    elseif ($command -match 'ps --all') {
        'server-container-id server exited (137)'
    }
    elseif ($Arguments[0] -eq 'inspect') {
        'Name=/server RestartCount=2 Status=exited Running=false OOMKilled=true ExitCode=137 Error=""'
    }
}

try {
    $successOutput = @(Invoke-RawCompose up success)
    if ($successOutput.Count -ne 0) {
        throw 'Successful Compose output leaked to the normal output stream.'
    }
    if ($composeCommandLogBuffer.ToString() -notmatch 'successful lifecycle noise') {
        throw 'Successful Compose output was not retained in the command log.'
    }

    $failed = $false
    try {
        Invoke-RawCompose up failure *> $null
    }
    catch {
        $failed = $_.Exception.Message -match 'exit code 17'
    }
    if (-not $failed -or $composeCommandLogBuffer.ToString() -notmatch 'actionable compose failure') {
        throw 'Compose failure output or exit status was hidden.'
    }

    $completedLine = [System.Threading.Tasks.TaskCompletionSource[string]]::new()
    $completedLine.SetResult('exited follower final line')
    $reader = [pscustomobject]@{}
    $reader | Add-Member -MemberType ScriptMethod -Name ReadLineAsync -Value { return $null }
    $serverLogProcess = [pscustomobject]@{ HasExited = $true; StandardOutput = $reader; StandardError = $reader }
    $serverLogOutputTask = $completedLine.Task
    $serverLogErrorTask = $null
    Stop-ServerLogFollower
    if ($serverLogBuffer.ToString() -notmatch 'exited follower final line') {
        throw 'Completed output from an exited log follower was discarded.'
    }

    # An always-completed reader must not keep a wait inside one poll past its deadline.
    Reset-ServerLogCollection
    $reader = [pscustomobject]@{ Calls = 0 }
    $reader | Add-Member -MemberType ScriptMethod -Name ReadLineAsync -Value {
        $this.Calls++
        return [System.Threading.Tasks.Task]::FromResult('hot producer line')
    }
    $serverLogProcess = [pscustomobject]@{ HasExited = $true; StandardOutput = $reader; StandardError = $reader }
    $serverLogOutputTask = $reader.ReadLineAsync()
    $serverLogErrorTask = $null
    Update-ServerLogs
    if ($reader.Calls -gt ($serverLogPollMaxLines + 1)) {
        throw 'A single poll consumed more than the configured line limit.'
    }
    $currentScenarioDeadline = [DateTime]::UtcNow.AddMilliseconds(250)
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    try { Wait-ForLog -Pattern 'never matches' | Out-Null } catch { $timedOut = $_.Exception -is [System.TimeoutException] }
    if (-not $timedOut -or $clock.Elapsed.TotalSeconds -gt 2 -or $reader.Calls -lt 2) {
        throw 'An always-completed follower prevented the wait deadline from being enforced.'
    }
    $serverLogProcess = $null
    $serverLogOutputTask = $null
    Reset-ServerLogCollection
    $serverLogMaxBytes = 180
    Add-ServerLogLine '{"component":"playerbot","event":"hunt_region_scan","phase":"candidate","candidate":{"name":"test"}}'
    if ($serverPlayerbotEvents.Count -ne 0 -or $serverLogLines.Count -ne 1) {
        throw 'Verbose candidate evidence was not retained only as raw logs.'
    }
    $budgetFailed = $false
    try { Add-ServerLogLine ('x' * 180) } catch { $budgetFailed = $_.Exception.Message -match 'capture exceeded' }
    if (-not $budgetFailed -or $serverLogLines.Count -ne 1 -or -not $serverLogLimitExceeded) {
        throw 'The log capture budget did not reject a line before retaining it.'
    }
    $budgetDirectory = Save-ScenarioFailureArtifacts -Name 'log budget' -Status 'fail' -Exception ([InvalidOperationException]::new('log budget')) -StartedAt ([DateTime]::UtcNow)
    if ((Get-Content (Join-Path $budgetDirectory 'server-stream.log') -Raw) -notmatch '"phase":"candidate"') {
        throw 'Partial raw diagnostics were lost after a log budget failure.'
    }
    $oldBuffer = $serverLogBuffer
    $oldLines = $serverLogLines
    $oldEvents = $serverPlayerbotEvents
    Reset-ServerLogCollection
    $serverLogMaxBytes = 128MB
    if ([object]::ReferenceEquals($oldBuffer, $serverLogBuffer) -or
        [object]::ReferenceEquals($oldLines, $serverLogLines) -or
        [object]::ReferenceEquals($oldEvents, $serverPlayerbotEvents) -or
        $serverLogBytes -ne 0 -or $serverLogLimitExceeded -or $serverLogLines.Count -ne 0 -or $serverLogBuffer.Length -ne 0) {
        throw 'Reset did not release the capture and clear its budget state.'
    }
    $oldBuffer = $oldLines = $oldEvents = $null
    Add-ServerLogLine '{"component":"playerbot","event":"hunt_region_scan","phase":"candidate","candidate":{"name":"test"}}'
    foreach ($eventName in @('hunt_region_candidate', 'hunt_planning_slice', 'hunt_planning_budget', 'hunt_route_connection')) {
        Add-ServerLogLine ('{"component":"playerbot","event":"' + $eventName + '","candidate_phase":"scored"}')
    }
    if ($serverPlayerbotEvents.Count -ne 0 -or $serverLogLines.Count -ne 5) {
        throw 'Current verbose hunt event families were retained as parsed objects or lost from raw evidence.'
    }
    $serverLogProcess = [pscustomobject]@{ HasExited = $true }
    $currentScenarioDeadline = [DateTime]::UtcNow.AddSeconds(2)
    $currentCandidateLogs = Wait-ForPlayerbotEvent { $_.event -eq 'hunt_region_candidate' }
    if ($currentCandidateLogs -notmatch '"event":"hunt_region_candidate"') {
        throw 'A generic wait could not find current raw-only hunt candidates.'
    }
    $candidateLogs = Wait-ForPlayerbotEvent { $_.candidate.name -eq 'test' }
    if ($candidateLogs -notmatch '"phase":"candidate"') {
        throw 'A generic event wait could not find verbose raw-only evidence.'
    }
    Reset-ServerLogCollection
    Add-ServerLogLine '{"component":"playerbot","event":"action_result","action":"hunt_waypoint","result":"reached"}'
    $countLogs = Wait-ForPlayerbotEventCount -Action 'hunt_waypoint' -Count 1
    if ($countLogs -notmatch '"result":"reached"') {
        throw 'Event-count waits did not inspect captured raw events.'
    }
    $serverLogProcess = $null

    Reset-ServerLogCollection
    Add-ServerLogLine '{"component":"playerbot","server_run_id":"prior","sequence":1,"bot":"Bot One","event":"lifecycle","status":"online"}'
    Add-ServerLogLine '{"component":"playerbot","server_run_id":"current","sequence":1,"bot":"Bot One","event":"lifecycle","status":"online"}'
    $artifactDirectory = Save-ScenarioFailureArtifacts -Name 'runtime failure' -Status 'fail' -Exception ([InvalidOperationException]::new('test failure')) -StartedAt ([DateTime]::UtcNow)
    foreach ($file in @('server.log', 'server-stream.log', 'playerbot-events.jsonl', 'compose-ps.txt', 'server-container-state.txt', 'compose-commands.log', 'failure.txt', 'metadata.json')) {
        if (-not (Test-Path (Join-Path $artifactDirectory $file))) {
            throw "Missing failure artifact: $file"
        }
    }
    $events = @(Get-Content (Join-Path $artifactDirectory 'playerbot-events.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    if ((Get-Content (Join-Path $artifactDirectory 'server.log') -Raw) -notmatch '"phase":"candidate"' -or
        @($events | Where-Object { $_.server_run_id -eq 'prior' -and $_.sequence -eq 1 }).Count -ne 1 -or
        @($events | Where-Object { $_.server_run_id -eq 'current' -and $_.sequence -eq 1 }).Count -ne 1 -or
        @($events | Where-Object { $_.server_run_id -eq 'current' -and $_.sequence -eq 2 -and $_.candidate.name -eq 'Final diagnostic' }).Count -ne 1) {
        throw 'Complete final diagnostics or prior-generation JSONL evidence was discarded or duplicated.'
    }
    $state = Get-Content (Join-Path $artifactDirectory 'server-container-state.txt') -Raw
    if ($state -notmatch 'RestartCount=2' -or $state -notmatch 'OOMKilled=true' -or $state -notmatch 'ExitCode=137') {
        throw 'Restart, exit, or OOM diagnostics were not captured.'
    }

    Reset-ServerLogCollection
    Add-ServerLogLine '{"component":"playerbot","server_run_id":"fallback","sequence":1,"bot":"Bot One","event":"terminal"}'
    $failLogFetch = $true
    $fallbackDirectory = Save-ScenarioFailureArtifacts -Name 'log fetch failure' -Status 'fail' -Exception ([InvalidOperationException]::new('test failure')) -StartedAt ([DateTime]::UtcNow)
    if ((Get-Content (Join-Path $fallbackDirectory 'server.log') -Raw) -notmatch '"server_run_id":"fallback"' -or
        (Get-Content (Join-Path $fallbackDirectory 'playerbot-events.jsonl') -Raw) -notmatch '"server_run_id":"fallback"') {
        throw 'Streamed logs were not used when complete log retrieval failed.'
    }

    $script:startedFollowers = 0
    function Invoke-RawCompose {
        param([Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
        $script:lastRawComposeArguments = @($Arguments)
    }
    function Start-ServerLogFollower { $script:startedFollowers++ }

    foreach ($startArguments in @(
        @('up', '--no-deps', '--detach', '--force-recreate', 'server'),
        @('up', '--detach', '--no-deps', 'server')
    )) {
        Reset-ServerLogCollection
        Add-ServerLogLine 'The Forgotten Server - Version old'
        Add-ServerLogLine '{"component":"playerbot","server_run_id":"old","sequence":1,"bot":"Bot One","event":"lifecycle","status":"online"}'
        Add-ServerLogLine 'PLAYERBOT_GAMEPLAY_TEST DEPOT_PASS'
        $minimumServerOnlineEvents = 0
        $serverLogSinceUtc = $null

        Invoke-Compose @startArguments
        if ($minimumServerOnlineEvents -ne 2 -or -not $serverLogSinceUtc -or $startedFollowers -lt 1 -or
            (@($lastRawComposeArguments) -join ' ') -cne ($startArguments -join ' ')) {
            throw "Generic server start did not require a new online event: $($startArguments -join ' ')."
        }
        if (Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)) {
            throw "Old pass marker satisfied a delayed generic start: $($startArguments -join ' ')."
        }

        Add-ServerLogLine 'The Forgotten Server - Version new'
        Add-ServerLogLine 'new startup output'
        if (Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)) {
            throw "Old pass marker satisfied a generic start before its online event: $($startArguments -join ' ')."
        }
        Add-ServerLogLine '{"component":"playerbot","server_run_id":"new","sequence":1,"bot":"Bot One","event":"lifecycle","status":"online"}'
        $newGeneration = Get-LatestServerGenerationLogs -Logs (Get-ServerLogs)
        if ($newGeneration -notmatch '^The Forgotten Server - Version new' -or
            $newGeneration -match 'PLAYERBOT_GAMEPLAY_TEST DEPOT_PASS') {
            throw "Generic server start did not isolate the new log generation: $($startArguments -join ' ')."
        }
    }
}
finally {
    Remove-Item -Recurse -Force $FailureArtifactsPath -ErrorAction SilentlyContinue
}

Write-Host 'Playerbot runtime regressions passed.'
