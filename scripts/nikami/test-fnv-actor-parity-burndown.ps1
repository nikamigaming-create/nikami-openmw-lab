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

Write-Host "FNV actor parity burn-down contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "BurnDownJson: $jsonPath"
Write-Host "BurnDownMarkdown: $markdownPath"
Write-Host "BurnDownCsv: $csvPath"
