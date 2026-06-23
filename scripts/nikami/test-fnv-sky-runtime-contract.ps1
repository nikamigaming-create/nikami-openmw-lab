param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 8
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-sky-runtime-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing file for ${Label}: $Path" }
    if (!(Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "Missing ${Label}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Label}: $Pattern"
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing file for ${Label}: $Path" }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Label}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Label}: $Pattern"
}

Write-ProofLine "FNV sky runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$SkyCpp = Join-Path $RepoRoot "apps/openmw/mwrender/sky.cpp"
$SkyUtilCpp = Join-Path $RepoRoot "apps/openmw/mwrender/skyutil.cpp"
$SkyVert = Join-Path $RepoRoot "files/shaders/compatibility/sky.vert"
$SkyFrag = Join-Path $RepoRoot "files/shaders/compatibility/sky.frag"
$SkyPasses = Join-Path $RepoRoot "files/shaders/lib/sky/passes.glsl"
$ShaderSettings = Join-Path $RepoRoot "components/settings/categories/shaders.hpp"
$SettingsDefault = Join-Path $RepoRoot "files/settings-default.cfg"
$FlatScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat.ps1"
$FlatProofScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat-proof.ps1"
$VrDeployScript = Join-Path $RepoRoot "scripts/nikami/deploy-fnv-vr-headset.ps1"
$WeatherFallbackScript = Join-Path $RepoRoot "scripts/nikami/fnv_weather_fallbacks.py"
$RuntimeSettingsScript = Join-Path $RepoRoot "scripts/nikami/fnv-runtime-settings.ps1"

function Get-BsaTool() {
    $candidate = Join-Path $RepoRoot "build-clean/Release/bsatool.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return $candidate
    }
    $downstream = "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe"
    if (Test-Path -LiteralPath $downstream -PathType Leaf) {
        return $downstream
    }
    return "bsatool.exe"
}

function Assert-BsaContains([string]$BsaName, [string]$Pattern, [string]$Label) {
    $bsaPath = Join-Path $FnvData $BsaName
    if (!(Test-Path -LiteralPath $bsaPath -PathType Leaf)) {
        throw "Missing BSA for ${Label}: $bsaPath"
    }
    $bsaTool = Get-BsaTool
    $entries = @(& $bsaTool list $bsaPath)
    if ($LASTEXITCODE -ne 0) {
        throw "bsatool failed for $bsaPath exit=$LASTEXITCODE"
    }
    $matches = @($entries | Where-Object { $_ -match $Pattern })
    if ($matches.Count -eq 0) {
        throw "Missing ${Label}: $Pattern in $bsaPath"
    }
    Write-ProofLine "OK BSA ${Label}: $($matches[0])"
}

