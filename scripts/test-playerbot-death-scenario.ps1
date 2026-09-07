# Run in a separate process: pwsh -File scripts/test-playerbot-death-scenario.ps1
$ErrorActionPreference = 'Stop'
$DeathTelemetry = $true
$script:released = $false
$script:observed = $false
$script:validated = $false
function Invoke-Scenario {
    param($Name, $DefaultTimeoutSeconds, $Body)
    if ($Name -ne 'death' -or $DefaultTimeoutSeconds -ne 45) { throw 'Scenario bound changed' }
    & $Body
}
function Invoke-Compose {}
function Wait-ForLog {}
function Start-Sleep {}
function Get-OnlineBotCount { 0 }
function Get-ServerLogs { 'fixture logs' }
function Assert-DeathEvents { param($Logs); $script:validated = $true }
function Invoke-DatabaseCommand {
    param($Query)
    if (-not $script:observed -or $Query -notmatch 'SELECT id, 50099, 1') { throw 'Premature or wrong release' }
    $script:released = $true
}
function Wait-ForPlayerbotEvent {
    param($Predicate)
    if ($script:released) {
        $event = [pscustomobject]@{event='lifecycle'; status='recovery_abandoned'; reason='death_loop_limit'}
        if (-not @($event | Where-Object $Predicate).Count) { throw 'Terminal wait changed' }
        return
    }
    $events = @(
        @{bot='Bot One'; event='service_discovered'},
        @{bot='Bot One'; event='lifecycle'; status='online'; recovered=$true; recovery_count=1},
        @{bot='Bot One'; event='service_discovered'},
        @{bot='Other'; event='lifecycle'; status='online'; recovered=$true; recovery_count=2},
        @{bot='Bot One'; event='service_discovered'},
        @{bot='Bot One'; event='lifecycle'; status='online'; recovered=$true; recovery_count=2},
        @{bot='Other'; event='service_discovered'},
        @{bot='Bot One'; event='service_discovered'}
    )
    for ($i = 0; $i -lt $events.Count; $i++) {
        $matched = @([pscustomobject]$events[$i] | Where-Object $Predicate).Count -gt 0
        if ($matched -ne ($i -eq $events.Count - 1)) { throw "Incorrect milestone match at event $i" }
    }
    $script:observed = $true
}
. "$PSScriptRoot/playerbot-gameplay/scenarios-combat-loot.ps1"
if (-not $script:released -or -not $script:validated) { throw 'Death scenario did not complete its contract' }
'PASS death scenario: ordered second-recovery milestone, DB release, terminal wait, assertions retained'
