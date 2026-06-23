param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 8
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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-dialogue-runtime-bridge-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

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
    Write-ProofLine "OK contract: $Description"
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

function Assert-Equal([string]$Description, [int]$Actual, [int]$Expected) {
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

function Get-FlatLedgerRecordCounts([string]$RecordsPath) {
    $flatContentSet = @{}
    foreach ($content in $FlatContent) {
        $flatContentSet[$content] = $true
    }

    $counts = @{}
    foreach ($plugin in (Read-JsonArray $RecordsPath "content ledger records")) {
        if (!$flatContentSet.ContainsKey([string]$plugin.plugin)) {
            continue
        }
        foreach ($record in @($plugin.records)) {
            $type = [string]$record.type
            if (!$counts.ContainsKey($type)) {
                $counts[$type] = 0
            }
            $counts[$type] += [int]$record.count
        }
    }
    return $counts
}

function Get-RecordCount([hashtable]$Counts, [string]$RecordType) {
    if ($Counts.ContainsKey($RecordType)) {
        return [int]$Counts[$RecordType]
    }
    return 0
}

function Get-RuntimeLoadedRecordTotal([string]$OpenMwLog, [string]$RecordType) {
    $pattern = "FNV/ESM4 inventory loaded: $([Regex]::Escape($RecordType))4 count=(\d+)"
    $total = 0
    foreach ($line in @(Select-String -LiteralPath $OpenMwLog -Pattern $pattern -AllMatches)) {
        foreach ($match in $line.Matches) {
            $total += [int]$match.Groups[1].Value
        }
    }
    return $total
}

function Get-BridgeCounts([string]$OpenMwLog) {
    $line = Select-String -LiteralPath $OpenMwLog -Pattern "FNV/ESM4 proof: bridged runtime records" |
        Select-Object -First 1
    if ($null -eq $line) {
        throw "Missing runtime bridge proof log in $OpenMwLog"
    }

    $counts = @{}
    foreach ($match in [Regex]::Matches($line.Line, "([A-Za-z]+)=([0-9]+)")) {
        $counts[$match.Groups[1].Value] = [int]$match.Groups[2].Value
    }
    return $counts
}

Write-ProofLine "FNV dialogue runtime bridge contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/mwworld/store.hpp" "ESM::Dialogue* insert(const ESM::Dialogue& dialogue);" "dialogue store runtime insert"
Assert-Text "apps/openmw/mwworld/store.hpp" "void rebuildRuntimeIndex();" "dialogue store runtime index rebuild"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "bridgeEsm4QuestDialogueStores" "ESM4 quest/dialogue bridge helper"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questDialogues=" "runtime bridge quest dialogue count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questNameInfos=" "runtime bridge quest name INFO count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questStageInfos=" "runtime bridge QUST stage INFO count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questObjectives=" "runtime bridge QUST objective accounting log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "addRuntimeDialogueInfo" "runtime bridge feeds ordered and direct dialogue info stores"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "topicDialogues=" "runtime bridge topic dialogue count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questInfos=" "runtime bridge quest INFO count log"
Assert-Text "components/esm4/loadinfo.cpp" "mSound = ESM::FormId::fromUint32(mResponseData.sound);" "FNV TRDT INFO sound loader"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "resolveEsm4SoundFile" "ESM4 INFO sound FormId resolver"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mSound = infoSound;" "INFO sound path transfer"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "resolvedInfoSounds=" "runtime bridge resolved INFO sound count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mResultScript = source.mScript.scriptSource;" "INFO result script transfer"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"INFO": "dialogue INFO rows are stored and partially bridged pending exhaustive conditions, choices, and result-script parity"' "INFO loaded-pending runtime boundary"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
$RecordsPath = Join-Path $ContentLedgerDir "records.json"
$FlatCounts = Get-FlatLedgerRecordCounts $RecordsPath
$FlatContentSet = @{}
foreach ($content in $FlatContent) {
    $FlatContentSet[$content] = $true
}
$QuestNameCount = 0
$QuestStageTextEntryCount = 0
$QuestObjectiveCount = 0
$QuestObjectiveTargetCount = 0
foreach ($row in (Read-JsonArray (Join-Path $ContentLedgerDir "quests.json") "content ledger quests")) {
    if (!$FlatContentSet.ContainsKey([string]$row.plugin)) {
        continue
    }
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    if ([int]$row.questNameLength -gt 0) {
        ++$QuestNameCount
    }
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
}
Assert-GreaterThan "flat quest stage text ledger count" $QuestStageTextEntryCount 0
Assert-GreaterThan "flat quest objective ledger count" $QuestObjectiveCount 0

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$RuntimeLoaded = @{}
foreach ($recordType in @("GMST", "GLOB", "SCPT", "QUST", "DIAL", "INFO")) {
    $expected = Get-RecordCount $FlatCounts $recordType
    $actual = Get-RuntimeLoadedRecordTotal $OpenMwLog $recordType
    Assert-Equal "runtime loaded $recordType count" $actual $expected
    $RuntimeLoaded[$recordType] = $actual
}

$BridgeCounts = Get-BridgeCounts $OpenMwLog
Assert-Equal "bridged GLOB count" ([int]$BridgeCounts["globals"]) ([int]$RuntimeLoaded["GLOB"])
Assert-Equal "skipped GMST count" ([int]$BridgeCounts["skippedGmst"]) 0
Assert-Equal "skipped GLOB count" ([int]$BridgeCounts["skippedGlobals"]) 0
Assert-Equal "skipped SCPT count" ([int]$BridgeCounts["skippedScripts"]) 0
Assert-Equal "skipped dialogue count" ([int]$BridgeCounts["skippedDialogues"]) 0
Assert-Equal "skipped INFO count" ([int]$BridgeCounts["skippedInfos"]) 9
Assert-Equal "unresolved INFO sound count" ([int]$BridgeCounts["unresolvedInfoSounds"]) 0
Assert-Equal "resolved INFO sound count" ([int]$BridgeCounts["resolvedInfoSounds"]) 228
$QuestNameRawToFinalDelta = $QuestNameCount - [int]$BridgeCounts["questNameInfos"]
Assert-Equal "quest name raw-to-final override delta" $QuestNameRawToFinalDelta 1
Assert-GreaterThan "quest name INFO count" ([int]$BridgeCounts["questNameInfos"]) 0
Assert-Equal "QUST stage INFO bridge count" ([int]$BridgeCounts["questStageInfos"]) $QuestStageTextEntryCount
Assert-Equal "QUST objective accounting count" ([int]$BridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "QUST objective target accounting count" ([int]$BridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "GMST override row count" ([int]$RuntimeLoaded["GMST"] - [int]$BridgeCounts["gmst"] - [int]$BridgeCounts["skippedGmst"]) 1
Assert-Equal "SCPT override row count" ([int]$RuntimeLoaded["SCPT"] - [int]$BridgeCounts["scripts"] - [int]$BridgeCounts["skippedScripts"]) 8
Assert-Equal "quest override row count" ([int]$RuntimeLoaded["QUST"] - [int]$BridgeCounts["questDialogues"]) 2
Assert-Equal "topic override row count" ([int]$RuntimeLoaded["DIAL"] - [int]$BridgeCounts["topicDialogues"]) 262
Assert-Equal "INFO override row count" ([int]$RuntimeLoaded["INFO"] - [int]$BridgeCounts["questInfos"] - [int]$BridgeCounts["skippedInfos"]) 37

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    runtimeLoaded = $RuntimeLoaded
    bridgeCounts = $BridgeCounts
    questLedgerCounts = [ordered]@{
        namedQuests = $QuestNameCount
        namedQuestFinalStoreDelta = $QuestNameRawToFinalDelta
        stageTextEntries = $QuestStageTextEntryCount
        objectives = $QuestObjectiveCount
        objectiveTargets = $QuestObjectiveTargetCount
    }
    overrideRows = [ordered]@{
        gameSettings = [int]$RuntimeLoaded["GMST"] - [int]$BridgeCounts["gmst"] - [int]$BridgeCounts["skippedGmst"]
        scripts = [int]$RuntimeLoaded["SCPT"] - [int]$BridgeCounts["scripts"] - [int]$BridgeCounts["skippedScripts"]
        quests = [int]$RuntimeLoaded["QUST"] - [int]$BridgeCounts["questDialogues"]
        topics = [int]$RuntimeLoaded["DIAL"] - [int]$BridgeCounts["topicDialogues"]
        infos = [int]$RuntimeLoaded["INFO"] - [int]$BridgeCounts["questInfos"] - [int]$BridgeCounts["skippedInfos"]
    }
}
$metadataPath = Join-Path $ProofDir "fnv-dialogue-runtime-bridge-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV dialogue runtime bridge contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