Assert-FileContains $SkyCpp "attachSkyNodeIfUnattached" "sky renderer preserves existing FNV wrapper parent"
Assert-FileContains $SkyCpp "FNV camera-relative sky mesh" "sky renderer creates camera-relative FNV wrapper"
Assert-FileContains $SkyCpp "hasConfiguredFalloutSkyModels" "sky renderer detects configured FNV sky models"
Assert-FileContains $SkyCpp "FNV/ESM4: sky shader mode" "sky renderer emits shader mode proof"
Assert-FileContains $SkyCpp "sky-interpreted" "FNV flat sky uses interpreted shader path"
Assert-FileContains $SkyCpp "interpreted sky material" "FNV flat sky attaches sky updaters"
Assert-FileContains $SkyCpp "updatersAttached=1" "FNV flat sky proves updater ownership"
Assert-FileContains $SkyCpp "generated-z-gradient" "FNV flat atmosphere uses generated vertex alpha gradient"
Assert-FileContains $SkyCpp "texture-alpha" "FNV flat clouds/stars use texture alpha mode"
Assert-FileContains $SkyCpp "vertexColorRgb=" "FNV flat sky logs vertex RGB usage"
Assert-FileContains $SkyCpp "not-used" "FNV flat sky logs vertex RGB disabled state"
Assert-FileContains $SkyCpp "sky mesh vertex colors" "FNV flat sky logs runtime vertex color stats"
Assert-FileContains $SkyCpp "rgbNonzero=" "FNV flat sky probes nonzero vertex RGB"
Assert-FileContains $SkyCpp "rgbVarying=" "FNV flat sky probes varying vertex RGB"
Assert-FileContains $SkyCpp "generated atmosphere shader alpha" "FNV flat sky logs generated atmosphere shader alpha"
Assert-FileContains $SkyCpp "calculateFalloutAtmosphereAlpha" "FNV flat sky calculates Fallout atmosphere alpha range"
Assert-FileContains $SkyUtilCpp "setFalloutAtmosphereZGradient" "FNV flat atmosphere updater exposes z-gradient uniform controls"
Assert-FileContains $SkyVert "useFalloutAtmosphereZGradient" "FNV flat sky vertex shader gates Fallout atmosphere z-gradient"
Assert-FileContains $SkyVert "falloutAtmosphereZRange" "FNV flat sky vertex shader consumes Fallout atmosphere z range"
Assert-FileContains $SkyVert "passColor\.a = clamp" "FNV flat sky vertex shader generates alpha from z range"
Assert-FileNotContains $SkyFrag "useVertexColorRgb" "unsupported FNV vertex RGB shader path"
Assert-FileNotContains $SkyCpp "updatersSkipped=1" "stale FNV raw sky bypass"
Assert-FileContains $ShaderSettings "mForceShaders" "shader settings expose explicit force-shaders key"
Assert-FileContains $SettingsDefault "force shaders = false" "default shader force flag is explicit off"
Assert-FileContains $SkyUtilCpp "enabled FNV sun billboard using texture" "FNV sky enables FNV sun billboard"
Assert-FileContains $SkyUtilCpp "enabled FNV sun glare using texture" "FNV sky enables FNV sun glare"
Assert-FileContains $SkyUtilCpp "enabled FNV " "FNV sky emits FNV moon billboard prefix"
Assert-FileContains $SkyUtilCpp " moon billboard using texture" "FNV sky enables FNV moon billboards"
Assert-FileNotContains $SkyUtilCpp "disabled OpenMW sun billboard for Fallout sky content" "stale FNV sun disable source path"
Assert-FileNotContains $SkyUtilCpp " moon billboard for Fallout sky content" "stale FNV moon disable source path"
Assert-FileContains $SkyFrag "premultiplied alpha blending" "moon shader keeps premultiplied blend path"
Assert-FileContains $SkyPasses "#define PASS_SUN 4" "sky shader pass table keeps sun pass id"
Assert-FileContains $SkyPasses "#define PASS_MOON 3" "sky shader pass table keeps moon pass id"
Assert-FileNotContains $SkyFrag "vec4 blendedLayer = phase \* moonBlend" "downstream legacy moon shader blend"
Assert-FileContains $FlatProofScript "OPENMW_FNV_SKY_MISSING_LOG" "flat proof enables sky diagnostics"
Assert-FileContains $FlatProofScript "RequireSkyColorSanity" "flat proof can gate sky screenshot colors"
Assert-FileContains $FlatProofScript "RequireSunVisible" "flat proof can gate visible sun screenshot"
Assert-FileContains $FlatProofScript "OPENMW_FNV_FLAT_CAMERA_YAW" "flat proof can steer camera toward sun"
Assert-FileContains $FlatProofScript "rawRedMaskLeak" "flat proof detects raw red sky-mask leakage"
Assert-FileContains $FlatProofScript "morrowindBluePaletteLeak" "flat proof detects stale Morrowind blue sky palette"
Assert-FileContains $FlatScript "force shaders = false" "FNV flat explicitly disables forced sky shader"
Assert-FileContains $WeatherFallbackScript "NAM0_GROUPS" "FNV WTHR fallback generator decodes NAM0 color groups"
Assert-FileContains $WeatherFallbackScript "SkyUpper" "FNV WTHR fallback generator maps Sky-Upper color"
Assert-FileContains $WeatherFallbackScript "SkyLower" "FNV WTHR fallback generator accounts for Sky-Lower color"
Assert-FileContains $WeatherFallbackScript "Horizon" "FNV WTHR fallback generator accounts for Horizon color"
Assert-FileContains $WeatherFallbackScript "Sunlight" "FNV WTHR fallback generator maps sunlight color"
Assert-FileContains $WeatherFallbackScript "payloadPolicy" "FNV WTHR fallback generator emits no-retail payload policy"
Assert-FileContains $WeatherFallbackScript "runtimeColorCoverage" "FNV WTHR fallback generator classifies vertical sky color coverage"
Assert-FileContains $RuntimeSettingsScript "Get-NikamiFnvWeatherFallbacks" "runtime settings expose generated FNV WTHR weather fallbacks"
Assert-FileContains $FlatScript "Get-NikamiFnvWeatherFallbacks" "FNV flat uses generated WTHR weather fallbacks"
Assert-FileContains $VrDeployScript "Get-NikamiFnvWeatherFallbacks" "FNV headset deploy uses generated WTHR weather fallbacks"
Assert-FileContains $VrDeployScript "force shaders = true" "FNV VR explicitly keeps forced shader path"
foreach ($scriptPath in @($FlatScript, $VrDeployScript)) {
    Assert-FileContains $scriptPath "skyatmosphere = meshes/sky/atmosphere.nif" "FNV atmosphere setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skyclouds = meshes/sky/clouds.nif" "FNV cloud setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skynight01 = meshes/sky/stars.nif" "FNV stars setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skynight02 = meshes/sky/stars.nif" "FNV alternate stars setting in $(Split-Path $scriptPath -Leaf)"
}

