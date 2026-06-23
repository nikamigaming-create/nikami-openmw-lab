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
$RenderingManagerCpp = Join-Path $RepoRoot "apps/openmw/mwrender/renderingmanager.cpp"
$WeatherCpp = Join-Path $RepoRoot "apps/openmw/mwworld/weather.cpp"
$WeatherHpp = Join-Path $RepoRoot "apps/openmw/mwworld/weather.hpp"
$FallbackValidateCpp = Join-Path $RepoRoot "components/fallback/validate.cpp"
$SkyVert = Join-Path $RepoRoot "files/shaders/compatibility/sky.vert"
$SkyFrag = Join-Path $RepoRoot "files/shaders/compatibility/sky.frag"
$SkyPasses = Join-Path $RepoRoot "files/shaders/lib/sky/passes.glsl"
$ShaderSettings = Join-Path $RepoRoot "components/settings/categories/shaders.hpp"
$SettingsDefault = Join-Path $RepoRoot "files/settings-default.cfg"
$SkyTextureStatsScript = Join-Path $RepoRoot "scripts/nikami/fnv_sky_texture_stats.py"
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
Assert-FileContains $SkyCpp "SkyVertexZRangeVisitor" "FNV flat sky derives atmosphere alpha range from vertex z"
Assert-FileContains $SkyCpp "vertex-z-gradient" "FNV flat sky proves vertex z gradient mode"
Assert-FileContains $SkyUtilCpp "setFalloutAtmosphereZGradient" "FNV flat atmosphere updater exposes z-gradient uniform controls"
Assert-FileContains $SkyUtilCpp "setFalloutAtmosphereGradientColors" "FNV flat atmosphere updater exposes vertical sky color uniforms"
Assert-FileContains $SkyUtilCpp "falloutAtmosphereSkyLowerColor" "FNV flat atmosphere updater binds SkyLower uniform"
Assert-FileContains $SkyUtilCpp "falloutAtmosphereSkyHorizonColor" "FNV flat atmosphere updater binds Horizon uniform"
Assert-FileContains $SkyUtilCpp "setFalloutSkyCloudVFlip" "FNV flat cloud updater exposes Fallout DDS V flip"
Assert-FileContains $SkyUtilCpp "useFalloutSkyCloudVFlip" "FNV flat cloud updater binds Fallout DDS V flip uniform"
Assert-FileContains $WeatherHpp "mSkyLowerColor" "weather result stores FNV SkyLower runtime color"
Assert-FileContains $WeatherHpp "mSkyHorizonColor" "weather result stores FNV Horizon runtime color"
Assert-FileContains $WeatherCpp "getOptionalWeatherColourInterpolator" "weather runtime reads optional generated FNV sky band fallbacks"
Assert-FileContains $WeatherCpp "skyLower=" "weather proof log emits runtime SkyLower color"
Assert-FileContains $WeatherCpp "skyHorizon=" "weather proof log emits runtime Horizon color"
Assert-FileContains $WeatherCpp "FNV/ESM4 proof: sun orbit" "weather proof log emits runtime sun orbit"
Assert-FileContains $RenderingManagerCpp "FNV/ESM4 proof: render sun direction" "renderer proof log emits consumed sun vector"
Assert-FileContains $FallbackValidateCpp "Sky_Lower" "fallback validator accepts generated FNV SkyLower keys"
Assert-FileContains $FallbackValidateCpp "Sky_Horizon" "fallback validator accepts generated FNV Horizon keys"
Assert-FileContains $SkyVert "useFalloutAtmosphereZGradient" "FNV flat sky vertex shader gates Fallout atmosphere z-gradient"
Assert-FileContains $SkyVert "falloutAtmosphereZRange" "FNV flat sky vertex shader consumes Fallout atmosphere z range"
Assert-FileContains $SkyVert "passColor\.a = clamp" "FNV flat sky vertex shader generates alpha from z range"
Assert-FileContains $SkyFrag "useFalloutAtmosphereGradientColors" "FNV flat sky fragment shader gates runtime vertical colors"
Assert-FileContains $SkyFrag "falloutAtmosphereSkyLowerColor" "FNV flat sky fragment shader consumes SkyLower color"
Assert-FileContains $SkyFrag "falloutAtmosphereSkyHorizonColor" "FNV flat sky fragment shader consumes Horizon color"
Assert-FileContains $SkyFrag "useFalloutSkyCloudVFlip" "FNV flat sky fragment shader gates Fallout cloud V flip"
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
Assert-FileContains $FlatProofScript "RequireSkyPaletteMatch" "flat proof can gate sky screenshot palette against generated WTHR colors"
Assert-FileContains $FlatProofScript "sky-palette-match.json" "flat proof emits generated sky palette match proof"
Assert-FileContains $FlatProofScript "nearestExpectedNormalizedDistance" "flat proof compares screenshot palette to generated WTHR palette"
Assert-FileContains $FlatProofScript "skyUpperPixelFraction" "flat proof counts visible generated SkyUpper pixels under cloud composite"
Assert-FileContains $FlatProofScript "bestSkyUpperNormalizedDistance" "flat proof records best visible generated SkyUpper pixel distance"
Assert-FileContains $FlatProofScript "topCloudCompositeBandPass" "flat proof separates cloud-composited top average from bare SkyUpper visibility"
Assert-FileContains $FlatProofScript "verticalBands" "flat proof emits vertical sky band proof"
Assert-FileContains $FlatProofScript "verticalBandOrderMatches" "flat proof gates vertical sky band ordering"
Assert-FileContains $FlatProofScript "distinctVerticalBandsPass" "flat proof requires multiple generated sky bands in screenshots"
Assert-FileContains $FlatProofScript "RequireSunVisible" "flat proof can gate visible sun screenshot"
Assert-FileContains $FlatProofScript "RequireSunDirectionRuntime" "flat proof can gate runtime sun vector chain"
Assert-FileContains $FlatProofScript "sun-direction.json" "flat proof emits runtime sun vector proof"
Assert-FileContains $FlatProofScript "OPENMW_FNV_FLAT_CAMERA_YAW" "flat proof can steer camera toward sun"
Assert-FileContains $FlatProofScript "rawRedMaskLeak" "flat proof detects raw red sky-mask leakage"
Assert-FileContains $FlatProofScript "morrowindBluePaletteLeak" "flat proof detects stale Morrowind blue sky palette"
Assert-FileContains $FlatScript "force shaders = false" "FNV flat explicitly disables forced sky shader"
Assert-FileContains $WeatherFallbackScript "NAM0_GROUPS" "FNV WTHR fallback generator decodes NAM0 color groups"
Assert-FileContains $WeatherFallbackScript "SkyUpper" "FNV WTHR fallback generator maps Sky-Upper color"
Assert-FileContains $WeatherFallbackScript "SkyLower" "FNV WTHR fallback generator accounts for Sky-Lower color"
Assert-FileContains $WeatherFallbackScript "Horizon" "FNV WTHR fallback generator accounts for Horizon color"
Assert-FileContains $WeatherFallbackScript "Weather_\{openmw_name\}_Sky_Lower_\{openmw_time\}_Color" "FNV WTHR fallback generator emits SkyLower fallback keys"
Assert-FileContains $WeatherFallbackScript "Weather_\{openmw_name\}_Sky_Horizon_\{openmw_time\}_Color" "FNV WTHR fallback generator emits Horizon fallback keys"
Assert-FileContains $WeatherFallbackScript "Sunlight" "FNV WTHR fallback generator maps sunlight color"
Assert-FileContains $WeatherFallbackScript "nam0Matrix" "FNV WTHR fallback generator emits NAM0 matrix proof"
Assert-FileContains $WeatherFallbackScript "rawRgbaBytes" "FNV WTHR fallback generator emits raw NAM0 slot bytes"
Assert-FileContains $WeatherFallbackScript "SunSunsetDisc" "FNV WTHR fallback generator separates sunset sun-disc coverage"
Assert-FileContains $WeatherFallbackScript "sourceWthrBytesClassification" "FNV WTHR fallback generator separates source byte classification"
Assert-FileContains $WeatherFallbackScript "payloadPolicy" "FNV WTHR fallback generator emits no-retail payload policy"
Assert-FileContains $WeatherFallbackScript "runtimeColorCoverage" "FNV WTHR fallback generator classifies vertical sky color coverage"
Assert-FileContains $SkyTextureStatsScript "derived-sky-texture-stats-no-retail-payloads" "FNV sky texture stats generator has no-payload policy"
Assert-FileContains $SkyTextureStatsScript "sunWarmChannelOrder" "FNV sky texture stats generator checks warm sun channel order"
Assert-FileContains $SkyTextureStatsScript "moonCoolChannelOrder" "FNV sky texture stats generator checks cool moon channel order"
Assert-FileContains $SkyTextureStatsScript "clearCloudTopBrighterThanBottom" "FNV sky texture stats generator checks cloud texture vertical orientation"
Assert-FileContains $SkyTextureStatsScript "wastelandUpperSkyBlueDominance" "FNV sky texture stats generator checks upper sky channel dominance"
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

