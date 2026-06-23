param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 30
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

$QuestId = "VMS57"
$StageIndex = 10
$ExpectedStageTextEntries = 1
$ScreenshotFrames = "180,300"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-stage-done-runtime-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null
$RuntimeRunSeconds = [Math]::Max($RunSeconds, 30)

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
    foreach ($key in @("questObjectives", "questObjectiveTargets", "questStageInfos")) {
        if (!$counts.ContainsKey($key)) {
            throw "Runtime bridge proof log missing key: $key line=$($line.Line)"
        }
    }
    return $counts
}

Write-ProofLine "FNV quest stage-done runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "QuestId: $QuestId"
Write-ProofLine "StageIndex: $StageIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves selected Fallout-native GetStageDone is compiler-bound and runtime-bound on PC-flat against actual quest entry state. SetJournalIndex alone moves current stage but does not mark the stage done; SetStage adds the real harvested stage entry and GetStageDone then returns true. It does not claim exhaustive condition evaluation, quest script semantics, rewards, target/HUD routing, or full quest lifecycle parity."
Write-ProofLine ""

Assert-Text "apps/openmw/mwbase/journal.hpp" "isQuestStageDone" "journal stage-done state API"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "JournalEntry::idFromIndex(id, index)" "stage-done resolves real journal info id"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "iter->mInfoId == infoId" "stage-done checks actual quest entry"
Assert-Text "components/compiler/opcodes.hpp" "opcodeGetStageDone = 0x2000331" "GetStageDone opcode id"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerFunction("getstagedone", ''l'', "cl", opcodeGetStageDone)' "GetStageDone compiler function"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpGetStageDone" "GetStageDone runtime opcode"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "installSegment5<OpGetStageDone>" "GetStageDone opcode installed"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "selected-stage-done-entry-state-runtime-supported" "bounded stage-done runtime classification"
Assert-Text "apps/openmw/mwscript/docs/vmformat.txt" "op 0x2000331: GetStageDone" "VM docs GetStageDone"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"QUST": "quest records are stored, QUST stage journal entries are bridged, selected SetStage/GetStage/GetStageDone, StartQuest/CompleteQuest/GetQuestCompleted/GetQuestRunning, and stage fragments execute, selected objective target references can resolve, and objective displayed/completed script state is bound pending full quest lifecycle/HUD-marker/condition parity"' "QUST remains loaded-pending runtime with selected stage-done support"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$QuestObjectiveCount = 0
$QuestObjectiveTargetCount = 0
$QuestStageTextEntryCount = 0
$QuestRow = $null
$StageRow = $null
foreach ($row in (Read-JsonArray (Join-Path $ContentLedgerDir "quests.json") "content ledger quests")) {
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
    if ([string]$row.editorId -eq $QuestId) {
        $QuestRow = $row
    }
}
if ($null -eq $QuestRow) {
    throw "Missing expected real FNV quest in content ledger: $QuestId"
}
foreach ($stage in @($QuestRow.stages)) {
    if ([int]$stage.stageIndex -eq $StageIndex) {
        $StageRow = $stage
        break
    }
}
if ($null -eq $StageRow) {
    throw "Missing expected real FNV quest stage in content ledger: ${QuestId}:${StageIndex}"
}
$StageTextEntries = @($StageRow.logEntries | Where-Object { [int]$_.logLength -gt 0 })
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0
Assert-Equal "selected quest stage text entries" $StageTextEntries.Count $ExpectedStageTextEntries
Write-ProofLine "Selected quest source: plugin=$($QuestRow.plugin) formId=$($QuestRow.formId) stage=$StageIndex textEntries=$($StageTextEntries.Count)"

$StageDoneStartupScript = Join-Path $ProofDir "quest-stage-done-opcodes-startup.txt"
@(
    "GetStageDone $QuestId $StageIndex",
    "SetJournalIndex $QuestId $StageIndex",
    "GetStage $QuestId",
    "GetStageDone $QuestId $StageIndex",
    "SetStage $QuestId $StageIndex",
    "GetStageDone $QuestId $StageIndex"
) | Set-Content -LiteralPath $StageDoneStartupScript -Encoding ASCII
Write-ProofLine "Stage-done opcode startup script: $StageDoneStartupScript"

