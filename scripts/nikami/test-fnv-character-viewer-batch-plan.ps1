param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Planner = Join-Path $PSScriptRoot "fnv_character_viewer_batch_plan.py"
$BatchRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer-batch-plan.ps1"
if (!(Test-Path -LiteralPath $Planner -PathType Leaf)) {
    throw "Missing FNV character viewer batch planner: $Planner"
}
if (!(Test-Path -LiteralPath $BatchRunner -PathType Leaf)) {
    throw "Missing FNV character viewer batch runner: $BatchRunner"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-character-viewer-batch-plan-contract/$Stamp"
$FixtureDir = Join-Path $ProofDir "fixture"
New-Item -ItemType Directory -Force -Path $FixtureDir | Out-Null

function New-Row(
    [string]$Component,
    [string]$ActorKind,
    [string]$ActorFormId,
    [string]$ActorEditorId,
    [string]$PlacedRefFormId = "",
    [string]$PlacedRefEditorId = "",
    [string]$SourceRecordType = "",
    [string]$ResolvedRecordType = "",
    [string]$ResolvedFormId = "",
    [string]$ResolvedEditorId = "",
    [string]$ProofAnchor = ""
) {
    if ([string]::IsNullOrWhiteSpace($SourceRecordType)) { $SourceRecordType = $ActorKind }
    [pscustomobject][ordered]@{
        plugin = "Contract.esm"
        actorKind = $ActorKind
        actorFormId = $ActorFormId
        actorEditorId = $ActorEditorId
        placedRefFormId = $PlacedRefFormId
        placedRefEditorId = $PlacedRefEditorId
        component = $Component
        sourceRecordType = $SourceRecordType
        sourceFormId = if (![string]::IsNullOrWhiteSpace($PlacedRefFormId)) { $PlacedRefFormId } else { $ActorFormId }
        sourceEditorId = if (![string]::IsNullOrWhiteSpace($PlacedRefEditorId)) { $PlacedRefEditorId } else { $ActorEditorId }
        resolvedRecordType = $ResolvedRecordType
        resolvedFormId = $ResolvedFormId
        resolvedEditorId = $ResolvedEditorId
        classification = "loaded-pending-runtime"
        firstFailingGate = "contract-fixture"
        proofAnchor = $ProofAnchor
        notes = "Synthetic no-payload contract row."
    }
}

$ledger = @(
    (New-Row "actor-base-record" "NPC_" "0x00000001" "ContractNpc" "" "" "NPC_" "" "" "" "npc-face-assembly"),
    (New-Row "npc-race" "NPC_" "0x00000001" "ContractNpc" "" "" "NPC_" "" "" "" "npc-face-assembly"),
    (New-Row "actor-base-record" "CREA" "0x00000002" "ContractCreature" "" "" "CREA" "" "" "" "creature-body-assembly"),
    (New-Row "creature-model" "CREA" "0x00000002" "ContractCreature" "" "" "CREA" "" "" "" "creature-body-assembly"),
    (New-Row "placed-reference" "NPC_" "0x00000001" "ContractNpc" "0x00000011" "ContractNpcRef" "ACHR" "NPC_" "0x00000001" "ContractNpc" "npc-face-assembly"),
    (New-Row "placed-reference" "CREA" "0x00000002" "ContractCreature" "0x00000012" "ContractCreatureRef" "ACRE" "CREA" "0x00000002" "ContractCreature" "creature-body-assembly")
)
$ledgerPath = Join-Path $FixtureDir "actor-presentation-ledger.json"
$ledger | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ledgerPath -Encoding UTF8

$result = [pscustomobject][ordered]@{
    status = "PASS"
    content = @("Contract.esm")
    npcBaseRecords = 1
    creatureBaseRecords = 1
    placedNpcCreatureRefs = 2
    unclassifiedCount = 0
    artifacts = [pscustomobject][ordered]@{
        ledger = $ledgerPath
        result = (Join-Path $FixtureDir "result.json")
    }
}
$resultPath = Join-Path $FixtureDir "result.json"
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8

$outDir = Join-Path $ProofDir "out"
& python $Planner --ledger-json $ledgerPath --result-json $resultPath --out-dir $outDir --require-pass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV character viewer batch planner fixture failed with exit code $LASTEXITCODE."
}

$planPath = Join-Path $outDir "viewer-batch-plan.json"
if (!(Test-Path -LiteralPath $planPath -PathType Leaf)) {
    throw "Missing generated viewer batch plan: $planPath"
}
$plan = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json

