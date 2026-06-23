param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ProofRoot = ""
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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-pcvr-publish-runner-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-Text([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing file for ${Description}: $RelativePath"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing ${Description}: $Needle in $RelativePath"
    }
    Write-ProofLine "OK source anchor: $Description"
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    if (!(Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "Missing ${Description}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Description}: $Pattern"
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Description}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Description}: $Pattern"
}

Write-ProofLine "FNV PCVR publish runner contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "Runtime mode: pcvr" "PCVR runner declares runtime mode"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "openmw_vr.exe" "PCVR runner targets openmw_vr executable"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "content=FNVR.esp" "PCVR runner includes FNVR plugin"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "force shaders = true" "PCVR runner forces shader path"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "stereo enabled = true" "PCVR runner enables stereo"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "Get-NikamiFnvWeatherFallbacks" "PCVR runner uses generated WTHR weather fallbacks"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" "pcvr-publish-runner" "classifier owns PCVR publish runner row"

$Runner = Join-Path $PSScriptRoot "run-fnv-pcvr-proof.ps1"
& $Runner `
    -FnvData $FnvData `
    -FnvConfigData $FnvConfigData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -GenerateOnly `
    -NoSound

$pcvrProofRoot = Join-Path $ProofRoot "fnv-pcvr-proof"
$latestPcvrProof = Get-ChildItem -LiteralPath $pcvrProofRoot -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1
if ($null -eq $latestPcvrProof) {
    throw "Missing generated PCVR proof under $pcvrProofRoot"
}
$pcvrSummary = Join-Path $latestPcvrProof.FullName "summary.txt"
$pcvrOpenMwCfg = Join-Path $latestPcvrProof.FullName "openmw.cfg"
$pcvrSettings = Join-Path $latestPcvrProof.FullName "settings.cfg"
$weatherFallbackRoot = Join-Path $ProofRoot "fnv-weather-fallbacks"
$latestWeatherFallback = Get-ChildItem -LiteralPath $weatherFallbackRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($null -eq $latestWeatherFallback) {
    throw "Missing generated FNV weather fallback proof under $weatherFallbackRoot"
}
$weatherFallbackJson = Join-Path $latestWeatherFallback.FullName "fnv-weather-fallbacks.json"
$weatherFallbackLines = Join-Path $latestWeatherFallback.FullName "fallbacks.cfg"

Write-ProofLine ""
Write-ProofLine "PCVR proof: $($latestPcvrProof.FullName)"
Write-ProofLine "PCVR summary: $pcvrSummary"
Write-ProofLine "PCVR openmw.cfg: $pcvrOpenMwCfg"
Write-ProofLine "PCVR settings.cfg: $pcvrSettings"
Write-ProofLine "Weather fallback proof: $weatherFallbackJson"

Assert-FileContains $pcvrSummary "^Runtime mode: pcvr$" "PCVR summary runtime mode"
Assert-FileContains $pcvrSummary "openmw_vr\.exe" "PCVR summary executable"
Assert-FileContains $pcvrSummary "^GenerateOnly: True$" "PCVR summary generate-only boundary"
Assert-FileContains $pcvrSummary "^FNVR content last: true$" "PCVR summary FNVR ordering"
Assert-FileContains $pcvrSummary "OpenXR hardware/runtime proof remains pending" "PCVR pending hardware boundary"
Assert-FileContains $pcvrOpenMwCfg "^data=.*resources/vfs-mw$" "PCVR resources VFS data line"
Assert-FileContains $pcvrOpenMwCfg "^data=.*Fallout New Vegas/Data$" "PCVR FNV Data line"
Assert-FileContains $pcvrOpenMwCfg "^fallback-archive=Fallout - Meshes\.bsa$" "PCVR mesh archive"
Assert-FileContains $pcvrOpenMwCfg "^fallback-archive=Fallout - Textures2\.bsa$" "PCVR texture archive"
Assert-FileContains $pcvrOpenMwCfg "^fallback=Weather_Clear_Cloud_Texture,textures/sky/.+\.dds$" "PCVR generated FNV clear cloud fallback"
Assert-FileContains $pcvrOpenMwCfg "^fallback=Weather_Clear_Sky_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "PCVR generated FNV clear sky day color"
Assert-FileContains $pcvrOpenMwCfg "^fallback=Weather_Clear_Fog_Day_Color,[0-9]{3},[0-9]{3},[0-9]{3}$" "PCVR generated FNV clear fog day color"
Assert-FileNotContains $pcvrOpenMwCfg "^fallback=Weather_Clear_Sky_Day_Color,095,135,203$" "PCVR stale OpenMW/Morrowind clear sky color"
Assert-FileContains $pcvrOpenMwCfg "^content=FNVR\.esp$" "PCVR FNVR content line"
Assert-FileContains $pcvrSettings "^force shaders = true$" "PCVR force shaders setting"
Assert-FileContains $pcvrSettings "^stereo enabled = true$" "PCVR stereo setting"
Assert-FileContains $pcvrSettings "^viewing distance = [0-9]+$" "PCVR generated viewing distance"

$contentLines = @(Select-String -LiteralPath $pcvrOpenMwCfg -Pattern "^content=" | ForEach-Object { $_.Line.Trim() })
if ($contentLines.Count -ne 11) {
    throw "Unexpected PCVR content line count: actual=$($contentLines.Count) expected=11"
}
if ($contentLines[-1] -ne "content=FNVR.esp") {
    throw "PCVR content order does not put FNVR.esp last: $($contentLines -join ', ')"
}
Write-ProofLine "OK PCVR content order: $($contentLines -join ', ')"

$pcvrConfigText = Get-Content -LiteralPath $pcvrOpenMwCfg -Raw
foreach ($line in (Get-Content -LiteralPath $weatherFallbackLines)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if (!$pcvrConfigText.Contains($line)) {
        throw "Generated PCVR openmw.cfg missing WTHR fallback line: $line"
    }
}
Assert-FileContains $weatherFallbackJson '"payloadPolicy"\s*:\s*"derived-weather-fallbacks-no-retail-assets"' "PCVR generated weather fallback payload policy"
Write-ProofLine "OK PCVR openmw.cfg includes every WTHR fallback line from $weatherFallbackLines"

$metadataPath = Join-Path $ProofDir "fnv-pcvr-publish-runner-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    proofDir = $ProofDir
    pcvrProofDir = $latestPcvrProof.FullName
    classification = "loaded-pending-runtime"
    runtimeBoundary = "Publish-tree PCVR profile generation is gated; OpenXR runtime execution remains a separate hardware proof."
    contentLines = $contentLines
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine "FNV PCVR publish runner contract PASS"
