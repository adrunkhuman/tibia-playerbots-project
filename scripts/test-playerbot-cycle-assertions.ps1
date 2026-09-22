#Requires -Version 7.0
$ErrorActionPreference = 'Stop'

. $PSScriptRoot/playerbot-gameplay/log-parsing.ps1
. $PSScriptRoot/playerbot-gameplay/assertions-cycle.ps1

function ConvertTo-FixtureLogs {
    param([object[]]$Events)

    ($Events | ForEach-Object {
        $_.component = 'playerbot'
        $_.bot = 'Bot One'
        $_ | ConvertTo-Json -Compress -Depth 5
    }) -join "`n"
}

function New-CarlinLocalServiceEvents {
    @(
        [ordered]@{ event = 'npc_reply'; npc_name = 'Rachel'; position = @{ x = 32346; y = 31827; z = 7 } }
        [ordered]@{ event = 'npc_reply'; npc_name = 'Eva'; position = @{ x = 32329; y = 31782; z = 7 } }
        [ordered]@{ event = 'action_result'; action = 'buy_potions'; result = 'success'; item_id = 7618; count = 19
            bank_before = 955; bank_after = 100; position = @{ x = 32346; y = 31827; z = 7 } }
        [ordered]@{ event = 'action_result'; action = 'bank_withdraw'; result = 'success'; count = 100
            bank_before = 100; bank_after = 0; position = @{ x = 32329; y = 31782; z = 7 } }
    )
}

function Assert-Rejected {
    param([string]$Name, [scriptblock]$Body)

    try {
        & $Body
    }
    catch {
        if ($_.Exception.Message -notlike 'Carlin service was not completed locally*') {
            throw "$Name failed for an unexpected reason: $($_.Exception.Message)"
        }
        return
    }
    throw "$Name unexpectedly passed."
}

Assert-CarlinLocalServiceEvents -Logs (ConvertTo-FixtureLogs (New-CarlinLocalServiceEvents))

foreach ($case in @('nine_potions', 'wrong_npc', 'wrong_location', 'wrong_purchase_balance', 'wrong_withdrawal_balance', 'failed_service_action')) {
    $events = @(New-CarlinLocalServiceEvents)
    switch ($case) {
        'nine_potions' { $events[2].count = 9 }
        'wrong_npc' { $events[0].npc_name = 'Xodet' }
        'wrong_location' { $events[2].position.x = 32339 }
        'wrong_purchase_balance' { $events[2].bank_after = 101 }
        'wrong_withdrawal_balance' { $events[3].bank_after = 1 }
        'failed_service_action' {
            $events += [ordered]@{ event = 'action_result'; action = 'buy_potions'; result = 'failed'; item_id = 7618 }
        }
    }
    Assert-Rejected "Carlin $case receipt" {
        Assert-CarlinLocalServiceEvents -Logs (ConvertTo-FixtureLogs $events)
    }
}

Write-Host 'Carlin local-service assertion contracts passed.'
