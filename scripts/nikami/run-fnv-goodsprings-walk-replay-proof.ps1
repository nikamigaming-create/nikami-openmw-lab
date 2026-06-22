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
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$args = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
    FnvData = $FnvData
    VcpkgRoot = $VcpkgRoot
    Triplet = $Triplet
    ProofRoot = $ProofRoot
    RunSeconds = $RunSeconds
    BootstrapCell = "FormId:0x10daeb9"
    BootstrapX = -67450
    BootstrapY = 2600
    BootstrapZ = 8425
    BootstrapRotX = 0
    BootstrapRotZ = -1.2
    TerrainProbePoints = "walk_start=-67450,2600,8425;walk_end=-67620,2357,8386"
    WalkEndX = -67620
    WalkEndY = 2357
    WalkEndZ = 8386
    WalkStartFrame = 160
    WalkEndFrame = 620
    WalkSpeed = 180
    WalkReachRadius = 96
    WalkMinZ = 8150
    ScreenshotFrames = "180,360,620"
    RequireFlatCameraSettled = $true
    RequirePlayerTerrainSupport = $true
    RequireTerrainProbeFullSupport = $true
    RequireLogPattern = @(
        "FNV/ESM4 proof walk: start",
        "FNV/ESM4 proof walk: summary reached=1 dropped=0"
    )
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $args.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $args.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $args.NoSound = $true }

& $FlatProof @args
