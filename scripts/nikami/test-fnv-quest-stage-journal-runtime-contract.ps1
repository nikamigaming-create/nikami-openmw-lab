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

$QuestId = "VMS57"
$StageIndex = 10
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-stage-journal-runtime-contract/$Stamp"
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

Write-ProofLine "FNV quest stage journal runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "QuestId: $QuestId"
Write-ProofLine "StageIndex: $StageIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves one real FNV QUST stage with harvested journal text can be resolved through the bridged runtime dialogue store, added by Journal, and read back by GetJournalIndex on PC-flat. It does not claim result script execution, condition evaluation, target marker routing, HUD objective rendering, save/load persistence for this stage, or full quest completion parity."
Write-ProofLine ""

Assert-Text "components/esm4/loadqust.cpp" 'case ESM::fourCC("INDX")' "QUST stage index loader"
Assert-Text "components/esm4/loadqust.cpp" 'case ESM::fourCC("QSDT")' "QUST stage entry loader"
Assert-Text "components/esm4/loadqust.cpp" 'reader.getZString(currentEntry->mLogEntry)' "QUST stage journal text loader"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questStageInfoId" "FNV quest stage journal info id bridge"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "entry.mLogEntry.empty()" "FNV quest bridge skips empty journal entries"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "makeJournalInfo(" "FNV quest bridge emits OpenMW journal info"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mResultScript = entry.mScript.scriptSource" "FNV quest bridge preserves stage result script source"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questStageInfos=" "runtime bridge QUST stage count log"
Assert-Text "apps/openmw/mwbase/journal.hpp" "virtual void addEntry" "journal add-entry API"
Assert-Text "apps/openmw/mwbase/journal.hpp" "virtual void setJournalIndex" "journal stage setter API"
Assert-Text "apps/openmw/mwbase/journal.hpp" "virtual int getJournalIndex" "journal stage getter API"
Assert-Text "apps/openmw/mwdialogue/journalentry.cpp" "JournalEntry::idFromIndex" "journal index resolves dialogue info"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "StampedJournalEntry::makeFromQuest" "journal opcode creates stamped quest entry"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "quest.addEntry(entry)" "journal add-entry mutates quest state"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "quest.setIndex(index)" "journal setter mutates quest index"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "return iter->second.getIndex()" "journal getter reads quest index"
Assert-Text "components/compiler/opcodes.hpp" "opcodeJournal = 0x2000133" "journal opcode id"
Assert-Text "components/compiler/opcodes.hpp" "opcodeSetJournalIndex = 0x2000134" "setjournalindex opcode id"
Assert-Text "components/compiler/opcodes.hpp" "opcodeGetJournalIndex = 0x2000135" "getjournalindex opcode id"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerInstruction("journal", "cl", opcodeJournal, opcodeJournalExplicit)' "journal compiler instruction"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerFunction("getjournalindex", ''l'', "c", opcodeGetJournalIndex)' "getjournalindex compiler function"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "OPENMW_FNV_PROOF_QUEST_JOURNAL_SCRIPT_TRACE" "journal opcode proof trace switch"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "entryAdded=" "journal trace distinguishes real entry add"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "fallbackSetIndex=" "journal trace distinguishes fallback path"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "installSegment5<OpGetJournalIndex>" "journal getter opcode installed"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_STARTUP_SCRIPT" "startup script proof fallback"
Assert-Text "apps/openmw/mwgui/console.cpp" "OPENMW_FNV_PROOF_CONSOLE_SCRIPT_TRACE" "console startup script proof trace"
Assert-Text "scripts/nikami/run-fnv-flat.ps1" '"--script-run"' "flat runner can execute startup scripts"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestJournalScriptTrace" "flat proof can trace journal opcodes"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"QUST": "quest records are stored, QUST stage journal entries are bridged, selected stage fragments can execute, and objective displayed/completed script state is bound pending full quest execution/objective-target/condition parity"' "QUST remains loaded-pending runtime"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
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
Assert-GreaterThan "selected quest stage text entries" $StageTextEntries.Count 0
Write-ProofLine "Selected quest source: plugin=$($QuestRow.plugin) formId=$($QuestRow.formId) stage=$StageIndex textEntries=$($StageTextEntries.Count)"

$JournalOpcodeScript = Join-Path $ProofDir "quest-stage-journal-opcodes-startup.txt"
@(
    "Journal $QuestId $StageIndex",
    "GetJournalIndex $QuestId",
    "SetJournalIndex $QuestId $StageIndex",
    "GetJournalIndex $QuestId"
) | Set-Content -LiteralPath $JournalOpcodeScript -Encoding ASCII
Write-ProofLine "Journal opcode startup script: $JournalOpcodeScript"

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -StartupScript $JournalOpcodeScript `
    -FnvQuestJournalScriptTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: quest journal MWScript opcode Journal .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex .*entryAdded=1 .*fallbackSetIndex=0",
        "FNV/ESM4 proof: quest journal MWScript opcode GetJournalIndex .*quest=.*$QuestId.*currentIndex=$StageIndex",
        "FNV/ESM4 proof: quest journal MWScript opcode SetJournalIndex .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex"
    ) `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
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
    selectedStageTextEntries = $StageTextEntries.Count
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    runSeconds = $RuntimeRunSeconds
    bridgeCounts = $BridgeCounts
    classification = "loaded-pending-runtime"
    provedRuntimeSlice = "Journal/AddEntry/GetJournalIndex/SetJournalIndex for one harvested FNV QUST text stage on PC-flat"
    runtimeBoundary = "Selected FNV QUST journal stage is runtime-addressable and read back through MWScript; result scripts, conditions, target routing, HUD objectives, persistence, and full quest completion parity remain pending."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-stage-journal-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest stage journal runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
