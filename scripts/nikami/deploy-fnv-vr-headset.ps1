param(
    [string]$AdbPath = "C:\Users\nbrys\AppData\Local\Android\Sdk\platform-tools\adb.exe",
    [string]$DeviceRoot = "/sdcard/OpenMWVR",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [switch]$FullData,
    [switch]$ConfigOnly,
    [switch]$UpdateResources,
    [switch]$SkipResources,
    [switch]$Launch,
    [int]$LaunchSeconds = 25,
    [int]$ViewingDistance = 10000
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$ProofRoot = Join-Path $RepoRoot "proof/headset-fnv-vr"
$StageRoot = Join-Path $ProofRoot "stage"
$ConfigStage = Join-Path $StageRoot "config"
$OverlayStage = Join-Path $StageRoot "fnv-config"
$FnvDeviceData = "$DeviceRoot/data/fnv/Data"
$FnvOverlayDeviceData = "$DeviceRoot/data/fnv-config"
$ActiveDeviceConfig = "$DeviceRoot/files/config"
$LegacyDeviceConfig = "$DeviceRoot/config"

if (!(Test-Path -LiteralPath $AdbPath)) {
    throw "Missing adb.exe: $AdbPath"
}

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = "D:\SteamLibrary\steamapps\common\Fallout New Vegas\Data"
}

if (!(Test-Path -LiteralPath $FnvData)) {
    throw "Missing FNV data directory: $FnvData"
}

if ([string]::IsNullOrWhiteSpace($FnvConfigData)) {
    $Candidate = "D:\Modlists\fnv\openmw-config\data"
    if (Test-Path -LiteralPath $Candidate) {
        $FnvConfigData = $Candidate
    }
}

New-Item -ItemType Directory -Force -Path $ConfigStage | Out-Null

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)
    & $AdbPath @Args
    if ($LASTEXITCODE -ne 0) {
        throw "adb failed: $($Args -join ' ')"
    }
}

function Push-IfExists {
    param(
        [string]$HostPath,
        [string]$DevicePath
    )

    if (Test-Path -LiteralPath $HostPath) {
        Write-Host "adb push $HostPath -> $DevicePath"
        Invoke-Adb push $HostPath $DevicePath
    } else {
        Write-Warning "Missing optional path: $HostPath"
    }
}

$EsmFiles = @(
    "FalloutNV.esm",
    "DeadMoney.esm",
    "HonestHearts.esm",
    "OldWorldBlues.esm",
    "LonesomeRoad.esm",
    "GunRunnersArsenal.esm",
    "CaravanPack.esm",
    "ClassicPack.esm",
    "MercenaryPack.esm",
    "TribalPack.esm",
    "FNVR.esp"
)

$BsaFiles = @(
    "Fallout - Meshes.bsa",
    "Fallout - Misc.bsa",
    "Fallout - Sound.bsa",
    "Fallout - Textures.bsa",
    "Fallout - Textures2.bsa",
    "Fallout - Voices1.bsa",
    "DeadMoney - Main.bsa",
    "DeadMoney - Sounds.bsa",
    "HonestHearts - Main.bsa",
    "HonestHearts - Sounds.bsa",
    "OldWorldBlues - Main.bsa",
    "OldWorldBlues - Sounds.bsa",
    "LonesomeRoad - Main.bsa",
    "LonesomeRoad - Sounds.bsa",
    "GunRunnersArsenal - Main.bsa",
    "GunRunnersArsenal - Sounds.bsa",
    "CaravanPack - Main.bsa",
    "ClassicPack - Main.bsa",
    "MercenaryPack - Main.bsa",
    "TribalPack - Main.bsa",
    "Update.bsa"
)

$MissingEsm = @($EsmFiles | Where-Object { !(Test-Path -LiteralPath (Join-Path $FnvData $_)) })
if ($MissingEsm.Count -gt 0) {
    throw "Missing required FNV ESM/ESP files: $($MissingEsm -join ', ')"
}

$ArchiveLines = $BsaFiles | ForEach-Object { "fallback-archive=$_" }
if ($FullData) {
    $MissingBsa = @($BsaFiles | Where-Object { !(Test-Path -LiteralPath (Join-Path $FnvData $_)) })
    if ($MissingBsa.Count -gt 0) {
        throw "Missing required FNV BSA files: $($MissingBsa -join ', ')"
    }
}

