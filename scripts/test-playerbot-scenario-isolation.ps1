#Requires -Version 7.0
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "playerbot-gameplay/runtime.ps1")

function Invoke-TimedStep { param($Name, [scriptblock]$Body) & $Body }
function Add-ScenarioResult { param($Name, $Status, $ErrorMessage, $ArtifactPath) }

$exactScenarioSelection = $false
$timeoutOverridden = $false
$ContinueOnFailure = $false
$previousPhase = $env:PLAYERBOT_DEPOT_RESTART_PHASE
try {
    Invoke-Scenario -Name "checkpoint" -DefaultTimeoutSeconds 30 -Body {
        $env:PLAYERBOT_DEPOT_RESTART_PHASE = "depart"
    }
    Invoke-Scenario -Name "ordinary" -DefaultTimeoutSeconds 30 -Body {
        if ($env:PLAYERBOT_DEPOT_RESTART_PHASE) { throw "Checkpoint leaked into the next scenario." }
        $env:PLAYERBOT_DEPOT_RESTART_PHASE = "deposit"
        if ($env:PLAYERBOT_DEPOT_RESTART_PHASE -ne "deposit") { throw "Scenario cannot opt into its checkpoint." }
    }
    Write-Host "Scenario checkpoint isolation passed."
}
finally {
    $env:PLAYERBOT_DEPOT_RESTART_PHASE = $previousPhase
}
