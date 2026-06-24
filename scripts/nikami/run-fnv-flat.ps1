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
    [double]$BootstrapHour = 10,
    [int]$MaxRunSeconds = 0,
    [string]$StartupScript = "",
    [string]$LoadSavegame = "",
    [switch]$Detached,
    [switch]$WithMenu,
    [switch]$IncludeFnvrPlugin,
    [switch]$DisableTerrainTrace,
    [switch]$EnableDiagnostics,
    [switch]$GrabInput,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "fnv-runtime-settings.ps1")
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

$ViewingDistance = Get-NikamiFnvViewingDistance -FnvData $FnvData
Write-Host "Using FNV viewing distance from harvested fBlockLoadDistance: $ViewingDistance"

function Test-FnvOverlayDataPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) {
        return $false
    }

    $HudMenu = Join-Path $Path "menus/main/hud_main_menu.xml"
    $CursorTexture = Join-Path $Path "textures/tx_cursor.dds"
    return (Test-Path -LiteralPath $HudMenu) -and (Test-Path -LiteralPath $CursorTexture)
}

function Assert-PublishOverlayPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if ($resolved.StartsWith("D:\Modlists\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "FNV publish overlay data must be generated publish data, not legacy modlist data: $resolved"
    }
}

if (![string]::IsNullOrWhiteSpace($FnvConfigData)) {
    if (!(Test-Path -LiteralPath $FnvConfigData)) {
        throw "Missing FNV config data directory: $FnvConfigData"
    }

    $ChildData = Join-Path $FnvConfigData "data"
    if ((Test-FnvOverlayDataPath $ChildData) -and !(Test-FnvOverlayDataPath $FnvConfigData)) {
        Write-Host "Using FNV overlay child data directory: $ChildData"
        $FnvConfigData = $ChildData
    }

    if (!(Test-FnvOverlayDataPath $FnvConfigData)) {
        Write-Warning "FNV config data path does not expose expected HUD/menu assets: $FnvConfigData"
    }

    Assert-PublishOverlayPath $FnvConfigData
    $FnvConfigData = (Resolve-Path -LiteralPath $FnvConfigData).Path
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
if (!$DisableTerrainTrace) {
    $env:OPENMW_FNV_TERRAIN_PROBE = "1"
}
$env:OPENMW_FNV_BOOTSTRAP_HOUR = [string]$BootstrapHour
$env:OPENMW_FNV_PROCEDURE_HOUR = [string]$BootstrapHour
if ($EnableDiagnostics) {
    $env:OPENMW_FNV_CREATURE_ANIM_GROUP_DIAG = "1"
    $env:OPENMW_FNV_SOUND_DIAG = "1"
}

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
        $removed = $false
        for ($attempt = 1; $attempt -le 5 -and !$removed; $attempt++) {
            try {
                Remove-Item -LiteralPath $Path -Force -ErrorAction Stop
                $removed = $true
            }
            catch {
                if ($attempt -eq 5) { throw }
                Start-Sleep -Milliseconds (200 * $attempt)
            }
        }
    }
}

$FnvFallbackLines = @()
$FnvRoot = Split-Path $FnvData -Parent
$FnvDefaultIni = Join-Path $FnvRoot "Fallout_default.ini"
if (Test-Path -LiteralPath $FnvDefaultIni) {
    $FnvIni = Get-Content -LiteralPath $FnvDefaultIni
    $IntroMovieLine = $FnvIni | Where-Object { $_ -match "^\s*sIntroMovie\s*=" } | Select-Object -First 1
    if (![string]::IsNullOrWhiteSpace($IntroMovieLine)) {
        $IntroMovie = ($IntroMovieLine -replace "^\s*sIntroMovie\s*=\s*", "").Trim()
        if (![string]::IsNullOrWhiteSpace($IntroMovie)) {
            $FnvFallbackLines += "fallback=Movies_Company_Logo,$IntroMovie"
        }
    }
}
$FnvWeatherFallbacks = Get-NikamiFnvWeatherFallbacks -FnvData $FnvData -ProofRoot $ProofRoot
Write-Host "Using generated FNV WTHR weather fallbacks: $($FnvWeatherFallbacks.JsonPath)"
$FnvFallbackLines += $FnvWeatherFallbacks.Lines
$FnvFallbackText = $FnvFallbackLines -join "`r`n"

