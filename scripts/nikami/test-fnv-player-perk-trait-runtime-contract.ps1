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

$SelectedTraits = @("BuiltToDestroy", "WildWasteland")
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-player-perk-trait-runtime-contract/$Stamp"
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

function Assert-Equal([string]$Description, [object]$Actual, [object]$Expected) {
    if ($Actual -ne $Expected) {
        throw "Unexpected ${Description}: actual=$Actual expected=$Expected"
    }
    Write-ProofLine "OK ${Description}: $Actual"
}

function Assert-GreaterThan([string]$Description, [int]$Actual, [int]$Minimum) {
    if ($Actual -le $Minimum) {
        throw "Unexpected ${Description}: actual=$Actual minimumExclusive=$Minimum"
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

Write-ProofLine "FNV player perk/trait runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "SelectedTraits: $($SelectedTraits -join ',')"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves selected real FNV PERK records resolve through the typed and generic runtime stores and can be added to/query player-owned Fallout perk state on PC-flat. It does not claim perk effects, conditions, level-up selection, actor-value progression, or UI choice flow."
Write-ProofLine ""

Assert-Text "components/esm4/loadperk.hpp" "REC_PERK4" "PERK loader declares typed record id"
Assert-Text "components/esm4/loadperk.cpp" "mConditions" "PERK loader captures condition chunks"
Assert-Text "components/esm4/loadperk.cpp" "mEffectData" "PERK loader captures effect data chunks"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Perk>" "PERK store in runtime ESMStore"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Perk>" "PERK dynamic store instantiation"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_PERK4:" "PERK participates in generic runtime ESMStore lookup"
Assert-Text "components/esm3/npcstats.hpp" "mFalloutPerks" "saved player Fallout perk state"
Assert-Text "components/esm3/npcstats.cpp" '"FPRK"' "saved-game FPRK perk subrecord"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "addFalloutPerk" "runtime add Fallout perk API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "hasFalloutPerk" "runtime query Fallout perk API"
Assert-Text "apps/openmw/mwmechanics/npcstats.cpp" "mFalloutPerks.insert" "runtime perk set insertion"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_PLAYER_PERKS" "player perk proof environment hook"
Assert-Text "apps/openmw/engine.cpp" "player-perk-membership-runtime-supported" "bounded player perk membership classification"
Assert-Text "apps/openmw/engine.cpp" "effectsRuntime=loaded-pending-runtime" "perk effects remain pending"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvPlayerPerkTrace" "flat proof can enable player perk trace"
Assert-Text "apps/openmw/mwgui/spellwindow.cpp" "perk membership runtime active; perk effects/level-up pending" "DATA pane does not overclaim perk effects"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
$GameplayRows = Read-JsonArray (Join-Path $ContentLedgerDir "gameplay-systems.json") "content ledger gameplay systems"
$PerkRows = @($GameplayRows | Where-Object { [string]$_.recordType -eq "PERK" })
Assert-GreaterThan "PERK ledger rows" $PerkRows.Count 0

$SelectedRows = @()
foreach ($trait in $SelectedTraits) {
    $row = $PerkRows | Where-Object { [string]$_.editorId -eq $trait } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing selected trait PERK row: $trait"
    }
    Assert-Equal "$trait classification" ([string]$row.classification) "loaded-pending-runtime"
    Assert-Equal "$trait first failing gate" ([string]$row.firstFailingGate) "runtime-player-perk-trait-binding"
    Assert-GreaterThan "$trait full name length" ([int]$row.fullNameLength) 0
    $SelectedRows += $row
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -FnvPlayerPerkTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: player perk runtime PASS .*builtEdid=BuiltToDestroy .*builtRecordType=0x[1-9a-fA-F][0-9a-fA-F]* .*builtHas=1 .*wildEdid=WildWasteland .*wildRecordType=0x[1-9a-fA-F][0-9a-fA-F]* .*wildHas=1 .*saveSubrecord=FPRK .*runtimeBoundary=player-perk-membership-runtime-supported .*effectsRuntime=loaded-pending-runtime .*levelUpSelectionRuntime=loaded-pending-runtime .*actorValueRuntime=loaded-pending-runtime"
    ) `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$summary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: player perk runtime PASS" "runtime player perk membership proof"
Assert-FileContains $summary "^FnvPlayerPerkTrace: True$" "flat proof required player perk trace" | Out-Null

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    selectedTraits = @($SelectedRows | ForEach-Object {
        [ordered]@{
            editorId = $_.editorId
            formId = $_.formId
            plugin = $_.plugin
            classificationBeforeGate = $_.classification
            firstFailingGateBeforeGate = $_.firstFailingGate
        }
    })
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "PERK player membership"
            item = "BuiltToDestroy, WildWasteland"
            classification = "runtime-supported"
            proof = "Selected real FNV PERK records resolve in typed/generic stores and are added to/queryable from player-owned Fallout perk state."
            notProven = "Conditions, effects, level-up selection UI, perk scripts, actor-value progression, and full PERK parity remain separate runtime gates."
        },
        [ordered]@{
            system = "PERK effects"
            item = "all PERK rows"
            classification = "loaded-pending-runtime"
            proof = "Effect payloads are loaded and accounted in the ledger."
            notProven = "Effect execution has not been bound to mechanics."
        },
        [ordered]@{
            system = "AVIF actor values"
            item = "all AVIF rows"
            classification = "loaded-pending-runtime"
            proof = "Actor-value rows remain explicitly bounded by the content ledger."
            notProven = "FNV actor-value progression and max-level rules are not implemented by this gate."
        }
    )
    runtimeBoundary = "Selected player PERK membership is runtime-supported; perk effects, level-up selection, scripts, and actor-value progression remain loaded-pending-runtime."
}
$metadataPath = Join-Path $ProofDir "fnv-player-perk-trait-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV player perk/trait runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
