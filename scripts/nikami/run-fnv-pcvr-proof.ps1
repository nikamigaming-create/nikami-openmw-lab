param(
    [string]$BuildDir = "build-vr",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$StartCell = "Goodsprings",
    [int]$BootstrapHour = 10,
    [int]$RunSeconds = 16,
    [switch]$GenerateOnly,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "fnv-runtime-settings.ps1")
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set NIKAMI_FNV_DATA or pass -FnvData."
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Set NIKAMI_VCPKG_ROOT or pass -VcpkgRoot."
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-pcvr-proof/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Test-FnvOverlayDataPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) {
        return $false
    }
    return (Test-Path -LiteralPath (Join-Path $Path "menus/main/hud_main_menu.xml")) -and
        (Test-Path -LiteralPath (Join-Path $Path "textures/tx_cursor.dds"))
}

function Assert-PublishOverlayPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if ($resolved.StartsWith("D:\Modlists\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "FNV PCVR publish overlay data must be generated publish data, not legacy modlist data: $resolved"
    }
}

$BuildPath = Join-Path $RepoRoot $BuildDir
$Exe = Join-Path $BuildPath "$Configuration/openmw_vr.exe"
$Resources = Join-Path $BuildPath "$Configuration/resources"
$BuildConfig = Join-Path $BuildPath "$Configuration/openmw.cfg"
$ConfigDir = Join-Path $ProofRoot "configs/fnv-pcvr-clean"
$RuntimeDir = Join-Path $ProofRoot "runtime/fnv-pcvr-clean"
$DataLocalDir = Join-Path $RuntimeDir "data-local"
$ConfigPath = Join-Path $ConfigDir "openmw.cfg"
$SettingsPath = Join-Path $ConfigDir "settings.cfg"
$ProcessStdoutLog = Join-Path $ConfigDir "openmw-vr-process.stdout.log"
$ProcessStderrLog = Join-Path $ConfigDir "openmw-vr-process.stderr.log"

foreach ($required in @(
        @{ Path = $Exe; Label = "PCVR executable openmw_vr.exe" },
        @{ Path = $Resources; Label = "OpenMW VR resources" },
        @{ Path = $BuildConfig; Label = "generated OpenMW VR config" },
        @{ Path = $FnvData; Label = "FNV data directory" }
    )) {
    if (!(Test-Path -LiteralPath $required.Path)) {
        throw "Missing $($required.Label): $($required.Path)"
    }
}

$FnvrPlugin = Join-Path $FnvData "FNVR.esp"
if (!(Test-Path -LiteralPath $FnvrPlugin -PathType Leaf)) {
    throw "Missing PCVR FNVR plugin in FNV data directory: $FnvrPlugin"
}

if (![string]::IsNullOrWhiteSpace($FnvConfigData)) {
    if (!(Test-Path -LiteralPath $FnvConfigData)) {
        throw "Missing FNV config data directory: $FnvConfigData"
    }
    $ChildData = Join-Path $FnvConfigData "data"
    if ((Test-FnvOverlayDataPath $ChildData) -and !(Test-FnvOverlayDataPath $FnvConfigData)) {
        $FnvConfigData = $ChildData
    }
    Assert-PublishOverlayPath $FnvConfigData
    $FnvConfigData = (Resolve-Path -LiteralPath $FnvConfigData).Path
}

