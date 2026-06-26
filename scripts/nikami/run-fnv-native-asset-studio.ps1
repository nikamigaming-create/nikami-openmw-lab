param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$AssetClass = "mesh",
    [string]$Record = "direct-model",
    [string]$Session = "native-session",
    [string]$Model = "meshes\armor\headgear\cowboyhat\cowboyhat.nif",
    [string]$View = "front",
    [string]$ActorProfile = "",
    [switch]$AllowPackageProcedureIdles,
    [double]$RotX = 0,
    [double]$RotY = 0,
    [double]$RotZ = 0,
    [double]$Scale = 1,
    [double]$Zoom = 1,
    [switch]$StartWorld,
    [switch]$Attached,
    [int]$RunSeconds = 0,
    [switch]$Sound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Resolve-FnvDataFromLatestHarvest([string]$ProofRoot) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) { return $null }

    $manifests = Get-ChildItem -LiteralPath $harvestRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "manifest.json" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }

    foreach ($manifestPath in $manifests) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            $candidate = [string]$manifest.fnvData
            if (![string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                return [pscustomobject][ordered]@{
                    FnvData = (Resolve-Path -LiteralPath $candidate).Path
                    Manifest = $manifestPath
                }
            }
        }
        catch {
        }
    }

    return $null
}

function Resolve-VcpkgRootFromKnownPaths([string]$RepoRoot) {
    $candidates = @(
        $env:NIKAMI_VCPKG_ROOT,
        "D:\code\c\FMODS\vcpkg",
        (Join-Path $RepoRoot "vcpkg"),
        (Join-Path (Split-Path $RepoRoot -Parent) "vcpkg")
    )

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $toolchain = Join-Path $candidate "scripts/buildsystems/vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return ""
}

function Set-ScopedEnv([hashtable]$Previous, [string]$Name, [string]$Value) {
    if (!$Previous.ContainsKey($Name)) {
        $Previous[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
    }
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Restore-ScopedEnv([hashtable]$Previous) {
    foreach ($name in $Previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $Previous[$name], "Process")
    }
}

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $harvestData = Resolve-FnvDataFromLatestHarvest $ProofRoot
    if ($null -ne $harvestData) {
        $FnvData = $harvestData.FnvData
        Write-Host "FnvData: latest generated harvest manifest $($harvestData.Manifest)"
    }
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Resolve-VcpkgRootFromKnownPaths $RepoRoot
}

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set NIKAMI_FNV_DATA, pass -FnvData, or generate a retail harvest under $ProofRoot\fnv-retail-harvest."
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Set NIKAMI_VCPKG_ROOT or pass -VcpkgRoot."
}

$FlatRunner = Join-Path $PSScriptRoot "run-fnv-flat.ps1"
if (!(Test-Path -LiteralPath $FlatRunner -PathType Leaf)) {
    throw "Missing FNV flat launcher: $FlatRunner"
}

$runtimeTag = "native-asset-studio"
$previousEnv = @{}

try {
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO" "1"
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_ASSET_CLASS" $AssetClass
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_RECORD" $Record
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_SESSION" $Session
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_MODEL" $Model
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_VIEW" $View
    if (![string]::IsNullOrWhiteSpace($ActorProfile)) {
        Set-ScopedEnv $previousEnv "OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE" $ActorProfile
    }
    if (!$AllowPackageProcedureIdles) {
        Set-ScopedEnv $previousEnv "OPENMW_FNV_DISABLE_PACKAGE_PROCEDURE_IDLES" "1"
    }
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_ROT_X" ($RotX.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture))
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_ROT_Y" ($RotY.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture))
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_ROT_Z" ($RotZ.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture))
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_SCALE" ($Scale.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture))
    Set-ScopedEnv $previousEnv "OPENMW_FNV_ASSET_STUDIO_ZOOM" ($Zoom.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture))

    $flatArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        RuntimeTag = $runtimeTag
    }

    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $flatArgs.FnvConfigData = $FnvConfigData }
    if (!$StartWorld) { $flatArgs.WithMenu = $true }
    if ($RunSeconds -gt 0) { $flatArgs.MaxRunSeconds = $RunSeconds }
    elseif (!$Attached) { $flatArgs.Detached = $true }
    if (!$Sound) { $flatArgs.NoSound = $true }

    Write-Host "Native FNV Asset Studio"
    Write-Host "RepoRoot: $RepoRoot"
    Write-Host "FnvData: $FnvData"
    Write-Host "AssetClass: $AssetClass"
    Write-Host "Record: $Record"
    Write-Host "Session: $Session"
    Write-Host "Model: $Model"
    Write-Host "View: $View"
    Write-Host "ActorProfile: $ActorProfile"
    Write-Host "ActorStandingIdle: $(!$AllowPackageProcedureIdles)"
    Write-Host "Rotation: $RotX,$RotY,$RotZ"
    Write-Host "Scale: $Scale"
    Write-Host "Zoom: $Zoom"
    Write-Host "World start: $StartWorld"
    Write-Host "Policy: generated config/proof output only; no retail assets are committed"
    & $FlatRunner @flatArgs
}
finally {
    Restore-ScopedEnv $previousEnv
}
