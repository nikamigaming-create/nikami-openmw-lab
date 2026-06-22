param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$BsaTool = $env:NIKAMI_BSATOOL,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [switch]$SkipUi,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData) -and ![string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($BsaTool)) {
    $CandidateBsaTool = Join-Path $RepoRoot "build-clean/Release/bsatool.exe"
    if (Test-Path -LiteralPath $CandidateBsaTool) {
        $BsaTool = $CandidateBsaTool
    }
    elseif (Test-Path -LiteralPath "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe") {
        $BsaTool = "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe"
    }
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$SliceDir = Join-Path $ProofRoot "fnv-opening-vertical-slice/$Stamp"
$SummaryFile = Join-Path $SliceDir "summary.txt"
New-Item -ItemType Directory -Force -Path $SliceDir | Out-Null

function Write-SliceLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Invoke-Gate([string]$Name, [string]$ScriptPath, [hashtable]$ScriptArgs) {
    $logPath = Join-Path $SliceDir "$Name.log"
    Write-SliceLine ""
    Write-SliceLine "Gate: $Name"
    Write-SliceLine "Script: $ScriptPath"
    try {
        & $ScriptPath @ScriptArgs 2>&1 | Tee-Object -FilePath $logPath | Out-Host
        Write-SliceLine "PASS: $Name"
    }
    catch {
        Write-SliceLine "FAIL: $Name"
        Write-SliceLine "Log: $logPath"
        throw
    }
    Write-SliceLine "Log: $logPath"
    Start-Sleep -Seconds 2
}

$CommonProofArgs = @{
    ProofRoot = $ProofRoot
}
if (![string]::IsNullOrWhiteSpace($FnvRoot)) { $CommonProofArgs.FnvRoot = $FnvRoot }
if (![string]::IsNullOrWhiteSpace($FnvData)) { $CommonProofArgs.FnvData = $FnvData }

$InventoryArgs = $CommonProofArgs.Clone()
if (![string]::IsNullOrWhiteSpace($BsaTool)) { $InventoryArgs.BsaTool = $BsaTool }

$FlatArgs = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
    FnvData = $FnvData
    VcpkgRoot = $VcpkgRoot
    Triplet = $Triplet
    ProofRoot = $ProofRoot
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $FlatArgs.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $FlatArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $FlatArgs.NoSound = $true }

Write-SliceLine "FNV opening vertical slice $Stamp"
Write-SliceLine "RepoRoot: $RepoRoot"
Write-SliceLine "FnvData: $FnvData"
Write-SliceLine "ProofRoot: $ProofRoot"
Write-SliceLine "SliceDir: $SliceDir"
Write-SliceLine ""
Write-SliceLine "Target spine:"
Write-SliceLine "- data inventory"
Write-SliceLine "- movie/cemetery/grave/Doc Mitchell opening spine records"
Write-SliceLine "- Pip-Boy DATA source records"
Write-SliceLine "- intro/menu/loading movie runtime path"
Write-SliceLine "- Doc Mitchell house runtime proof"
Write-SliceLine "- Goodsprings world/collision sample"
Write-SliceLine "- Goodsprings queued walk replay"
Write-SliceLine "- HUD/Pip-Boy analog panes"

Invoke-Gate "01-data-inventory" (Join-Path $PSScriptRoot "test-fnv-data-inventory.ps1") $InventoryArgs
Invoke-Gate "02-movie-grave-doc-spine-data" (Join-Path $PSScriptRoot "test-fnv-opening-data.ps1") $CommonProofArgs
Invoke-Gate "03-pipboy-data-source-records" (Join-Path $PSScriptRoot "test-fnv-data-pane-records.ps1") $CommonProofArgs

$DocArgs = $FlatArgs.Clone()
$DocArgs.RunSeconds = 14
Invoke-Gate "04-doc-mitchell-house-runtime" (Join-Path $PSScriptRoot "run-fnv-opening-doc-proof.ps1") $DocArgs

$MenuArgs = $FlatArgs.Clone()
$MenuArgs.WithMenu = $true
$MenuArgs.RunSeconds = 16
$MenuArgs.ScreenshotFrames = "160"
$MenuArgs.RequireLogPattern = @(
    "FNV/ESM4 proof: bound 20 real FNV loading screen texture",
    "FNV/ESM4 diag: requested INI intro movie .* using installed loose FNV intro video/fnvintro.bik",
    "FNV/ESM4 proof: video playback opened requested=.*resolved=video/fnvintro.bik",
    "FNV/ESM4 proof: video texture ready video/fnvintro.bik"
)
Invoke-Gate "05-menu-movie-startup" (Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1") $MenuArgs

$WorldArgs = $FlatArgs.Clone()
$WorldArgs.RunSeconds = 24
$WorldArgs.ScreenshotFrames = "90,180,300"
$WorldArgs.TerrainProbePoints = "baseline=-67450,2600,8425;flag_guess=-67620,1780,8500"
$WorldArgs.TerrainProbeGrid = "saloon_sweep=-67800,800,-67100,2600,8425,350"
$WorldArgs.RequireTerrainProbeFullSupport = $true
Invoke-Gate "06-goodsprings-world-grid" (Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1") $WorldArgs

$WalkArgs = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
    FnvData = $FnvData
    VcpkgRoot = $VcpkgRoot
    Triplet = $Triplet
    ProofRoot = $ProofRoot
}
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $WalkArgs.FnvConfigData = $FnvConfigData }
if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $WalkArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
if ($NoSound) { $WalkArgs.NoSound = $true }
Invoke-Gate "07-goodsprings-walk-replay" (Join-Path $PSScriptRoot "run-fnv-goodsprings-walk-replay-proof.ps1") $WalkArgs

if (!$SkipUi) {
    $UiArgs = $FlatArgs.Clone()
    $UiArgs.RunSeconds = 24
    Invoke-Gate "08-hud-pipboy-baseline" (Join-Path $PSScriptRoot "run-fnv-ui-baseline-proof.ps1") $UiArgs
}
else {
    Write-SliceLine ""
    Write-SliceLine "SKIP: 08-hud-pipboy-baseline"
}

Write-SliceLine ""
Write-SliceLine "FNV opening vertical slice PASS"
Write-SliceLine "ProofDir: $SliceDir"
