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

$SelectedActorValues = @(
    "AVStrength",
    "AVPerception",
    "AVEndurance",
    "AVCharisma",
    "AVIntelligence",
    "AVAgility",
    "AVLuck"
)

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-avif-actor-value-runtime-contract/$Stamp"
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

function Get-InventoryTotal([string]$Path, [string]$Pattern) {
    $total = 0
    foreach ($line in @(Select-String -LiteralPath $Path -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue)) {
        foreach ($match in $line.Matches) {
            $total += [int]$match.Groups["count"].Value
        }
    }
    return $total
}

Write-ProofLine "FNV AVIF actor value runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "SelectedActorValues: $($SelectedActorValues -join ',')"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves selected real FNV AVIF records resolve through the typed and generic runtime stores and can be set/query as player-owned Fallout actor values on PC-flat. It does not claim SPECIAL derivation, skill formulas, level-up/max-level flow, perk effects, or perk-tree UI."
Write-ProofLine ""

Assert-Text "components/esm4/loadavif.hpp" "REC_AVIF4" "AVIF loader declares typed record id"
Assert-Text "components/esm4/loadavif.cpp" "mProgressionMarkers" "AVIF loader captures progression marker payloads"
Assert-Text "components/esm4/loadavif.cpp" 'case ESM::fourCC("ANAM")' "AVIF loader handles ANAM marker"
Assert-Text "components/esm4/records.hpp" "loadavif.hpp" "AVIF loader is included in ESM4 record bundle"
Assert-Text "components/CMakeLists.txt" "loadavif" "AVIF loader participates in components build"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::ActorValueInfo>" "AVIF store in runtime ESMStore"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::ActorValueInfo>" "AVIF dynamic store instantiation"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_AVIF4:" "AVIF participates in generic runtime lookup"
Assert-Text "components/esm3/npcstats.hpp" "mFalloutActorValues" "saved player Fallout actor value state"
Assert-Text "components/esm3/npcstats.cpp" '"FAVB"' "saved-game FAVB actor value id subrecord"
Assert-Text "components/esm3/npcstats.cpp" '"FAVF"' "saved-game FAVF actor value float subrecord"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "setFalloutActorValue" "runtime set Fallout actor value API"
Assert-Text "apps/openmw/mwmechanics/npcstats.hpp" "getFalloutActorValue" "runtime query Fallout actor value API"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_ACTOR_VALUES" "actor value proof environment hook"
Assert-Text "apps/openmw/engine.cpp" "selected-special-actor-value-state-runtime-supported" "bounded actor value state classification"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvActorValueTrace" "flat proof can enable actor value trace"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "typed-loaded pending full SPECIAL" "content ledger bounds AVIF typed loading"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$GameplayRows = Read-JsonArray (Join-Path $ContentLedgerDir "gameplay-systems.json") "content ledger gameplay systems"
$AvifRows = @($GameplayRows | Where-Object { [string]$_.recordType -eq "AVIF" })
Assert-Equal "AVIF ledger rows" $AvifRows.Count 65

$SelectedRows = @()
foreach ($actorValue in $SelectedActorValues) {
    $row = $AvifRows | Where-Object { [string]$_.editorId -eq $actorValue } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing selected AVIF row: $actorValue"
    }
    Assert-Equal "$actorValue classification" ([string]$row.classification) "loaded-pending-runtime"
    Assert-Equal "$actorValue first failing gate" ([string]$row.firstFailingGate) "runtime-actor-value-progression-binding"
    Assert-GreaterThan "$actorValue progression markers" ([int]$row.progressionMarkerTotal) 0
    Assert-GreaterThan "$actorValue icon length" ([string]$row.icon).Length 0
    Assert-GreaterThan "$actorValue description length" ([int]$row.descriptionLength) 0
    $SelectedRows += $row
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -FnvActorValueTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: actor value runtime PASS .*avifRecords=[1-9][0-9]* .*selectedSpecial=7 .*labels=AVStrength,AVPerception,AVEndurance,AVCharisma,AVIntelligence,AVAgility,AVLuck .*missing=0 .*badRecordTypes=0 .*playerMissingValues=0 .*afterCount=7 .*progressionMarkers=7 .*icons=7 .*descriptions=7 .*saveSubrecords=FAVB,FAVF .*runtimeBoundary=selected-special-actor-value-state-runtime-supported .*progressionRuntime=loaded-pending-runtime .*maxLevelRuntime=loaded-pending-runtime .*perkEffectRuntime=loaded-pending-runtime"
    ) `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$summary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: actor value runtime PASS" "runtime actor value state proof"
Assert-FileContains $summary "^FnvActorValueTrace: True$" "flat proof required actor value trace" | Out-Null
$loadedAvif = Get-InventoryTotal $OpenMwLog "FNV/ESM4 inventory loaded:\s+AVIF4\s+count=(?<count>[0-9]+)"
$rawPendingAvif = Get-InventoryTotal $OpenMwLog "FNV/ESM4 inventory raw-loaded pending:\s+AVIF4\s+count=(?<count>[0-9]+)"
Assert-Equal "runtime typed AVIF loaded total" $loadedAvif 65
Assert-Equal "runtime raw-pending AVIF total" $rawPendingAvif 0

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    selectedActorValues = @($SelectedRows | ForEach-Object {
        [ordered]@{
            editorId = $_.editorId
            formId = $_.formId
            plugin = $_.plugin
            progressionMarkerTotal = $_.progressionMarkerTotal
            classificationBeforeGate = $_.classification
            firstFailingGateBeforeGate = $_.firstFailingGate
        }
    })
    loadedAvifTotal = $loadedAvif
    rawPendingAvifTotal = $rawPendingAvif
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "AVIF selected SPECIAL player state"
            item = $SelectedActorValues -join ","
            classification = "runtime-supported"
            proof = "Selected real FNV AVIF records resolve in typed/generic stores and can be set/queryable from player-owned Fallout actor value state."
            notProven = "SPECIAL derivation, skills, level-up/max-level flow, perk effects, conditions, and perk-tree UI remain separate runtime gates."
        },
        [ordered]@{
            system = "AVIF full progression"
            item = "all AVIF rows"
            classification = "loaded-pending-runtime"
            proof = "AVIF records are typed-loaded and counted by the runtime inventory."
            notProven = "Full actor-value progression and max-level rules are not implemented by this gate."
        }
    )
    runtimeBoundary = "Selected SPECIAL actor-value player state is runtime-supported; full progression, max-level flow, skill formulas, and perk effects remain loaded-pending-runtime."
}
$metadataPath = Join-Path $ProofDir "fnv-avif-actor-value-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV AVIF actor value runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
