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

$SelectedQuestFunctionCounts = [ordered]@{
    "56" = 3
    "58" = 17
    "59" = 0
    "420" = 20
    "421" = 7
    "546" = 2
}

$RuntimeSupportedFunctions = @("56", "58", "420", "421", "546")

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-quest-condition-runtime-contract/$Stamp"
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

function Assert-QuestFunctionCount([object[]]$ConditionRows, [string]$Function, [int]$ExpectedCount) {
    $rows = @($ConditionRows | Where-Object {
        [string]$_.recordType -eq "QUST" -and [int]$_.function -eq [int]$Function
    })
    Assert-Equal "QUST CTDA function $Function row count" $rows.Count $ExpectedCount
    $checked = 0
    foreach ($row in $rows) {
        if ([string]$row.ownerScope -ne "QUST-objective-target") {
            throw "Unexpected QUST CTDA function $Function owner scope: $($row.ownerScope)"
        }
        if ([string]$row.conditionSchema -ne "TargetCondition") {
            throw "Unexpected QUST CTDA function $Function schema: $($row.conditionSchema)"
        }
        if ([string]$row.subrecord -ne "CTDA") {
            throw "Unexpected QUST CTDA function $Function subrecord: $($row.subrecord)"
        }
        ++$checked
    }
    Write-ProofLine "OK QUST CTDA function $Function row schema/accounting checked: $checked"
}

Write-ProofLine "FNV quest condition runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves selected real QUST CTDA quest/objective condition rows resolve adjusted quest FormIds and evaluate true/false against journal/objective runtime state. Full condition VM parity, non-QUST owners, global comparisons, reference/runOn semantics, and unsupported condition functions remain loaded-pending-runtime."
Write-ProofLine ""

Assert-Text "components/esm4/loadqust.cpp" "adjustSelectedQuestConditionParams" "selected QUST CTDA param adjustment helper"
Assert-Text "components/esm4/loadqust.cpp" "reader.adjustFormId(cond.param1)" "selected quest condition param1 FormId adjustment"
Assert-Text "components/esm4/script.hpp" "FUN_GetQuestRunning = 56" "GetQuestRunning function id"
Assert-Text "components/esm4/script.hpp" "FUN_GetObjectiveCompleted = 420" "GetObjectiveCompleted function id"
Assert-Text "components/esm4/script.hpp" "FUN_GetQuestCompleted = 546" "GetQuestCompleted function id"
Assert-Text "apps/openmw/engine.cpp" "runFalloutQuestConditionProof" "runtime quest condition proof hook"
Assert-Text "apps/openmw/engine.cpp" "selected-quest-condition-evaluator-runtime-supported" "bounded evaluator runtime classification"
Assert-Text "apps/openmw/engine.cpp" "fullConditionRuntime=loaded-pending-runtime" "full condition runtime remains pending"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvQuestConditionTrace" "flat proof can enable quest condition trace"
Assert-Text "scripts/nikami/fnv_content_ledger.py" '"conditions": proof_dir / "conditions.json"' "content ledger emits conditions"
Assert-Text "scripts/nikami/fnv_content_ledger.py" '"conditionFunctions": proof_dir / "condition-functions.json"' "content ledger emits condition functions"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"condition-functions.json": "conditionFunctionCount"' "no-silent-skip accounts condition-functions artifact"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$Result = Read-Json (Join-Path $ContentLedgerDir "result.json") "content ledger result"
$ConditionRows = @(Read-JsonArray (Join-Path $ContentLedgerDir "conditions.json") "content ledger conditions")
$FunctionRows = @(Read-JsonArray (Join-Path $ContentLedgerDir "condition-functions.json") "content ledger condition functions")

Assert-Equal "content ledger status" ([string]$Result.status) "PASS"
Assert-Equal "PC-flat QUST condition rows" ([int]$Result.conditionOwnerRecordCounts.QUST) 2084
Assert-Equal "PC-flat QUST objective-target condition rows" ([int]$Result.conditionOwnerScopeCounts."QUST-objective-target") 1736
Assert-Equal "condition unknown function count" ([int]$Result.conditionUnknownFunctionCount) 0
Assert-Equal "condition malformed count" ([int]$Result.conditionMalformedCount) 0
Assert-Equal "condition unaccounted count" ([int]$Result.conditionUnaccountedCount) 0
Assert-Equal "condition function unclassified count" ([int]$Result.conditionFunctionUnclassified) 0

