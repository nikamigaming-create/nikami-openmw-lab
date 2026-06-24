param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$ItemTarget = "",
    [string]$ItemKind = "",
    [string]$ItemRecordType = "",
    [string]$ItemFormId = "",
    [string]$ItemPlugin = "",
    [string]$ItemModel = "",
    [string[]]$Angles = @("front", "front-left", "front-right"),
    [int]$RunSeconds = 35,
    [int]$ItemFrame = 1,
    [string]$ScreenshotFrames = "420",
    [string]$StartCell = "Goodsprings",
    [string]$BootstrapCell = "FormId:0x10daeb9",
    [double]$BootstrapX = -67480,
    [double]$BootstrapY = 1500,
    [double]$BootstrapZ = 8425,
    [double]$BootstrapRotX = 0,
    [double]$BootstrapRotY = 0,
    [double]$BootstrapRotZ = 1.5708,
    [double]$BootstrapHour = 10,
    [double]$ItemStageX = 0,
    [double]$ItemStageY = 0,
    [double]$ItemStageZ = 0,
    [double]$ItemStageRotX = 0,
    [double]$ItemStageRotY = 0,
    [double]$ItemStageRotZ = 1.5708,
    [double]$ItemStageScale = 1.0,
    [double]$ItemViewDistance = 140,
    [double]$ItemViewOffsetZ = [double]::NaN,
    [double]$ItemViewTargetZ = [double]::NaN,
    [string]$OutDir = "",
    [switch]$NoSound,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($ItemTarget)) {
    $ItemTarget = $ItemFormId
}
if ([string]::IsNullOrWhiteSpace($ItemTarget)) {
    throw "Set -ItemTarget or -ItemFormId for the item viewer."
}
if ([string]::IsNullOrWhiteSpace($ItemModel)) {
    throw "Set -ItemModel. The first item viewer slice only supports model-backed visual spawn proof."
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
if (!(Test-Path -LiteralPath $FlatProof -PathType Leaf)) {
    throw "Missing flat proof runner: $FlatProof"
}

function ConvertTo-SafeName([string]$Value) {
    $safe = $Value -replace '[^A-Za-z0-9_.-]+', '_'
    if ([string]::IsNullOrWhiteSpace($safe)) { return "item" }
    return $safe.Trim("_")
}

function ConvertTo-HtmlText([string]$Value) {
    return [System.Net.WebUtility]::HtmlEncode($Value)
}

function Normalize-List([string[]]$Values, [string]$Name) {
    $items = New-Object "System.Collections.Generic.List[string]"
    foreach ($value in $Values) {
        foreach ($part in ($value -split ",")) {
            $trimmed = $part.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed)) {
                $items.Add($trimmed)
            }
        }
    }
    if ($items.Count -eq 0) { throw "No $Name selected." }
    return @($items)
}

function Get-ProofDirectories {
    $base = Join-Path $ProofRoot "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $base -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}

function Get-NewProofDirectory([string[]]$Before, [datetime]$StartedAt) {
    $base = Join-Path $ProofRoot "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $base -PathType Container)) { return $null }
    $beforeSet = New-Object "System.Collections.Generic.HashSet[string]"
    foreach ($path in $Before) {
        if (![string]::IsNullOrWhiteSpace($path)) { $null = $beforeSet.Add($path) }
    }
    $candidate = Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Where-Object { !$beforeSet.Contains($_.FullName) -and $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $candidate) { return $null }
    return $candidate.FullName
}

function Copy-IfPresent([string]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        return $true
    }
    return $false
}

