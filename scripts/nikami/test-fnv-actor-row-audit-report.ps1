param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Runner = Join-Path $PSScriptRoot "run-fnv-actor-row-audit-report.ps1"
if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "Missing FNV actor row audit report runner: $Runner"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-actor-row-audit-report-contract/$Stamp"
$FixtureDir = Join-Path $ProofDir "fixture"
$RunRoot = Join-Path $FixtureDir "runs"
$OutDir = Join-Path $ProofDir "out"
New-Item -ItemType Directory -Force -Path $FixtureDir, $RunRoot, $OutDir | Out-Null

function New-Row(
    [string]$Id,
    [string]$Target,
    [string]$Phase,
    [string]$Gate,
    [string]$Classification,
    [string[]]$States = @("neutral")
) {
    [pscustomobject][ordered]@{
        id = $Id
        entryId = "fixture-$Target"
        priority = if ($Target -eq "GSEasyPete") { "easy-pete" } else { "normal" }
        source = "actor-base-record"
        actorKind = "npc"
        target = $Target
        runtimeTarget = $Target
        placedTarget = ""
        baseActorTarget = $Target
        actorFormId = "0x00000001"
        actorEditorId = $Target
        phase = $Phase
        gate = $Gate
        runtimeStates = @($States)
        requiredComponents = @("actor-base-record")
        presentComponents = @("actor-base-record")
        classification = $Classification
        firstFailingGate = "$Gate-runtime-proof"
        notes = "contract fixture"
        proofExpectations = @("runtime log gate", "screenshots", "actor-kit")
        runGateCommand = "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 -Targets '$Target' -ActorKind npc -Phases $Phase"
        payloadPolicy = "generated IDs, commands, classifications, and asset path provenance only; no retail payload bytes"
    }
}

$rows = @(
    (New-Row "000001:talk:02:voice-lip-sidecar" "GSEasyPete" "talk" "voice-lip-sidecar" "loaded-pending-runtime" @("talk", "mouth-open")),
    (New-Row "000002:talk:02:voice-lip-sidecar" "PendingPete" "talk" "voice-lip-sidecar" "loaded-pending-runtime" @("talk", "mouth-open")),
    (New-Row "000003:weapon:03:projectile-muzzle-sound" "FailingPete" "weapon" "projectile-muzzle-sound" "loaded-pending-runtime" @("projectile-fire")),
    (New-Row "000004:weapon:03:projectile-muzzle-sound" "UnrunPete" "weapon" "projectile-muzzle-sound" "loaded-pending-runtime" @("projectile-fire")),
    (New-Row "000005:head:02:facegen-morph-targets" "BlockedPete" "head" "facegen-morph-targets" "known-blocked" @("neutral")),
    (New-Row "000006:headgear:01:headgear-slot-composition" "NoHatPete" "headgear" "headgear-slot-composition" "intentionally-excluded-with-proof" @("neutral"))
)

$burnDownPath = Join-Path $FixtureDir "actor-parity-burndown.json"
[pscustomobject][ordered]@{
    schema = "nikami-fnv-actor-parity-burndown-v1"
    status = "PASS"
    createdAt = (Get-Date).ToString("s")
    sourcePlan = "contract-fixture"
    payloadPolicy = "generated proof/control metadata only; no retail assets or mod payload bytes"
    runtimeBoundary = "loaded-pending-runtime means accounted and queued, not complete"
    counts = [pscustomobject][ordered]@{
        entries = 6
        rows = 6
        criticalRows = 1
        invalidClassification = 0
        unclassified = 0
        missingRuntimeCommand = 0
    }
    classificationCounts = [pscustomobject][ordered]@{
        "loaded-pending-runtime" = 4
        "known-blocked" = 1
        "intentionally-excluded-with-proof" = 1
    }
    rows = @($rows)
} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $burnDownPath -Encoding UTF8

function Write-Run(
    [string]$DirName,
    [object[]]$Results
) {
    $dir = Join-Path $RunRoot $DirName
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [pscustomobject][ordered]@{
        schema = "nikami-fnv-actor-parity-burndown-run-v1"
        status = if (@($Results | Where-Object { $_.status -eq "FAIL" }).Count -eq 0) { "PASS" } else { "PASS" }
        dryRun = $false
        burnDownJson = $burnDownPath
        selectedRowsJson = Join-Path $dir "selected-rows.json"
        selectedRows = $Results.Count
        rowGateAuditCounts = [pscustomobject][ordered]@{
            runtimeSupported = @($Results | Where-Object { $_.rowGateAudit.status -eq "PASS" }).Count
            loadedPendingRuntime = @($Results | Where-Object { $_.rowGateAudit.status -eq "PENDING" }).Count
            failed = @($Results | Where-Object { $_.rowGateAudit.status -eq "FAIL" }).Count
        }
        payloadPolicy = "generated row/run metadata and proof links only; no retail asset payload bytes"
        results = @($Results)
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $dir "actor-parity-burndown-run.json") -Encoding UTF8
}

