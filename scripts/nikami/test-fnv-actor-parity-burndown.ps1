param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Generator = Join-Path $PSScriptRoot "fnv_actor_parity_burndown.py"
$Runner = Join-Path $PSScriptRoot "run-fnv-actor-parity-burndown.ps1"
if (!(Test-Path -LiteralPath $Generator -PathType Leaf)) {
    throw "Missing FNV actor parity burn-down generator: $Generator"
}
if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "Missing FNV actor parity burn-down runner: $Runner"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-actor-parity-burndown-contract/$Stamp"
$FixtureDir = Join-Path $ProofDir "fixture"
New-Item -ItemType Directory -Force -Path $FixtureDir | Out-Null

function New-Entry(
    [string]$Id,
    [string]$ActorKind,
    [string]$Target,
    [string[]]$Phases,
    [hashtable]$ComponentCounts,
    [string]$Source = "actor-base-record"
) {
    $componentEvidence = @()
    foreach ($key in $ComponentCounts.Keys) {
        $componentEvidence += [pscustomobject][ordered]@{
            component = $key
            sourceRecordType = "CONTRACT"
            sourceFormId = "0x00000001"
            resolvedRecordType = "CONTRACT"
            resolvedFormId = "0x00000001"
            assetPath = "generated/contract/$key"
            classification = "loaded-pending-runtime"
            firstFailingGate = "contract-fixture"
            proofAnchor = "contract-$key"
        }
    }
    [pscustomobject][ordered]@{
        id = $Id
        source = $Source
        actorKind = $ActorKind
        target = $Target
        runtimeTarget = $Target
        placedTarget = if ($Source -eq "placed-reference") { "$Target`Ref" } else { "" }
        baseActorTarget = $Target
        actorFormId = "0x00000001"
        actorEditorId = $Target
        phases = $Phases
        classification = "loaded-pending-runtime"
        firstFailingGate = "contract-fixture"
        componentCounts = [pscustomobject]$ComponentCounts
        componentEvidence = $componentEvidence
        command = "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 -Targets '$Target' -ActorKind $ActorKind"
    }
}

$plan = [pscustomobject][ordered]@{
    schema = "nikami-fnv-character-viewer-batch-plan-v1"
    status = "PASS"
    artifacts = [pscustomobject][ordered]@{ plan = (Join-Path $FixtureDir "viewer-batch-plan.json") }
    entries = @(
        (New-Entry "contract-npc-full" "npc" "GSEasyPete" @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk") @{
            "actor-base-record" = 1
            "npc-race" = 1
            "npc-headpart" = 4
            "hair" = 1
            "eyes" = 1
            "facegen-symmetric-shape" = 1
            "facegen-asymmetric-shape" = 1
            "facegen-symmetric-texture" = 1
            "default-outfit" = 1
            "equipment-armor" = 1
            "equipment-armor-addon" = 1
            "equipment-weapon" = 1
            "animation-kffz" = 1
            "voice-type" = 1
        }),
        (New-Entry "contract-npc-no-optional" "npc" "ContractSettler" @("body", "face", "weapon", "headgear", "talk") @{
            "actor-base-record" = 1
            "npc-race" = 1
            "eyes" = 1
            "facegen-symmetric-texture" = 1
        }),
        (New-Entry "contract-creature" "creature" "ContractCreature" @("creature-model", "creature-body", "creature-animation", "creature-full") @{
            "creature-model" = 1
            "creature-body-nif" = 1
            "bodypart-data" = 1
            "animation-kffz" = 1
        } "placed-reference")
    )
}
$planPath = Join-Path $FixtureDir "viewer-batch-plan.json"
$plan | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $planPath -Encoding UTF8

$outDir = Join-Path $ProofDir "out"
& $Runner -ProofRoot $ProofRoot -PlanJson $planPath -OutDir $outDir -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down fixture failed with exit code $LASTEXITCODE."
}

$jsonPath = Join-Path $outDir "actor-parity-burndown.json"
$markdownPath = Join-Path $outDir "actor-parity-burndown.md"
$csvPath = Join-Path $outDir "actor-parity-burndown.csv"
foreach ($path in @($jsonPath, $markdownPath, $csvPath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing actor parity burn-down artifact: $path"
    }
}

