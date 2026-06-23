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

$LifecycleQuestId = "VMS39"
$CompleteStageQuestId = "VFSAtomicPimp"
$CompleteStageIndex = 10
$ExpectedCompleteStageFlags = 1
$ExpectedCompleteStageConditionCount = 0
$ExpectedCompleteStageLogLength = 31
$ExpectedCompleteStageScriptLength = 0
$ScreenshotFrames = "180,300"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-status-runtime-contract/$Stamp"
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
    foreach ($key in @("questObjectives", "questObjectiveTargets", "questStageInfos", "completeQuestStageInfos")) {
        if (!$counts.ContainsKey($key)) {
            throw "Runtime bridge proof log missing key: $key line=$($line.Line)"
        }
    }
    return $counts
}

Write-ProofLine "FNV quest status runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "LifecycleQuestId: $LifecycleQuestId"
Write-ProofLine "CompleteStageQuestId: $CompleteStageQuestId"
Write-ProofLine "CompleteStageIndex: $CompleteStageIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves selected Fallout-native StartQuest, CompleteQuest, GetQuestCompleted, and GetQuestRunning MWScript opcodes are compiler-bound and runtime-bound on PC-flat, and that a harvested QUST complete-stage flag can drive completed state through SetStage. It does not claim quest script startup, StopQuest/ResetQuest semantics, condition evaluation, rewards, target/HUD routing, or full quest lifecycle parity."
Write-ProofLine ""