$skyTextureProofDir = Join-Path $ProofDir "sky-texture-stats"
$skyTextureBsaTool = Get-BsaTool
& python $SkyTextureStatsScript --fnv-data $FnvData --bsa-tool $skyTextureBsaTool --proof-dir $skyTextureProofDir |
    ForEach-Object { Write-ProofLine $_ }
if ($LASTEXITCODE -ne 0) {
    throw "FNV sky texture stats generator failed with exit code $LASTEXITCODE"
}
$skyTextureStatsJson = Join-Path $skyTextureProofDir "fnv-sky-texture-stats.json"
Assert-FileContains $skyTextureStatsJson '"status"\s*:\s*"PASS"' "derived FNV sky texture stats status"
Assert-FileContains $skyTextureStatsJson '"payloadPolicy"\s*:\s*"derived-sky-texture-stats-no-retail-payloads"' "derived FNV sky texture stats payload policy"
Assert-FileContains $skyTextureStatsJson '"classification"\s*:\s*"runtime-supported"' "derived FNV sky texture runtime classifications"
Assert-FileContains $skyTextureStatsJson '"entry"\s*:\s*"textures/sky/sun\.dds"' "derived FNV sun texture stats row"
Assert-FileContains $skyTextureStatsJson '"sunWarmChannelOrder"\s*:\s*true' "derived FNV sun warm channel order"
Assert-FileContains $skyTextureStatsJson '"sunHasCutoutAlpha"\s*:\s*true' "derived FNV sun alpha cutout"
Assert-FileContains $skyTextureStatsJson '"entry"\s*:\s*"textures/sky/nv_sunglare\.dds"' "derived FNV sunglare texture stats row"
Assert-FileContains $skyTextureStatsJson '"sunglareNeutralChannelBalance"\s*:\s*true' "derived FNV sunglare neutral channel balance"
Assert-FileContains $skyTextureStatsJson '"entry"\s*:\s*"textures/sky/skymoonfull\.dds"' "derived FNV moon texture stats row"
Assert-FileContains $skyTextureStatsJson '"moonCoolChannelOrder"\s*:\s*true' "derived FNV moon cool channel order"
Assert-FileContains $skyTextureStatsJson '"entry"\s*:\s*"textures/sky/nvcloudlight\.dds"' "derived FNV clear cloud texture stats row"
Assert-FileContains $skyTextureStatsJson '"clearCloudBlueGreenRedOrder"\s*:\s*true' "derived FNV clear cloud channel order"
Assert-FileContains $skyTextureStatsJson '"clearCloudTopBrighterThanBottom"\s*:\s*true' "derived FNV clear cloud vertical orientation stats"
Assert-FileContains $skyTextureStatsJson '"entry"\s*:\s*"textures/sky/nv_wastelanduppersky\.dds"' "derived FNV wasteland upper sky texture stats row"
Assert-FileContains $skyTextureStatsJson '"wastelandUpperSkyBlueDominance"\s*:\s*true' "derived FNV wasteland upper sky blue dominance"
Assert-FileNotContains $skyTextureStatsJson '"status"\s*:\s*"FAIL"' "derived FNV sky texture stats failure"