$burn = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
if ($burn.schema -ne "nikami-fnv-actor-parity-burndown-v1") {
    throw "Unexpected burn-down schema: $($burn.schema)"
}
if ($burn.status -ne "PASS") {
    throw "Burn-down fixture did not pass: $($burn.status)"
}
if ([int]$burn.counts.entries -ne 3 -or [int]$burn.counts.rows -le 30) {
    throw "Burn-down fixture did not expand actor entries into enough phase/gate rows."
}
if ([int]$burn.counts.unclassified -ne 0 -or [int]$burn.counts.invalidClassification -ne 0) {
    throw "Burn-down fixture has unclassified or invalid rows."
}

$allowed = @(
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof"
)
$rows = @($burn.rows)
$bad = @($rows | Where-Object { $allowed -notcontains $_.classification })
if ($bad.Count -gt 0) {
    throw "Burn-down emitted invalid row classifications: $($bad.Count)"
}
foreach ($gate in @(
        "face-skin-tone-wrinkles",
        "eyes-mouth-teeth-tongue",
        "hair-beard-brow",
        "headgear-slot-composition",
        "hat-hair-occlusion",
        "weapon-prop-attachment",
        "projectile-muzzle-sound",
        "voice-lip-sidecar",
        "mouth-teeth-lip-sync",
        "creature-idle-walk-run",
        "creature-attack-hit-death"
    )) {
    if (@($rows | Where-Object { $_.gate -eq $gate }).Count -eq 0) {
        throw "Burn-down missing required gate: $gate"
    }
}
foreach ($state in @("idle", "walk", "run", "talk", "mouth-open", "attack", "reload", "projectile-fire", "death")) {
    if (@($rows | Where-Object { @($_.runtimeStates) -contains $state }).Count -eq 0) {
        throw "Burn-down missing required runtime state: $state"
    }
}
$optionalExcluded = @($rows | Where-Object {
        $_.runtimeTarget -eq "ContractSettler" -and
        $_.classification -eq "intentionally-excluded-with-proof" -and
        ($_.gate -eq "weapon-prop-attachment" -or $_.gate -eq "voice-lip-sidecar" -or $_.gate -eq "headgear-slot-composition")
    })
if ($optionalExcluded.Count -lt 3) {
    throw "Burn-down did not explicitly exclude absent optional weapon/dialogue/headgear paths with proof."
}
$pendingRows = @($rows | Where-Object { $_.classification -eq "loaded-pending-runtime" })
if (@($pendingRows | Where-Object { [string]::IsNullOrWhiteSpace($_.runGateCommand) -or $_.runGateCommand -notmatch "run-fnv-character-viewer.ps1" }).Count -gt 0) {
    throw "Loaded-pending runtime rows do not expose runnable viewer proof commands."
}
$criticalRows = @($rows | Where-Object { $_.priority -eq "easy-pete" })
if ($criticalRows.Count -eq 0) {
    throw "Burn-down did not mark Easy Pete as a critical actor fixture."
}
$markdown = Get-Content -LiteralPath $markdownPath -Raw
if (!$markdown.Contains("FNV Actor Parity Burn-Down") -or !$markdown.Contains("GECK facial animation")) {
    throw "Burn-down Markdown does not describe the generated parity matrix and GECK references."
}
$csv = Get-Content -LiteralPath $csvPath -Raw
if (!$csv.Contains("projectile-muzzle-sound") -or !$csv.Contains("voice-lip-sidecar")) {
    throw "Burn-down CSV does not expose weapon projectile or voice/LIP rows."
}

$targetOutDir = Join-Path $ProofDir "target-filtered-out"
& $Runner -ProofRoot $ProofRoot -PlanJson $planPath -OutDir $targetOutDir -ActorKind "npc" -Target "GSEasyPete" -Priority "easy-pete" -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down target-filtered fixture failed with exit code $LASTEXITCODE."
}
$targetJsonPath = Join-Path $targetOutDir "actor-parity-burndown.json"
if (!(Test-Path -LiteralPath $targetJsonPath -PathType Leaf)) {
    throw "Missing target-filtered actor parity burn-down artifact: $targetJsonPath"
}
$targetBurn = Get-Content -LiteralPath $targetJsonPath -Raw | ConvertFrom-Json
if ($targetBurn.schema -ne "nikami-fnv-actor-parity-burndown-v1" -or $targetBurn.status -ne "PASS") {
    throw "Target-filtered burn-down did not pass with the expected schema."
}
if ([int]$targetBurn.counts.sourceEntries -ne 3 -or [int]$targetBurn.counts.filteredEntries -ne 1 -or [int]$targetBurn.counts.entries -ne 1) {
    throw "Target-filtered burn-down did not filter the source plan to one Easy Pete entry."
}
if ($targetBurn.entryFilters.actorKind -ne "npc" -or $targetBurn.entryFilters.target -ne "GSEasyPete" -or $targetBurn.entryFilters.priority -ne "easy-pete") {
    throw "Target-filtered burn-down did not preserve generator filter metadata."
}
$targetRows = @($targetBurn.rows)
if ($targetRows.Count -eq 0 -or @($targetRows | Where-Object { $_.runtimeTarget -ne "GSEasyPete" }).Count -gt 0) {
    throw "Target-filtered burn-down emitted rows for an actor other than GSEasyPete."
}

