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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-objective-runtime-contract/$Stamp"
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

Write-ProofLine "FNV quest objective runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: QUST objective definitions and displayed/completed state are runtime-addressable, persisted through save-game QOBJ records, and bound to basic Set/Get Objective Displayed/Completed MWScript state opcodes. This does not claim HUD marker rendering, target marker routing, quest stage execution, exhaustive condition execution, or full FNV script VM parity."
Write-ProofLine ""

Assert-Text "apps/openmw/mwbase/journal.hpp" "setQuestObjectiveDisplayed" "journal objective displayed setter"
Assert-Text "apps/openmw/mwbase/journal.hpp" "getQuestObjectiveCompleted" "journal objective completed getter"
Assert-Text "apps/openmw/mwdialogue/journalimp.hpp" "mQuestObjectiveStates" "objective state backing store"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "setQuestObjectiveDisplayed" "objective displayed runtime state implementation"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "setQuestObjectiveCompleted" "objective completed runtime state implementation"
Assert-Text "components/esm/defs.hpp" 'REC_QOBJ = esm3Recname("QOBJ")' "objective saved-game record id"
Assert-Text "components/esm3/questobjectivestate.hpp" "struct QuestObjectiveState" "objective saved-game record schema"
Assert-Text "components/esm3/questobjectivestate.cpp" 'esm.writeHNT("QDIS", mDisplayed)' "objective displayed save field"
Assert-Text "components/esm3/questobjectivestate.cpp" 'esm.writeHNT("QDON", mCompleted)' "objective completed save field"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "count += mQuestObjectiveStates.size();" "objective state save record count"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "writer.startRecord(ESM::REC_QOBJ)" "objective state save path"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "ESM::QuestObjectiveState record" "objective state load record"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "mQuestObjectiveStates[QuestObjectiveKey(record.mTopic, record.mObjective)] = state" "objective state restore path"
Assert-Text "apps/openmw/mwstate/statemanagerimp.cpp" "case ESM::REC_QOBJ:" "state manager routes objective saved-game record"
Assert-Text "components/compiler/opcodes.hpp" "opcodeSetObjectiveDisplayed = 0x2000327" "objective displayed opcode id"
Assert-Text "components/compiler/opcodes.hpp" "opcodeGetObjectiveCompleted = 0x200032a" "objective completed getter opcode id"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerInstruction("setobjectivedisplayed", "cll", opcodeSetObjectiveDisplayed)' "objective displayed compiler instruction"
Assert-Text "components/compiler/extensions0.cpp" 'extensions.registerFunction("getobjectivecompleted", ''l'', "cl", opcodeGetObjectiveCompleted)' "objective completed compiler function"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "class OpSetObjectiveDisplayed" "objective displayed opcode runtime class"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "setQuestObjectiveDisplayed(" "objective displayed opcode mutates journal"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "getQuestObjectiveCompleted(quest, objective) ? 1 : 0" "objective completed opcode reads journal"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "installSegment5<OpGetObjectiveCompleted>" "objective completed opcode installed"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "OPENMW_FNV_PROOF_QUEST_OBJECTIVE_SCRIPT_TRACE" "objective opcode proof trace switch"
Assert-Text "apps/openmw/mwscript/docs/vmformat.txt" "op 0x2000327: SetObjectiveDisplayed" "objective displayed opcode documented"
Assert-Text "apps/openmw/engine.cpp" "executeStartupScriptWhenRunning" "startup script executes after state is running"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_STARTUP_SCRIPT" "startup script proof fallback"
Assert-Text "apps/openmw/mwgui/console.cpp" "OPENMW_FNV_PROOF_CONSOLE_SCRIPT_TRACE" "console startup script proof trace"
Assert-Text "scripts/nikami/run-fnv-flat.ps1" '"--script-run"' "flat runner can execute startup scripts"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestObjectiveScriptTrace" "flat proof can trace objective script opcodes"
Assert-Text "apps/openmw/mwlua/types/player.cpp" "struct QuestObjective" "Lua quest objective handle"
Assert-Text "apps/openmw/mwlua/types/player.cpp" 'quest["objectives"]' "Lua quest objectives entry point"
Assert-Text "apps/openmw/mwlua/types/player.cpp" "findQuestObjectiveDefinition" "Lua objective definition guard"
Assert-Text "apps/openmw/mwlua/types/player.cpp" "setQuestObjectiveDisplayedAction" "Lua displayed mutation action"
Assert-Text "apps/openmw/mwlua/types/player.cpp" "setQuestObjectiveCompletedAction" "Lua completed mutation action"
Assert-Text "apps/openmw/mwgui/spellwindow.cpp" "Runtime stage journal bridge and objective script state active; HUD target routing pending" "proof UI bounded quest claim"
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
foreach ($row in (Read-JsonArray (Join-Path $ContentLedgerDir "quests.json") "content ledger quests")) {
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
}
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0

$ObjectiveOpcodeScript = Join-Path $ProofDir "quest-objective-opcodes-startup.txt"
@(
    "SetObjectiveDisplayed VCG01 10 1",
    "SetObjectiveCompleted VCG01 10 1",
    "GetObjectiveDisplayed VCG01 10",
    "GetObjectiveCompleted VCG01 10"
) | Set-Content -LiteralPath $ObjectiveOpcodeScript -Encoding ASCII
Write-ProofLine "Objective opcode startup script: $ObjectiveOpcodeScript"

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -StartupScript $ObjectiveOpcodeScript `
    -FnvQuestObjectiveScriptTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: quest objective MWScript opcode SetObjectiveDisplayed .* objective=10 value=1",
        "FNV/ESM4 proof: quest objective MWScript opcode SetObjectiveCompleted .* objective=10 value=1",
        "FNV/ESM4 proof: quest objective MWScript opcode GetObjectiveDisplayed .* objective=10 value=1",
        "FNV/ESM4 proof: quest objective MWScript opcode GetObjectiveCompleted .* objective=10 value=1"
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
Assert-Equal "runtime QUST stage journal bridge remains intact" ([int]$BridgeCounts["questStageInfos"]) $QuestStageTextEntryCount

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    runSeconds = $RuntimeRunSeconds
    bridgeCounts = $BridgeCounts
    runtimeBoundary = "Objective definitions and displayed/completed state are runtime-addressable, saved as QOBJ records, and bound to basic Set/Get Objective Displayed/Completed MWScript state opcodes. HUD markers, target routing, quest stage execution, exhaustive conditions, and full FNV script VM parity remain pending."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-objective-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest objective runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