$ContentFiles = @($EsmFiles)
$ContentLines = $ContentFiles | ForEach-Object { "content=$_" }
$OptionalOverlayLine = "data=$FnvOverlayDeviceData"
if (![string]::IsNullOrWhiteSpace($FnvConfigData) -and (Test-Path -LiteralPath $FnvConfigData)) {
    $OptionalOverlayLine = "data=$FnvOverlayDeviceData"
}

$FallbackLines = @()
$GlobalOpenMwCfg = Join-Path $RepoRoot "files/openmw.cfg"
if (Test-Path -LiteralPath $GlobalOpenMwCfg) {
    $FallbackLines = @(Get-Content -LiteralPath $GlobalOpenMwCfg | Where-Object { $_ -match "^\s*fallback=" })
}

$OpenMwCfg = @"
replace=data-local
replace=user-data
replace=resources
replace=data
replace=fallback-archive
replace=content
data-local=$DeviceRoot/data
user-data=$DeviceRoot/user
resources=$DeviceRoot/resources
data=$DeviceRoot/resources/vfs-mw
data=$FnvDeviceData
$OptionalOverlayLine
$($FallbackLines -join "`n")
$($ArchiveLines -join "`n")
$($ContentLines -join "`n")
encoding=win1252
"@

$SettingsCfg = @"
[Camera]
field of view = 91
viewing distance = $ViewingDistance
head bobbing = false

[Fog]
sky blending start = 0.8
exponential fog = true
radial fog = true
sky blending = true

[Game]
best attack = false
smooth movement = false

[General]
anisotropy = 16
texture mipmap = linear
texture mag filter = linear
texture min filter = linear

[GUI]
controller menus = false
keyboard navigation = true
scaling factor = 1.0
stretch menu background = false
tooltip delay = 0

[HUD]
crosshair = false

[Map]
global = true

[Models]
load unsupported nif files = true
skyatmosphere = meshes/sky/atmosphere.nif
skyclouds = meshes/sky/clouds.nif
skynight01 = meshes/sky/stars.nif
skynight02 = meshes/sky/stars.nif

[Saves]
character = player - 1

[Shaders]
force shaders = true
apply lighting to environment maps = true
auto use object normal maps = true
auto use object specular maps = true
auto use terrain normal maps = true
auto use terrain specular maps = true

[Shadows]
actor shadows = false
enable shadows = false
object shadows = false
player shadows = false
terrain shadows = false
enable indoor shadows = false

[Stereo]
stereo enabled = true

[Video]
fullscreen = false
framerate limit = 0
resolution x = 1920
resolution y = 1080
vsync = false

[VR]
fallout hand mesh offset x = 0
fallout hand mesh offset y = 0.0857
fallout hand mesh offset z = -0.1714
fallout left hand mesh offset x = 0
fallout left hand mesh offset y = 0
fallout left hand mesh offset z = 0
fallout left hand mesh pitch = 0
fallout left hand mesh yaw = -90
fallout left hand mesh roll = 90
fallout right hand mesh offset x = 0
fallout right hand mesh offset y = 0
fallout right hand mesh offset z = 0
fallout right hand mesh pitch = 0
fallout right hand mesh yaw = 90
fallout right hand mesh roll = -90
fallout left hand roll flip = true
left handed mode = false

[Windows]
inventory h = 0.605469
inventory maximized = false
inventory w = 0.820312
inventory x = 0.0898438
inventory y = 0.197266
map hidden = false
stats hidden = true
"@

$OpenMwCfgPath = Join-Path $ConfigStage "openmw.cfg"
$SettingsCfgPath = Join-Path $ConfigStage "settings.cfg"
Set-Content -LiteralPath $OpenMwCfgPath -Value $OpenMwCfg -Encoding ASCII
Set-Content -LiteralPath $SettingsCfgPath -Value $SettingsCfg -Encoding ASCII

Write-Host "Preparing headset directories under $DeviceRoot"
Invoke-Adb shell "mkdir -p '$ActiveDeviceConfig' '$LegacyDeviceConfig' '$DeviceRoot/user' '$DeviceRoot/resources' '$DeviceRoot/data' '$FnvDeviceData'"

