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

$TargetClassifications = [ordered]@{
    QUST = "loaded-pending-runtime"
    SCPT = "loaded-pending-runtime"
    PERK = "loaded-pending-runtime"
    AVIF = "loaded-pending-runtime"
    LVLC = "loaded-pending-runtime"
    LVLI = "loaded-pending-runtime"
    LVLN = "loaded-pending-runtime"
}

$RawPendingTargets = @()
$TypedPendingTargets = @("AVIF", "PERK", "LVLC", "LVLI", "LVLN")
$RuntimeBoundary = "PC-flat proves record accounting, typed loading, and selected runtime bridges. It does not claim quest stage execution, FNV script VM semantics, player perk/trait effects, actor value progression, or full leveled-list parity. Loaded-pending-runtime means bytes or typed records are accounted and not silently skipped while gameplay parity remains gated separately."
$ExpectedBridgeKeys = @(
    "gmst",
    "globals",
    "scripts",
    "questDialogues",
    "questNameInfos",
    "questStageInfos",
    "completeQuestStageInfos",
    "failedQuestStageInfos",
    "questObjectives",
    "questObjectiveTargets",
    "topicDialogues",
    "questInfos",
    "resolvedInfoSounds",
    "skippedGmst",
    "skippedGlobals",
    "skippedScripts",
    "skippedDialogues",
    "skippedInfos",
    "unresolvedInfoSounds"
)

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-script-perk-trait-accounting/$Stamp"
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
    Write-ProofLine "OK code anchor: $Description -> $RelativePath"
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

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing ${Description}: $Pattern in $Path"
    }
    Write-ProofLine "OK file pattern: $Description"
}