if ($plan.schema -ne "nikami-fnv-character-viewer-batch-plan-v1") {
    throw "Unexpected batch plan schema: $($plan.schema)"
}
if ($plan.status -ne "PASS") {
    throw "Batch plan fixture did not pass: $($plan.status)"
}
if ([int]$plan.expectedCounts.baseActors -ne 2 -or [int]$plan.expectedCounts.placedActorRefs -ne 2) {
    throw "Batch plan fixture expected counts are wrong."
}
if ([int]$plan.plannedCounts.baseActors -ne 2 -or [int]$plan.plannedCounts.placedActorRefs -ne 2) {
    throw "Batch plan fixture planned counts are wrong."
}
if ([int]$plan.plannedCounts.npc -ne 2 -or [int]$plan.plannedCounts.creature -ne 2) {
    throw "Batch plan fixture actor-kind counts are wrong."
}
if ([int]$plan.failures.invalidClassification -ne 0 -or [int]$plan.failures.missingTarget -ne 0 -or [int]$plan.failures.missingCommand -ne 0) {
    throw "Batch plan fixture reported target/classification failures."
}

$entries = @($plan.entries)
$creatureEntries = @($entries | Where-Object { $_.actorKind -eq "creature" })
$npcEntries = @($entries | Where-Object { $_.actorKind -eq "npc" })
if ($creatureEntries.Count -ne 2 -or $npcEntries.Count -ne 2) {
    throw "Batch plan fixture did not emit both NPC and creature entries."
}
if (@($creatureEntries | Where-Object { $_.command -notmatch "-ActorKind creature" -or $_.command -notmatch "-CreatureDiagnostics" }).Count -gt 0) {
    throw "Creature batch commands do not enable creature diagnostics."
}
if (@($npcEntries | Where-Object { $_.command -notmatch "-ActorKind npc" }).Count -gt 0) {
    throw "NPC batch commands do not set ActorKind npc."
}
$allowed = @(
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof"
)
$badClassifications = @($entries | Where-Object { $allowed -notcontains $_.classification })
if ($badClassifications.Count -gt 0) {
    throw "Batch plan has invalid classifications: $($badClassifications.Count)"
}

$beforeRunDirs = @()
$batchRunRoot = Join-Path $ProofRoot "fnv-character-viewer-batch-run"
if (Test-Path -LiteralPath $batchRunRoot -PathType Container) {
    $beforeRunDirs = @(Get-ChildItem -LiteralPath $batchRunRoot -Directory | Select-Object -ExpandProperty FullName)
}
& $BatchRunner -PlanJson $planPath -DryRun -ActorKind creature -MaxEntries 1 -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV character viewer batch dry-run failed with exit code $LASTEXITCODE."
}
$afterRunDirs = @(Get-ChildItem -LiteralPath $batchRunRoot -Directory | Sort-Object LastWriteTime -Descending)
$runDir = $null
foreach ($dir in $afterRunDirs) {
    if ($beforeRunDirs -notcontains $dir.FullName) {
        $runDir = $dir.FullName
        break
    }
}
if ([string]::IsNullOrWhiteSpace($runDir)) {
    $runDir = ($afterRunDirs | Select-Object -First 1).FullName
}
$runPath = Join-Path $runDir "viewer-batch-run.json"
if (!(Test-Path -LiteralPath $runPath -PathType Leaf)) {
    throw "Missing generated batch dry-run summary: $runPath"
}
$run = Get-Content -LiteralPath $runPath -Raw | ConvertFrom-Json
if ($run.schema -ne "nikami-fnv-character-viewer-batch-run-v1") {
    throw "Unexpected batch dry-run schema: $($run.schema)"
}
if ($run.status -ne "PASS" -or !$run.dryRun -or [int]$run.selectedEntries -ne 1) {
    throw "Batch dry-run summary has wrong status/selection."
}
$runResult = @($run.results)[0]
if ($runResult.actorKind -ne "creature" -or $runResult.status -ne "DRY-RUN") {
    throw "Batch dry-run did not select a creature entry."
}
if ($runResult.command -notmatch "-ActorKind creature" -or $runResult.command -notmatch "-CreatureDiagnostics") {
    throw "Batch dry-run result does not preserve runnable creature command."
}
$runHtml = Join-Path $runDir "viewer-batch-run.html"
$runMarkdown = Join-Path $runDir "viewer-batch-run.md"
if (!(Test-Path -LiteralPath $runHtml -PathType Leaf)) {
    throw "Missing generated batch dry-run HTML: $runHtml"
}
if (!(Test-Path -LiteralPath $runMarkdown -PathType Leaf)) {
    throw "Missing generated batch dry-run Markdown: $runMarkdown"
}
$runHtmlText = Get-Content -LiteralPath $runHtml -Raw
$runMarkdownText = Get-Content -LiteralPath $runMarkdown -Raw
if (!$runHtmlText.Contains("FNV Character Viewer Batch Run") -or !$runHtmlText.Contains("ContractCreature")) {
    throw "Batch dry-run HTML does not expose selected creature entry."
}
if (!$runMarkdownText.Contains("DRY-RUN") -or !$runMarkdownText.Contains("ContractCreature")) {
    throw "Batch dry-run Markdown does not expose selected creature entry."
}

Write-Host "FNV character viewer batch plan contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "Plan: $planPath"
Write-Host "DryRun: $runPath"
Write-Host "DryRunHtml: $runHtml"