Assert-BsaContains "Fallout - Meshes.bsa" "meshes[\\/]+sky[\\/]+atmosphere\.nif$" "FNV atmosphere mesh entry"
Assert-BsaContains "Fallout - Meshes.bsa" "meshes[\\/]+sky[\\/]+clouds\.nif$" "FNV cloud mesh entry"
Assert-BsaContains "Fallout - Meshes.bsa" "meshes[\\/]+sky[\\/]+stars\.nif$" "FNV stars mesh entry"
Assert-BsaContains "Fallout - Textures2.bsa" "textures[\\/]+sky[\\/]+sun\.dds$" "FNV sun texture entry"
Assert-BsaContains "Fallout - Textures2.bsa" "textures[\\/]+sky[\\/]+skymoonfull\.dds$" "FNV moon texture entry"
Assert-BsaContains "Fallout - Textures2.bsa" "textures[\\/]+sky[\\/]+nv_sunglare\.dds$" "FNV sunglare texture entry"
Assert-BsaContains "Fallout - Textures2.bsa" "textures[\\/]+sky[\\/]+nvcloudlight\.dds$" "FNV WTHR-selected clear cloud texture entry"
Assert-BsaContains "Fallout - Textures2.bsa" "textures[\\/]+sky[\\/]+nv_wastelanduppersky\.dds$" "FNV WTHR-selected cloudy cloud texture entry"

$requiredLogPatterns = @(
    "FNV/ESM4: sky shader mode forceShaders=0 falloutSkyModels=1 program=sky-interpreted",
    "FNV/ESM4: sky mesh vertex colors day atmosphere \(meshes/sky/atmosphere\.nif\).*colorArrays=0 samples=0 rgbNonzero=0 rgbVarying=0",
    "FNV/ESM4: generated atmosphere shader alpha day atmosphere \(meshes/sky/atmosphere\.nif\).*mode=bound-z-gradient.*applied=1",
    "FNV/ESM4: interpreted sky material day atmosphere \(meshes/sky/atmosphere\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere updatersAttached=1 vertexAlpha=generated-z-gradient vertexColorRgb=not-used",
    "FNV/ESM4: interpreted sky material night atmosphere \(meshes/sky/stars\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere-night updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: interpreted sky material clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: interpreted sky material next clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: wrapped sky mesh day atmosphere \(meshes/sky/atmosphere\.nif\)",
    "FNV/ESM4: wrapped sky mesh night atmosphere \(meshes/sky/stars\.nif\)",
    "FNV/ESM4: wrapped sky mesh clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: wrapped sky mesh next clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds",
    "FNV/ESM4: enabled FNV Masser moon billboard using texture textures/sky/masser_full\.dds",
    "FNV/ESM4: enabled FNV Secunda moon billboard using texture textures/sky/skymoonfull\.dds"
)

$flatProofRoot = Join-Path $ProofRoot "fnv-flat-proof"
$before = @()
if (Test-Path -LiteralPath $flatProofRoot) {
    $before = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}

& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound `
    -ScreenshotFrames "180,300" `
    -RequireSkyColorSanity `
    -RequireLogPattern $requiredLogPatterns

$after = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)
$latestFlatProof = $after | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
if ($null -eq $latestFlatProof) {
    $latestFlatProof = $after | Select-Object -First 1
}
if ($null -eq $latestFlatProof) {
    throw "No FNV flat proof directory was produced"
}

$flatOpenMwLog = Join-Path $latestFlatProof.FullName "openmw.log"
$flatSettings = Join-Path $latestFlatProof.FullName "settings.cfg"
$flatOpenMwCfg = Join-Path $latestFlatProof.FullName "openmw.cfg"
$flatSummary = Join-Path $latestFlatProof.FullName "summary.txt"
$flatSkyColorSanity = Join-Path $latestFlatProof.FullName "sky-color-sanity.json"
$weatherFallbackRoot = Join-Path $ProofRoot "fnv-weather-fallbacks"
$latestWeatherFallback = Get-ChildItem -LiteralPath $weatherFallbackRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($null -eq $latestWeatherFallback) {
    throw "No generated FNV weather fallback proof directory under $weatherFallbackRoot"
}
$weatherFallbackJson = Join-Path $latestWeatherFallback.FullName "fnv-weather-fallbacks.json"
$weatherFallbackLines = Join-Path $latestWeatherFallback.FullName "fallbacks.cfg"
Write-ProofLine ""
Write-ProofLine "Flat proof: $($latestFlatProof.FullName)"
Write-ProofLine "OpenMW log: $flatOpenMwLog"
Write-ProofLine "Settings: $flatSettings"
Write-ProofLine "Weather fallback proof: $weatherFallbackJson"

Assert-FileContains $flatSummary "^Runtime mode: pc-flat$" "flat proof runtime mode"
Assert-FileContains $flatSummary "^IncludeFnvrPlugin: False$" "flat proof excludes FNVR/PCVR layer"
Assert-FileContains $flatOpenMwCfg "^fallback-archive=Fallout - Meshes\.bsa$" "generated flat meshes BSA"
Assert-FileContains $flatOpenMwCfg "^fallback-archive=Fallout - Textures2\.bsa$" "generated flat sky texture BSA"
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Cloud_Texture,textures/sky/.+\.dds$" "generated FNV clear cloud fallback"
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Sky_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated FNV clear sky day color fallback"
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Fog_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated FNV clear fog day color fallback"
Assert-FileNotContains $flatOpenMwCfg "^fallback=Weather_Clear_Sky_Day_Color,095,135,203$" "stale OpenMW/Morrowind clear sky day color"
Assert-FileContains $flatSettings "^skyatmosphere = meshes/sky/atmosphere\.nif$" "generated flat atmosphere setting"
Assert-FileContains $flatSettings "^skyclouds = meshes/sky/clouds\.nif$" "generated flat cloud setting"
Assert-FileContains $flatSettings "^skynight01 = meshes/sky/stars\.nif$" "generated flat stars setting"
Assert-FileContains $flatSettings "^skynight02 = meshes/sky/stars\.nif$" "generated flat alternate stars setting"
Assert-FileContains $flatSettings "^force shaders = false$" "generated flat force-shaders setting"
Assert-FileNotContains $flatSettings "^force shaders = true$" "generated flat VR shader mode"
Assert-FileContains $flatSettings "^sky blending = true$" "generated flat sky blending"
Assert-FileContains $flatSummary "^RequireSkyColorSanity: True$" "flat proof required sky color sanity"
Assert-FileContains $weatherFallbackJson '"payloadPolicy"\s*:\s*"derived-weather-fallbacks-no-retail-assets"' "generated weather fallback payload policy"
Assert-FileContains $weatherFallbackJson '"selectedWeather"' "generated weather fallback selected weather map"
Assert-FileContains $weatherFallbackJson '"skyColorGroups"' "generated weather fallback vertical sky color proof"
Assert-FileContains $weatherFallbackJson '"SkyLower"' "generated weather fallback SkyLower proof"
Assert-FileContains $weatherFallbackJson '"Horizon"' "generated weather fallback Horizon proof"
Assert-FileContains $weatherFallbackJson '"SkyLower"\s*:\s*"loaded-pending-runtime"' "generated weather fallback SkyLower runtime classification"
Assert-FileContains $weatherFallbackJson '"Horizon"\s*:\s*"loaded-pending-runtime"' "generated weather fallback Horizon runtime classification"
Assert-FileContains $weatherFallbackJson '"Clear"' "generated weather fallback clear selection"
Assert-FileContains $weatherFallbackJson '"runtimeBoundary"' "generated weather fallback runtime boundary"
Assert-FileContains $weatherFallbackLines "^fallback=Weather_Clear_Sky_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated weather fallback clear sky line"

$flatConfigText = Get-Content -LiteralPath $flatOpenMwCfg -Raw
foreach ($line in (Get-Content -LiteralPath $weatherFallbackLines)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if (!$flatConfigText.Contains($line)) {
        throw "Generated openmw.cfg missing WTHR fallback line: $line"
    }
}
Write-ProofLine "OK generated openmw.cfg includes every WTHR fallback line from $weatherFallbackLines"

Assert-FileNotContains $flatOpenMwLog "meshes/sky_atmosphere\.nif|meshes/sky_clouds_01\.nif|meshes/sky_night_01\.nif" "legacy OpenMW sky mesh path"
Assert-FileNotContains $flatOpenMwLog "marker_error|Failed to compile|failed to compile|linking failed|GLSL.*error|shader.*error" "sky shader/blocker line"
Assert-FileContains $flatOpenMwLog "Adding BSA archive .*Fallout - Meshes\.bsa" "runtime registered meshes BSA"
Assert-FileContains $flatOpenMwLog "Adding BSA archive .*Fallout - Textures2\.bsa" "runtime registered textures2 BSA"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: sky shader mode forceShaders=0 falloutSkyModels=1 program=sky-interpreted" "runtime FNV flat sky shader mode"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: sky mesh vertex colors day atmosphere \(meshes/sky/atmosphere\.nif\).*colorArrays=0 samples=0 rgbNonzero=0 rgbVarying=0" "runtime FNV atmosphere creation-time vertex RGB probe"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: generated atmosphere shader alpha day atmosphere \(meshes/sky/atmosphere\.nif\).*mode=bound-z-gradient.*applied=1" "runtime FNV atmosphere generated alpha"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: interpreted sky material day atmosphere \(meshes/sky/atmosphere\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere updatersAttached=1 vertexAlpha=generated-z-gradient vertexColorRgb=not-used" "runtime interpreted FNV atmosphere material"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: interpreted sky material clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used" "runtime interpreted FNV clouds material"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: interpreted sky material next clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used" "runtime interpreted FNV next clouds material"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: interpreted sky material night atmosphere \(meshes/sky/stars\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere-night updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used" "runtime interpreted FNV stars material"
Assert-FileNotContains $flatOpenMwLog "native sky material .*updatersSkipped=1|program=fixed-function-protected" "runtime stale FNV raw sky bypass"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh day atmosphere \(meshes/sky/atmosphere\.nif\)" "runtime wrapped FNV atmosphere"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh clouds \(meshes/sky/clouds\.nif\)" "runtime wrapped FNV clouds"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh next clouds \(meshes/sky/clouds\.nif\)" "runtime wrapped FNV next clouds"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh night atmosphere \(meshes/sky/stars\.nif\)" "runtime wrapped FNV stars"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds" "runtime FNV sun texture"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds" "runtime FNV sun glare texture"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: enabled FNV Masser moon billboard using texture textures/sky/masser_full\.dds" "runtime FNV Masser texture"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: enabled FNV Secunda moon billboard using texture textures/sky/skymoonfull\.dds" "runtime FNV Secunda texture"
Assert-FileNotContains $flatOpenMwLog "FNV/ESM4: disabled OpenMW sun billboard|FNV/ESM4: disabled OpenMW .* moon billboard" "stale FNV sun/moon disable path"
Assert-FileContains $flatSkyColorSanity '"rawRedMaskLeak"\s*:\s*false' "runtime sky screenshot color sanity"
Assert-FileNotContains $flatSkyColorSanity '"rawRedMaskLeak"\s*:\s*true' "runtime raw red sky-mask leakage"
Assert-FileContains $flatSkyColorSanity '"morrowindBluePaletteLeak"\s*:\s*false' "runtime no stale Morrowind blue sky palette"
Assert-FileNotContains $flatSkyColorSanity '"morrowindBluePaletteLeak"\s*:\s*true' "runtime stale Morrowind blue sky palette leakage"

$sunProofBefore = @()
if (Test-Path -LiteralPath $flatProofRoot) {
    $sunProofBefore = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}
$sunLogPatterns = @(
    "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds",
    "FNV/ESM4 diag: settled flat startup camera .*cameraPitch=0\.35 cameraYaw=-1\.16"
)

& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound `
    -ScreenshotFrames "180,300" `
    -FlatCameraYaw -1.16 `
    -FlatCameraPitch 0.35 `
    -RequireSunVisible `
    -RequireLogPattern $sunLogPatterns