$requiredLogPatterns = @(
    "FNV/ESM4: sky shader mode forceShaders=0 falloutSkyModels=1 program=sky-interpreted",
    "FNV/ESM4: sky mesh vertex colors day atmosphere \(meshes/sky/atmosphere\.nif\).*colorArrays=0 samples=0 rgbNonzero=0 rgbVarying=0",
    "FNV/ESM4: generated atmosphere shader alpha day atmosphere \(meshes/sky/atmosphere\.nif\).*mode=vertex-z-gradient.*vertexSamples=[1-9][0-9]*.*applied=1",
    "FNV/ESM4: interpreted sky material day atmosphere \(meshes/sky/atmosphere\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere updatersAttached=1 vertexAlpha=generated-z-gradient vertexColorRgb=not-used",
    "FNV/ESM4: interpreted sky material night atmosphere \(meshes/sky/stars\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere-night updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: interpreted sky material clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: cloud texture coordinates clouds \(meshes/sky/clouds\.nif\).*vMode=fallout-dds-v-flip runtime-supported",
    "FNV/ESM4: interpreted sky material next clouds \(meshes/sky/clouds\.nif\) nativeMaterial=0 skyProgram=sky skyPass=clouds updatersAttached=1 vertexAlpha=texture-alpha vertexColorRgb=not-used",
    "FNV/ESM4: cloud texture coordinates next clouds \(meshes/sky/clouds\.nif\).*vMode=fallout-dds-v-flip runtime-supported",
    "FNV/ESM4: wrapped sky mesh day atmosphere \(meshes/sky/atmosphere\.nif\)",
    "FNV/ESM4: atmosphere vertical colors runtime-supported skyUpper=.*skyLower=.*horizon=.*",
    "FNV/ESM4 proof: weather render state .*sky=.*skyLower=.*skyHorizon=.*fog=.*",
    "FNV/ESM4: wrapped sky mesh night atmosphere \(meshes/sky/stars\.nif\)",
    "FNV/ESM4: wrapped sky mesh clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: wrapped sky mesh next clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds",
    "FNV/ESM4: enabled FNV Masser moon billboard using texture textures/sky/masser_full\.dds",
    "FNV/ESM4: enabled FNV Secunda moon billboard using texture textures/sky/skymoonfull\.dds",
    "FNV/ESM4 proof: sun orbit .* expectedSkyPosition=.*",
    "FNV/ESM4 proof: render sun direction .* normalizedSky=.*"
)

