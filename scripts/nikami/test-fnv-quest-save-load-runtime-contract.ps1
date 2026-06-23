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

$JournalQuestId = "VMS57"
$StageIndex = 10
$ObjectiveQuestId = "VCG01"
$ObjectiveIndex = 10
$HookFrame = 420
$ScreenshotFrames = "180,300"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-save-load-runtime-contract/$Stamp"
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

function Get-LatestSavegame([string]$Root) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing save root: $Root"
    }
    $save = Get-ChildItem -LiteralPath $Root -Recurse -File -Filter "*.omwsave" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $save) {
        throw "No generated savegame found under $Root"
    }
    if ($save.Length -le 0) {
        throw "Generated savegame is empty: $($save.FullName)"
    }
    $proofRootResolved = (Resolve-Path -LiteralPath $ProofDir).Path
    $saveResolved = (Resolve-Path -LiteralPath $save.FullName).Path
    if (!$saveResolved.StartsWith($proofRootResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Generated savegame escaped proof directory: $saveResolved"
    }
    return $save
}

Write-ProofLine "FNV quest save/load runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "JournalQuestId: $JournalQuestId"
Write-ProofLine "StageIndex: $StageIndex"
Write-ProofLine "ObjectiveQuestId: $ObjectiveQuestId"
Write-ProofLine "ObjectiveIndex: $ObjectiveIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves one real FNV QUST journal stage and one real FNV objective displayed/completed state survive the actual OpenMW save/load path on PC-flat. It does not claim result script execution, condition evaluation, target marker routing, HUD objective rendering, or full quest completion parity."
Write-ProofLine ""

Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_QUEST_SAVELOAD" "quest save/load proof environment hook"
Assert-Text "apps/openmw/engine.cpp" "selected-fnv-quest-stage-and-objective-save-load-runtime-supported" "bounded quest save/load runtime classification"
Assert-Text "apps/openmw/engine.cpp" "stateManager.saveGame" "quest save/load proof uses normal state manager save path"
Assert-Text "apps/openmw/engine.cpp" "stateManager.requestQuit" "quest save/load proof exits without killing process"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "writer.startRecord(ESM::REC_QUES)" "quest index save path"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "writer.startRecord(ESM::REC_JOUR)" "journal entry save path"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "writer.startRecord(ESM::REC_QOBJ)" "objective state save path"
Assert-Text "apps/openmw/mwdialogue/journalimp.cpp" "type == ESM::REC_QOBJ" "objective state load path"
Assert-Text "apps/openmw/mwstate/statemanagerimp.cpp" "case ESM::REC_QUES:" "state manager routes quest saved-game record"
Assert-Text "apps/openmw/mwstate/statemanagerimp.cpp" "case ESM::REC_QOBJ:" "state manager routes objective saved-game record"
Assert-Text "apps/openmw/options.cpp" '"load-savegame"' "OpenMW CLI load-savegame option"
Assert-Text "scripts/nikami/run-fnv-flat.ps1" '"--load-savegame"' "flat runner can load exact savegame"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestSaveLoadMode" "flat proof can enable quest save/load proof"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestSaveLoadFrame" "flat proof can delay quest save/load proof until after screenshot sentinel"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$QuestObjectiveCount = 0
$QuestObjectiveTargetCount = 0
$QuestStageTextEntryCount = 0
$JournalQuestRow = $null
$StageRow = $null
$ObjectiveQuestRow = $null
$ObjectiveRow = $null
foreach ($row in (Read-JsonArray (Join-Path $ContentLedgerDir "quests.json") "content ledger quests")) {
    if ([string]::IsNullOrWhiteSpace([string]$row.editorId)) {
        continue
    }
    $QuestObjectiveCount += [int]$row.objectiveCount
    $QuestObjectiveTargetCount += [int]$row.objectiveTargetCount
    $QuestStageTextEntryCount += [int]$row.stageTextEntryCount
    if ([string]$row.editorId -eq $JournalQuestId) {
        $JournalQuestRow = $row
    }
    if ([string]$row.editorId -eq $ObjectiveQuestId) {
        $ObjectiveQuestRow = $row
    }
}
if ($null -eq $JournalQuestRow) {
    throw "Missing expected real FNV journal quest in content ledger: $JournalQuestId"
}
if ($null -eq $ObjectiveQuestRow) {
    throw "Missing expected real FNV objective quest in content ledger: $ObjectiveQuestId"
}
foreach ($stage in @($JournalQuestRow.stages)) {
    if ([int]$stage.stageIndex -eq $StageIndex) {
        $StageRow = $stage
        break
    }
}
foreach ($objective in @($ObjectiveQuestRow.objectives)) {
    if ([int]$objective.objectiveIndex -eq $ObjectiveIndex) {
        $ObjectiveRow = $objective
        break
    }
}
if ($null -eq $StageRow) {
    throw "Missing expected real FNV quest stage in content ledger: ${JournalQuestId}:${StageIndex}"
}
if ($null -eq $ObjectiveRow) {
    throw "Missing expected real FNV objective in content ledger: ${ObjectiveQuestId}:${ObjectiveIndex}"
}
$StageTextEntries = @($StageRow.logEntries | Where-Object { [int]$_.logLength -gt 0 })
$ObjectiveTargets = @($ObjectiveRow.targets)
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0
Assert-GreaterThan "selected quest stage text entries" $StageTextEntries.Count 0
Assert-GreaterThan "selected objective targets accounted" $ObjectiveTargets.Count 0
Write-ProofLine "Selected journal quest source: plugin=$($JournalQuestRow.plugin) formId=$($JournalQuestRow.formId) stage=$StageIndex textEntries=$($StageTextEntries.Count)"
Write-ProofLine "Selected objective source: plugin=$($ObjectiveQuestRow.plugin) formId=$($ObjectiveQuestRow.formId) objective=$ObjectiveIndex targets=$($ObjectiveTargets.Count)"

