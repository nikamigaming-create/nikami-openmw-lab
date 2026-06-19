param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$StartCell = "Goodsprings",
    [int]$MaxRunSeconds = 0,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$BuildPath = Join-Path $RepoRoot $BuildDir

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set NIKAMI_FNV_DATA or pass -FnvData."
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
$ConfigDir = Join-Path $ProofRoot "configs/fnv-flat-clean"
$RuntimeDir = Join-Path $ProofRoot "runtime/fnv-flat-clean"
$DataLocalDir = Join-Path $RuntimeDir "data-local"
$ConfigPath = Join-Path $ConfigDir "openmw.cfg"
$SettingsPath = Join-Path $ConfigDir "settings.cfg"
$ProcessStdoutLog = Join-Path $ConfigDir "openmw-process.stdout.log"
$ProcessStderrLog = Join-Path $ConfigDir "openmw-process.stderr.log"

if (!(Test-Path -LiteralPath $Exe)) {
    throw "Missing OpenMW executable. Run scripts/nikami/build-clean-openmw.ps1 first: $Exe"
}

if (!(Test-Path -LiteralPath $Resources)) {
    throw "Missing OpenMW resources directory: $Resources"
}

if (!(Test-Path -LiteralPath $BuildConfig)) {
    throw "Missing generated OpenMW config: $BuildConfig"
}

if (!(Test-Path -LiteralPath $FnvData)) {
    throw "Missing FNV data directory: $FnvData"
}

if (![string]::IsNullOrWhiteSpace($FnvConfigData) -and !(Test-Path -LiteralPath $FnvConfigData)) {
    throw "Missing FNV config data directory: $FnvConfigData"
}

$RuntimeDllDirs = @(
    (Join-Path $VcpkgRoot "installed/$Triplet/bin"),
    (Join-Path $VcpkgRoot "installed/$Triplet/bin/$Configuration"),
    (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin"),
    (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin/$Configuration")
) | Where-Object { Test-Path -LiteralPath $_ }

$env:Path = ($RuntimeDllDirs + $env:Path) -join ";"
$PathValue = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $PathValue

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
    "openmw-process.stdout.log",
    "openmw-process.stderr.log",
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

$OptionalDataLine = ""
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) {
    $OptionalDataLine = "data=$($FnvConfigData.Replace("\", "/"))"
}

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
$OptionalDataLine
data=$($FnvData.Replace("\", "/"))

$FallbackText

fallback-archive=Fallout - Meshes.bsa
fallback-archive=Fallout - Misc.bsa
fallback-archive=Fallout - Sound.bsa
fallback-archive=Fallout - Textures.bsa
fallback-archive=Fallout - Textures2.bsa
fallback-archive=Fallout - Voices1.bsa
fallback-archive=DeadMoney - Main.bsa
fallback-archive=DeadMoney - Sounds.bsa
fallback-archive=HonestHearts - Main.bsa
fallback-archive=HonestHearts - Sounds.bsa
fallback-archive=OldWorldBlues - Main.bsa
fallback-archive=OldWorldBlues - Sounds.bsa
fallback-archive=LonesomeRoad - Main.bsa
fallback-archive=LonesomeRoad - Sounds.bsa
fallback-archive=GunRunnersArsenal - Main.bsa
fallback-archive=GunRunnersArsenal - Sounds.bsa
fallback-archive=CaravanPack - Main.bsa
fallback-archive=ClassicPack - Main.bsa
fallback-archive=MercenaryPack - Main.bsa
fallback-archive=TribalPack - Main.bsa
fallback-archive=Update.bsa

content=FalloutNV.esm
content=DeadMoney.esm
content=HonestHearts.esm
content=OldWorldBlues.esm
content=LonesomeRoad.esm
content=GunRunnersArsenal.esm
content=CaravanPack.esm
content=ClassicPack.esm
content=MercenaryPack.esm
content=TribalPack.esm

encoding=win1252

[Navigator]
enable=false
"@

$SettingsText = @"
[Camera]
field of view = 91
viewing distance = 10000

[Models]
load unsupported nif files = true

[Video]
resolution x = 1280
resolution y = 720
fullscreen = false
window mode = 2
vsync mode = 0
framerate limit = 60

[Map]
global = true
"@

Set-Content -LiteralPath $ConfigPath -Value $ConfigText -Encoding ASCII
Set-Content -LiteralPath $SettingsPath -Value $SettingsText -Encoding ASCII

$OpenMwArgs = @("--replace", "config", "--config", $ConfigDir, "--user-data", $RuntimeDir, "--skip-menu", "--start", $StartCell, "--no-grab")
if ($NoSound) {
    $OpenMwArgs += "--no-sound"
}

Write-Host "Launching $Exe"
Write-Host "Config $ConfigPath"
if ($MaxRunSeconds -gt 0) {
    $Process = Start-Process -FilePath $Exe -ArgumentList $OpenMwArgs -WorkingDirectory (Split-Path $Exe -Parent) -RedirectStandardOutput $ProcessStdoutLog -RedirectStandardError $ProcessStderrLog -PassThru
    if (!$Process.WaitForExit($MaxRunSeconds * 1000)) {
        Stop-Process -Id $Process.Id -Force
        Write-Warning "Stopped OpenMW after $MaxRunSeconds seconds for probe logging."
    }
    Write-Host "OpenMW process logs:"
    Write-Host "  $ProcessStdoutLog"
    Write-Host "  $ProcessStderrLog"
} else {
    & $Exe @OpenMwArgs *> $ProcessStdoutLog
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        Write-Error "OpenMW exited with code $ExitCode. See $ProcessStdoutLog and $(Join-Path $ConfigDir "openmw.log")."
    }
}