$ViewingDistance = Get-NikamiFnvViewingDistance -FnvData $FnvData
$FnvRoot = Split-Path $FnvData -Parent
$FnvDefaultIni = Join-Path $FnvRoot "Fallout_default.ini"
$FnvFallbackLines = @()
if (Test-Path -LiteralPath $FnvDefaultIni) {
    $IntroMovieLine = Get-Content -LiteralPath $FnvDefaultIni |
        Where-Object { $_ -match "^\s*sIntroMovie\s*=" } |
        Select-Object -First 1
    if (![string]::IsNullOrWhiteSpace($IntroMovieLine)) {
        $IntroMovie = ($IntroMovieLine -replace "^\s*sIntroMovie\s*=\s*", "").Trim()
        if (![string]::IsNullOrWhiteSpace($IntroMovie)) {
            $FnvFallbackLines += "fallback=Movies_Company_Logo,$IntroMovie"
        }
    }
}
$FnvWeatherFallbacks = Get-NikamiFnvWeatherFallbacks -FnvData $FnvData -ProofRoot $ProofRoot -Stamp $Stamp
Write-ProofLine "FNV weather fallback proof: $($FnvWeatherFallbacks.JsonPath)"
$FnvFallbackLines += $FnvWeatherFallbacks.Lines
$FnvFallbackText = $FnvFallbackLines -join "`r`n"
$OptionalDataLine = ""
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) {
    $OptionalDataLine = "data=$($FnvConfigData.Replace("\", "/"))"
}

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
New-Item -ItemType Directory -Force -Path $DataLocalDir | Out-Null
foreach ($name in @(
        "console_history.txt",
        "global_storage.bin",
        "input_v3.xml",
        "MyGUI.log",
        "openmw.log",
        "openmw-vr-process.stdout.log",
        "openmw-vr-process.stderr.log",
        "player_storage.bin",
        "settings.cfg",
        "shaders.yaml"
    )) {
    $path = Join-Path $ConfigDir $name
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

$ConfigText = @"
replace=config
replace=data-local
replace=data
replace=fallback-archive
replace=content

resources=$($Resources.Replace("\", "/"))
user-data=$($RuntimeDir.Replace("\", "/"))
data-local=$($DataLocalDir.Replace("\", "/"))
data=$((Join-Path $Resources "vfs-mw").Replace("\", "/"))
data=$($FnvData.Replace("\", "/"))
$OptionalDataLine

$FnvFallbackText

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
content=FNVR.esp

encoding=win1252

[Navigator]
enable=false
"@

$SettingsText = @"
[Camera]
field of view = 91
viewing distance = $ViewingDistance

[Fog]
sky blending start = 0.8
exponential fog = true
radial fog = true
sky blending = true

[Terrain]
distant terrain = true
object paging = true
object paging active grid = true
object paging min size = 0.01

[General]
anisotropy = 16
texture mipmap = linear
texture mag filter = linear
texture min filter = linear

[Models]
load unsupported nif files = true
skyatmosphere = meshes/sky/atmosphere.nif
skyclouds = meshes/sky/clouds.nif
skynight01 = meshes/sky/stars.nif
skynight02 = meshes/sky/stars.nif

[Shaders]
force shaders = true
apply lighting to environment maps = true
auto use object normal maps = true
auto use object specular maps = true
auto use terrain normal maps = true
auto use terrain specular maps = true
soft particles = true

[Shadows]
actor shadows = false
enable shadows = false
object shadows = false
player shadows = false
terrain shadows = false
enable indoor shadows = false
shadow map resolution = 1024

[Stereo]
stereo enabled = true

[Video]
resolution x = 1920
resolution y = 1080

[Saves]
character = player - 1

[Map]
global = true

[Windows]
inventory h = 0.605469
inventory maximized = false
inventory w = 0.820312
inventory x = 0.0898438
inventory y = 0.197266
map hidden = false
stats hidden = true
"@

Set-Content -LiteralPath $ConfigPath -Value $ConfigText -Encoding ASCII
Set-Content -LiteralPath $SettingsPath -Value $SettingsText -Encoding ASCII
Copy-Item -LiteralPath $ConfigPath -Destination (Join-Path $ProofDir "openmw.cfg") -Force
Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProofDir "settings.cfg") -Force

Write-ProofLine "FNV PCVR publish proof $Stamp"
Write-ProofLine "Runtime mode: pcvr"
Write-ProofLine "Runtime priority: pc-flat-first pcvr-second android-last"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "BuildDir: $BuildDir"
Write-ProofLine "Executable: $Exe"
Write-ProofLine "ConfigDir: $ConfigDir"
Write-ProofLine "RuntimeDir: $RuntimeDir"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "FnvConfigData: $FnvConfigData"
Write-ProofLine "ViewingDistance: $ViewingDistance"
Write-ProofLine "GenerateOnly: $GenerateOnly"
Write-ProofLine "FNVR content last: true"
Write-ProofLine "OpenMW config: $(Join-Path $ProofDir "openmw.cfg")"
Write-ProofLine "Settings: $(Join-Path $ProofDir "settings.cfg")"

if ($GenerateOnly) {
    Write-ProofLine "PCVR runtime execution skipped: GenerateOnly requested; OpenXR hardware/runtime proof remains pending."
    Write-ProofLine "FNV PCVR publish proof PASS"
    exit 0
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
}
else {
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/plugins/osgPlugins-3.6.5"
}
$OutputOsgPluginDir = Join-Path $BuildPath "$Configuration/osgPlugins-3.6.5"
$OsgPluginDirs = @($OutputOsgPluginDir, $OsgPluginDir, $ExtraOsgPluginDir) | Where-Object {
    ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_)
}
$env:OSG_LIBRARY_PATH = $OsgPluginDirs -join ";"
$env:OPENMW_FNV_TERRAIN_PROBE = "1"
$env:OPENMW_FNV_BOOTSTRAP_HOUR = [string]$BootstrapHour
$env:OPENMW_FNV_PROCEDURE_HOUR = [string]$BootstrapHour

$OpenMwArgs = @("--replace", "config", "--config", $ConfigDir, "--user-data", $RuntimeDir, "--no-grab", "--skip-menu", "--start", $StartCell)
if ($NoSound) {
    $OpenMwArgs += "--no-sound"
}

Write-ProofLine "Launching: $Exe"
$process = Start-Process -FilePath $Exe -ArgumentList $OpenMwArgs -WorkingDirectory (Split-Path $Exe -Parent) -RedirectStandardOutput $ProcessStdoutLog -RedirectStandardError $ProcessStderrLog -PassThru
if (!$process.WaitForExit($RunSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force
    Write-ProofLine "Stopped OpenMW VR after $RunSeconds seconds for probe logging."
}
else {
    Write-ProofLine "OpenMW VR exited with code $($process.ExitCode)"
}

foreach ($artifact in @(
        @{ Source = (Join-Path $ConfigDir "openmw.log"); Dest = "openmw.log" },
        @{ Source = (Join-Path $ConfigDir "MyGUI.log"); Dest = "MyGUI.log" },
        @{ Source = $ProcessStdoutLog; Dest = "openmw-vr-process.stdout.log" },
        @{ Source = $ProcessStderrLog; Dest = "openmw-vr-process.stderr.log" }
    )) {
    if (Test-Path -LiteralPath $artifact.Source) {
        Copy-Item -LiteralPath $artifact.Source -Destination (Join-Path $ProofDir $artifact.Dest) -Force
    }
}

Write-ProofLine "FNV PCVR publish proof PASS"
