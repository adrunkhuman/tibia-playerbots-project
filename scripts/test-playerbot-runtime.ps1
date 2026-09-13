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
}
finally {
    Remove-Item -Recurse -Force $FailureArtifactsPath -ErrorAction SilentlyContinue
}

Write-Host 'Playerbot runtime regressions passed.'