function Assert-NoUnqualifiedGameplayClaim([string]$Text, [string]$Description) {
    $forbidden = @(
        "working gameplay",
        "quests supported",
        "scripts run",
        "perk support",
        "full FNV gameplay support"
    )
    foreach ($pattern in $forbidden) {
        if ($Text -match [Regex]::Escape($pattern)) {
            throw "Unqualified gameplay claim in ${Description}: $pattern"
        }
    }
    Write-ProofLine "OK bounded claim language: $Description"
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

function Add-Count([hashtable]$Counts, [string]$Key, [int]$Count) {
    if (!$Counts.ContainsKey($Key)) {
        $Counts[$Key] = 0
    }
    $Counts[$Key] += $Count
}

function Get-Count([hashtable]$Counts, [string]$Key) {
    if ($Counts.ContainsKey($Key)) {
        return [int]$Counts[$Key]
    }
    return 0
}

function Get-LedgerRecordCounts([string]$RecordsPath) {
    $counts = @{}
    foreach ($plugin in (Read-JsonArray $RecordsPath "content ledger records")) {
        foreach ($record in @($plugin.records)) {
            Add-Count $counts ([string]$record.type) ([int]$record.count)
        }
    }
    return $counts
}

function Assert-LedgerRowsBounded([string]$RowsPath, [string]$Label, [string]$ExpectedClassification, [string]$ExpectedFailingGate) {
    $rows = Read-JsonArray $RowsPath $Label
    Assert-GreaterThan "$Label row count" $rows.Count 0
    foreach ($row in $rows) {
        if ([string]$row.classification -ne $ExpectedClassification) {
            throw "Unexpected $Label classification: $($row.classification) expected=$ExpectedClassification"
        }
        if ([string]$row.readiness -ne $ExpectedClassification) {
            throw "Unexpected $Label readiness: $($row.readiness) expected=$ExpectedClassification"
        }
        if ([string]$row.firstFailingGate -ne $ExpectedFailingGate) {
            throw "Unexpected $Label first failing gate: $($row.firstFailingGate) expected=$ExpectedFailingGate"
        }
    }
    Write-ProofLine "OK bounded content rows: $Label rows=$($rows.Count) classification=$ExpectedClassification gate=$ExpectedFailingGate"
}

function Assert-PerkTraitRows([string]$GameplaySystemsPath, [int]$ExpectedPerks) {
    $rows = @(Read-JsonArray $GameplaySystemsPath "gameplay systems ledger" | Where-Object {
            [string]$_.recordType -eq "PERK"
        })
    Assert-Equal "PERK gameplay row count" $rows.Count $ExpectedPerks
    foreach ($row in $rows) {
        if ([string]$row.classification -ne "loaded-pending-runtime") {
            throw "Unexpected PERK gameplay classification for $($row.editorId): $($row.classification)"
        }
        if ([string]$row.firstFailingGate -ne "runtime-player-perk-trait-binding") {
            throw "Unexpected PERK gameplay first failing gate for $($row.editorId): $($row.firstFailingGate)"
        }
    }
    foreach ($trait in @("BuiltToDestroy", "WildWasteland")) {
        $traitRows = @($rows | Where-Object { [string]$_.editorId -eq $trait })
        Assert-Equal "trait-like PERK row $trait" $traitRows.Count 1
    }
    Write-ProofLine "OK bounded PERK/trait rows: PERK=$($rows.Count) traits=BuiltToDestroy,WildWasteland gate=runtime-player-perk-trait-binding"
}

function Assert-AvifProgressionRows([string]$GameplaySystemsPath, [int]$ExpectedAvif) {
    $rows = @(Read-JsonArray $GameplaySystemsPath "gameplay systems ledger" | Where-Object {
            [string]$_.recordType -eq "AVIF"
        })
    Assert-Equal "AVIF progression row count" $rows.Count $ExpectedAvif
    $markerTotal = 0
    foreach ($row in $rows) {
        if ([string]$row.classification -ne "loaded-pending-runtime") {
            throw "Unexpected AVIF gameplay classification for $($row.editorId): $($row.classification)"
        }
        if ([string]$row.readiness -ne "loaded-pending-runtime") {
            throw "Unexpected AVIF gameplay readiness for $($row.editorId): $($row.readiness)"
        }
        if ([string]$row.firstFailingGate -ne "runtime-actor-value-progression-binding") {
            throw "Unexpected AVIF first failing gate for $($row.editorId): $($row.firstFailingGate)"
        }
        $markerTotal += [int]$row.progressionMarkerTotal
    }
    Assert-GreaterThan "AVIF progression marker total" $markerTotal 0
    Write-ProofLine "OK bounded AVIF progression rows: AVIF=$($rows.Count) markerTotal=$markerTotal gate=runtime-actor-value-progression-binding"
}

function Get-ClassificationByRecord([string]$ClassificationLedger) {
    $result = @{}
    foreach ($row in (Read-JsonArray $ClassificationLedger "classification ledger")) {
        if ([string]$row.itemType -ne "esm4-record-type") {
            continue
        }
        $recordTypeProperty = $row.PSObject.Properties["recordType"]
        if ($null -eq $recordTypeProperty) {
            continue
        }
        $recordType = [string]$recordTypeProperty.Value
        if ([string]::IsNullOrWhiteSpace($recordType)) {
            continue
        }
        if (!$result.ContainsKey($recordType)) {
            $result[$recordType] = @{}
        }
        $result[$recordType][[string]$row.classification] = $true
    }
    return $result
}

function Convert-RuntimeRecordNameToRecordType([string]$RecordName) {
    if ($RecordName.Length -eq 5 -and $RecordName.EndsWith("4", [System.StringComparison]::Ordinal)) {
        return $RecordName.Substring(0, $RecordName.Length - 1)
    }
    return $RecordName
}

function Get-RuntimeInventoryCounts([string]$OpenMwLog, [string]$Pattern) {
    $counts = @{}
    foreach ($line in @(Select-String -LiteralPath $OpenMwLog -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue)) {
        foreach ($match in $line.Matches) {
            $recordName = [string]$match.Groups["type"].Value
            $recordType = Convert-RuntimeRecordNameToRecordType $recordName
            Add-Count $counts $recordType ([int]$match.Groups["count"].Value)
        }
    }
    return $counts
}

function Get-BridgeCounts([string]$OpenMwLog) {
    $lines = @(Select-String -LiteralPath $OpenMwLog -Pattern "FNV/ESM4 proof: bridged runtime records")
    if ($lines.Count -ne 1) {
        if ($lines.Count -eq 0) {
            throw "Missing runtime bridge proof log in $OpenMwLog"
        }
        throw "Expected exactly one runtime bridge proof log in $OpenMwLog, found $($lines.Count)"
    }

    $line = $lines[0]
    if ($null -eq $line) {
        throw "Missing runtime bridge proof log in $OpenMwLog"
    }

    $counts = @{}
    foreach ($match in [Regex]::Matches($line.Line, "([A-Za-z]+)=([0-9]+)")) {
        $counts[$match.Groups[1].Value] = [int]$match.Groups[2].Value
    }
    foreach ($key in $ExpectedBridgeKeys) {
        if (!$counts.ContainsKey($key)) {
            throw "Runtime bridge proof log missing key: $key line=$($line.Line)"
        }
    }
    return $counts
}

function Assert-FlatContentLines([string]$OpenMwCfg) {
    if (!(Test-Path -LiteralPath $OpenMwCfg -PathType Leaf)) {
        throw "Missing generated openmw.cfg: $OpenMwCfg"
    }
    $contentLines = @(Select-String -LiteralPath $OpenMwCfg -Pattern "^content=" | ForEach-Object {
            $_.Line.Substring("content=".Length)
        })
    Assert-Equal "PC-flat content line count" $contentLines.Count $FlatContent.Count
    for ($i = 0; $i -lt $FlatContent.Count; $i++) {
        if ($contentLines[$i] -ne $FlatContent[$i]) {
            throw "Unexpected PC-flat content line ${i}: actual=$($contentLines[$i]) expected=$($FlatContent[$i])"
        }
    }
    if ($contentLines -contains "FNVR.esp") {
        throw "PC-flat openmw.cfg unexpectedly includes FNVR.esp"
    }
    Write-ProofLine "OK PC-flat content order excludes FNVR.esp: $($contentLines -join ', ')"
}

Write-ProofLine "FNV quest/script/perk/trait accounting contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: $RuntimeBoundary"
Assert-NoUnqualifiedGameplayClaim $RuntimeBoundary "runtime boundary"
Write-ProofLine ""

Assert-Text "components/esm4/loadqust.hpp" "REC_QUST4" "QUST typed loader record id"
Assert-Text "components/esm4/loadqust.hpp" "QuestStageEntry" "QUST stage entry store"
Assert-Text "components/esm4/loadqust.hpp" "QuestObjectiveTarget" "QUST objective target store"
Assert-Text "components/esm4/loadqust.cpp" "mStages.emplace_back" "QUST stage collection loading"
Assert-Text "components/esm4/loadqust.cpp" "mObjectives.emplace_back" "QUST objective collection loading"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questStageInfoId" "QUST stage journal info IDs"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questStageInfos=" "QUST stage bridge count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questObjectiveTargets=" "QUST objective target count log"
Assert-Text "components/esm4/loadscpt.hpp" "REC_SCPT4" "SCPT typed loader record id"
Assert-Text "components/esm4/loadperk.hpp" "REC_PERK4" "PERK typed loader record id"
Assert-Text "components/esm4/loadlvlc.hpp" "REC_LVLC4" "LVLC typed loader record id"
Assert-Text "components/esm4/loadlvli.hpp" "REC_LVLI4" "LVLI typed loader record id"
Assert-Text "components/esm4/loadlvln.hpp" "REC_LVLN4" "LVLN typed loader record id"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Perk>" "PERK store registered"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::LevelledCreature>" "LVLC store registered"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::LevelledItem>" "LVLI store registered"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::LevelledNpc>" "LVLN store registered"
Assert-Text "components/esm4/loadavif.hpp" "REC_AVIF4" "AVIF typed loader record id"
Assert-Text "components/esm4/loadavif.cpp" "mProgressionMarkers" "AVIF typed loader captures progression markers"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::ActorValueInfo>" "AVIF store registered"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::ActorValueInfo>" "AVIF dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "mSubrecordTypeCounts" "raw-pending subrecord type inventory"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "raw-loaded pending subrecords" "raw-pending subrecord summary log"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "TraceRawPendingRecord" "flat proof raw-pending trace parameter"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "def actor_value_row" "content ledger writes AVIF progression rows"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "runtime-actor-value-progression-binding" "content ledger bounds AVIF progression rows"
Assert-Text "apps/openmw/mwgui/spellwindow.cpp" "Runtime membership active; owned=" "perk/trait membership runtime boundary is explicit"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat content ledger proof failed with exit code $LASTEXITCODE."
}
$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV flat content ledger"
$RecordsPath = Join-Path $ContentLedgerDir "records.json"
$LedgerCounts = Get-LedgerRecordCounts $RecordsPath
$QuestsPath = Join-Path $ContentLedgerDir "quests.json"
$ScriptsPath = Join-Path $ContentLedgerDir "scripts.json"
$GameplaySystemsPath = Join-Path $ContentLedgerDir "gameplay-systems.json"
Assert-LedgerRowsBounded $QuestsPath "quest content ledger" "loaded-pending-runtime" "runtime-quest-execution"
Assert-LedgerRowsBounded $ScriptsPath "script content ledger" "loaded-pending-runtime" "missing-fnv-script-runtime"
Assert-PerkTraitRows $GameplaySystemsPath (Get-Count $LedgerCounts "PERK")
Assert-AvifProgressionRows $GameplaySystemsPath (Get-Count $LedgerCounts "AVIF")
$QuestRows = Read-JsonArray $QuestsPath "quest content ledger structured rows"
$QuestNameCount = 0
$QuestStageCount = 0
$QuestStageLogEntryCount = 0
$QuestStageTextEntryCount = 0
$QuestObjectiveCount = 0
$QuestObjectiveTargetCount = 0
foreach ($row in $QuestRows) {
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    if ([int]$row.questNameLength -gt 0) {
        ++$QuestNameCount
    }
    $QuestStageCount += [int]$row.stageCount
    $QuestStageLogEntryCount += [int]$row.stageLogEntryCount
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
}
Assert-GreaterThan "quest ledger stage count" $QuestStageCount 0
Assert-GreaterThan "quest ledger stage text entry count" $QuestStageTextEntryCount 0
Assert-GreaterThan "quest ledger objective count" $QuestObjectiveCount 0
Assert-GreaterThan "quest ledger objective target count" $QuestObjectiveTargetCount 0
Write-ProofLine "Flat content ledger: $ContentLedgerDir"

$ClassificationScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
& $ClassificationScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -ContentLedgerDir $ContentLedgerDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}
$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV classification"
$ClassificationLedger = Join-Path $ClassificationDir "classification-ledger.json"
$ClassificationByRecord = Get-ClassificationByRecord $ClassificationLedger
Write-ProofLine "Classification ledger: $ClassificationLedger"

foreach ($recordType in $TargetClassifications.Keys) {
    $expectedCount = Get-Count $LedgerCounts $recordType
    Assert-GreaterThan "ledger count $recordType" $expectedCount 0
    if (!$ClassificationByRecord.ContainsKey($recordType)) {
        throw "Missing classification row for ESM4 record type $recordType"
    }
    $actualClassification = @($ClassificationByRecord[$recordType].Keys | Sort-Object) -join ","
    $expectedClassification = [string]$TargetClassifications[$recordType]
    if ($actualClassification -ne $expectedClassification) {
        throw "Unexpected classification for ${recordType}: actual=$actualClassification expected=$expectedClassification"
    }
    Write-ProofLine "OK classification: $recordType=$actualClassification"
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound `
    -ClassificationDir $ClassificationDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}
$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$OpenMwCfg = Join-Path $FlatProofDir "openmw.cfg"
if (!(Test-Path -LiteralPath $OpenMwLog -PathType Leaf)) {
    throw "Missing FNV flat OpenMW log: $OpenMwLog"
}
Assert-FlatContentLines $OpenMwCfg
$LoadedCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory loaded:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"
$RawPendingCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory raw-loaded pending:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"
$SkippedCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory skipped unsupported:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"

$runtimeTypes = @{}
foreach ($table in @($LoadedCounts, $RawPendingCounts, $SkippedCounts)) {
    foreach ($recordType in $table.Keys) {
        $runtimeTypes[$recordType] = $true
    }
}
foreach ($recordType in ($runtimeTypes.Keys | Sort-Object)) {
    if (!$LedgerCounts.ContainsKey($recordType)) {
        throw "Runtime inventory logged record type absent from PC-flat ledger: $recordType"
    }
}

$Rows = @()
foreach ($recordType in $TargetClassifications.Keys) {
    $expectedCount = Get-Count $LedgerCounts $recordType
    $loaded = Get-Count $LoadedCounts $recordType
    $rawPending = Get-Count $RawPendingCounts $recordType
    $skipped = Get-Count $SkippedCounts $recordType
    Assert-Equal "runtime loaded $recordType count" $loaded $expectedCount
    Assert-Equal "runtime skipped $recordType count" $skipped 0
    if ($RawPendingTargets -contains $recordType) {
        Assert-Equal "runtime raw-pending $recordType count" $rawPending $expectedCount
    }
    else {
        Assert-Equal "runtime raw-pending $recordType count" $rawPending 0
    }
    $Rows += [ordered]@{
        recordType = $recordType
        ledger = $expectedCount
        loaded = $loaded
        rawPending = $rawPending
        skipped = $skipped
        classification = [string]$TargetClassifications[$recordType]
    }
}

foreach ($recordType in $TypedPendingTargets) {
    Assert-Equal "typed pending $recordType raw-pending subset" (Get-Count $RawPendingCounts $recordType) 0
}

$BridgeCounts = Get-BridgeCounts $OpenMwLog
Assert-Equal "bridge globals count" ([int]$BridgeCounts["globals"]) (Get-Count $LoadedCounts "GLOB")
Assert-Equal "bridge skipped GMST count" ([int]$BridgeCounts["skippedGmst"]) 0
Assert-Equal "bridge skipped global count" ([int]$BridgeCounts["skippedGlobals"]) 0
Assert-Equal "bridge skipped script count" ([int]$BridgeCounts["skippedScripts"]) 0
Assert-Equal "bridge skipped dialogue count" ([int]$BridgeCounts["skippedDialogues"]) 0
Assert-Equal "bridge skipped INFO count" ([int]$BridgeCounts["skippedInfos"]) 9
Assert-Equal "bridge unresolved INFO sound count" ([int]$BridgeCounts["unresolvedInfoSounds"]) 0
Assert-Equal "bridge resolved INFO sound count" ([int]$BridgeCounts["resolvedInfoSounds"]) 228
Assert-GreaterThan "bridged runtime script count" ([int]$BridgeCounts["scripts"]) 0
Assert-GreaterThan "bridged quest dialogue count" ([int]$BridgeCounts["questDialogues"]) 0
$QuestNameRawToFinalDelta = $QuestNameCount - [int]$BridgeCounts["questNameInfos"]
Assert-Equal "quest name raw-to-final override delta" $QuestNameRawToFinalDelta 1
Assert-GreaterThan "bridged quest name info count" ([int]$BridgeCounts["questNameInfos"]) 0
Assert-Equal "bridged QUST stage info count" ([int]$BridgeCounts["questStageInfos"]) $QuestStageTextEntryCount
Assert-GreaterThan "bridged complete QUST stage info count" ([int]$BridgeCounts["completeQuestStageInfos"]) 0
Assert-GreaterThan "accounted failed QUST stage info count" ([int]$BridgeCounts["failedQuestStageInfos"]) 0
Assert-Equal "accounted QUST objective count" ([int]$BridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "accounted QUST objective target count" ([int]$BridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "SCPT bridge override row count" ((Get-Count $LoadedCounts "SCPT") - [int]$BridgeCounts["scripts"] - [int]$BridgeCounts["skippedScripts"]) 8
Assert-Equal "QUST bridge override row count" ((Get-Count $LoadedCounts "QUST") - [int]$BridgeCounts["questDialogues"]) 2

$summary = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    contentLedgerDir = $ContentLedgerDir
    classificationDir = $ClassificationDir
    flatProofDir = $FlatProofDir
    flatContent = $FlatContent
    rows = $Rows
    questLedgerCounts = [ordered]@{
        namedQuests = $QuestNameCount
        namedQuestFinalStoreDelta = $QuestNameRawToFinalDelta
        stages = $QuestStageCount
        stageLogEntries = $QuestStageLogEntryCount
        stageTextEntries = $QuestStageTextEntryCount
        objectives = $QuestObjectiveCount
        objectiveTargets = $QuestObjectiveTargetCount
    }
    bridgeCounts = $BridgeCounts
    runtimeBoundary = $RuntimeBoundary
}
$ContractPath = Join-Path $ProofDir "fnv-quest-script-perk-trait-accounting-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV quest/script/perk/trait accounting contract PASS"
