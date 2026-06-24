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
    BootstrapX = 2243
    BootstrapY = 2276
    BootstrapZ = 7360
    BootstrapRotX = 0
    BootstrapRotZ = 0
    BootstrapCameraDistance = 0
    ActorTarget = "DocMitchell"
    ActorFrame = 120
    ActorViewOffsetX = 96
    ActorViewOffsetY = 0
    ActorViewOffsetZ = 96
    ActorViewTargetZ = 92
    ActorViewLocalOffset = $true
    FnvPartMatrixAudit = $true
    ScreenshotFrames = "170"
    RequireLogPattern = @(
        "FNV/ESM4 proof: moved player to proof cell.*request=`"GSDocMitchellHouse`".*type=interior",
        "FNV/ESM4 FACE CHECK DocMitchell:.*head=OK.*leftEye=OK.*rightEye=OK.*eyeTexture=OK.*hairAttached=OK",
        "FNV/ESM4 diag: actor controller audit result .*matched=[1-9][0-9]* missing=0",
        "FNV/ESM4 diag: play matched FormId:0x1104c0c group 'idle'.*controllers=[1-9][0-9]*.*playing=1",
        "FNV/ESM4 proof: active-cell actor match target=`"DocMitchell`""
    )
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $args.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $args.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $args.NoSound = $true }

& $FlatProof @args
