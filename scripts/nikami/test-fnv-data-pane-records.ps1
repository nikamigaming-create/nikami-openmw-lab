param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($FnvRoot) -and [string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set -FnvRoot, -FnvData, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof."
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvRoot = Split-Path $FnvData -Parent
}
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Rg = Get-Command rg -ErrorAction SilentlyContinue
if ($null -eq $Rg) {
    throw "Missing rg. Install ripgrep or run this proof in the Codex lab shell."
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-data-pane-records/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Write-Section([string]$Name) {
    Write-ProofLine ""
    Write-ProofLine "[$Name]"
}

function Assert-File([string]$Path, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    Write-ProofLine "OK file: $Description -> $Path ($($item.Length) bytes)"
}

function Assert-EsmString([string]$Needle, [string]$Description) {
    & $Rg.Source -a -q --fixed-strings -- $Needle $EsmPath
    if ($LASTEXITCODE -ne 0) {
        throw "Missing ${Description}: $Needle"
    }
    Write-ProofLine "OK FNV DATA anchor: $Description -> $Needle"
}

function Assert-Loader([string]$RelativePath, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    Assert-File $path $Description
}

$EsmPath = Join-Path $FnvData "FalloutNV.esm"

Write-ProofLine "FNV DATA pane source-record proof $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine ""

Write-Section "Files and loaders"
Assert-File $EsmPath "main ESM"
Assert-Loader "components/esm4/loadqust.hpp" "ESM4 QUST loader declaration"
Assert-Loader "components/esm4/loadqust.cpp" "ESM4 QUST loader implementation"
Assert-Loader "components/esm4/loadnote.hpp" "ESM4 NOTE loader declaration"
Assert-Loader "components/esm4/loadnote.cpp" "ESM4 NOTE loader implementation"
Assert-Loader "components/esm4/loaddial.hpp" "ESM4 DIAL loader declaration"
Assert-Loader "components/esm4/loaddial.cpp" "ESM4 DIAL loader implementation"
Assert-Loader "components/esm4/loadtact.hpp" "ESM4 TACT loader declaration"
Assert-Loader "components/esm4/loadtact.cpp" "ESM4 TACT loader implementation"
Assert-Loader "components/esm4/loadammo.hpp" "ESM4 AMMO loader declaration"
Assert-Loader "components/esm4/loadammo.cpp" "ESM4 AMMO loader implementation"

Write-Section "Quests"
Assert-EsmString "VCG01" "opening quest"
Assert-EsmString "GSQuest" "Goodsprings quest family"
Assert-EsmString "GSRadioQuest" "Goodsprings radio quest"
Assert-EsmString "MQ01LookingForManny" "main quest objective/topic anchor"
Assert-EsmString "QuestCompleted" "quest completion status text anchor"

Write-Section "Notes and messages"
Assert-EsmString "CaravanRulesNote" "Caravan note"
Assert-EsmString "CrimsonCaravanHolotapeNV" "Crimson Caravan holotape note"
Assert-EsmString "BoulderCityNote" "Boulder City note"
Assert-EsmString "BMTabithaJournal1Note" "Black Mountain journal note"
Assert-EsmString "BMHutRadioMsg" "Black Mountain radio message"

Write-Section "Radio"
Assert-EsmString "RadioNewVegas" "Radio New Vegas station/dialogue anchor"
Assert-EsmString "RadioNewVegasSCRIPT" "Radio New Vegas script"
Assert-EsmString "RadioNewVegasTA" "Radio New Vegas talking activator"
Assert-EsmString "BlackMountainRadio" "Black Mountain Radio station anchor"
Assert-EsmString "BMRadioMusicMsg" "Black Mountain radio music message"

Write-Section "Perks and traits"
Assert-EsmString "TraitMenu" "trait menu anchor"
Assert-EsmString "TraitMenuMaxNumTraits" "trait menu max-trait setting"
Assert-EsmString "PerkBuiltToDestroy" "Built to Destroy trait/perk"
Assert-EsmString "PerkWildWasteland" "Wild Wasteland trait/perk"
Assert-EsmString "PerkGoodNatured" "Good Natured trait/perk"
Assert-EsmString "Perk_Educated" "Educated perk"

Write-Section "Ammo and alternate ammo"
Assert-EsmString "Ammo9mm" "9mm base ammo"
Assert-EsmString "Ammo9mmHollowPoint" "9mm hollow point alternate ammo"
Assert-EsmString "Ammo9mmP" "9mm plus-P alternate ammo family"
Assert-EsmString "Ammo308" ".308 base ammo"
Assert-EsmString "Ammo308ArmorPiercing" ".308 armor piercing alternate ammo"
Assert-EsmString "Ammo308HollowPoint" ".308 hollow point alternate ammo"
Assert-EsmString "AmmoMicroFusionCell" "microfusion cell base ammo"
Assert-EsmString "AmmoMicroFusionCellMaxCharge" "microfusion cell max-charge alternate ammo"

Write-ProofLine ""
Write-ProofLine "FNV DATA pane source-record proof PASS"
Write-ProofLine "ProofDir: $ProofDir"