$runRoot = Join-Path $ProofRoot "fnv-actor-parity-burndown-run"
$beforeRunDirs = @()
if (Test-Path -LiteralPath $runRoot -PathType Container) {
    $beforeRunDirs = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}
& $Runner -ProofRoot $ProofRoot -BurnDownJson $jsonPath -DryRun -ActorKind "npc" -Target "GSEasyPete" -Priority "easy-pete" -Classification "loaded-pending-runtime" -Phase "weapon" -Gate "projectile-muzzle-sound" -MaxRows 1 -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down dry-run row fixture failed with exit code $LASTEXITCODE."
}
$newRunDir = Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Where-Object { $beforeRunDirs -notcontains $_.FullName } |
    Select-Object -First 1
if ($null -eq $newRunDir) {
    throw "Dry-run row fixture did not create a new burn-down run directory."
}
$runJsonPath = Join-Path $newRunDir.FullName "actor-parity-burndown-run.json"
$runHtmlPath = Join-Path $newRunDir.FullName "actor-parity-burndown-run.html"
$runMarkdownPath = Join-Path $newRunDir.FullName "actor-parity-burndown-run.md"
$selectedRowsPath = Join-Path $newRunDir.FullName "selected-rows.json"
foreach ($path in @($runJsonPath, $runHtmlPath, $runMarkdownPath, $selectedRowsPath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing actor parity burn-down run artifact: $path"
    }
}
$run = Get-Content -LiteralPath $runJsonPath -Raw | ConvertFrom-Json
if ($run.schema -ne "nikami-fnv-actor-parity-burndown-run-v1") {
    throw "Unexpected burn-down row run schema: $($run.schema)"
}
if ($run.status -ne "PASS" -or $run.dryRun -ne $true -or [int]$run.selectedRows -ne 1) {
    throw "Dry-run row fixture did not pass with exactly one selected row."
}
$runResult = @($run.results) | Select-Object -First 1
if ($null -eq $runResult -or $runResult.status -ne "DRY-RUN") {
    throw "Dry-run row fixture did not mark the selected row as DRY-RUN."
}
if ($runResult.phase -ne "weapon" -or $runResult.gate -ne "projectile-muzzle-sound") {
    throw "Dry-run row fixture selected the wrong phase/gate: $($runResult.phase)/$($runResult.gate)"
}
if ([string]$runResult.command -notmatch "run-fnv-character-viewer.ps1") {
    throw "Dry-run row fixture did not expose the runnable viewer command."
}
if (@($runResult.evidence) -notcontains "selected-row" -or @($runResult.evidence) -notcontains "selected-gate" -or @($runResult.evidence) -notcontains "selected-runtime-states" -or @($runResult.evidence) -notcontains "no-runtime-launch") {
    throw "Dry-run row fixture did not record no-launch evidence."
}
$selectedRows = @(Get-Content -LiteralPath $selectedRowsPath -Raw | ConvertFrom-Json)
if ($selectedRows.Count -ne 1 -or [string]$selectedRows[0].gate -ne "projectile-muzzle-sound") {
    throw "Dry-run selected-rows.json is not a stable one-row projectile array."
}
$rowJson = Get-Content -LiteralPath ([string]$runResult.rowJson) -Raw | ConvertFrom-Json
if ([string]$rowJson.gate -ne "projectile-muzzle-sound" -or @($rowJson.runtimeStates) -notcontains "projectile-fire") {
    throw "Dry-run row.json did not preserve selected gate/runtime state."
}
$runHtmlText = Get-Content -LiteralPath $runHtmlPath -Raw
$runMarkdownText = Get-Content -LiteralPath $runMarkdownPath -Raw
if (!$runHtmlText.Contains("FNV Actor Parity Burn-Down Run") -or !$runHtmlText.Contains("projectile-muzzle-sound")) {
    throw "Dry-run row HTML does not expose the selected projectile gate."
}
if (!$runMarkdownText.Contains("FNV Actor Parity Burn-Down Run") -or !$runMarkdownText.Contains("projectile-muzzle-sound")) {
    throw "Dry-run row Markdown does not expose the selected projectile gate."
}