Assert-Text "apps/openmw/mwbase/journal.hpp" "setQuestFinished" "journal quest completion state API"
Assert-Text "apps/openmw/mwbase/journal.hpp" "isQuestStarted" "journal quest started state API"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "getQuestFinished" "quest completion runtime state implementation"
Assert-Text "apps/openmw/mwdialogue/quest.cpp" "mFinished = info->mQuestStatus == ESM::DialInfo::QS_Finished" "complete-stage flag mutates quest completion"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "Flag_CompleteQuest" "FNV QUST complete flag bridge"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mQuestStatus = ESM::DialInfo::QS_Finished" "FNV complete stage emits finished journal info"
Assert-Text "components/compiler/opcodes.hpp" "opcodeStartQuest = 0x200032d" "StartQuest opcode id"
Assert-Text "components/compiler/opcodes.hpp" "opcodeGetQuestRunning = 0x2000330" "GetQuestRunning opcode id"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerInstruction("startquest", "c", opcodeStartQuest)' "StartQuest compiler instruction"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerFunction("getquestcompleted", ''l'', "c", opcodeGetQuestCompleted)' "GetQuestCompleted compiler function"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpStartQuest" "StartQuest runtime opcode"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpCompleteQuest" "CompleteQuest runtime opcode"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpGetQuestCompleted" "GetQuestCompleted runtime opcode"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpGetQuestRunning" "GetQuestRunning runtime opcode"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "installSegment5<OpGetQuestRunning>" "quest running opcode installed"
Assert-Text "apps/openmw/mwscript/docs/vmformat.txt" "op 0x200032d: StartQuest" "VM docs StartQuest"
Assert-Text "apps/openmw/mwscript/docs/vmformat.txt" "op 0x2000330: GetQuestRunning" "VM docs GetQuestRunning"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"QUST": "quest records are stored, QUST stage journal entries are bridged, selected SetStage/GetStage/GetStageDone, StartQuest/CompleteQuest/GetQuestCompleted/GetQuestRunning, and stage fragments execute, selected objective target references can resolve, and objective displayed/completed script state is bound pending full quest lifecycle/HUD-marker/condition parity"' "QUST remains loaded-pending runtime with selected quest status support"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$QuestObjectiveCount = 0
$QuestObjectiveTargetCount = 0
$QuestStageTextEntryCount = 0
$CompleteQuestStageTextEntryCount = 0
$LifecycleQuestRow = $null
$CompleteQuestRow = $null
$CompleteStageRow = $null
$CompleteStageEntry = $null
foreach ($row in (Read-JsonArray (Join-Path $ContentLedgerDir "quests.json") "content ledger quests")) {
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
    foreach ($stage in @($row.stages)) {
        foreach ($entry in @($stage.logEntries)) {
            if ((([int]$entry.flags -band 1) -ne 0) -and [int]$entry.logLength -gt 0) {
                ++$CompleteQuestStageTextEntryCount
            }
        }
    }
    if ([string]$row.editorId -eq $LifecycleQuestId) {
        $LifecycleQuestRow = $row
    }
    if ([string]$row.editorId -eq $CompleteStageQuestId) {
        $CompleteQuestRow = $row
    }
}
if ($null -eq $LifecycleQuestRow) {
    throw "Missing expected real FNV lifecycle quest in content ledger: $LifecycleQuestId"
}
if ($null -eq $CompleteQuestRow) {
    throw "Missing expected real FNV completion-stage quest in content ledger: $CompleteStageQuestId"
}
foreach ($stage in @($CompleteQuestRow.stages)) {
    if ([int]$stage.stageIndex -eq $CompleteStageIndex) {
        $CompleteStageRow = $stage
        break
    }
}
if ($null -eq $CompleteStageRow) {
    throw "Missing expected real FNV completion stage in content ledger: ${CompleteStageQuestId}:${CompleteStageIndex}"
}
foreach ($entry in @($CompleteStageRow.logEntries)) {
    if ((([int]$entry.flags -band 1) -ne 0) -and [int]$entry.logLength -gt 0) {
        $CompleteStageEntry = $entry
        break
    }
}
if ($null -eq $CompleteStageEntry) {
    throw "Missing selected completion-stage entry in content ledger: ${CompleteStageQuestId}:${CompleteStageIndex}"
}
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0
Assert-GreaterThan "ledger complete-stage text entries" $CompleteQuestStageTextEntryCount 0
Assert-Equal "selected complete-stage flags" ([int]$CompleteStageEntry.flags) $ExpectedCompleteStageFlags
Assert-Equal "selected complete-stage condition count" ([int]$CompleteStageEntry.conditionCount) $ExpectedCompleteStageConditionCount
Assert-Equal "selected complete-stage log length" ([int]$CompleteStageEntry.logLength) $ExpectedCompleteStageLogLength
Assert-Equal "selected complete-stage script length" ([int]$CompleteStageEntry.script.sourceLength) $ExpectedCompleteStageScriptLength
Write-ProofLine "Lifecycle quest source: plugin=$($LifecycleQuestRow.plugin) formId=$($LifecycleQuestRow.formId)"
Write-ProofLine "Complete-stage quest source: plugin=$($CompleteQuestRow.plugin) formId=$($CompleteQuestRow.formId) stage=$CompleteStageIndex flags=$($CompleteStageEntry.flags)"