function New-ViewerHtml([string]$Path, [object[]]$Cases, [pscustomobject]$Manifest) {
    $caseCards = foreach ($case in $Cases) {
        $screens = foreach ($shot in @($case.screenshots)) {
            $name = ConvertTo-HtmlText ([string]$shot.name)
            "<img src=`"$name`" alt=`"$name`">"
        }
        $statusClass = if ($case.runtimeGateStatus -eq "PASS") { "pass" } else { "fail" }
        @"
<section class="card $statusClass">
  <h2>$((ConvertTo-HtmlText $case.angle)) <span>$((ConvertTo-HtmlText $case.runtimeGateStatus))</span></h2>
  <div class="shots">$($screens -join "`n")</div>
  <pre>$((ConvertTo-HtmlText ($case | ConvertTo-Json -Depth 8)))</pre>
</section>
"@
    }
    $html = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>FNV Item Viewer - $((ConvertTo-HtmlText $Manifest.item.target))</title>
<style>
body { margin: 0; font-family: Arial, sans-serif; background: #111; color: #eee; }
header { padding: 14px 18px; border-bottom: 1px solid #333; background: #171717; }
h1 { margin: 0 0 8px; font-size: 20px; }
.meta { display: flex; flex-wrap: wrap; gap: 8px; color: #bbb; font-size: 13px; }
main { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 12px; padding: 12px; }
.card { border: 1px solid #333; border-radius: 6px; background: #181818; padding: 10px; min-width: 0; }
.card.pass { border-color: #2c6b42; }
.card.fail { border-color: #8a3b3b; }
.card h2 { display: flex; justify-content: space-between; gap: 8px; margin: 0 0 8px; font-size: 16px; }
.shots { display: grid; gap: 8px; }
img { width: 100%; background: #050505; border: 1px solid #333; }
pre { max-height: 240px; overflow: auto; white-space: pre-wrap; background: #0b0b0b; border: 1px solid #2c2c2c; padding: 8px; font-size: 12px; }
</style>
</head>
<body>
<header>
  <h1>FNV Item Viewer</h1>
  <div class="meta">
    <span>target: $((ConvertTo-HtmlText $Manifest.item.target))</span>
    <span>kind: $((ConvertTo-HtmlText $Manifest.item.kind))</span>
    <span>record: $((ConvertTo-HtmlText $Manifest.item.recordType))</span>
    <span>model: $((ConvertTo-HtmlText $Manifest.item.model))</span>
    <span>status: $((ConvertTo-HtmlText $Manifest.overallStatus))</span>
  </div>
</header>
<main>
$($caseCards -join "`n")
</main>
</body>
</html>
"@
    $html | Set-Content -LiteralPath $Path -Encoding UTF8
}

$Angles = Normalize-List $Angles "angles"
$diagonal = $ItemViewDistance * 0.7071067811865476
$AllAngles = @(
    [pscustomobject]@{ Name = "front"; OffsetX = $ItemViewDistance; OffsetY = 0.0 },
    [pscustomobject]@{ Name = "front-left"; OffsetX = $diagonal; OffsetY = -$diagonal },
    [pscustomobject]@{ Name = "front-right"; OffsetX = $diagonal; OffsetY = $diagonal }
)
$KnownAngleNames = @($AllAngles | ForEach-Object { $_.Name })
$unknownAngles = @($Angles | Where-Object { $KnownAngleNames -notcontains $_ })
if ($unknownAngles.Count -gt 0) {
    throw "Unknown item viewer camera angle(s): $($unknownAngles -join ','). Valid angles: $($KnownAngleNames -join ',')"
}
$CameraAngles = @($AllAngles | Where-Object { $Angles -contains $_.Name })

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $root = Join-Path $ProofRoot "fnv-item-viewer"
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $OutDir = Join-Path $root (Get-Date -Format "yyyyMMdd_HHmmss")
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "FNV item viewer $((Split-Path $OutDir -Leaf))"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "OutDir: $OutDir"
Write-Host "ItemTarget: $ItemTarget"
Write-Host "ItemKind: $ItemKind"
Write-Host "ItemRecordType: $ItemRecordType"
Write-Host "ItemFormId: $ItemFormId"
Write-Host "ItemPlugin: $ItemPlugin"
Write-Host "ItemModel: $ItemModel"
Write-Host "Angles: $($CameraAngles.Name -join ',')"
Write-Host "Policy: generated proof/viewer output only; no retail assets are committed"

$Cases = New-Object "System.Collections.Generic.List[object]"
foreach ($angle in $CameraAngles) {
    $caseName = ConvertTo-SafeName $angle.Name
    $caseDir = Join-Path $OutDir $caseName
    New-Item -ItemType Directory -Force -Path $caseDir | Out-Null
    $before = @(Get-ProofDirectories)
    $startedAt = Get-Date
    $runtimeGateStatus = "PASS"
    $runtimeGateError = ""
    $proofArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        StartCell = $StartCell
        RunSeconds = $RunSeconds
        ScreenshotFrames = $ScreenshotFrames
        BootstrapCell = $BootstrapCell
        BootstrapX = $BootstrapX
        BootstrapY = $BootstrapY
        BootstrapZ = $BootstrapZ
        BootstrapRotX = $BootstrapRotX
        BootstrapRotY = $BootstrapRotY
        BootstrapRotZ = $BootstrapRotZ
        BootstrapHour = $BootstrapHour
        ProofItemTarget = $ItemTarget
        ProofItemKind = $ItemKind
        ProofItemRecordType = $ItemRecordType
        ProofItemFormId = $ItemFormId
        ProofItemPlugin = $ItemPlugin
        ProofItemModel = $ItemModel
        ProofItemFrame = $ItemFrame
        ProofItemStageX = $ItemStageX
        ProofItemStageY = $ItemStageY
        ProofItemStageZ = $ItemStageZ
        ProofItemStageRotX = $ItemStageRotX
        ProofItemStageRotY = $ItemStageRotY
        ProofItemStageRotZ = $ItemStageRotZ
        ProofItemStageScale = $ItemStageScale
        ProofItemViewOffsetX = [double]$angle.OffsetX
        ProofItemViewOffsetY = [double]$angle.OffsetY
        ProofItemViewOffsetZ = $ItemViewOffsetZ
        ProofItemViewTargetZ = $ItemViewTargetZ
        ProofItemViewCameraDistance = 0
        ProofItemViewLocalOffset = $true
        RequireLogPattern = @("FNV/ESM4 proof item model spawn", "FNV/ESM4 proof: aligned player camera to item")
    }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if ($NoSound) { $proofArgs.NoSound = $true }

    try {
        & $FlatProof @proofArgs | Out-Host
    }
    catch {
        $runtimeGateStatus = "FAIL"
        $runtimeGateError = $_.Exception.Message
        Write-Warning "Item viewer runtime gate failed for ${caseName}: $runtimeGateError"
    }

    $proofDir = Get-NewProofDirectory $before $startedAt
    if ([string]::IsNullOrWhiteSpace($proofDir)) {
        throw "Unable to find generated flat proof directory for item case $caseName"
    }
    Copy-IfPresent (Join-Path $proofDir "openmw.log") (Join-Path $caseDir "openmw.log") | Out-Null
    Copy-IfPresent (Join-Path $proofDir "summary.txt") (Join-Path $caseDir "flat-summary.txt") | Out-Null
    Copy-IfPresent (Join-Path $proofDir "screenshot-timing.json") (Join-Path $caseDir "screenshot-timing.json") | Out-Null
    Copy-IfPresent (Join-Path $proofDir "screenshot-stability.json") (Join-Path $caseDir "screenshot-stability.json") | Out-Null
    Copy-IfPresent (Join-Path $proofDir "fnv-data-provenance.json") (Join-Path $caseDir "fnv-data-provenance.json") | Out-Null
    $screenshots = @()
    Get-ChildItem -LiteralPath $proofDir -Filter "*.png" -File -ErrorAction SilentlyContinue | ForEach-Object {
        $dest = Join-Path $caseDir $_.Name
        Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
        $screenshots += [pscustomobject][ordered]@{
            name = "$caseName/$($_.Name)"
            path = $dest
            source = $_.FullName
        }
    }
    if ($screenshots.Count -eq 0) {
        $runtimeGateStatus = "FAIL"
        if ([string]::IsNullOrWhiteSpace($runtimeGateError)) { $runtimeGateError = "item viewer did not capture a screenshot" }
    }
    $case = [pscustomobject][ordered]@{
        case = $caseName
        angle = $angle.Name
        runtimeGateStatus = $runtimeGateStatus
        runtimeGateError = $runtimeGateError
        proofDir = $proofDir
        caseDir = $caseDir
        screenshots = $screenshots
        itemStage = [pscustomobject][ordered]@{
            x = $ItemStageX
            y = $ItemStageY
            z = $ItemStageZ
            rotX = $ItemStageRotX
            rotY = $ItemStageRotY
            rotZ = $ItemStageRotZ
            scale = $ItemStageScale
        }
        itemCamera = [pscustomobject][ordered]@{
            angle = $angle.Name
            offsetX = [double]$angle.OffsetX
            offsetY = [double]$angle.OffsetY
            offsetZ = $(if ([double]::IsNaN($ItemViewOffsetZ)) { $null } else { $ItemViewOffsetZ })
            targetZ = $(if ([double]::IsNaN($ItemViewTargetZ)) { $null } else { $ItemViewTargetZ })
            localOffset = $true
        }
        gates = @(
            [pscustomobject][ordered]@{
                gate = "runtime-visual-model-spawn"
                classification = $(if ($runtimeGateStatus -eq "PASS") { "runtime-supported" } else { "known-blocked" })
                proof = "PC-flat runtime loaded the item model path, staged it, aligned the camera, and captured screenshots."
            },
            [pscustomobject][ordered]@{
                gate = "inventory-equip-or-activate-behavior"
                classification = "loaded-pending-runtime"
                proof = "This visual spawn proof does not claim inventory, equip, activation, pickup, collision, or gameplay behavior."
            }
        )
    }
    $Cases.Add($case)
}

$overallStatus = if (@($Cases | Where-Object { $_.runtimeGateStatus -ne "PASS" }).Count -eq 0) { "PASS" } else { "FAIL" }
$manifest = [pscustomobject][ordered]@{
    schema = "nikami-fnv-item-viewer-manifest-v1"
    status = $overallStatus
    overallStatus = $overallStatus
    item = [pscustomobject][ordered]@{
        target = $ItemTarget
        kind = $ItemKind
        recordType = $ItemRecordType
        formId = $ItemFormId
        plugin = $ItemPlugin
        model = $ItemModel
    }
    bootstrap = [pscustomobject][ordered]@{
        startCell = $StartCell
        bootstrapCell = $BootstrapCell
        x = $BootstrapX
        y = $BootstrapY
        z = $BootstrapZ
        rotX = $BootstrapRotX
        rotY = $BootstrapRotY
        rotZ = $BootstrapRotZ
        hour = $BootstrapHour
    }
    cases = @($Cases.ToArray())
    gates = [pscustomobject][ordered]@{
        runtimeVisualModelSpawn = $overallStatus
        inventoryEquipOrActivateBehavior = "loaded-pending-runtime"
        collisionAndPickupRuntime = "loaded-pending-runtime"
    }
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailAssetsCommitted = $true
        noRetailPayloadBytes = $true
    }
}
$manifestPath = Join-Path $OutDir "item-viewer-manifest.json"
$htmlPath = Join-Path $OutDir "item-viewer.html"
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
New-ViewerHtml $htmlPath @($Cases.ToArray()) $manifest

Write-Host "viewer-html=$htmlPath"
Write-Host "viewer-json=$manifestPath"
Write-Host "status=$overallStatus cases=$($Cases.Count)"
Write-Host "generated proof/viewer output only; no retail assets are committed"

if ($RequirePass -and $overallStatus -ne "PASS") {
    throw "FNV item viewer failed. See $manifestPath"
}
