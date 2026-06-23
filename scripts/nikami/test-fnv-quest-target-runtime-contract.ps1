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
$ObjectiveIndex = 10
$TargetIndex = 0
$ExpectedTargetFormId = "0x000e61a2"
$ExpectedRuntimeTargetFormIdPattern = "FormId:0x10e61a2"
$ScreenshotFrames = "180,300"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-target-runtime-contract/$Stamp"
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

Write-ProofLine "FNV quest target runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "QuestId: $QuestId"
Write-ProofLine "ObjectiveIndex: $ObjectiveIndex"
Write-ProofLine "TargetIndex: $TargetIndex"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves one selected real FNV QSTA objective target resolves at runtime to a loaded placed reference with parent cell, base record, and finite position. It does not claim HUD marker rendering, target condition execution, pathing, compass/map routing, or full quest completion parity."
Write-ProofLine ""

Assert-Text "components/esm4/loadqust.hpp" "QuestObjectiveTarget" "QUST objective target store"
Assert-Text "components/esm4/loadqust.cpp" 'case ESM::fourCC("QSTA")' "QSTA target subrecord loader"
Assert-Text "apps/openmw/engine.cpp" "resolveFalloutQuestTarget" "runtime QSTA target resolver"
Assert-Text "apps/openmw/engine.cpp" "runFalloutQuestTargetProof" "runtime QSTA target proof hook"
Assert-Text "apps/openmw/engine.cpp" "selected-quest-objective-target-resolution-runtime-supported" "bounded target runtime classification"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestTargetTrace" "flat proof can trace quest targets"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"QUST": "quest records are stored, QUST stage journal entries are bridged, selected stage fragments can execute, selected objective target references can resolve, and objective displayed/completed script state is bound pending full quest execution/HUD-marker/condition parity"' "QUST remains loaded-pending runtime with selected target support"

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
foreach ($objective in @($QuestRow.objectives)) {
    if ([int]$objective.objectiveIndex -eq $ObjectiveIndex) {
        $ObjectiveRow = $objective
        break
    }
}
if ($null -eq $ObjectiveRow) {
    throw "Missing expected real FNV objective in content ledger: ${QuestId}:${ObjectiveIndex}"
}
$ObjectiveTargets = @($ObjectiveRow.targets)
if ($TargetIndex -ge $ObjectiveTargets.Count) {
    throw "Selected target index is out of range: targetIndex=$TargetIndex targetCount=$($ObjectiveTargets.Count)"
}
$SelectedTarget = $ObjectiveTargets[$TargetIndex]
Assert-GreaterThan "ledger quest objective definitions" $QuestObjectiveCount 0
Assert-GreaterThan "ledger quest objective targets" $QuestObjectiveTargetCount 0
Assert-GreaterThan "ledger quest stage text entries" $QuestStageTextEntryCount 0
Assert-GreaterThan "selected objective targets" $ObjectiveTargets.Count 0
Assert-Equal "selected target form id" ([string]$SelectedTarget.targetFormId) $ExpectedTargetFormId
Assert-Equal "selected target condition count" ([int]$SelectedTarget.conditionCount) 0
Write-ProofLine "Selected objective source: plugin=$($QuestRow.plugin) formId=$($QuestRow.formId) objective=$ObjectiveIndex target=$ExpectedTargetFormId targets=$($ObjectiveTargets.Count)"

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
    "FNV/ESM4 proof: quest target runtime PASS .*quest=$QuestId .*questFound=1 .*objective=$ObjectiveIndex .*objectiveFound=1 .*targetIndex=$TargetIndex .*targetCount=$($ObjectiveTargets.Count) .*targetFormId=$ExpectedRuntimeTargetFormIdPattern .*targetFlags=0 .*targetConditions=0 .*targetResolved=1 .*targetRecordType=(REFR4|ACHR4|ACRE4) .*cellFound=1 .*baseFormId=FormId:0x[0-9a-fA-F]+ .*baseRecordType=[A-Z0-9_]+ .*baseRecordTypeHex=0x[1-9a-fA-F][0-9a-fA-F]* .*positionFinite=1 .*runtimeBoundary=selected-quest-objective-target-resolution-runtime-supported .*hudMarkerRuntime=loaded-pending-runtime .*conditionRuntime=loaded-pending-runtime .*pathingRuntime=loaded-pending-runtime"
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
    -FnvQuestTargetTrace `
    -FnvQuestTargetQuest $QuestId `
    -FnvQuestTargetObjective $ObjectiveIndex `
    -FnvQuestTargetIndex $TargetIndex `
    -FnvQuestTargetFrame 150 `
    -RequireLogPattern $RuntimePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest target runtime PASS .*quest=$QuestId" "runtime quest target proof"
Assert-FileContains $FlatSummary "^FnvQuestTargetTrace: True$" "flat proof required quest target trace" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestTargetQuest: $QuestId$" "flat proof selected target quest" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestTargetObjective: $ObjectiveIndex$" "flat proof selected target objective" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestTargetIndex: $TargetIndex$" "flat proof selected target index" | Out-Null
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
    objectiveIndex = $ObjectiveIndex
    targetIndex = $TargetIndex
    expectedLedgerTargetFormId = $ExpectedTargetFormId
    expectedRuntimeTargetFormId = $ExpectedRuntimeTargetFormIdPattern
    selectedObjectiveTargets = $ObjectiveTargets.Count
    questObjectiveCount = $QuestObjectiveCount
    questObjectiveTargetCount = $QuestObjectiveTargetCount
    questStageTextEntryCount = $QuestStageTextEntryCount
    bridgeCounts = $BridgeCounts
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "selected FNV quest objective target reference resolution"
            classification = "runtime-supported"
            proof = "VMS57 objective 10 QSTA target 0 resolves to a loaded placed reference with parent cell, base record, and finite position."
        },
        [ordered]@{
            system = "HUD markers, target conditions, pathing, compass/map routing, full quest completion"
            classification = "loaded-pending-runtime"
            proof = "Target reference resolution works, but display/routing and condition semantics remain separate gates."
        }
    )
    runtimeBoundary = "Selected FNV QUST objective target reference resolution is runtime-supported; HUD markers, target conditions, pathing, compass/map routing, and full quest completion remain loaded-pending-runtime."
    skySentinel = "Runtime proof required sky color sanity, palette match, sun-direction runtime, and sky/sun/moon texture log anchors."
}
$metadataPath = Join-Path $ProofDir "fnv-quest-target-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV quest target runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