$QuestStatusStartupScript = Join-Path $ProofDir "quest-status-opcodes-startup.txt"
@(
    "GetQuestCompleted $LifecycleQuestId",
    "GetQuestRunning $LifecycleQuestId",
    "StartQuest $LifecycleQuestId",
    "GetQuestRunning $LifecycleQuestId",
    "CompleteQuest $LifecycleQuestId",
    "GetQuestCompleted $LifecycleQuestId",
    "GetQuestRunning $LifecycleQuestId",
    "GetQuestCompleted $CompleteStageQuestId",
    "SetStage $CompleteStageQuestId $CompleteStageIndex",
    "GetStage $CompleteStageQuestId",
    "GetQuestCompleted $CompleteStageQuestId",
    "GetQuestRunning $CompleteStageQuestId"
) | Set-Content -LiteralPath $QuestStatusStartupScript -Encoding ASCII
Write-ProofLine "Quest status opcode startup script: $QuestStatusStartupScript"

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
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestCompleted .*quest=.*$LifecycleQuestId.*started=0 .*running=0 .*completed=0 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestRunning .*quest=.*$LifecycleQuestId.*started=0 .*running=0 .*completed=0 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode StartQuest .*quest=.*$LifecycleQuestId.*started=1 .*running=1 .*completed=0 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode CompleteQuest .*quest=.*$LifecycleQuestId.*started=1 .*running=0 .*completed=1 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestCompleted .*quest=.*$LifecycleQuestId.*started=1 .*running=0 .*completed=1 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestRunning .*quest=.*$LifecycleQuestId.*started=1 .*running=0 .*completed=1 .*currentIndex=0",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestCompleted .*quest=.*$CompleteStageQuestId.*started=0 .*running=0 .*completed=0 .*currentIndex=0",
    "FNV/ESM4 proof: quest journal MWScript opcode SetStage .*quest=.*$CompleteStageQuestId.*requestedIndex=$CompleteStageIndex .*currentIndex=$CompleteStageIndex .*entryAdded=1 .*fallbackSetIndex=0",
    "FNV/ESM4 proof: quest journal MWScript opcode GetStage .*quest=.*$CompleteStageQuestId.*currentIndex=$CompleteStageIndex",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestCompleted .*quest=.*$CompleteStageQuestId.*started=1 .*running=0 .*completed=1 .*currentIndex=$CompleteStageIndex",
    "FNV/ESM4 proof: quest status MWScript opcode GetQuestRunning .*quest=.*$CompleteStageQuestId.*started=1 .*running=0 .*completed=1 .*currentIndex=$CompleteStageIndex"
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
    -StartupScript $QuestStatusStartupScript `
    -FnvQuestJournalScriptTrace `
    -RequireLogPattern $RuntimePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest status MWScript opcode CompleteQuest .*quest=.*$LifecycleQuestId" "runtime CompleteQuest proof"
Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest status MWScript opcode GetQuestCompleted .*quest=.*$CompleteStageQuestId.*completed=1" "runtime complete-stage GetQuestCompleted proof" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestJournalScriptTrace: True$" "flat proof required journal/status trace" | Out-Null
Assert-FileContains $FlatSummary "^Screenshots: [1-9][0-9]*$" "flat proof kept sky screenshot sentinel" | Out-Null
$BridgeCounts = Get-BridgeCounts $OpenMwLog
Assert-Equal "runtime QUST objective definition accounting" ([int]$BridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "runtime QUST objective target accounting" ([int]$BridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "runtime QUST stage journal bridge accounting" ([int]$BridgeCounts["questStageInfos"]) $QuestStageTextEntryCount
Assert-Equal "runtime complete-stage bridge accounting" ([int]$BridgeCounts["completeQuestStageInfos"]) $CompleteQuestStageTextEntryCount

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    lifecycleQuestId = $LifecycleQuestId
    lifecycleQuestPlugin = $LifecycleQuestRow.plugin
    lifecycleQuestFormId = $LifecycleQuestRow.formId
    completeStageQuestId = $CompleteStageQuestId
    completeStageQuestPlugin = $CompleteQuestRow.plugin
    completeStageQuestFormId = $CompleteQuestRow.formId
    completeStageIndex = $CompleteStageIndex
    completeStageFlags = $CompleteStageEntry.flags
    completeStageLogLength = $CompleteStageEntry.logLength
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    completeQuestStageTextEntryCount = $CompleteQuestStageTextEntryCount
    bridgeCounts = $BridgeCounts
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "selected FNV quest status opcodes and complete-stage flag"
            classification = "runtime-supported"
            proof = "StartQuest/CompleteQuest/GetQuestCompleted/GetQuestRunning mutate and read VMS39 status, and SetStage VFSAtomicPimp 10 completes the quest through a harvested QSDT complete flag."
        },
        [ordered]@{
            system = "quest scripts, StopQuest/ResetQuest semantics, conditions, rewards, HUD markers, target routing, full quest lifecycle"
            classification = "loaded-pending-runtime"
            proof = "Selected status state works, but full FNV quest lifecycle parity remains separate gates."
        }
    )
    runtimeBoundary = "Selected FNV quest status opcodes and complete-stage flag handling are runtime-supported; quest scripts, StopQuest/ResetQuest semantics, conditions, rewards, HUD markers, target routing, and full lifecycle parity remain loaded-pending-runtime."
    skySentinel = "Runtime proof required sky color sanity, palette match, sun-direction runtime, and sky/sun/moon texture log anchors."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-status-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest status runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
