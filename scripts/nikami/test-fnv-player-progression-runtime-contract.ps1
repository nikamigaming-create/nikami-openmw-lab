param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 20
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}

$FlatContent = @(
    "FalloutNV.esm",
    "DeadMoney.esm",
    "HonestHearts.esm",
    "OldWorldBlues.esm",
    "LonesomeRoad.esm",
    "GunRunnersArsenal.esm",
    "CaravanPack.esm",
    "ClassicPack.esm",
    "MercenaryPack.esm",
    "TribalPack.esm"
)

$ExpectedGmst = [ordered]@{
    iMaxCharacterLevel = 30
    iTraitMenuMaxNumTraits = 2
    iLevelUpSkillPointsBase = 11
    iLevelUpSkillPointsInterval = 1
    iXPBumpBase = 150
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-player-progression-runtime-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null
$RuntimeRunSeconds = [Math]::Max($RunSeconds, 20)

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-Text([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing file for ${Description}: $RelativePath"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing ${Description}: $Needle in $RelativePath"
    }
    Write-ProofLine "OK code anchor: $Description"
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing ${Description}: pattern=$Pattern path=$Path"
    }
    Write-ProofLine "OK ${Description}"
    return $match
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $match) {
        throw "Unexpected ${Description}: $($match.Line.Trim())"
    }
    Write-ProofLine "OK absent ${Description}: $Pattern"
}

function Assert-Equal([string]$Description, [object]$Actual, [object]$Expected) {
    if ($Actual -ne $Expected) {
        throw "Unexpected ${Description}: actual=$Actual expected=$Expected"
    }
    Write-ProofLine "OK ${Description}: $Actual"
}

function Get-LatestProofDir([string]$Root, [string]$Label) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Label proof root: $Root"
    }
    $dir = Get-ChildItem -LiteralPath $Root -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $dir) {
        throw "Missing $Label proof directory under $Root"
    }
    return $dir.FullName
}

function Read-JsonArray([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Label}: $Path"
    }
    $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $value) {
        return @()
    }
    if ($value -is [System.Array]) {
        return @($value)
    }
    return @($value)
}

Write-ProofLine "FNV player progression runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves player-owned max-level progression state can be set/query/save-backed from real bridged FNV GMST values on PC-flat. It does not claim the retail XP curve, skill-point formula, perk-award cadence, trait UI, or perk effects."
Write-ProofLine ""

Assert-Text "components/esm3/npcstats.hpp" "mFalloutExperience" "saved Fallout experience field"
Assert-Text "components/esm3/npcstats.cpp" '"FEXP"' "saved-game Fallout experience subrecord"
Assert-Text "components/esm3/npcstats.cpp" '"FPPT"' "saved-game Fallout pending perk points subrecord"
Assert-Text "components/esm3/npcstats.cpp" '"FTPT"' "saved-game Fallout pending trait points subrecord"
Assert-Text "components/esm3/npcstats.cpp" '"FMLV"' "saved-game Fallout max-level subrecord"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "setFalloutProgressionState" "runtime set Fallout progression API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "getFalloutExperience" "runtime query Fallout experience API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "getFalloutPendingPerkPoints" "runtime query Fallout perk point API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "getFalloutPendingTraitPoints" "runtime query Fallout trait point API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "getFalloutMaxLevel" "runtime query Fallout max-level API"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_PROGRESSION" "progression proof environment hook"
Assert-Text "apps/openmw/engine.cpp" "player-max-level-progression-state-runtime-supported" "bounded progression runtime classification"
Assert-Text "apps/openmw/engine.cpp" "xpCurveRuntime=loaded-pending-runtime" "XP curve remains bounded"
Assert-Text "apps/openmw/engine.cpp" "perkAwardRuntime=loaded-pending-runtime" "perk award cadence remains bounded"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvProgressionTrace" "flat proof can enable progression trace"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$GameSettingsRows = Read-JsonArray (Join-Path $ContentLedgerDir "game-settings.json") "content ledger game settings"
$GmstValues = [ordered]@{}
foreach ($entry in $ExpectedGmst.GetEnumerator()) {
    $row = $GameSettingsRows | Where-Object { [string]$_.editorId -eq [string]$entry.Key } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing FNV GMST in content ledger: $($entry.Key)"
    }
    Assert-Equal "GMST $($entry.Key)" ([int]$row.value) ([int]$entry.Value)
    $GmstValues[$entry.Key] = [int]$row.value
}

$ExpectedExperience = [int]$GmstValues.iXPBumpBase * [int]$GmstValues.iMaxCharacterLevel
$ExpectedPerkPoints = [int]$GmstValues.iMaxCharacterLevel - 1
$ExpectedSkillPointBaseline = [int]$GmstValues.iLevelUpSkillPointsBase `
    + [int]([int]$GmstValues.iMaxCharacterLevel / [int]$GmstValues.iLevelUpSkillPointsInterval)

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -FnvProgressionTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: progression runtime PASS .*maxLevelGmst=$($GmstValues.iMaxCharacterLevel) .*traitSlotsGmst=$($GmstValues.iTraitMenuMaxNumTraits) .*skillPointBaseGmst=$($GmstValues.iLevelUpSkillPointsBase) .*skillPointIntervalGmst=$($GmstValues.iLevelUpSkillPointsInterval) .*xpBumpBaseGmst=$($GmstValues.iXPBumpBase) .*gmstsFound=1 .*afterLevel=$($GmstValues.iMaxCharacterLevel) .*afterExperience=$ExpectedExperience .*pendingPerkPoints=$ExpectedPerkPoints .*pendingTraitPoints=$($GmstValues.iTraitMenuMaxNumTraits) .*pendingSkillPointBaseline=$ExpectedSkillPointBaseline .*saveSubrecords=FEXP,FPPT,FTPT,FMLV .*runtimeBoundary=player-max-level-progression-state-runtime-supported .*xpCurveRuntime=loaded-pending-runtime .*skillPointFormulaRuntime=loaded-pending-runtime .*perkAwardRuntime=loaded-pending-runtime .*traitMenuRuntime=loaded-pending-runtime"
    ) `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: progression runtime PASS" "runtime progression state proof"
Assert-FileContains $FlatSummary "^FnvProgressionTrace: True$" "flat proof required progression trace" | Out-Null
Assert-FileNotContains $OpenMwLog "progression runtime FAIL|progression runtime BLOCKED" "progression runtime failure"

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    gmstValues = $GmstValues
    expectedState = [ordered]@{
        maxLevel = [int]$GmstValues.iMaxCharacterLevel
        experience = $ExpectedExperience
        pendingPerkPoints = $ExpectedPerkPoints
        pendingTraitPoints = [int]$GmstValues.iTraitMenuMaxNumTraits
        pendingSkillPointBaseline = $ExpectedSkillPointBaseline
    }
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "player progression state"
            classification = "runtime-supported"
            proof = "Player level and Fallout progression counters are set/query-backed from real bridged FNV GMST values in PC-flat runtime."
        },
        [ordered]@{
            system = "XP curve / skill points / perk awards / trait menu"
            classification = "loaded-pending-runtime"
            proof = "The runtime state exists, but exact retail formula execution and UI selection flows remain separate gates."
        }
    )
    runtimeBoundary = "Player-owned max-level progression state is runtime-supported; exact retail progression formulas remain loaded-pending-runtime."
}
$metadataPath = Join-Path $ProofDir "fnv-player-progression-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV player progression runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
