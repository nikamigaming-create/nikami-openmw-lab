param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 14,
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
    BootstrapCell = "GSDocMitchellHouse"
    BootstrapX = 0
    BootstrapY = 0
    BootstrapZ = 128
    BootstrapRotX = 0
    BootstrapRotZ = 0
    ActorTarget = "DocMitchellREF"
    ActorFrame = 180
    ActorViewOffsetX = 96
    ActorViewOffsetY = 0
    ActorViewOffsetZ = 96
    ActorViewTargetZ = 92
    ScreenshotFrames = "220"
    RequireFlatCameraSettled = $true
    RequirePlayerTerrainSupport = $true
    RequireLogPattern = @(
        "FNV/ESM4 proof: moved player to proof cell.*request=`"GSDocMitchellHouse`".*type=interior",
        "Nikami FNV interior floor probe:",
        "FNV/ESM4 proof: active-cell actor match target=`"DocMitchellREF`""
    )
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $args.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $args.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $args.NoSound = $true }

& $FlatProof @args