$CommonSkyPatterns = @(
    "FNV/ESM4: sky shader mode forceShaders=0 falloutSkyModels=1 program=sky-interpreted",
    "FNV/ESM4: atmosphere vertical colors runtime-supported skyUpper=.*skyLower=.*horizon=.*",
    "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds",
    "FNV/ESM4: enabled FNV Masser moon billboard using texture textures/sky/masser_full\.dds",
    "FNV/ESM4: enabled FNV Secunda moon billboard using texture textures/sky/skymoonfull\.dds",
    "FNV/ESM4 proof: render sun direction .* normalizedSky=.*"
)
$SavePatterns = @(
    "FNV/ESM4 proof: quest save/load runtime PASS .*phase=save .*journalQuest=.*$JournalQuestId.*stageIndex=$StageIndex .*currentIndex=$StageIndex .*entryAdded=1 .*fallbackSetIndex=0 .*objectiveQuest=.*$ObjectiveQuestId.*objective=$ObjectiveIndex .*displayed=1 .*completed=1 .*saveRecords=QUES,JOUR,QOBJ .*runtimeBoundary=selected-fnv-quest-stage-and-objective-save-load-runtime-supported .*resultScriptRuntime=loaded-pending-runtime .*conditionRuntime=loaded-pending-runtime .*targetMarkerRuntime=loaded-pending-runtime .*fullQuestCompletionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest save/load runtime saved description=fnv-quest-saveload-proof"
) + $CommonSkyPatterns
$LoadPatterns = @(
    "FNV/ESM4 proof: quest save/load runtime PASS .*phase=load .*journalQuest=.*$JournalQuestId.*stageIndex=$StageIndex .*currentIndex=$StageIndex .*entryAdded=-1 .*fallbackSetIndex=-1 .*objectiveQuest=.*$ObjectiveQuestId.*objective=$ObjectiveIndex .*displayed=1 .*completed=1 .*saveRecords=QUES,JOUR,QOBJ .*runtimeBoundary=selected-fnv-quest-stage-and-objective-save-load-runtime-supported .*resultScriptRuntime=loaded-pending-runtime .*conditionRuntime=loaded-pending-runtime .*targetMarkerRuntime=loaded-pending-runtime .*fullQuestCompletionRuntime=loaded-pending-runtime"
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
    -FnvQuestSaveLoadMode "save" `
    -FnvQuestSaveLoadFrame $HookFrame `
    -RequireLogPattern $SavePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV save-phase flat proof failed with exit code $LASTEXITCODE."
}

$SaveFlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV save-phase flat proof"
$SaveOpenMwLog = Join-Path $SaveFlatProofDir "openmw.log"
$SaveSummary = Join-Path $SaveFlatProofDir "summary.txt"
Assert-FileContains $SaveSummary "^FnvQuestSaveLoadMode: save$" "save phase enabled quest save/load hook" | Out-Null
Assert-FileContains $SaveSummary "^Screenshots: [1-9][0-9]*$" "save phase kept sky screenshot sentinel" | Out-Null
$SaveBridgeCounts = Get-BridgeCounts $SaveOpenMwLog
Assert-Equal "save-phase runtime QUST objective definition accounting" ([int]$SaveBridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "save-phase runtime QUST objective target accounting" ([int]$SaveBridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "save-phase runtime QUST stage journal bridge accounting" ([int]$SaveBridgeCounts["questStageInfos"]) $QuestStageTextEntryCount

$SaveRoot = Join-Path $ProofDir "runtime/fnv-flat-clean/saves"
$GeneratedSave = Get-LatestSavegame $SaveRoot
Write-ProofLine "Generated savegame: $($GeneratedSave.FullName)"
Write-ProofLine "Generated savegame bytes: $($GeneratedSave.Length)"

& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofDir `
    -RunSeconds $RuntimeRunSeconds `
    -ScreenshotFrames $ScreenshotFrames `
    -RequireSkyColorSanity `
    -RequireSkyPaletteMatch `
    -RequireSunDirectionRuntime `
    -LoadSavegame $GeneratedSave.FullName `
    -FnvQuestSaveLoadMode "load" `
    -FnvQuestSaveLoadFrame $HookFrame `
    -RequireLogPattern $LoadPatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV load-phase flat proof failed with exit code $LASTEXITCODE."
}

$LoadFlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV load-phase flat proof"
$LoadOpenMwLog = Join-Path $LoadFlatProofDir "openmw.log"
$LoadSummary = Join-Path $LoadFlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $LoadOpenMwLog "FNV/ESM4 proof: quest save/load runtime PASS .*phase=load" "load phase restored quest state"
Assert-FileContains $LoadSummary "^FnvQuestSaveLoadMode: load$" "load phase enabled quest save/load hook" | Out-Null
Assert-FileContains $LoadSummary "^LoadSavegame: .+\.omwsave$" "load phase used generated savegame" | Out-Null
Assert-FileContains $LoadSummary "^Screenshots: [1-9][0-9]*$" "load phase kept sky screenshot sentinel" | Out-Null
$LoadBridgeCounts = Get-BridgeCounts $LoadOpenMwLog
Assert-Equal "load-phase runtime QUST objective definition accounting" ([int]$LoadBridgeCounts["questObjectives"]) $QuestObjectiveCount
Assert-Equal "load-phase runtime QUST objective target accounting" ([int]$LoadBridgeCounts["questObjectiveTargets"]) $QuestObjectiveTargetCount
Assert-Equal "load-phase runtime QUST stage journal bridge accounting" ([int]$LoadBridgeCounts["questStageInfos"]) $QuestStageTextEntryCount

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    saveFlatProofDir = $SaveFlatProofDir
    loadFlatProofDir = $LoadFlatProofDir
    generatedSavegame = $GeneratedSave.FullName
    generatedSavegameBytes = $GeneratedSave.Length
    journalQuestId = $JournalQuestId
    journalQuestPlugin = $JournalQuestRow.plugin
    journalQuestFormId = $JournalQuestRow.formId
    stageIndex = $StageIndex
    selectedStageTextEntries = $StageTextEntries.Count
    objectiveQuestId = $ObjectiveQuestId
    objectiveQuestPlugin = $ObjectiveQuestRow.plugin
    objectiveQuestFormId = $ObjectiveQuestRow.formId
    objectiveIndex = $ObjectiveIndex
    selectedObjectiveTargets = $ObjectiveTargets.Count
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    saveBridgeCounts = $SaveBridgeCounts
    loadBridgeCounts = $LoadBridgeCounts
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "selected FNV quest stage and objective save/load"
            classification = "runtime-supported"
            proof = "Selected real FNV journal stage and objective state survive the actual OpenMW save/load path."
        },
        [ordered]@{
            system = "result scripts, conditions, target markers, HUD objective routing, full quest completion"
            classification = "loaded-pending-runtime"
            proof = "The save/load state path is proven, but retail quest execution semantics remain separate gates."
        }
    )
    runtimeBoundary = "Selected FNV quest stage and objective save/load is runtime-supported; result scripts, conditions, target markers, HUD objective routing, and full quest completion remain loaded-pending-runtime."
    skySentinel = "Both save and load phases required sky color sanity, palette match, sun-direction runtime, and sky/sun/moon texture log anchors."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-save-load-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Save flat proof: $SaveFlatProofDir"
Write-ProofLine "Load flat proof: $LoadFlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest save/load runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