foreach ($entry in $SelectedQuestFunctionCounts.GetEnumerator()) {
    Assert-QuestFunctionCount $ConditionRows ([string]$entry.Key) ([int]$entry.Value)
}

foreach ($function in @("56", "58", "59", "420", "421", "546")) {
    $row = $FunctionRows | Where-Object { [int]$_.function -eq [int]$function } | Select-Object -First 1
    if ($null -eq $row) {
        throw "Missing condition function row: $function"
    }
    Assert-GreaterThan "condition function $function total observed rows" ([int]$row.conditionCount) 0
    Assert-Equal "condition function $function classification" ([string]$row.classification) "loaded-pending-runtime"
    Assert-Equal "condition function $function semantic classification" ([string]$row.functionSemanticsClassification) "runtime-supported"
    Assert-Equal "condition function $function first failing gate" ([string]$row.firstFailingGate) "runtime-condition-evaluator"
}

$RuntimePatterns = @(
    "FNV/ESM4 proof: quest condition evaluator PASS .*function=56 .*ownerQuest=VMS49 .*paramQuest=VFSHighTimes .*param1=FormId:0x110e1da .*passStateApplied=1 .*passValue=1 .*passResult=1 .*failStateApplied=1 .*failValue=0 .*failResult=0 .*fullConditionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest condition evaluator PASS .*function=58 .*ownerQuest=VMQYesManFailsafe .*paramQuest=VMQYesMan01 .*param1=FormId:0x1157321 .*passStateApplied=1 .*passValue=101 .*passResult=1 .*failStateApplied=1 .*failValue=100 .*failResult=0 .*fullConditionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest condition evaluator PASS .*function=420 .*ownerQuest=VMS53 .*paramQuest=VMS53 .*param1=FormId:0x1151403 .*param2=10 .*passStateApplied=1 .*passValue=1 .*passResult=1 .*failStateApplied=1 .*failValue=0 .*failResult=0 .*fullConditionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest condition evaluator PASS .*function=421 .*ownerQuest=VMS49 .*paramQuest=VMS03 .*param1=FormId:0x10e282d .*param2=20 .*passStateApplied=1 .*passValue=1 .*passResult=1 .*failStateApplied=1 .*failValue=0 .*failResult=0 .*fullConditionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest condition evaluator PASS .*function=546 .*ownerQuest=NVDLC01MQ03 .*paramQuest=NVDLC01MQ03b .*param1=FormId:0x2013a43 .*passStateApplied=1 .*passValue=1 .*passResult=1 .*failStateApplied=1 .*failValue=0 .*failResult=0 .*fullConditionRuntime=loaded-pending-runtime",
    "FNV/ESM4 proof: quest condition evaluator summary PASS .*evaluated=5 .*expected=5 .*missingObservedQuestGetStageDone=1 .*fullConditionRuntime=loaded-pending-runtime"
)

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofDir `
    -RunSeconds $RuntimeRunSeconds `
    -FnvQuestConditionTrace `
    -FnvQuestConditionFrame 120 `
    -RequireLogPattern $RuntimePatterns `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$FlatSummary = Join-Path $FlatProofDir "summary.txt"
Assert-FileContains $FlatSummary "^FnvQuestConditionTrace: True$" "flat proof required quest condition trace" | Out-Null
Assert-FileContains $FlatSummary "^FnvQuestConditionFrame: 120$" "flat proof selected quest condition frame" | Out-Null
foreach ($function in $RuntimeSupportedFunctions) {
    Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest condition evaluator PASS .*function=$function .*runtimeBoundary=selected-quest-condition-evaluator-runtime-supported" "runtime condition function $function pass" | Out-Null
}
Assert-FileContains $OpenMwLog "FNV/ESM4 proof: quest condition evaluator summary PASS .*evaluated=5" "runtime condition evaluator summary" | Out-Null

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    selectedQuestFunctionCounts = $SelectedQuestFunctionCounts
    runtimeSupportedFunctions = $RuntimeSupportedFunctions
    missingObservedQuestGetStageDone = 1
    runtimeBoundary = "selected-quest-condition-evaluator-runtime-supported"
    fullConditionRuntime = "loaded-pending-runtime"
}
$metadataPath = Join-Path $ProofDir "quest-condition-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding ASCII
Write-ProofLine "Metadata: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV quest condition runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