function New-Result(
    [object]$Row,
    [string]$RunStatus,
    [string]$AuditStatus,
    [string]$RuntimeClass,
    [string[]]$Missing,
    [string[]]$Observed,
    [string]$ProofMode
) {
    $safeId = $Row.id -replace '[^A-Za-z0-9_.-]', '_'
    $childDir = Join-Path $FixtureDir "child-$safeId"
    $proofDir = Join-Path $childDir "proof"
    New-Item -ItemType Directory -Force -Path $proofDir | Out-Null

    $childFailures = @()
    if ($AuditStatus -eq "PENDING") {
        $childFailures = @("contract child pending failure")
    } elseif ($AuditStatus -eq "FAIL") {
        $childFailures = @("contract child failure")
    }
    $viewerJson = Join-Path $childDir "character-viewer-manifest.json"
    $viewerManifest = Join-Path $childDir "viewer-runs.json"
    $suiteJson = Join-Path $childDir "character-builder-suite.json"
    [pscustomobject][ordered]@{
        reportStatus = $RunStatus
        runtimeGateStatus = $AuditStatus
        proofDir = $proofDir
        phase = $Row.phase
        angle = "front"
        screenshots = @("screenshot000.png")
        failures = @($childFailures)
        runtimeGateError = if ($AuditStatus -eq "FAIL") { "contract runtime gate error" } else { "" }
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $suiteJson -Encoding UTF8
    [pscustomobject][ordered]@{
        reportStatus = $RunStatus
        runtimeGateStatus = $AuditStatus
        proofDir = $proofDir
        screenshots = @("screenshot000.png")
        failures = @($childFailures)
        suiteJson = $suiteJson
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $viewerJson -Encoding UTF8
    [pscustomobject][ordered]@{
        schema = "contract-viewer-runs-v1"
        viewerJson = $viewerJson
        suiteJson = $suiteJson
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $viewerManifest -Encoding UTF8

    [pscustomobject][ordered]@{
        id = $Row.id
        entryId = $Row.entryId
        priority = $Row.priority
        actorKind = $Row.actorKind
        target = $Row.target
        runtimeTarget = $Row.runtimeTarget
        phase = $Row.phase
        gate = $Row.gate
        classification = $Row.classification
        firstFailingGate = $Row.firstFailingGate
        runtimeStates = @($Row.runtimeStates)
        status = $RunStatus
        dryRun = $false
        rowJson = Join-Path $FixtureDir "$safeId.json"
        command = $Row.runGateCommand
        viewerIndex = Join-Path $childDir "$safeId.html"
        viewerManifest = $viewerManifest
        viewerJson = $viewerJson
        actorKit = Join-Path $childDir "$safeId-actor-kit.json"
        rowRuntimeClassification = $RuntimeClass
        rowGateProofMode = $ProofMode
        rowGateAudit = [pscustomobject][ordered]@{
            schema = "nikami-fnv-actor-row-gate-audit-v1"
            status = $AuditStatus
            classification = $RuntimeClass
            selectedPhase = $Row.phase
            selectedGate = $Row.gate
            selectedRuntimeStates = @($Row.runtimeStates)
            requestedAngles = @("front", "front-left", "front-right")
            requiredEvidenceKinds = @("viewer-manifest", "phase-angle")
            observedEvidenceKinds = @($Observed)
            missingEvidenceKinds = @($Missing)
            errors = if ($AuditStatus -eq "FAIL") { @("contract failure") } else { @() }
        }
        runtimeEvidenceBoundary = "contract fixture"
        evidence = @($Observed)
        error = if ($RunStatus -eq "FAIL") { "contract failure" } else { "" }
    }
}

Write-Run "run-001" @(
    (New-Result $rows[0] "PASS" "PASS" "runtime-supported" @() @("viewer-manifest", "phase-angle", "mouth-runtime-evidence", "screenshots", "dialogue-mode") "viewer-gate-state-runtime-supported"),
    (New-Result $rows[1] "PASS" "PENDING" "loaded-pending-runtime" @("mouth-runtime-evidence", "screenshots") @("viewer-manifest", "phase-angle", "dialogue-mode") "viewer-phase-proof-pending-gate-state-audit"),
    (New-Result $rows[2] "FAIL" "FAIL" "known-blocked" @("projectile-runtime-evidence") @("viewer-manifest") "viewer-gate-state-failed")
)

& $Runner -ProofRoot $ProofRoot -BurnDownJson $burnDownPath -RunRoot $RunRoot -OutDir $OutDir | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor row audit report fixture failed with exit code $LASTEXITCODE."
}

$reportPath = Join-Path $OutDir "actor-row-audit-report.json"
$markdownPath = Join-Path $OutDir "actor-row-audit-report.md"
$htmlPath = Join-Path $OutDir "actor-row-audit-report.html"
$csvPath = Join-Path $OutDir "actor-row-audit-report.csv"
foreach ($path in @($reportPath, $markdownPath, $htmlPath, $csvPath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing actor row audit report artifact: $path"
    }
}

$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
if ($report.schema -ne "nikami-fnv-actor-row-audit-report-v1") {
    throw "Unexpected actor row audit report schema: $($report.schema)"
}
if ($report.status -ne "PASS" -or $report.runtimeStatus -ne "FAIL") {
    throw "Actor row audit report did not preserve generation PASS plus runtime FAIL status."
}
if ([int]$report.counts.burnDownRows -ne 6 -or [int]$report.counts.runtimeRequiredRows -ne 4) {
    throw "Actor row audit report row counts are wrong."
}
if ([int]$report.counts.runMatchedRows -ne 3 -or [int]$report.counts.unrunRuntimeRequiredRows -ne 1) {
    throw "Actor row audit report run coverage counts are wrong."
}
if ([int]$report.counts.runtimeSupportedRows -ne 1 -or [int]$report.counts.loadedPendingRuntimeRows -ne 2 -or [int]$report.counts.failedRows -ne 1) {
    throw "Actor row audit report runtime classification counts are wrong."
}
if ([int]$report.counts.rowsWithScreenshots -ne 3 -or [int]$report.counts.childFailureRows -ne 2) {
    throw "Actor row audit report child proof counts are wrong."
}
if ([int]$report.effectiveClassificationCounts."known-blocked" -ne 2 -or [int]$report.effectiveClassificationCounts."intentionally-excluded-with-proof" -ne 1) {
    throw "Actor row audit report did not preserve blocked/excluded accounting."
}
if ([int]$report.missingEvidenceCounts."runtime-row-run" -ne 1 -or [int]$report.missingEvidenceCounts."mouth-runtime-evidence" -ne 1 -or [int]$report.missingEvidenceCounts."projectile-runtime-evidence" -ne 1) {
    throw "Actor row audit report missing-evidence counts are wrong."
}
if ([int]$report.childFailureCounts."contract child pending failure" -ne 1 -or [int]$report.childFailureCounts."contract child failure" -ne 1) {
    throw "Actor row audit report child failure counts are wrong."
}
$supportedRow = @($report.rows | Where-Object { $_.runtimeTarget -eq "GSEasyPete" })[0]
if ([int]$supportedRow.screenshotCount -ne 1 -or !$supportedRow.mainImage.Contains("screenshot000.png")) {
    throw "Actor row audit report did not surface child screenshot evidence."
}

$markdown = Get-Content -LiteralPath $markdownPath -Raw
$html = Get-Content -LiteralPath $htmlPath -Raw
$csv = Get-Content -LiteralPath $csvPath -Raw
foreach ($needle in @("FNV Actor Row Audit Report", "Runtime status", "mouth-runtime-evidence", "runtime-row-run", "Rows with screenshots", "contract child pending failure", "screenshot000.png")) {
    if (!$markdown.Contains($needle)) { throw "Actor row audit Markdown missing $needle" }
}
foreach ($needle in @("FNV Actor Row Audit Report", "Runtime: FAIL", "projectile-runtime-evidence", "contract child failure", "screenshot000.png")) {
    if (!$html.Contains($needle)) { throw "Actor row audit HTML missing $needle" }
}
if (!$csv.Contains("runtime-row-run") -or !$csv.Contains("viewer-gate-state-runtime-supported") -or !$csv.Contains("contract child pending failure") -or !$csv.Contains("screenshot000.png")) {
    throw "Actor row audit CSV missing expected row proof values."
}

Write-Host "FNV actor row audit report contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "ReportJson: $reportPath"