$sunProofAfter = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)
$sunProof = $sunProofAfter | Where-Object { $sunProofBefore -notcontains $_.FullName } | Select-Object -First 1
if ($null -eq $sunProof) {
    $sunProof = $sunProofAfter | Select-Object -First 1
}
if ($null -eq $sunProof) {
    throw "No FNV sun-visible proof directory was produced"
}

$sunOpenMwLog = Join-Path $sunProof.FullName "openmw.log"
$sunSummary = Join-Path $sunProof.FullName "summary.txt"
$sunVisibilityJson = Join-Path $sunProof.FullName "sun-visibility.json"
Write-ProofLine ""
Write-ProofLine "Sun-visible proof: $($sunProof.FullName)"
Write-ProofLine "Sun-visible log: $sunOpenMwLog"
Assert-FileContains $sunSummary "^RequireSunVisible: True$" "sun proof required visible sun"
Assert-FileContains $sunSummary "^FlatCameraYaw: -1\.16$" "sun proof camera yaw"
Assert-FileContains $sunSummary "^FlatCameraPitch: 0\.35$" "sun proof camera pitch"
Assert-FileContains $sunOpenMwLog "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds" "sun proof FNV sun texture"
Assert-FileContains $sunOpenMwLog "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds" "sun proof FNV glare texture"
Assert-FileContains $sunOpenMwLog "FNV/ESM4 diag: settled flat startup camera .*cameraPitch=0\.35 cameraYaw=-1\.16" "sun proof camera points at sun"
Assert-FileContains $sunVisibilityJson '"sunVisible"\s*:\s*true' "runtime visible FNV sun screenshot"