Write-Host "Clearing stale Android FNV proof/debug properties"
foreach ($Prop in @(
    "debug.openmw.fnv.start_cell",
    "debug.openmw.fnv.terrain_probe",
    "debug.openmw.fnv.floor_watchdog",
    "debug.openmw.fnv.bootstrap_cell",
    "debug.openmw.fnv.bootstrap_x",
    "debug.openmw.fnv.bootstrap_y",
    "debug.openmw.fnv.bootstrap_z",
    "debug.openmw.fnv.bootstrap_rot_z",
    "debug.openmw.fnv.bootstrap_camera_distance",
    "debug.openmw.fnv.bootstrap_hour"
)) {
    Invoke-Adb shell "setprop '$Prop' __unset"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
Invoke-Adb shell "if [ -f '$ActiveDeviceConfig/openmw.cfg' ]; then cp '$ActiveDeviceConfig/openmw.cfg' '$ActiveDeviceConfig/openmw.cfg.pre-fnv-$Stamp'; fi"
Invoke-Adb shell "if [ -f '$ActiveDeviceConfig/settings.cfg' ]; then cp '$ActiveDeviceConfig/settings.cfg' '$ActiveDeviceConfig/settings.cfg.pre-fnv-$Stamp'; fi"
Invoke-Adb shell "if [ -f '$LegacyDeviceConfig/openmw.cfg' ]; then cp '$LegacyDeviceConfig/openmw.cfg' '$LegacyDeviceConfig/openmw.cfg.pre-fnv-$Stamp'; fi"
Invoke-Adb shell "if [ -f '$LegacyDeviceConfig/settings.cfg' ]; then cp '$LegacyDeviceConfig/settings.cfg' '$LegacyDeviceConfig/settings.cfg.pre-fnv-$Stamp'; fi"

if ($UpdateResources -and !$SkipResources) {
    $BuiltResources = Join-Path $RepoRoot "build-clean/Release/resources"
    if (!(Test-Path -LiteralPath $BuiltResources)) {
        $BuiltResources = Join-Path $RepoRoot "build-vr/Release/resources"
    }
    Push-IfExists $BuiltResources "$DeviceRoot/"
} else {
    Write-Host "Keeping headset resources in place. Pass -UpdateResources to replace them from this checkout."
}

foreach ($ConfigDir in @($ActiveDeviceConfig, $LegacyDeviceConfig)) {
    Push-IfExists (Join-Path $RepoRoot "files/openxrinteractionprofiles.xml") "$ConfigDir/"
    Push-IfExists (Join-Path $RepoRoot "files/settings-overrides-vr.cfg") "$ConfigDir/"
    Push-IfExists $OpenMwCfgPath "$ConfigDir/"
    Push-IfExists $SettingsCfgPath "$ConfigDir/"
}

Write-Host "Removing stale Lua bootstrap defaults from headset overlay; native default globals are used for Android FNV startup."
Invoke-Adb shell "rm -f '$FnvOverlayDeviceData/nikami-fnv-bootstrap.omwscripts' '$FnvOverlayDeviceData/scripts/nikami/fnv-default-globals.lua'"

if (!$ConfigOnly -and ![string]::IsNullOrWhiteSpace($FnvConfigData) -and (Test-Path -LiteralPath $FnvConfigData)) {
    Write-Host "Staging FNV overlay/config data: $FnvConfigData"
    Invoke-Adb shell "mkdir -p '$FnvOverlayDeviceData'"
    Invoke-Adb push "$FnvConfigData/." "$FnvOverlayDeviceData/"
}

if (!$ConfigOnly) {
    foreach ($File in $EsmFiles) {
        Push-IfExists (Join-Path $FnvData $File) "$FnvDeviceData/"
    }

    if ($FullData) {
        foreach ($File in $BsaFiles) {
            Push-IfExists (Join-Path $FnvData $File) "$FnvDeviceData/"
        }
    } else {
        Write-Host "ESM/ESP pass only. Re-run with -FullData to push the FNV BSA payload."
    }
} else {
    Write-Host "Config-only pass. Existing FNV data on the headset is left untouched."
}

Write-Host "Active device config:"
Invoke-Adb shell cat "$ActiveDeviceConfig/openmw.cfg"

if ($Launch) {
    Write-Host "Launching org.openmw.vrquest for $LaunchSeconds seconds"
    Invoke-Adb shell am force-stop org.openmw.vrquest
    Invoke-Adb shell am start -n org.openmw.vrquest/.OpenMWNativeActivity
    Start-Sleep -Seconds $LaunchSeconds
    Invoke-Adb shell am force-stop org.openmw.vrquest
    Invoke-Adb shell tail -160 "$DeviceRoot/files/config/openmw.log"
}
