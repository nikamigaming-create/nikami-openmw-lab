param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$ProofRoot = "",
    [switch]$SkipStrictFlatCounts
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

$AllowedClassifications = @(
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof"
)

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-condition-function-accounting/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Read-Json([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Label}: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Read-JsonArray([string]$Path, [string]$Label) {
    $value = Read-Json $Path $Label
    if ($null -eq $value) {
        return @()
    }
    if ($value -is [System.Array]) {
        return @($value)
    }
    return @($value)
}

function Get-LatestProofDir([string]$Root, [string]$Label) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Label proof root: $Root"
    }
    $dir = Get-ChildItem -LiteralPath $Root -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -eq $dir) {
        throw "Missing $Label proof directory under $Root"
    }
    return $dir.FullName
}

function Get-PropertyValue([object]$Object, [string]$Name, [object]$Default = $null) {
    if ($null -eq $Object) {
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }
    return $property.Value
}

function Assert-Equal([string]$Label, [object]$Actual, [object]$Expected) {
    if ($Actual -ne $Expected) {
        throw "${Label}: actual=$Actual expected=$Expected"
    }
    Write-ProofLine "OK ${Label}: $Actual"
}

function Assert-GreaterThan([string]$Label, [int]$Actual, [int]$Floor) {
    if ($Actual -le $Floor) {
        throw "${Label}: actual=$Actual must be greater than $Floor"
    }
    Write-ProofLine "OK ${Label}: $Actual"
}

function Assert-Text([string]$RelativePath, [string]$Pattern, [string]$Label) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source anchor for ${Label}: $path"
    }
    if (-not (Select-String -LiteralPath $path -Pattern $Pattern -SimpleMatch -Quiet)) {
        throw "Missing source anchor for ${Label}: $RelativePath :: $Pattern"
    }
    Write-ProofLine "OK source anchor: $Label"
}

function Assert-AllowedClassification([string]$Label, [string]$Classification) {
    if ($AllowedClassifications -notcontains $Classification) {
        throw "${Label}: invalid classification '$Classification'"
    }
}

function Assert-NoForbiddenPayloadKeys([object]$Row, [string]$Label) {
    $forbidden = @("source", "questName", "topicName", "response", "notes", "edits", "text", "fullName", "value")
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($Row)
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if ($null -eq $current -or $current -is [string]) {
            continue
        }
        if ($current -is [System.Array]) {
            foreach ($item in @($current)) {
                $queue.Enqueue($item)
            }
            continue
        }
        if ($current.GetType().FullName -ne "System.Management.Automation.PSCustomObject") {
            continue
        }
        foreach ($property in @($current.PSObject.Properties)) {
            if ($forbidden -contains $property.Name) {
                throw "${Label}: forbidden retail payload-looking key '$($property.Name)' in condition function artifact"
            }
            $queue.Enqueue($property.Value)
        }
    }
}

Write-ProofLine "FNV condition-function accounting contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "Runtime boundary: CTDA/CTDT rows and function IDs are fully accounted. Function opcode semantics are noted where already proven, but full condition evaluation remains loaded-pending-runtime."
Write-ProofLine ""

Assert-Text "scripts/nikami/fnv_content_ledger.py" '"conditions": proof_dir / "conditions.json"' "content ledger emits conditions artifact"
Assert-Text "scripts/nikami/fnv_content_ledger.py" '"conditionFunctions": proof_dir / "condition-functions.json"' "content ledger emits condition-functions artifact"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "condition-function-semantics-pending" "pending condition semantics boundary"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"condition-functions.json": "conditionFunctionCount"' "no-silent-skip condition-function artifact count"
Assert-Text "components/esm4/script.hpp" "FUN_GetStageDone = 59" "condition function name source"
Assert-Text "components/esm4/loadpack.hpp" "std::uint32_t unknown4" "PACK CTDA schema caveat"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}
$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
$ResultPath = Join-Path $ContentLedgerDir "result.json"
$ConditionsPath = Join-Path $ContentLedgerDir "conditions.json"
$ConditionFunctionsPath = Join-Path $ContentLedgerDir "condition-functions.json"
$QuestPath = Join-Path $ContentLedgerDir "quests.json"

$Result = Read-Json $ResultPath "content ledger result"
$ConditionRows = @(Read-JsonArray $ConditionsPath "condition rows")
$FunctionRows = @(Read-JsonArray $ConditionFunctionsPath "condition functions")
$QuestRows = @(Read-JsonArray $QuestPath "quest rows")

