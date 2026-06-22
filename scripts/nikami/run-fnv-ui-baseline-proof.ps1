param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 24,
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
    ProofGuiMode = "data"
    ProofGuiFrame = 220
    ScreenshotFrames = "260,420"
    RequireFlatCameraSettled = $true
    RequireLogPattern = @(
        "FNV/ESM4 proof: flat Fallout HUD readouts active HP/AP/AMMO/text compass",
        "FNV/ESM4 proof: pushed inventory GUI mode page=`"data`"",
        "FNV/ESM4 proof: DATA pane source-backed alternate ammo active; quests/notes/radio/perks runtime binding pending"
    )
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $args.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $args.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $args.NoSound = $true }

& $FlatProof @args