$fakeViewer = Join-Path $FixtureDir "fake-run-fnv-character-viewer.ps1"
@'
param(
    [string]$ProofRoot,
    [string[]]$Targets,
    [string]$ActorKind,
    [string[]]$Phases,
    [string[]]$Angles,
    [int]$RunSeconds,
    [int]$ActorFrame,
    [string]$ScreenshotFrames,
    [string]$ActorKitAnimationGroup = "",
    [string]$ActorKitDialogueMode = "",
    [switch]$CreatureDiagnostics,
    [switch]$NoSound,
    [switch]$Serve,
    [int]$ServePort = 0
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $ProofRoot "fnv-character-viewer/fake-$stamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$viewerHtml = Join-Path $runDir "character-viewer.html"
$viewerJson = Join-Path $runDir "character-viewer-manifest.json"
$actorKitJson = Join-Path $runDir "character-actor-kit.json"
$cases = @()
foreach ($phase in @($Phases)) {
    foreach ($angle in @($Angles)) {
        $cases += [pscustomobject][ordered]@{
            case = "$($phase)_$($angle)"
            phase = $phase
            angle = $angle
            runtimeGateStatus = "PASS"
            reportStatus = "PASS"
            actorKitSelection = [pscustomobject][ordered]@{
                animationGroup = $ActorKitAnimationGroup
                dialogueMode = $ActorKitDialogueMode
            }
            screenshots = @()
            faceDrawables = @()
            materialEvidence = @()
            morphLines = @()
            weaponLines = @()
            animationPlayback = @()
            creatureEvidence = @()
        }
    }
}
"<html><body>fake viewer</body></html>" | Set-Content -LiteralPath $viewerHtml -Encoding UTF8
[pscustomobject][ordered]@{
    overallStatus = "PASS"
    target = @($Targets)[0]
    actorKind = $ActorKind
    phases = @($Phases)
    angles = @($Angles)
    actorKitAnimationGroup = $ActorKitAnimationGroup
    actorKitDialogueMode = $ActorKitDialogueMode
    runSeconds = $RunSeconds
    actorFrame = $ActorFrame
    screenshotFrames = $ScreenshotFrames
    cases = @($cases)
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $viewerJson -Encoding UTF8
[pscustomobject][ordered]@{
    schema = "fake-actor-kit-v1"
    target = @($Targets)[0]
    phases = @($Phases)
    angles = @($Angles)
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $actorKitJson -Encoding UTF8
$runs = @([pscustomobject][ordered]@{
    Target = @($Targets)[0]
    Status = "PASS"
    SuiteDir = $runDir
    ViewerHtml = $viewerHtml
    ViewerJson = $viewerJson
    ActorKitJson = $actorKitJson
})
ConvertTo-Json -InputObject $runs -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir "viewer-runs.json") -Encoding UTF8
"<html><body>fake run index</body></html>" | Set-Content -LiteralPath (Join-Path $runDir "index.html") -Encoding UTF8
'@ | Set-Content -LiteralPath $fakeViewer -Encoding UTF8

$beforeNonDryDirs = @()
if (Test-Path -LiteralPath $runRoot -PathType Container) {
    $beforeNonDryDirs = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}
& $Runner -ProofRoot $ProofRoot -BurnDownJson $jsonPath -RunRows -ActorKind "npc" -Target "GSEasyPete" -Priority "easy-pete" -Classification "loaded-pending-runtime" -Phase "talk" -Gate "voice-lip-sidecar" -MaxRows 1 -ViewerRunner $fakeViewer -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down fake non-dry row fixture failed with exit code $LASTEXITCODE."
}
$nonDryRunDir = Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Where-Object { $beforeNonDryDirs -notcontains $_.FullName } |
    Select-Object -First 1
if ($null -eq $nonDryRunDir) {
    throw "Fake non-dry row fixture did not create a burn-down run directory."
}
$nonDryJsonPath = Join-Path $nonDryRunDir.FullName "actor-parity-burndown-run.json"
$nonDryRun = Get-Content -LiteralPath $nonDryJsonPath -Raw | ConvertFrom-Json
$nonDryResult = @($nonDryRun.results) | Select-Object -First 1
if ($nonDryRun.status -ne "PASS" -or $nonDryResult.status -ne "PASS") {
    throw "Fake non-dry row run did not pass."
}
if ($nonDryResult.rowGateProofMode -ne "viewer-phase-proof-pending-gate-state-audit") {
    throw "Fake non-dry row run falsely claimed exact gate-state proof."
}
if ($nonDryResult.rowRuntimeClassification -ne "loaded-pending-runtime" -or $nonDryResult.rowGateAudit.status -ne "PENDING") {
    throw "Fake non-dry row run did not classify missing gate evidence as loaded-pending-runtime."
}
if (@($nonDryResult.evidence) -notcontains "viewer-manifest-phase-angle-match" -or @($nonDryResult.evidence) -notcontains "selected-runtime-states") {
    throw "Fake non-dry row run did not preserve child manifest and selected runtime-state evidence."
}
if (@($nonDryResult.rowGateAudit.missingEvidenceKinds) -notcontains "mouth-runtime-evidence") {
    throw "Fake non-dry row run did not name missing mouth runtime evidence."
}
$childViewerManifest = Get-Content -LiteralPath ([string]$nonDryResult.viewerJson) -Raw | ConvertFrom-Json
if (@($childViewerManifest.phases) -notcontains "talk" -or @($childViewerManifest.angles) -notcontains "front-left" -or [string]$childViewerManifest.actorKitDialogueMode -ne "mouth-open-pose") {
    throw "Fake non-dry child viewer manifest did not receive selected phase/angles/dialogue mode."
}

$exactViewer = Join-Path $FixtureDir "fake-exact-run-fnv-character-viewer.ps1"
@'
param(
    [string]$ProofRoot,
    [string[]]$Targets,
    [string]$ActorKind,
    [string[]]$Phases,
    [string[]]$Angles,
    [int]$RunSeconds,
    [int]$ActorFrame,
    [string]$ScreenshotFrames,
    [string]$ActorKitAnimationGroup = "",
    [string]$ActorKitDialogueMode = "",
    [switch]$CreatureDiagnostics,
    [switch]$NoSound,
    [switch]$Serve,
    [int]$ServePort = 0
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
$runDir = Join-Path $ProofRoot "fnv-character-viewer/fake-exact-$stamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$viewerHtml = Join-Path $runDir "character-viewer.html"
$viewerJson = Join-Path $runDir "character-viewer-manifest.json"
$actorKitJson = Join-Path $runDir "character-actor-kit.json"
$cases = @()
foreach ($phase in @($Phases)) {
    foreach ($angle in @($Angles)) {
        $cases += [pscustomobject][ordered]@{
            case = "$($phase)_$($angle)"
            phase = $phase
            angle = $angle
            runtimeGateStatus = "PASS"
            reportStatus = "PASS"
            actorKitSelection = [pscustomobject][ordered]@{
                animationGroup = $ActorKitAnimationGroup
                dialogueMode = $ActorKitDialogueMode
            }
            screenshots = @([pscustomobject][ordered]@{ name = "$($phase)_$($angle).png"; path = "$($phase)_$($angle).png" })
            faceDrawables = @([pscustomobject][ordered]@{ drawable = "faceMouth"; model = "generated/contract-mouth.mesh"; texture = "generated/contract-mouth.dds" })
            materialEvidence = @()
            morphLines = @("FNV/ESM4 proof: mouth driver active dialogue pose for selected actor")
            weaponLines = @()
            animationPlayback = @()
            creatureEvidence = @()
        }
    }
}
"<html><body>fake exact viewer</body></html>" | Set-Content -LiteralPath $viewerHtml -Encoding UTF8
[pscustomobject][ordered]@{
    overallStatus = "PASS"
    target = @($Targets)[0]
    actorKind = $ActorKind
    phases = @($Phases)
    angles = @($Angles)
    actorKitDialogueMode = $ActorKitDialogueMode
    cases = @($cases)
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $viewerJson -Encoding UTF8
[pscustomobject][ordered]@{
    schema = "fake-actor-kit-v1"
    target = @($Targets)[0]
    phases = @($Phases)
    angles = @($Angles)
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $actorKitJson -Encoding UTF8
$runs = @([pscustomobject][ordered]@{
    Target = @($Targets)[0]
    Status = "PASS"
    SuiteDir = $runDir
    ViewerHtml = $viewerHtml
    ViewerJson = $viewerJson
    ActorKitJson = $actorKitJson
})
ConvertTo-Json -InputObject $runs -Depth 8 | Set-Content -LiteralPath (Join-Path $runDir "viewer-runs.json") -Encoding UTF8
"<html><body>fake exact run index</body></html>" | Set-Content -LiteralPath (Join-Path $runDir "index.html") -Encoding UTF8
'@ | Set-Content -LiteralPath $exactViewer -Encoding UTF8

$beforeExactDirs = @()
if (Test-Path -LiteralPath $runRoot -PathType Container) {
    $beforeExactDirs = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}
& $Runner -ProofRoot $ProofRoot -BurnDownJson $jsonPath -RunRows -ActorKind "npc" -Target "GSEasyPete" -Priority "easy-pete" -Classification "loaded-pending-runtime" -Phase "talk" -Gate "voice-lip-sidecar" -MaxRows 1 -ViewerRunner $exactViewer -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down exact fake row fixture failed with exit code $LASTEXITCODE."
}
$exactRunDir = Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Where-Object { $beforeExactDirs -notcontains $_.FullName } |
    Select-Object -First 1
if ($null -eq $exactRunDir) {
    throw "Exact fake row fixture did not create a burn-down run directory."
}
$exactJsonPath = Join-Path $exactRunDir.FullName "actor-parity-burndown-run.json"
$exactRun = Get-Content -LiteralPath $exactJsonPath -Raw | ConvertFrom-Json
$exactResult = @($exactRun.results) | Select-Object -First 1
if ($exactResult.rowGateProofMode -ne "viewer-gate-state-runtime-supported") {
    throw "Exact fake row run did not promote gate/state proof mode."
}
if ($exactResult.rowRuntimeClassification -ne "runtime-supported" -or $exactResult.rowGateAudit.status -ne "PASS") {
    throw "Exact fake row run did not classify row as runtime-supported."
}
if (@($exactResult.rowGateAudit.missingEvidenceKinds).Count -ne 0 -or @($exactResult.rowGateAudit.observedEvidenceKinds) -notcontains "mouth-runtime-evidence") {
    throw "Exact fake row audit did not consume the expected mouth runtime evidence."
}

$emptySelectionFailed = $false
try {
    & $Runner -ProofRoot $ProofRoot -BurnDownJson $jsonPath -DryRun -Target "DefinitelyMissingActorForContract" -MaxRows 1 | Out-Host
}
catch {
    $emptySelectionFailed = $true
}
if (!$emptySelectionFailed) {
    throw "Empty dry-run row selection did not fail."
}

$badBurn = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
$badBurn.status = "PASS"
$badBurn.rows[0].classification = ""
$badBurn.counts.unclassified = 1
$badBurnPath = Join-Path $FixtureDir "bad-actor-parity-burndown.json"
$badBurn | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $badBurnPath -Encoding UTF8
$badBurnFailed = $false
try {
    & $Runner -ProofRoot $ProofRoot -BurnDownJson $badBurnPath -DryRun -MaxRows 1 | Out-Host
}
catch {
    $badBurnFailed = $true
}
if (!$badBurnFailed) {
    throw "Malformed burn-down matrix did not fail before row dry-run."
}

$invalidAngleFailed = $false
try {
    & $Runner -ProofRoot $ProofRoot -BurnDownJson $jsonPath -DryRun -Angles "left" -MaxRows 1 | Out-Host
}
catch {
    $invalidAngleFailed = $true
}
if (!$invalidAngleFailed) {
    throw "Invalid dry-run camera angle did not fail."
}

Write-Host "FNV actor parity burn-down contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "BurnDownJson: $jsonPath"
Write-Host "BurnDownMarkdown: $markdownPath"
Write-Host "BurnDownCsv: $csvPath"
Write-Host "BurnDownRunJson: $runJsonPath"
Write-Host "BurnDownFakeNonDryRunJson: $nonDryJsonPath"
Write-Host "BurnDownExactFakeRunJson: $exactJsonPath"