Assert-Equal "content ledger status" ([string]$Result.status) "PASS"
Assert-Equal "conditions artifact path" ([string]$Result.artifacts.conditions) $ConditionsPath
Assert-Equal "condition-functions artifact path" ([string]$Result.artifacts.conditionFunctions) $ConditionFunctionsPath
Assert-Equal "condition row JSON count" $ConditionRows.Count ([int]$Result.conditionRowCount)
Assert-Equal "condition function JSON count" $FunctionRows.Count ([int]$Result.conditionFunctionCount)

$functionUseSum = 0
foreach ($row in $FunctionRows) {
    $functionUseSum += [int]$row.conditionCount
    Assert-AllowedClassification "condition function $($row.function)" ([string]$row.classification)
    Assert-AllowedClassification "condition function semantics $($row.function)" ([string]$row.functionSemanticsClassification)
    Assert-NoForbiddenPayloadKeys $row "condition function $($row.function)"
}
Assert-Equal "condition function use count" $functionUseSum ([int]$Result.conditionFunctionUseCount)
Assert-Equal "condition rows accounted by functions" $functionUseSum $ConditionRows.Count
Assert-Equal "condition unknown function count" ([int]$Result.conditionUnknownFunctionCount) 0
Assert-Equal "condition high-word function count" ([int]$Result.conditionHighWordNonzeroCount) 0
Assert-Equal "condition malformed count" ([int]$Result.conditionMalformedCount) 0
Assert-Equal "condition unaccounted count" ([int]$Result.conditionUnaccountedCount) 0
Assert-Equal "condition function unclassified count" ([int]$Result.conditionFunctionUnclassified) 0

if (!$SkipStrictFlatCounts) {
    Assert-Equal "PC-flat CTDA row count" ([int]$Result.conditionRowCount) 80627
    Assert-Equal "PC-flat condition function count" ([int]$Result.conditionFunctionCount) 148
    Assert-Equal "PC-flat CTDA subrecord count" ([int](Get-PropertyValue $Result.conditionSubrecordCounts "CTDA" 0)) 80627
    Assert-Equal "PC-flat CTDT subrecord count" ([int](Get-PropertyValue $Result.conditionSubrecordCounts "CTDT" 0)) 0
    Assert-Equal "PC-flat INFO condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "INFO" 0)) 69332
    Assert-Equal "PC-flat QUST condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "QUST" 0)) 2084
    Assert-Equal "PC-flat PACK condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "PACK" 0)) 3801
    Assert-Equal "PC-flat IDLE condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "IDLE" 0)) 1907
    Assert-Equal "PC-flat TERM condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "TERM" 0)) 802
    Assert-Equal "PC-flat CPTH condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "CPTH" 0)) 593
    Assert-Equal "PC-flat ALCH condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "ALCH" 0)) 487
    Assert-Equal "PC-flat MESG condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "MESG" 0)) 419
    Assert-Equal "PC-flat ENCH condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "ENCH" 0)) 323
    Assert-Equal "PC-flat SPEL condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "SPEL" 0)) 252
    Assert-Equal "PC-flat RCPE condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "RCPE" 0)) 156
    Assert-Equal "PC-flat PERK condition rows" ([int](Get-PropertyValue $Result.conditionOwnerRecordCounts "PERK" 0)) 471
    Assert-Equal "PC-flat QUST top-level condition rows" ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-top-level" 0)) 322
    Assert-Equal "PC-flat QUST stage-entry condition rows" ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-stage-entry" 0)) 26
    Assert-Equal "PC-flat QUST objective-target condition rows" ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-objective-target" 0)) 1736
    Assert-Equal "PC-flat record-scoped condition rows" ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "record" 0)) 4939
    Assert-Equal "PC-flat TargetCondition schema rows" ([int](Get-PropertyValue $Result.conditionSchemaCounts "TargetCondition" 0)) 76826
    Assert-Equal "PC-flat PACK CTDA schema rows" ([int](Get-PropertyValue $Result.conditionSchemaCounts "PACK-CTDA" 0)) 3801
}