$flatProofRoot = Join-Path $ProofRoot "fnv-flat-proof"
$before = @()
if (Test-Path -LiteralPath $flatProofRoot) {
    $before = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}
$SkyRunSeconds = [Math]::Max($RunSeconds, 30)

& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $SkyRunSeconds `
    -NoSound `
    -ScreenshotFrames "180,300" `
    -RequireSkyColorSanity `
    -RequireSkyPaletteMatch `
    -RequireSunDirectionRuntime `
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
$flatSkyPaletteMatch = Join-Path $latestFlatProof.FullName "sky-palette-match.json"
$flatSunDirection = Join-Path $latestFlatProof.FullName "sun-direction.json"
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
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Sky_Lower_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated FNV clear sky lower day color fallback"
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Sky_Horizon_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated FNV clear sky horizon day color fallback"
Assert-FileContains $flatOpenMwCfg "^fallback=Weather_Clear_Fog_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated FNV clear fog day color fallback"
Assert-FileNotContains $flatOpenMwCfg "^fallback=Weather_Clear_Sky_Day_Color,095,135,203$" "stale OpenMW/Morrowind clear sky day color"
Assert-FileContains $flatSettings "^skyatmosphere = meshes/sky/atmosphere\.nif$" "generated flat atmosphere setting"
Assert-FileContains $flatSettings "^skyclouds = meshes/sky/clouds\.nif$" "generated flat cloud setting"
Assert-FileContains $flatSettings "^skynight01 = meshes/sky/stars\.nif$" "generated flat stars setting"
Assert-FileContains $flatSettings "^skynight02 = meshes/sky/stars\.nif$" "generated flat alternate stars setting"
Assert-FileContains $flatSettings "^force shaders = false$" "generated flat force-shaders setting"
Assert-FileNotContains $flatSettings "^force shaders = true$" "generated flat VR shader mode"
Assert-FileContains $flatSettings "^sky blending = true$" "generated flat sky blending"
Assert-FileContains $flatSummary "^ScreenshotFrames: 180,300$" "flat proof screenshot frames"
Assert-FileContains $flatSummary "^RequireSkyColorSanity: True$" "flat proof required sky color sanity"
Assert-FileContains $flatSummary "^RequireSkyPaletteMatch: True$" "flat proof required generated sky palette match"
Assert-FileContains $flatSummary "^RequireSunDirectionRuntime: True$" "flat proof required runtime sun vector chain"
Assert-FileContains $weatherFallbackJson '"payloadPolicy"\s*:\s*"derived-weather-fallbacks-no-retail-assets"' "generated weather fallback payload policy"
Assert-FileContains $weatherFallbackJson '"sourceFormat"\s*:\s*"FNV WTHR NAM0 color layout"' "generated weather fallback source format"
Assert-FileContains $weatherFallbackJson '"sourceWthrBytesClassification"\s*:\s*"loaded-pending-runtime"' "generated weather fallback source byte classification"
Assert-FileContains $weatherFallbackJson '"derivedFallbackRenderingClassification"\s*:\s*"runtime-supported"' "generated weather fallback runtime classification"
Assert-FileContains $weatherFallbackJson '"selectedWeather"' "generated weather fallback selected weather map"
Assert-FileContains $weatherFallbackJson '"skyColorGroups"' "generated weather fallback vertical sky color proof"
Assert-FileContains $weatherFallbackJson '"nam0Matrix"' "generated weather fallback NAM0 matrix proof"
Assert-FileContains $weatherFallbackJson '"rawRgbaBytes"' "generated weather fallback raw NAM0 slot proof"
Assert-FileContains $weatherFallbackJson '"emittedFallbackKey"\s*:\s*"Weather_Clear_Sun_Disc_Sunset_Color"' "generated weather fallback sun disc target"
Assert-FileContains $weatherFallbackJson '"SkyLower"' "generated weather fallback SkyLower proof"
Assert-FileContains $weatherFallbackJson '"Horizon"' "generated weather fallback Horizon proof"
Assert-FileContains $weatherFallbackJson '"SkyLower"\s*:\s*"runtime-supported"' "generated weather fallback SkyLower runtime classification"
Assert-FileContains $weatherFallbackJson '"Horizon"\s*:\s*"runtime-supported"' "generated weather fallback Horizon runtime classification"
Assert-FileContains $weatherFallbackJson '"Sun"\s*:\s*"loaded-pending-runtime"' "generated weather fallback full Sun coverage remains pending"
Assert-FileContains $weatherFallbackJson '"SunSunsetDisc"\s*:\s*"runtime-supported"' "generated weather fallback sunset sun-disc runtime classification"
Assert-FileNotContains $weatherFallbackJson '"SkyLower"\s*:\s*"loaded-pending-runtime"' "stale SkyLower loaded-pending classification"
Assert-FileNotContains $weatherFallbackJson '"Horizon"\s*:\s*"loaded-pending-runtime"' "stale Horizon loaded-pending classification"
Assert-FileNotContains $weatherFallbackJson '"sourceFormat"\s*:\s*"FNV WTHR NAM0/PNAM layout"' "stale unsupported PNAM proof claim"
Assert-FileContains $weatherFallbackJson '"Clear"' "generated weather fallback clear selection"
Assert-FileContains $weatherFallbackJson '"runtimeBoundary"' "generated weather fallback runtime boundary"
Assert-FileContains $weatherFallbackLines "^fallback=Weather_Clear_Sky_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated weather fallback clear sky line"
Assert-FileContains $weatherFallbackLines "^fallback=Weather_Clear_Sky_Lower_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated weather fallback clear sky lower line"
Assert-FileContains $weatherFallbackLines "^fallback=Weather_Clear_Sky_Horizon_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "generated weather fallback clear sky horizon line"

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
Assert-FileContains $flatOpenMwLog "FNV/ESM4: generated atmosphere shader alpha day atmosphere \(meshes/sky/atmosphere\.nif\).*mode=vertex-z-gradient.*vertexSamples=[1-9][0-9]*.*applied=1" "runtime FNV atmosphere generated alpha"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: interpreted sky material day atmosphere \(meshes/sky/atmosphere\.nif\) nativeMaterial=0 skyProgram=sky skyPass=atmosphere updatersAttached=1 vertexAlpha=generated-z-gradient vertexColorRgb=not-used" "runtime interpreted FNV atmosphere material"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: atmosphere vertical colors runtime-supported skyUpper=.*skyLower=.*horizon=.*" "runtime FNV atmosphere vertical color uniforms"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: cloud texture coordinates clouds \(meshes/sky/clouds\.nif\).*vMode=fallout-dds-v-flip runtime-supported" "runtime FNV cloud DDS V flip"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: cloud texture coordinates next clouds \(meshes/sky/clouds\.nif\).*vMode=fallout-dds-v-flip runtime-supported" "runtime FNV next-cloud DDS V flip"
Assert-FileContains $flatOpenMwLog "FNV/ESM4 proof: weather render state .*sky=.*skyLower=.*skyHorizon=.*fog=.*" "runtime weather interpolation supplies vertical sky colors"
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
Assert-FileContains $flatSkyPaletteMatch '"status"\s*:\s*"PASS"' "runtime generated sky palette match status"
Assert-FileContains $flatSkyPaletteMatch '"expectedWeather"\s*:\s*"Clear"' "runtime generated sky palette match weather"
Assert-FileContains $flatSkyPaletteMatch '"expectedTime"\s*:\s*"Day"' "runtime generated sky palette match time"
Assert-FileContains $flatSkyPaletteMatch '"channelOrderMatches"\s*:\s*true' "runtime sky palette channel ordering"
Assert-FileContains $flatSkyPaletteMatch '"normalizedDistancePass"\s*:\s*true' "runtime sky palette normalized distance"
Assert-FileContains $flatSkyPaletteMatch '"verticalBands"' "runtime sky palette vertical band samples"
Assert-FileContains $flatSkyPaletteMatch '"skyUpperBandPass"\s*:\s*true' "runtime sky palette observes generated upper sky band at top"
Assert-FileContains $flatSkyPaletteMatch '"skyUpperPixelFraction"\s*:\s*0\.[0-9]*[1-9][0-9]*' "runtime sky palette counts visible generated upper sky pixels"
Assert-FileContains $flatSkyPaletteMatch '"skyUpperVisiblePass"\s*:\s*true' "runtime sky palette passes visible generated upper sky pixels"
Assert-FileContains $flatSkyPaletteMatch '"topCloudCompositeBandPass"\s*:\s*true' "runtime sky palette accounts for FNV cloud-composited top band"
Assert-FileContains $flatSkyPaletteMatch '"nearestExpectedBand"\s*:\s*"Horizon"' "runtime sky palette observes generated horizon band"
Assert-FileContains $flatSkyPaletteMatch '"verticalBandOrderMatches"\s*:\s*true' "runtime sky palette vertical band ordering"
Assert-FileContains $flatSkyPaletteMatch '"distinctVerticalBandsPass"\s*:\s*true' "runtime sky palette distinct vertical bands"
Assert-FileContains $flatSkyPaletteMatch '"verticalBandDistancePass"\s*:\s*true' "runtime sky palette per-band normalized distance"
Assert-FileNotContains $flatSkyPaletteMatch '"paletteMatches"\s*:\s*false' "runtime generated sky palette mismatch"
Assert-FileContains $flatSunDirection '"status"\s*:\s*"PASS"' "runtime sun vector proof status"
Assert-FileContains $flatSunDirection '"chainMatches"\s*:\s*true' "runtime sun vector weather/render chain"
Assert-FileContains $flatSunDirection '"skyPositionZPositive"\s*:\s*true' "runtime sun vector above horizon"
Assert-FileContains $flatSunDirection '"fnvOrbitParity"\s*:\s*"loaded-pending-runtime"' "runtime sun orbit parity remains bounded"

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
    -RunSeconds $SkyRunSeconds `
    -NoSound `
    -ScreenshotFrames "180,300" `
    -FlatCameraYaw -1.16 `
    -FlatCameraPitch 0.35 `
    -RequireSunVisible `
    -RequireSunDirectionRuntime `
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
$sunDirectionJson = Join-Path $sunProof.FullName "sun-direction.json"
Write-ProofLine ""
Write-ProofLine "Sun-visible proof: $($sunProof.FullName)"
Write-ProofLine "Sun-visible log: $sunOpenMwLog"
Assert-FileContains $sunSummary "^RequireSunVisible: True$" "sun proof required visible sun"
Assert-FileContains $sunSummary "^RequireSunDirectionRuntime: True$" "sun proof required runtime sun vector chain"
Assert-FileContains $sunSummary "^ScreenshotFrames: 180,300$" "sun proof screenshot frames"
Assert-FileContains $sunSummary "^FlatCameraYaw: -1\.16$" "sun proof camera yaw"
Assert-FileContains $sunSummary "^FlatCameraPitch: 0\.35$" "sun proof camera pitch"
Assert-FileContains $sunOpenMwLog "FNV/ESM4: enabled FNV sun billboard using texture textures/sky/sun\.dds" "sun proof FNV sun texture"
Assert-FileContains $sunOpenMwLog "FNV/ESM4: enabled FNV sun glare using texture textures/sky/nv_sunglare\.dds" "sun proof FNV glare texture"
Assert-FileContains $sunOpenMwLog "FNV/ESM4 diag: settled flat startup camera .*cameraPitch=0\.35 cameraYaw=-1\.16" "sun proof camera points at sun"
Assert-FileContains $sunVisibilityJson '"sunVisible"\s*:\s*true' "runtime visible FNV sun screenshot"
Assert-FileContains $sunDirectionJson '"status"\s*:\s*"PASS"' "sun proof runtime sun vector status"
Assert-FileContains $sunDirectionJson '"chainMatches"\s*:\s*true' "sun proof runtime sun vector chain"

$result = [ordered]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    flatProofDir = $latestFlatProof.FullName
    sunVisibleProofDir = $sunProof.FullName
    skyTextureStatsJson = $skyTextureStatsJson
    classification = "runtime-supported"
    checked = @(
        "explicit FNV sky model settings",
        "camera-relative wrapped FNV atmosphere/cloud/star meshes",
        "FNV atmosphere/cloud/star meshes use interpreted sky shader passes in PC flat",
        "FNV creation-time log records no loaded atmosphere vertex RGB arrays and the shader keeps vertex RGB disabled",
        "FNV atmosphere runtime generates a bound-derived shader z-gradient before the sky shader uses passColor alpha",
        "FNV weather runtime interpolates SkyUpper, SkyLower, and Horizon and binds them to atmosphere shader uniforms",
        "FNV sky shader does not consume unsupported vertex RGB data",
        "FNV WTHR-derived weather fallbacks supply sky/fog/ambient/sun colors without committing retail assets",
        "FNV WTHR NAM0 matrix proof separates loaded source bytes from derived runtime fallback targets",
        "FNV WTHR SkyLower and Horizon colors are emitted into generated fallback config and classified runtime-supported",
        "FNV NAM0 Sunlight is bound to directional sun color while NAM0 Sun is only sunset-disc runtime-supported",
        "FNV screenshot palette is compared against generated Clear/Day WTHR sky colors without committed image baselines",
        "FNV screenshot palette separately proves upper/lower sky bands and horizon band placement",
        "FNV screenshot sky color sanity rejects raw red channel/mask leakage and stale Morrowind blue palette leakage",
        "FNV sky DDS texture stats prove expected sun/moon/cloud channel signatures and cloud vertical orientation without storing retail payloads",
        "FNV sun/moon billboard path uses Fallout sky textures",
        "FNV sun-facing screenshot proves visible sun disc/glare core",
        "FNV runtime sun vector chain is logged and internally consistent while retail orbit parity remains pending",
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
