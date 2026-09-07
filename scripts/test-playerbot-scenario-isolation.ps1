#Requires -Version 7.0
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "playerbot-gameplay/runtime.ps1")

function Invoke-TimedStep { param($Name, [scriptblock]$Body) & $Body }
function Add-ScenarioResult { param($Name, $Status, $ErrorMessage, $ArtifactPath) $script:lastStatus = $Status }
function Save-ScenarioFailureArtifacts { param($Name, $Status, $Exception, $StartedAt) return "stub-artifacts" }
function Assert-Environment($Expected) {
    foreach ($key in $Expected.Keys) {
        # Before PowerShell 7.5, setting an empty environment value removes it.
        if ([string][Environment]::GetEnvironmentVariable($key) -cne [string]$Expected[$key]) {
            throw "Unexpected environment value for $key."
        }
    }
}

$defaults = @{
    PLAYERBOT_GAMEPLAY_MODE = "cycle"
    PLAYERBOT_HUNT_DURATION_SECONDS = "1500"
    PLAYERBOT_RELOG_DELAY_SECONDS = "5"
    PLAYERBOT_MAX_CONSECUTIVE_DEATHS = "3"
    PLAYERBOT_DEPOT_RESTART_PHASE = ""
    PLAYERBOT_DEPOT_VERIFIER_PHASE = ""
    PLAYERBOT_DEPOT_MOVE_CASE = "normal"
}
$unrelated = @{ PLAYERBOT_SPEED_BONUS = "123"; PLAYERBOT_REGRESSION_MODE = "untouched" }
$previous = @{}
foreach ($key in @($defaults.Keys) + @($unrelated.Keys)) {
    $previous[$key] = [Environment]::GetEnvironmentVariable($key)
}
$exactScenarioSelection = $false
$timeoutOverridden = $true
$TimeoutSeconds = 77
$ContinueOnFailure = $false
try {
    foreach ($key in $unrelated.Keys) { [Environment]::SetEnvironmentVariable($key, $unrelated[$key]) }
    # Exercise incoming populated, absent, and explicitly empty values.
    foreach ($incomingValue in @("incoming", $null, "")) {
        $incoming = @{}
        foreach ($key in $defaults.Keys) {
            [Environment]::SetEnvironmentVariable($key, $incomingValue)
            $incoming[$key] = [Environment]::GetEnvironmentVariable($key)
        }
        Invoke-Scenario -Name "override" -DefaultTimeoutSeconds 30 -Body {
            Assert-Environment $defaults
            Assert-Environment $unrelated
            if ($currentWaitTimeoutSeconds -ne 77) { throw "CLI timeout was lost." }
            foreach ($key in $defaults.Keys) { [Environment]::SetEnvironmentVariable($key, "case-override") }
            $override = @{}
            foreach ($key in $defaults.Keys) { $override[$key] = "case-override" }
            Assert-Environment $override
        }
        if ($lastStatus -ne "pass") { throw "Override scenario did not pass." }
        Assert-Environment $incoming
        Invoke-Scenario -Name "ordinary" -DefaultTimeoutSeconds 30 -Body { Assert-Environment $defaults }
        Assert-Environment $incoming

        foreach ($continue in @($false, $true)) {
            $ContinueOnFailure = $continue
            $caught = $false
            try {
                Invoke-Scenario -Name "throwing" -DefaultTimeoutSeconds 30 -Body {
                    Assert-Environment $defaults
                    foreach ($key in $defaults.Keys) { [Environment]::SetEnvironmentVariable($key, "failed-case") }
                    throw "deliberate failure"
                }
            }
            catch {
                if ($_.Exception.Message -ne "deliberate failure") { throw }
                $caught = $true
            }
            if ($caught -eq $continue -or $lastStatus -ne "fail") { throw "Failure handling changed." }
            Assert-Environment $incoming
            Invoke-Scenario -Name "after-failure" -DefaultTimeoutSeconds 30 -Body { Assert-Environment $defaults }
        }
        $exactScenarioSelection = $true
        $selectedScenarios = [System.Collections.Generic.HashSet[string]]::new()
        Invoke-Scenario -Name "skipped" -DefaultTimeoutSeconds 30 -Body { throw "Skipped body ran." }
        if ($lastStatus -ne "skipped") { throw "Scenario was not skipped." }
        Assert-Environment $incoming
        Assert-Environment $unrelated
        $exactScenarioSelection = $false
    }
    Write-Host "Scenario environment isolation passed."
}
finally {
    foreach ($key in $previous.Keys) { [Environment]::SetEnvironmentVariable($key, $previous[$key]) }
}