$CommonSkyPatterns = @(
    "FNV/ESM4: sky shader mode forceShaders=0 falloutSkyModels=1 program=sky-interpreted",
    "FNV/ESM4: atmosphere vertical colors runtime-supported skyUpper=.*skyLower=.*horizon=.*",
    "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds",
    "FNV/ESM4: enabled FNV Masser moon billboard using texture textures/sky/masser_full\.dds",
    "FNV/ESM4: enabled FNV Secunda moon billboard using texture textures/sky/skymoonfull\.dds",
    "FNV/ESM4 proof: render sun direction .* normalizedSky=.*"
)
$RuntimePatterns = @(
    "FNV/ESM4 proof: quest stage-done MWScript opcode GetStageDone .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=0 .*stageDone=0 .*runtimeBoundary=selected-stage-done-entry-state-runtime-supported",
    "FNV/ESM4 proof: quest journal MWScript opcode SetJournalIndex .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex",
    "FNV/ESM4 proof: quest journal MWScript opcode GetStage .*quest=.*$QuestId.*currentIndex=$StageIndex",
    "FNV/ESM4 proof: quest stage-done MWScript opcode GetStageDone .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex .*stageDone=0 .*runtimeBoundary=selected-stage-done-entry-state-runtime-supported",
    "FNV/ESM4 proof: quest journal MWScript opcode SetStage .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex .*entryAdded=1 .*fallbackSetIndex=0",
    "FNV/ESM4 proof: quest stage-done MWScript opcode GetStageDone .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex .*stageDone=1 .*runtimeBoundary=selected-stage-done-entry-state-runtime-supported"
) + $CommonSkyPatterns

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofDir `
    -RunSeconds $RuntimeRunSeconds `
    -ScreenshotFrames $ScreenshotFrames `
    -RequireSkyColorSanity `
    -RequireSkyPaletteMatch `
    -RequireSunDirectionRuntime `
    -StartupScript $StageDoneStartupScript `
    -FnvQuestJournalScriptTrace `
    -RequireLogPattern $RuntimePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest stage-done MWScript opcode GetStageDone .*quest=.*$QuestId.*stageDone=1" "runtime GetStageDone proof"
Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest stage-done MWScript opcode GetStageDone .*quest=.*$QuestId.*currentIndex=$StageIndex .*stageDone=0" "runtime SetJournalIndex negative proof" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestJournalScriptTrace: True$" "flat proof required journal/stage-done trace" | Out-Null
Assert-FileContains $FlatSummary "^Screenshots: [1-9][0-9]*$" "flat proof kept sky screenshot sentinel" | Out-Null
$BridgeCounts = Get-BridgeCounts $OpenMwLog
Assert-Equal "runtime QUST objective definition accounting" ([int]$BridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "runtime QUST objective target accounting" ([int]$BridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "runtime QUST stage journal bridge accounting" ([int]$BridgeCounts["questStageInfos"]) $QuestStageTextEntryCount

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    questId = $QuestId
    questPlugin = $QuestRow.plugin
    questFormId = $QuestRow.formId
    stageIndex = $StageIndex
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    bridgeCounts = $BridgeCounts
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "selected FNV GetStageDone entry-backed stage state"
            classification = "runtime-supported"
            proof = "GetStageDone VMS57 10 stays false after SetJournalIndex VMS57 10 and becomes true only after SetStage VMS57 10 adds the real harvested stage entry."
        },
        [ordered]@{
            system = "condition evaluation, quest script semantics, rewards, HUD markers, target routing, full quest lifecycle"
            classification = "loaded-pending-runtime"
            proof = "Selected stage-done state works, but full FNV quest condition and lifecycle parity remains separate gates."
        }
    )
    runtimeBoundary = "Selected FNV GetStageDone is runtime-supported against actual quest entry state; conditions, quest script semantics, rewards, HUD markers, target routing, and full lifecycle parity remain loaded-pending-runtime."
    skySentinel = "Runtime proof required sky color sanity, palette match, sun-direction runtime, and sky/sun/moon texture log anchors."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-stage-done-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest stage-done runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
