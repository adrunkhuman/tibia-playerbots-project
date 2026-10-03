#Requires -Version 7.0

param()

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$spellFile = Join-Path $projectRoot "server\data\spells\spells.xml"
$npcScriptRoot = Join-Path $projectRoot "server\data\npc\scripts"
[xml]$registry = Get-Content -LiteralPath $spellFile -Raw

# Words, levels, mana, prices, and premium flags come from CipSoft's spell list captured before the
# December 2010 spell overhaul: https://web.archive.org/web/20100530041734/http://www.tibia.com/library/?subtopic=spells
# Vocations, soul points, and teaching cities come from the latest per-spell pages captured before that update:
# https://web.archive.org/web/2010*/http://www.tibia.com/library/?subtopic=spells&spell=<name>
$freeCities = @("Ab'Dendriel", "Ankrahmun", "Carlin", "Darashia", "Kazordoon", "Liberty Bay", "Port Hope", "Svargrond", "Thais", "Venore", "Yalahar")
$conjureCities = @($freeCities | Where-Object { $_ -ne "Kazordoon" })
$premiumCities = @("Ankrahmun", "Darashia", "Edron", "Liberty Bay", "Port Hope", "Svargrond", "Yalahar")
$both = @("Paladin", "Royal Paladin")

# Registry names stay where they differ from the 8.60 public names (Antidote, Destroy Field, Desintegrate,
# Holy Missile, Invisible); #94 owns that shared rename.
$paladinSpells = @(
    @{ Name = "Find Person"; Words = "exiva"; Level = 8; Mana = 20; Soul = 0; Price = 80; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Light"; Words = "utevo lux"; Level = 8; Mana = 20; Soul = 0; Price = 100; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Light Healing"; Words = "exura"; Level = 9; Mana = 20; Soul = 0; Price = 170; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Magic Rope"; Words = "exani tera"; Level = 9; Mana = 20; Soul = 0; Price = 200; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Cure Poison"; Words = "exana pox"; Level = 10; Mana = 30; Soul = 0; Price = 150; Premium = 0; Vocations = $both; Cities = $freeCities }
    # Intense Healing was level 11 until the December 2010 update raised it to 20.
    @{ Name = "Intense Healing"; Words = "exura gran"; Level = 11; Mana = 70; Soul = 0; Price = 350; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Levitate"; Words = "exani hur"; Level = 12; Mana = 50; Soul = 0; Price = 500; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Great Light"; Words = "utevo gran lux"; Level = 13; Mana = 60; Soul = 0; Price = 500; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Conjure Arrow"; Words = "exevo con"; Level = 13; Mana = 100; Soul = 1; Price = 450; Premium = 0; Vocations = $both; Cities = $conjureCities }
    @{ Name = "Haste"; Words = "utani hur"; Level = 14; Mana = 60; Soul = 0; Price = 600; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Conjure Poisoned Arrow"; Words = "exevo con pox"; Level = 16; Mana = 130; Soul = 2; Price = 700; Premium = 0; Vocations = $both; Cities = $conjureCities }
    @{ Name = "Conjure Bolt"; Words = "exevo con mort"; Level = 17; Mana = 140; Soul = 2; Price = 750; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Destroy Field Rune"; Words = "adito grav"; Level = 17; Mana = 120; Soul = 2; Price = 700; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Disintegrate Rune"; Words = "adito tera"; Level = 21; Mana = 200; Soul = 3; Price = 900; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Ethereal Spear"; Words = "exori con"; Level = 23; Mana = 25; Soul = 0; Price = 1100; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Conjure Sniper Arrow"; Words = "exevo con hur"; Level = 24; Mana = 160; Soul = 3; Price = 800; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Conjure Explosive Arrow"; Words = "exevo con flam"; Level = 25; Mana = 290; Soul = 3; Price = 1000; Premium = 0; Vocations = $both; Cities = $conjureCities }
    @{ Name = "Holy Missile Rune"; Words = "adori san"; Level = 27; Mana = 300; Soul = 3; Price = 1600; Premium = 1; Vocations = $both; Cities = @("Edron") }
    @{ Name = "Protect Party"; Words = "utamo mas sio"; Level = 32; Mana = 0; Soul = 0; Price = 4000; Premium = 1; Vocations = $both; Cities = @("Edron") }
    @{ Name = "Conjure Piercing Bolt"; Words = "exevo con grav"; Level = 33; Mana = 180; Soul = 3; Price = 850; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Divine Healing"; Words = "exura san"; Level = 35; Mana = 210; Soul = 0; Price = 2100; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Invisibility"; Words = "utana vid"; Level = 35; Mana = 440; Soul = 0; Price = 2000; Premium = 0; Vocations = $both; Cities = $freeCities }
    @{ Name = "Divine Missile"; Words = "exori san"; Level = 40; Mana = 20; Soul = 0; Price = 1800; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Enchant Spear"; Words = "exeta con"; Level = 45; Mana = 350; Soul = 3; Price = 2000; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Divine Caldera"; Words = "exevo mas san"; Level = 50; Mana = 160; Soul = 0; Price = 3000; Premium = 1; Vocations = $both; Cities = $premiumCities }
    @{ Name = "Swift Foot"; Words = "utamo tempo san"; Level = 55; Mana = 400; Soul = 0; Price = 6000; Premium = 1; Vocations = $both; Cities = @("Edron") }
    @{ Name = "Conjure Power Bolt"; Words = "exevo con vis"; Level = 59; Mana = 700; Soul = 4; Price = 2000; Premium = 1; Vocations = @("Royal Paladin"); Cities = @("Eremo's isle") }
    @{ Name = "Sharpshooter"; Words = "utito tempo san"; Level = 60; Mana = 450; Soul = 0; Price = 8000; Premium = 1; Vocations = $both; Cities = @("Edron") }
)

# Blank Rune (adori blank) is absent from the 8.60 list but shared by every caster vocation; #94 owns its removal.
$classifiedExtras = @("Blank Rune")

$contractNames = @($paladinSpells | ForEach-Object { $_.Name })
$registeredPaladinSpells = @($registry.spells.instant | Where-Object {
        @($_.vocation | Where-Object { $_.name -in $both }).Count -gt 0
    } | ForEach-Object { [string]$_.name })
$unexpected = @($registeredPaladinSpells | Where-Object { $_ -notin $contractNames -and $_ -notin $classifiedExtras })
if ($unexpected.Count -gt 0) {
    throw "Paladin vocations can use spells outside the 8.60 contract: $($unexpected -join ', ')."
}

foreach ($expected in $paladinSpells) {
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

    $soul = if ($spell.soul) { [int]$spell.soul } else { 0 }
    $premium = if ($spell.premium) { [int]$spell.premium } else { 0 }
    if ($soul -ne $expected.Soul -or $premium -ne $expected.Premium -or [int]$spell.needlearn -ne 1) {
        throw "$($expected.Name) has soul=$soul premium=$premium needlearn=$($spell.needlearn); expected soul=$($expected.Soul) premium=$($expected.Premium) needlearn=1."
    }

    $paladinVocations = @($spell.vocation | ForEach-Object { [string]$_.name } | Where-Object { $_ -in $both })
    $vocationDifference = @($paladinVocations | Where-Object { $_ -notin $expected.Vocations }) +
        @($expected.Vocations | Where-Object { $_ -notin $paladinVocations })
    if ($vocationDifference.Count -gt 0) {
        throw "$($expected.Name) has Paladin vocations '$($paladinVocations -join ', ')', expected '$($expected.Vocations -join ', ')'."
    }

    $scriptPath = Join-Path (Split-Path $spellFile) "scripts\$($spell.script)"
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        throw "$($expected.Name) references missing script '$($spell.script)'."
    }
}

# Trainers that teach Paladins on the map, by town.
$trainerTowns = @{
    Elane = "Thais"; Legola = "Carlin"; Ethan = "Yalahar"; Asrak = "Venore"; Razan = "Darashia"; Helor = "Port Hope"
    Isolde = "Liberty Bay"; Hawkyr = "Svargrond"; Dario = "Ankrahmun"; Ursula = "Edron"; Eliza = "Edron"; Zoltan = "Edron"
    Maealil = "Ab'Dendriel"; Faluae = "Ab'Dendriel"; Eroth = "Ab'Dendriel"; Eremo = "Eremo's isle"
}
$contract = @{}
foreach ($spell in $paladinSpells) {
    $contract[$spell.Name] = $spell
}

$offerPattern = "spellName = '(?<name>[^']+)', price = (?<price>\d+), level = (?<level>\d+)(?<premium>, premium = true)?, vocation =\{(?<vocations>[^}]*)\}"
$paladinOffersByTown = @{}
foreach ($file in Get-ChildItem -LiteralPath $npcScriptRoot -Filter "*.lua" -File) {
    $npc = $file.BaseName
    foreach ($match in [regex]::Matches((Get-Content -LiteralPath $file.FullName -Raw), $offerPattern)) {
        $name = $match.Groups["name"].Value
        $vocations = @($match.Groups["vocations"].Value -split "," | ForEach-Object { $_.Trim() })

        # Shared spells keep one price, level, and premium rule across every vocation's trainers.
        if ($contract.ContainsKey($name)) {
            $expected = $contract[$name]
            $premium = if ($match.Groups["premium"].Success) { 1 } else { 0 }
            if ([int]$match.Groups["price"].Value -ne $expected.Price -or [int]$match.Groups["level"].Value -ne $expected.Level -or $premium -ne $expected.Premium) {
                throw "$npc offers $name for $($match.Groups["price"].Value) gp at level $($match.Groups["level"].Value) with premium=$premium; expected $($expected.Price) gp, level $($expected.Level), premium=$($expected.Premium)."
            }
        }

        if ("3" -notin $vocations) {
            continue
        }
        if (-not $trainerTowns.ContainsKey($npc)) {
            throw "$npc offers $name to Paladins but has no recorded town."
        }
        if (-not $contract.ContainsKey($name)) {
            throw "$npc offers $name to Paladins outside the 8.60 contract."
        }
        $town = $trainerTowns[$npc]
        if ($town -notin $contract[$name].Cities) {
            throw "$npc offers $name to Paladins in $town, which did not teach it in 8.60."
        }
        if (-not $paladinOffersByTown.ContainsKey($town)) {
            $paladinOffersByTown[$town] = [System.Collections.Generic.HashSet[string]]::new()
        }
        [void]$paladinOffersByTown[$town].Add($name)
    }
}

# Kazordoon has no Paladin trainer on this map.
foreach ($town in @($trainerTowns.Values | Sort-Object -Unique)) {
    $missing = @($paladinSpells | Where-Object { $town -in $_.Cities -and -not ($paladinOffersByTown[$town] -and $paladinOffersByTown[$town].Contains($_.Name)) } | ForEach-Object { $_.Name })
    if ($missing.Count -gt 0) {
        throw "No Paladin trainer in $town offers: $($missing -join ', ')."
    }
}

$loadedData = Get-ChildItem -LiteralPath (Join-Path $projectRoot "server\data") -Recurse -File |
    Where-Object { $_.Extension -in @(".lua", ".xml") } |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }
$loadedText = $loadedData -join "`n"
foreach ($unsupported in @("Summon Emberwing")) {
    if ($loadedText.Contains($unsupported)) {
        throw "Unsupported Paladin spell offer remains: $unsupported."
    }
}

"Paladin spell contract PASS"
