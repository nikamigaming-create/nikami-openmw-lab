param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 18,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Write-Host "Movable static physics classification lines: removed from runtime; generic ESM4 object collision is now the gate."
Write-Host "captured removed MSTT collision surgery: proof fails if old MSTT/effect/tumbleweed skip anchors return."

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
    TerrainProbePoints = "goodsprings_collision=-67450,2600,8425"
    ScreenshotFrames = "220"
    RequireFlatCameraSettled = $true
    RequirePlayerTerrainSupport = $true
    RequireTerrainProbeFullSupport = $true
    RequireLogPattern = @(
        "FNV/ESM4 proof: moved player to proof cell",
        "Nikami FNV named terrain probe: label=goodsprings_collision"
    )
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $args.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $args.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $args.NoSound = $true }

& $FlatProof @args
