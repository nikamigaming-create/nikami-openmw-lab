param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$MorrowindData = $env:NIKAMI_MORROWIND_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$BuildPath = Join-Path $RepoRoot $BuildDir

if ([string]::IsNullOrWhiteSpace($MorrowindData)) {
    throw "Set NIKAMI_MORROWIND_DATA or pass -MorrowindData."
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Set NIKAMI_VCPKG_ROOT or pass -VcpkgRoot."
}

if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Exe = Join-Path $BuildPath "$Configuration/openmw.exe"
$Resources = Join-Path $BuildPath "$Configuration/resources"
$BuildConfig = Join-Path $BuildPath "$Configuration/openmw.cfg"
$ConfigDir = Join-Path $ProofRoot "configs/morrowind-flat-clean"
$RuntimeDir = Join-Path $ProofRoot "runtime/morrowind-flat-clean"
$DataLocalDir = Join-Path $RuntimeDir "data-local"
$ConfigPath = Join-Path $ConfigDir "openmw.cfg"

if (!(Test-Path -LiteralPath $Exe)) {
    throw "Missing OpenMW executable. Run scripts/nikami/build-clean-openmw.ps1 first: $Exe"
}

if (!(Test-Path -LiteralPath $Resources)) {
    throw "Missing OpenMW resources directory: $Resources"
}

if (!(Test-Path -LiteralPath $BuildConfig)) {
    throw "Missing generated OpenMW config: $BuildConfig"
}

if (!(Test-Path -LiteralPath $MorrowindData)) {
    throw "Missing Morrowind data directory: $MorrowindData"
}

$RuntimeDllDirs = @(
    (Join-Path $VcpkgRoot "installed/$Triplet/bin"),
    (Join-Path $VcpkgRoot "installed/$Triplet/bin/$Configuration"),
    (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin"),
    (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin/$Configuration")
) | Where-Object { Test-Path -LiteralPath $_ }

$env:Path = ($RuntimeDllDirs + $env:Path) -join ";"

if ($Configuration -eq "Debug") {
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/debug/plugins/osgPlugins-3.6.5"
} else {
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/plugins/osgPlugins-3.6.5"
}

$OutputOsgPluginDir = Join-Path $BuildPath "$Configuration/osgPlugins-3.6.5"
$OsgPluginDirs = @($OutputOsgPluginDir, $OsgPluginDir, $ExtraOsgPluginDir) | Where-Object {
    ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_)
}
$env:OSG_LIBRARY_PATH = $OsgPluginDirs -join ";"

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataLocalDir | Out-Null

$GeneratedState = @(
    "console_history.txt",
    "global_storage.bin",
    "input_v3.xml",
    "MyGUI.log",
    "openmw.log",
    "player_storage.bin",
    "settings.cfg",
    "shaders.yaml"
)

foreach ($Name in $GeneratedState) {
    $Path = Join-Path $ConfigDir $Name
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
}

$FallbackLines = Get-Content -LiteralPath $BuildConfig | Where-Object { $_ -like "fallback=*" }
$FallbackText = $FallbackLines -join "`r`n"

$ConfigText = @"
replace=config
replace=data-local
replace=data
replace=fallback
replace=fallback-archive
replace=content

resources=$($Resources.Replace("\", "/"))
user-data=$($RuntimeDir.Replace("\", "/"))
data-local=$($DataLocalDir.Replace("\", "/"))
data=$((Join-Path $Resources "vfs-mw").Replace("\", "/"))
data=$($MorrowindData.Replace("\", "/"))

$FallbackText

fallback-archive=Morrowind.bsa
fallback-archive=Tribunal.bsa
fallback-archive=Bloodmoon.bsa

content=Morrowind.esm
content=Tribunal.esm
content=Bloodmoon.esm

encoding=win1252

[Navigator]
enable=false

[Video]
resolution x=1280
resolution y=720
fullscreen=false
window mode=2
vsync mode=0
framerate limit=60
"@

Set-Content -LiteralPath $ConfigPath -Value $ConfigText -Encoding ASCII

$OpenMwArgs = @("--config", $ConfigDir, "--skip-menu", "--new-game")
if ($NoSound) {
    $OpenMwArgs += "--no-sound"
}

Write-Host "Launching $Exe"
Write-Host "Config $ConfigPath"
& $Exe @OpenMwArgs