$OptionalDataLine = ""
if (![string]::IsNullOrWhiteSpace($FnvConfigData)) {
    $OptionalDataLine = "data=$($FnvConfigData.Replace("\", "/"))"
}

$FnvrContentLine = ""
if ($IncludeFnvrPlugin) {
    $FnvrPlugin = Join-Path $FnvData "FNVR.esp"
    if (!(Test-Path -LiteralPath $FnvrPlugin)) {
        throw "Requested -IncludeFnvrPlugin but FNVR.esp is missing: $FnvrPlugin"
    }
    $FnvrContentLine = "content=FNVR.esp"
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
$FnvrContentLine

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
force shaders = false
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

$OpenMwArgs = @("--replace", "config", "--config", $ConfigDir, "--user-data", $RuntimeDir)
if (!$GrabInput) {
    $OpenMwArgs += "--no-grab"
}
if (!$WithMenu) {
    $OpenMwArgs += "--skip-menu"
    if ([string]::IsNullOrWhiteSpace($LoadSavegame)) {
        $OpenMwArgs += @("--start", $StartCell)
    }
}
if (![string]::IsNullOrWhiteSpace($LoadSavegame)) {
    if (!(Test-Path -LiteralPath $LoadSavegame -PathType Leaf)) {
        throw "Missing load savegame: $LoadSavegame"
    }
    $OpenMwArgs += @("--load-savegame", (Resolve-Path -LiteralPath $LoadSavegame).Path)
}
if ($NoSound) {
    $OpenMwArgs += "--no-sound"
}
if (![string]::IsNullOrWhiteSpace($StartupScript)) {
    if (!(Test-Path -LiteralPath $StartupScript -PathType Leaf)) {
        throw "Missing startup script: $StartupScript"
    }
    $OpenMwArgs += @("--script-run", (Resolve-Path -LiteralPath $StartupScript).Path)
}

Write-Host "Launching $Exe"
Write-Host "OpenMW args: $($OpenMwArgs -join ' ')"
Write-Host "Config $ConfigPath"
Write-Host "OpenMW log $((Join-Path $ConfigDir "openmw.log"))"
Write-Host "FNV bootstrap/package hour: $BootstrapHour"
Write-Host "Startup script: $StartupScript"
Write-Host "Load savegame: $LoadSavegame"
Write-Host "Player terrain trace enabled: $(!$DisableTerrainTrace)"
Write-Host "Heavy diagnostics enabled: $EnableDiagnostics"
Write-Host "Input grab enabled: $GrabInput"
Write-Host "MSTT collision surgery: removed; generic object collision path required"
if ($Detached) {
    $Process = Start-Process -FilePath $Exe -ArgumentList $OpenMwArgs -WorkingDirectory (Split-Path $Exe -Parent) -RedirectStandardOutput $ProcessStdoutLog -RedirectStandardError $ProcessStderrLog -PassThru
    Write-Host "Detached OpenMW pid $($Process.Id)"
    Write-Host "OpenMW process logs:"
    Write-Host "  $ProcessStdoutLog"
    Write-Host "  $ProcessStderrLog"
} elseif ($MaxRunSeconds -gt 0) {
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

Remove-Item Env:OPENMW_FNV_TERRAIN_PROBE -ErrorAction SilentlyContinue
Remove-Item Env:OPENMW_FNV_BOOTSTRAP_HOUR -ErrorAction SilentlyContinue
Remove-Item Env:OPENMW_FNV_PROCEDURE_HOUR -ErrorAction SilentlyContinue
Remove-Item Env:OPENMW_FNV_CREATURE_ANIM_GROUP_DIAG -ErrorAction SilentlyContinue
Remove-Item Env:OPENMW_FNV_SOUND_DIAG -ErrorAction SilentlyContinue