$result = [ordered]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    flatProofDir = $latestFlatProof.FullName
    sunVisibleProofDir = $sunProof.FullName
    classification = "runtime-supported"
    checked = @(
        "explicit FNV sky model settings",
        "camera-relative wrapped FNV atmosphere/cloud/star meshes",
        "FNV atmosphere/cloud/star meshes use interpreted sky shader passes in PC flat",
        "FNV creation-time log records no loaded atmosphere vertex RGB arrays and the shader keeps vertex RGB disabled",
        "FNV atmosphere runtime generates a bound-derived shader z-gradient before the sky shader uses passColor alpha",
        "FNV sky shader does not consume unsupported vertex RGB data",
        "FNV WTHR-derived weather fallbacks supply sky/fog/ambient/sun colors without committing retail assets",
        "FNV WTHR SkyLower and Horizon colors are harvested into proof metadata and classified loaded-pending-runtime",
        "FNV screenshot sky color sanity rejects raw red channel/mask leakage and stale Morrowind blue palette leakage",
        "FNV sun/moon billboard path uses Fallout sky textures",
        "FNV sun-facing screenshot proves visible sun disc/glare core",
        "FNV flat shader mode is sky-interpreted",
        "FNV sky meshes and sun/moon textures present in retail BSA inventory",
        "sky shader pass ids and premultiplied moon blend guarded",
        "no legacy OpenMW sky mesh fallback",
        "no shader/blocker log lines"
    )
}
$resultPath = Join-Path $ProofDir "fnv-sky-runtime-contract.json"
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Contract JSON: $resultPath"
Write-ProofLine "FNV sky runtime contract PASS"