$supportedSemanticFunctions = @{
    56 = "test-fnv-quest-status-runtime-contract.ps1"
    58 = "test-fnv-quest-setstage-runtime-contract.ps1"
    59 = "test-fnv-quest-stage-done-runtime-contract.ps1"
    420 = "test-fnv-quest-objective-runtime-contract.ps1"
    421 = "test-fnv-quest-objective-runtime-contract.ps1"
    546 = "test-fnv-quest-status-runtime-contract.ps1"
}
foreach ($id in ($supportedSemanticFunctions.Keys | Sort-Object)) {
    $row = $FunctionRows | Where-Object { [int]$_.function -eq [int]$id } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing observed condition function row: $id"
    }
    Assert-Equal "condition function $id classification" ([string]$row.classification) "loaded-pending-runtime"
    Assert-Equal "condition function $id semantic classification" ([string]$row.functionSemanticsClassification) "runtime-supported"
    Assert-Equal "condition function $id runtime gate" ([string]$row.runtimeProofGate) ([string]$supportedSemanticFunctions[$id])
    Assert-Equal "condition function $id first failing gate" ([string]$row.firstFailingGate) "runtime-condition-evaluator"
    Assert-GreaterThan "condition function $id condition count" ([int]$row.conditionCount) 0
}

$packRows = @($ConditionRows | Where-Object { [string]$_.recordType -eq "PACK" } | Select-Object -First 5)
Assert-GreaterThan "PACK condition sample rows" $packRows.Count 0
foreach ($row in $packRows) {
    Assert-Equal "PACK condition schema" ([string]$row.conditionSchema) "PACK-CTDA"
    if ($null -eq $row.PSObject.Properties["unknownTail"]) {
        throw "PACK condition row did not preserve unknownTail schema field"
    }
}

$questTopLevel = 0
$questStageEntry = 0
$questObjectiveTarget = 0
foreach ($quest in $QuestRows) {
    $questTopLevel += [int]$quest.conditionCount
    foreach ($stage in @($quest.stages)) {
        foreach ($entry in @($stage.logEntries)) {
            $questStageEntry += [int]$entry.conditionCount
        }
    }
    foreach ($objective in @($quest.objectives)) {
        foreach ($target in @($objective.targets)) {
            $questObjectiveTarget += [int]$target.conditionCount
        }
    }
}
Assert-Equal "QUEST top-level structured condition count" $questTopLevel ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-top-level" 0))
Assert-Equal "QUEST stage-entry structured condition count" $questStageEntry ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-stage-entry" 0))
Assert-Equal "QUEST objective-target structured condition count" $questObjectiveTarget ([int](Get-PropertyValue $Result.conditionOwnerScopeCounts "QUST-objective-target" 0))

$NoSilentSkipScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
& $NoSilentSkipScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -ContentLedgerDir $ContentLedgerDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}
$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV no-silent-skip classification"
$ClassificationResultPath = Join-Path $ClassificationDir "result.json"
$ClassificationResult = Read-Json $ClassificationResultPath "no-silent-skip result"
Assert-Equal "no-silent-skip status" ([string]$ClassificationResult.status) "PASS"
Assert-Equal "no-silent-skip condition function rows" ([int]$ClassificationResult.counts.conditionFunctionRows) ([int]$Result.conditionFunctionCount)
Assert-Equal "no-silent-skip condition function unclassified" ([int]$ClassificationResult.counts.conditionFunctionUnclassified) 0
$conditionFunctionCoverage = Get-PropertyValue $ClassificationResult.contentArtifactCoverage.checkedCounts "condition-functions.json" $null
$conditionRowsCoverage = Get-PropertyValue $ClassificationResult.contentArtifactCoverage.checkedCounts "conditions.json" $null
Assert-Equal "no-silent-skip condition-functions artifact actual" ([int](Get-PropertyValue $conditionFunctionCoverage "actual" 0)) ([int]$Result.conditionFunctionCount)
Assert-Equal "no-silent-skip conditions artifact actual" ([int](Get-PropertyValue $conditionRowsCoverage "actual" 0)) ([int]$Result.conditionRowCount)

$Metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    contentLedgerDir = $ContentLedgerDir
    noSilentSkipDir = $ClassificationDir
    conditionRows = [int]$Result.conditionRowCount
    conditionFunctions = [int]$Result.conditionFunctionCount
    conditionFunctionUseCount = [int]$Result.conditionFunctionUseCount
    unknownFunctions = [int]$Result.conditionUnknownFunctionCount
    malformedRows = [int]$Result.conditionMalformedCount
    unaccountedRows = [int]$Result.conditionUnaccountedCount
    runtimeBoundary = "CTDA/CTDT rows are fully harvested and classified; condition evaluator gameplay remains loaded-pending-runtime."
}
$MetadataPath = Join-Path $ProofDir "result.json"
$Metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $MetadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "FNV condition-function accounting contract PASS"
Write-ProofLine "ContentLedgerDir: $ContentLedgerDir"
Write-ProofLine "NoSilentSkipDir: $ClassificationDir"
Write-ProofLine "Result: $MetadataPath"
