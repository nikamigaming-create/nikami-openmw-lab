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

$QuestId = "VMS55"
$StageIndex = 10
$ObjectiveIndex = 5
$ExpectedStageLogLength = 145
$ExpectedFragmentLength = 49
$ExpectedFragmentHash = "187da92fb7d256e778192c4246a21f84a71c4c78643efe563cccec24f3aebb8b"
$ScreenshotFrames = "180,300"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-stage-fragment-runtime-contract/$Stamp"
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

Write-ProofLine "FNV quest stage fragment runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "QuestId: $QuestId"
Write-ProofLine "StageIndex: $StageIndex"
Write-ProofLine "ObjectiveIndex: $ObjectiveIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves one selected real FNV QUST stage fragment executes when its journal stage is added, and that the fragment can drive objective displayed state through MWScript. It does not claim exhaustive condition evaluation, target marker routing, result script coverage for all opcodes, HUD objective rendering, rewards, or full quest completion parity."
Write-ProofLine ""

Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mResultScript = entry.mScript.scriptSource" "FNV QUST stage fragment source bridge"
Assert-Text "apps/openmw/mwbase/dialoguemanager.hpp" "executeScript" "dialogue result script public runtime API"
Assert-Text "apps/openmw/mwdialogue/dialoguemanagerimp.cpp" "void DialogueManager::executeScript" "dialogue MWScript executor"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "findJournalInfo" "journal resolves selected stage info"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "executeScript(info->mResultScript" "journal executes selected stage result script"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "OPENMW_FNV_PROOF_QUEST_STAGE_FRAGMENT_TRACE" "stage fragment proof trace switch"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "selected-stage-fragment-result-script-runtime-supported" "bounded stage fragment runtime classification"
Assert-Text "apps/openmw/mwscript/dialogueextensions.cpp" "OPENMW_FNV_PROOF_QUEST_OBJECTIVE_SCRIPT_TRACE" "objective opcode proof trace switch"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestStageFragmentTrace" "flat proof can trace stage fragments"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"QUST": "quest records are stored, QUST stage journal entries are bridged, selected SetStage/GetStage, StartQuest/CompleteQuest/GetQuestCompleted/GetQuestRunning, and stage fragments execute, selected objective target references can resolve, and objective displayed/completed script state is bound pending full quest lifecycle/HUD-marker/condition parity"' "QUST remains loaded-pending runtime with selected stage, fragment, and target support"

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
$ObjectiveRow = $null
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
foreach ($objective in @($QuestRow.objectives)) {
    if ([int]$objective.objectiveIndex -eq $ObjectiveIndex) {
        $ObjectiveRow = $objective
        break
    }
}
if ($null -eq $StageRow) {
    throw "Missing expected real FNV quest stage in content ledger: ${QuestId}:${StageIndex}"
}
if ($null -eq $ObjectiveRow) {
    throw "Missing expected real FNV objective in content ledger: ${QuestId}:${ObjectiveIndex}"
}
$StageTextEntries = @($StageRow.logEntries | Where-Object { [int]$_.logLength -gt 0 })
$FragmentEntries = @($StageRow.logEntries | Where-Object {
        $null -ne $_.script `
            -and [int]$_.script.sourceLength -eq $ExpectedFragmentLength `
            -and [string]$_.script.sourceHash -eq $ExpectedFragmentHash
    })
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0
Assert-GreaterThan "selected quest stage text entries" $StageTextEntries.Count 0
Assert-GreaterThan "selected fragment entries" $FragmentEntries.Count 0
Assert-Equal "selected stage log length" ([int]$StageTextEntries[0].logLength) $ExpectedStageLogLength
Assert-Equal "selected fragment source length" ([int]$FragmentEntries[0].script.sourceLength) $ExpectedFragmentLength
Assert-Equal "selected fragment source hash" ([string]$FragmentEntries[0].script.sourceHash) $ExpectedFragmentHash
Write-ProofLine "Selected quest source: plugin=$($QuestRow.plugin) formId=$($QuestRow.formId) stage=$StageIndex objective=$ObjectiveIndex"

$FragmentStartupScript = Join-Path $ProofDir "quest-stage-fragment-opcodes-startup.txt"
@(
    "Journal $QuestId $StageIndex",
    "GetJournalIndex $QuestId",
    "GetObjectiveDisplayed $QuestId $ObjectiveIndex"
) | Set-Content -LiteralPath $FragmentStartupScript -Encoding ASCII
Write-ProofLine "Fragment opcode startup script: $FragmentStartupScript"

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
    "FNV/ESM4 proof: quest journal MWScript opcode Journal .*quest=.*$QuestId.*requestedIndex=$StageIndex .*currentIndex=$StageIndex .*entryAdded=1 .*fallbackSetIndex=0",
    "FNV/ESM4 proof: quest journal MWScript opcode GetJournalIndex .*quest=.*$QuestId.*currentIndex=$StageIndex",
    "FNV/ESM4 proof: quest stage fragment runtime executed .*quest=.*$QuestId.*stageIndex=$StageIndex .*scriptBytes=$ExpectedFragmentLength .*runtimeBoundary=selected-stage-fragment-result-script-runtime-supported .*setStageRuntime=runtime-supported .*conditionRuntime=loaded-pending-runtime .*fullQuestCompletionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest objective MWScript opcode SetObjectiveDisplayed .*quest=.*$QuestId.*objective=$ObjectiveIndex value=1",
    "FNV/ESM4 proof: quest objective MWScript opcode GetObjectiveDisplayed .*quest=.*$QuestId.*objective=$ObjectiveIndex value=1"
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
    -StartupScript $FragmentStartupScript `
    -FnvQuestJournalScriptTrace `
    -FnvQuestObjectiveScriptTrace `
    -FnvQuestStageFragmentTrace `
    -RequireLogPattern $RuntimePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest stage fragment runtime executed .*quest=.*$QuestId" "runtime stage fragment proof"
Assert-FileContains $FlatSummary "^FnvQuestStageFragmentTrace: True$" "flat proof required stage fragment trace" | Out-Null
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
    objectiveIndex = $ObjectiveIndex
    expectedStageLogLength = $ExpectedStageLogLength
    expectedFragmentLength = $ExpectedFragmentLength
    expectedFragmentHash = $ExpectedFragmentHash
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    bridgeCounts = $BridgeCounts
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "selected FNV quest stage fragment execution"
            classification = "runtime-supported"
            proof = "Journal VMS55 10 executes the selected harvested stage fragment and drives objective 5 displayed through MWScript."
        },
        [ordered]@{
            system = "conditions, target markers, full result script opcode coverage, rewards, full quest completion"
            classification = "loaded-pending-runtime"
            proof = "Selected fragment execution works, but broad FNV quest VM parity remains separate gates."
        }
    )
    runtimeBoundary = "Selected FNV QUST stage fragment execution is runtime-supported; conditions, target markers, full opcode coverage, rewards, and full quest completion remain loaded-pending-runtime."
    skySentinel = "Runtime proof required sky color sanity, palette match, sun-direction runtime, and sky/sun/moon texture log anchors."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-stage-fragment-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest stage fragment runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
