param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 22,
    [int]$GuiFrame = 180,
    [string]$HudScreenshotFrames = "760",
    [string]$MenuScreenshotFrames = "320",
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$SweepStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$SweepDir = Join-Path $ProofRoot "fnv-ui-baseline-proof/$SweepStamp"
New-Item -ItemType Directory -Force -Path $SweepDir | Out-Null

$passes = @(
    @{ Name = "hud"; GuiMode = ""; StageActor = $true; ActorStageRotZ = 1.5708; ScreenshotFrames = $HudScreenshotFrames },
    @{ Name = "status"; GuiMode = "status"; StageActor = $false; ScreenshotFrames = $MenuScreenshotFrames },
    @{ Name = "items"; GuiMode = "items"; StageActor = $false; ScreenshotFrames = $MenuScreenshotFrames },
    @{ Name = "map"; GuiMode = "map"; StageActor = $false; ScreenshotFrames = $MenuScreenshotFrames },
    @{ Name = "data"; GuiMode = "data"; StageActor = $false; ScreenshotFrames = $MenuScreenshotFrames }
)

foreach ($pass in $passes) {
    $before = @(Get-ChildItem -LiteralPath (Join-Path $ProofRoot "fnv-flat-proof") -Directory -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName)

    $proofArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        RunSeconds = $RunSeconds
        ScreenshotFrames = [string]$pass.ScreenshotFrames
        BootstrapCell = "FormId:0x10daeb9"
        BootstrapX = -67480
        BootstrapY = 1500
        BootstrapZ = 8425
        BootstrapRotX = 0
        BootstrapRotZ = 1.5708
    }
    if ($pass.StageActor) {
        $proofArgs.ActorTarget = "GSEasyPete"
        $proofArgs.StageActor = $true
        $proofArgs.ActorFrame = 620
        $proofArgs.ActorStageX = -67480
        $proofArgs.ActorStageY = 1500
        $proofArgs.ActorStageZ = 8425
        $proofArgs.ActorStageRotZ = [double]$pass.ActorStageRotZ
        $proofArgs.ActorViewOffsetX = 18
        $proofArgs.ActorViewOffsetY = 0
        $proofArgs.ActorViewOffsetZ = 70
        $proofArgs.ActorViewTargetZ = 78
    }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if (![string]::IsNullOrWhiteSpace($pass.GuiMode)) {
        $proofArgs.GuiMode = $pass.GuiMode
        $proofArgs.GuiFrame = $GuiFrame
    }
    if ($NoSound) { $proofArgs.NoSound = $true }

    & $FlatProof @proofArgs | Out-Host

    $latest = Get-ChildItem -LiteralPath (Join-Path $ProofRoot "fnv-flat-proof") -Directory |
        Where-Object { $before -notcontains $_.FullName } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "Unable to find proof directory for pass $($pass.Name)"
    }

    $shot = Get-ChildItem -LiteralPath $latest.FullName -Filter "*.png" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $shot) {
        throw "No screenshot found in $($latest.FullName)"
    }

    Copy-Item -LiteralPath $shot.FullName -Destination (Join-Path $SweepDir "$($pass.Name).png") -Force
    Copy-Item -LiteralPath (Join-Path $latest.FullName "summary.txt") -Destination (Join-Path $SweepDir "$($pass.Name)_summary.txt") -Force
    Copy-Item -LiteralPath (Join-Path $latest.FullName "openmw.log") -Destination (Join-Path $SweepDir "$($pass.Name)_openmw.log") -Force
}

Write-Host "FNV UI baseline proof:"
Write-Host "  $SweepDir"
Get-ChildItem -LiteralPath $SweepDir -Filter "*.png" | Select-Object FullName, Length

& (Join-Path $PSScriptRoot "test-fnv-ui-baseline-proof.ps1") -ProofDir $SweepDir
