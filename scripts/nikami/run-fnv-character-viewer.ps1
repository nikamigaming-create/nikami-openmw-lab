param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string[]]$Targets = @("GSEasyPete"),
    [string[]]$Phases = @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "760",
    [double]$ActorViewDistance = 52,
    [double]$ActorViewOffsetZ = 108,
    [double]$ActorViewTargetZ = 108,
    [string]$SuiteDir = "",
    [switch]$NoRun,
    [switch]$NoSound,
    [switch]$OpenViewer
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$BuilderRunner = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
$BundleScript = Join-Path $PSScriptRoot "fnv_character_viewer_bundle.py"
if (!(Test-Path -LiteralPath $BuilderRunner)) { throw "Missing character builder tester: $BuilderRunner" }
if (!(Test-Path -LiteralPath $BundleScript)) { throw "Missing character viewer bundle generator: $BundleScript" }

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

function Get-SuiteDirectories {
    $base = Join-Path $ProofRoot "fnv-character-builder"
    if (!(Test-Path -LiteralPath $base)) { return @() }
    return @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}

function Get-NewSuiteDirectory([string[]]$Before, [datetime]$StartedAt) {
    $base = Join-Path $ProofRoot "fnv-character-builder"
    if (!(Test-Path -LiteralPath $base)) { return $null }

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

function ConvertTo-HtmlText([string]$Value) {
    return [System.Net.WebUtility]::HtmlEncode($Value)
}

function ConvertTo-RelativeHref([string]$BaseDirectory, [string]$TargetPath) {
    $basePath = (Resolve-Path -LiteralPath $BaseDirectory).Path
    if (!$basePath.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $basePath += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetResolved = (Resolve-Path -LiteralPath $TargetPath).Path
    $relative = ([uri]$basePath).MakeRelativeUri([uri]$targetResolved).ToString()
    return [uri]::UnescapeDataString($relative)
}

function Write-ViewerIndex([string]$IndexPath, [object[]]$Runs) {
    $lines = New-Object "System.Collections.Generic.List[string]"
    $lines.Add("<!doctype html>")
    $lines.Add("<html lang=`"en`"><head><meta charset=`"utf-8`"><meta name=`"viewport`" content=`"width=device-width, initial-scale=1`">")
    $lines.Add("<title>FNV Character Viewer Runs</title>")
    $lines.Add("<style>body{margin:0;background:#111316;color:#eceff3;font:14px Segoe UI,Arial,sans-serif}main{padding:16px}a{color:#9fc2ff}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #363c45;padding:8px;text-align:left}.PASS{color:#64d488}.FAIL{color:#ff6f61}.MISSING{color:#e8c86a}</style>")
    $lines.Add("</head><body><main><h1>FNV Character Viewer Runs</h1><table><thead><tr><th>Target</th><th>Status</th><th>Viewer</th><th>Manifest</th><th>Suite</th></tr></thead><tbody>")
    $baseDirectory = Split-Path $IndexPath -Parent
    foreach ($run in $Runs) {
        $viewer = ConvertTo-HtmlText $run.ViewerHtml
        $manifest = ConvertTo-HtmlText $run.ViewerJson
        $suite = ConvertTo-HtmlText $run.SuiteDir
        $target = ConvertTo-HtmlText $run.Target
        $status = ConvertTo-HtmlText $run.Status
        $viewerRel = ConvertTo-HtmlText (ConvertTo-RelativeHref $baseDirectory $run.ViewerHtml)
        $manifestRel = ConvertTo-HtmlText (ConvertTo-RelativeHref $baseDirectory $run.ViewerJson)
        $lines.Add("<tr><td>$target</td><td class=`"$status`">$status</td><td><a href=`"$viewerRel`">$viewer</a></td><td><a href=`"$manifestRel`">$manifest</a></td><td>$suite</td></tr>")
    }
    $lines.Add("</tbody></table></main></body></html>")
    $lines | Set-Content -LiteralPath $IndexPath -Encoding UTF8
}

$Targets = Normalize-List $Targets "targets"
$Phases = Normalize-List $Phases "phases"
$ViewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
New-Item -ItemType Directory -Force -Path $ViewerRoot | Out-Null
$RunStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ViewerRoot $RunStamp
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

Write-Host "FNV standalone character viewer run $RunStamp"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "RunDir: $RunDir"
Write-Host "Targets: $($Targets -join ',')"
Write-Host "Phases: $($Phases -join ',')"
Write-Host "Policy: generated proof/viewer output only; no retail assets are committed"

$Runs = New-Object "System.Collections.Generic.List[object]"

if ($NoRun -or ![string]::IsNullOrWhiteSpace($SuiteDir)) {
    if ([string]::IsNullOrWhiteSpace($SuiteDir)) {
        throw "-NoRun requires -SuiteDir."
    }
    $resolvedSuite = (Resolve-Path -LiteralPath $SuiteDir).Path
    $targetName = $Targets[0]
    $viewerHtml = Join-Path $resolvedSuite "character-viewer.html"
    $viewerJson = Join-Path $resolvedSuite "character-viewer-manifest.json"
    & python $BundleScript --suite-dir $resolvedSuite --out-html $viewerHtml --out-json $viewerJson | Out-Host
    $manifest = Get-Content -LiteralPath $viewerJson -Raw | ConvertFrom-Json
    $Runs.Add([pscustomobject][ordered]@{
        Target = $targetName
        Status = $manifest.overallStatus
        SuiteDir = $resolvedSuite
        ViewerHtml = $viewerHtml
        ViewerJson = $viewerJson
    })
}
else {
    foreach ($target in $Targets) {
        Write-Host ""
        Write-Host "TARGET $target"
        $before = @(Get-SuiteDirectories)
        $startedAt = Get-Date

        $builderArgs = @{
            BuildDir = $BuildDir
            Configuration = $Configuration
            FnvData = $FnvData
            VcpkgRoot = $VcpkgRoot
            Triplet = $Triplet
            ProofRoot = $ProofRoot
            ActorTarget = $target
            Phases = $Phases
            RunSeconds = $RunSeconds
            ActorFrame = $ActorFrame
            ScreenshotFrames = $ScreenshotFrames
            ActorViewDistance = $ActorViewDistance
            ActorViewOffsetZ = $ActorViewOffsetZ
            ActorViewTargetZ = $ActorViewTargetZ
        }
        if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $builderArgs.FnvConfigData = $FnvConfigData }
        if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $builderArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
        if ($NoSound) { $builderArgs.NoSound = $true }

        & $BuilderRunner @builderArgs | Out-Host
        $newSuite = Get-NewSuiteDirectory $before $startedAt
        if ([string]::IsNullOrWhiteSpace($newSuite)) {
            throw "Unable to find generated character builder suite for $target."
        }

        $viewerHtml = Join-Path $newSuite "character-viewer.html"
        $viewerJson = Join-Path $newSuite "character-viewer-manifest.json"
        & python $BundleScript --suite-dir $newSuite --out-html $viewerHtml --out-json $viewerJson | Out-Host
        $manifest = Get-Content -LiteralPath $viewerJson -Raw | ConvertFrom-Json
        $Runs.Add([pscustomobject][ordered]@{
            Target = $target
            Status = $manifest.overallStatus
            SuiteDir = $newSuite
            ViewerHtml = $viewerHtml
            ViewerJson = $viewerJson
        })
    }
}

$RunsJson = Join-Path $RunDir "viewer-runs.json"
$Runs | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $RunsJson -Encoding UTF8
$IndexHtml = Join-Path $RunDir "index.html"
Write-ViewerIndex $IndexHtml @($Runs.ToArray())

Write-Host ""
Write-Host "Viewer index: $IndexHtml"
foreach ($run in $Runs) {
    Write-Host "Viewer for $($run.Target): $($run.ViewerHtml)"
    Write-Host "Manifest for $($run.Target): $($run.ViewerJson)"
}

if ($OpenViewer) {
    Invoke-Item -LiteralPath $IndexHtml
}
