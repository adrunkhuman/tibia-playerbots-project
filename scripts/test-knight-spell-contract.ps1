param()

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$spellFile = Join-Path $projectRoot "server\data\spells\spells.xml"
$npcScriptRoot = Join-Path $projectRoot "server\data\npc\scripts"
[xml]$registry = Get-Content -LiteralPath $spellFile -Raw

# Values come from CipSoft's spell library captured before the December 2010
# spell overhaul: https://web.archive.org/web/20100530041734/http://www.tibia.com/library/?subtopic=spells
$knightSpells = @(
    @{ Name = "Find Person"; Words = "exiva"; Level = 8; Mana = 20; Premium = 0; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Light"; Words = "utevo lux"; Level = 8; Mana = 20; Premium = 0; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Light Healing"; Words = "exura"; Level = 9; Mana = 20; Premium = 0; Vocations = @("Knight", "Elite Knight") }
    # The 8.60 public name was Antidote; Cure Poison remains the current registry identity until #94 reconciles shared spell names.
    @{ Name = "Cure Poison"; Words = "exana pox"; Level = 10; Mana = 30; Premium = 0; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Magic Rope"; Words = "exani tera"; Level = 9; Mana = 20; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Levitate"; Words = "exani hur"; Level = 12; Mana = 50; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Great Light"; Words = "utevo gran lux"; Level = 13; Mana = 60; Premium = 0; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Haste"; Words = "utani hur"; Level = 14; Mana = 60; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Whirlwind Throw"; Words = "exori hur"; Level = 15; Mana = 40; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Challenge"; Words = "exeta res"; Level = 20; Mana = 30; Premium = 1; Vocations = @("Elite Knight") }
    @{ Name = "Charge"; Words = "utani tempo hur"; Level = 25; Mana = 100; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Wound Cleansing"; Words = "exana mort"; Level = 30; Mana = 65; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Train Party"; Words = "utito mas sio"; Level = 32; Mana = 0; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Groundshaker"; Words = "exori mas"; Level = 33; Mana = 160; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Berserk"; Words = "exori"; Level = 35; Mana = 115; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Protector"; Words = "utamo tempo"; Level = 55; Mana = 200; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Blood Rage"; Words = "utito tempo"; Level = 60; Mana = 290; Premium = 1; Vocations = @("Knight", "Elite Knight") }
    @{ Name = "Fierce Berserk"; Words = "exori gran"; Level = 70; Mana = 340; Premium = 1; Vocations = @("Knight", "Elite Knight") }
)

foreach ($expected in $knightSpells) {
    $spell = @($registry.spells.instant | Where-Object { $_.name -eq $expected.Name })
    if ($spell.Count -ne 1) {
        throw "Expected one registered '$($expected.Name)' spell, found $($spell.Count)."
    }

    $spell = $spell[0]
    foreach ($attribute in @("Words", "Level", "Mana")) {
        $xmlName = $attribute.ToLowerInvariant()
        if ([string]$spell.$xmlName -ne [string]$expected.$attribute) {
            throw "$($expected.Name) has $xmlName='$($spell.$xmlName)', expected '$($expected.$attribute)'."
        }
    }

    $premium = if ($spell.premium) { [int]$spell.premium } else { 0 }
    if ($premium -ne $expected.Premium -or [int]$spell.needlearn -ne 1) {
        throw "$($expected.Name) has premium=$premium needlearn=$($spell.needlearn); expected premium=$($expected.Premium) needlearn=1."
    }

    $vocations = @($spell.vocation | ForEach-Object { [string]$_.name })
    $knightVocations = @($vocations | Where-Object { $_ -in @("Knight", "Elite Knight") })
    $vocationDifference = @($knightVocations | Where-Object { $_ -notin $expected.Vocations }) +
        @($expected.Vocations | Where-Object { $_ -notin $knightVocations })
    if ($vocationDifference.Count -gt 0) {
        throw "$($expected.Name) has Knight vocations '$($knightVocations -join ', ')', expected '$($expected.Vocations -join ', ')'."
    }

    $scriptPath = Join-Path (Split-Path $spellFile) "scripts\$($spell.script)"
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        throw "$($expected.Name) references missing script '$($spell.script)'."
    }
}

function Assert-Offer {
    param(
        [string]$Npc,
        [string]$Spell,
        [int]$Price,
        [int]$Level,
        [switch]$Premium
    )

    $content = Get-Content -LiteralPath (Join-Path $npcScriptRoot "$Npc.lua") -Raw
    $premiumPattern = if ($Premium) { ", premium = true" } else { "" }
    $pattern = "spellName = '$([regex]::Escape($Spell))', price = $Price, level = $Level$premiumPattern, vocation =\{[^}]*4[^}]*\}"
    if ($content -notmatch $pattern) {
        throw "$Npc does not offer $Spell for $Price gp at level $Level with the expected premium rule."
    }
}

function Assert-NoOffer {
    param(
        [string]$Npc,
        [string]$Spell
    )

    $content = Get-Content -LiteralPath (Join-Path $npcScriptRoot "$Npc.lua") -Raw
    if ($content -match "spellName = '$([regex]::Escape($Spell))'") {
        throw "$Npc unexpectedly offers $Spell."
    }
}

Assert-Offer -Npc Gregor -Spell "Light Healing" -Price 170 -Level 9
Assert-Offer -Npc Gregor -Spell "Light" -Price 100 -Level 8
Assert-Offer -Npc Puffels -Spell "Whirlwind Throw" -Price 800 -Level 15 -Premium
Assert-Offer -Npc Puffels -Spell "Wound Cleansing" -Price 300 -Level 30 -Premium
Assert-Offer -Npc Eremo -Spell "Challenge" -Price 2000 -Level 20 -Premium
Assert-Offer -Npc Eliza -Spell "Train Party" -Price 4000 -Level 32 -Premium
Assert-Offer -Npc Ursula -Spell "Groundshaker" -Price 1500 -Level 33 -Premium
Assert-Offer -Npc Zoltan -Spell "Fierce Berserk" -Price 5000 -Level 70 -Premium
Assert-NoOffer -Npc Gregor -Spell "Whirlwind Throw"

$loadedData = Get-ChildItem -LiteralPath (Join-Path $projectRoot "server\data") -Recurse -File |
    Where-Object { $_.Extension -in @(".lua", ".xml") } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }
$loadedText = $loadedData -join "`n"
foreach ($unsupported in @("Brutal Strike", "Summon Skullfrost")) {
    if ($loadedText.Contains($unsupported)) {
        throw "Unsupported Knight spell offer remains: $unsupported."
    }
}

"Knight spell contract PASS"
