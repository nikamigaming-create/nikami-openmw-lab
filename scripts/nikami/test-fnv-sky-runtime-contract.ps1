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
$FlatScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat.ps1"
$FlatProofScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat-proof.ps1"
$VrDeployScript = Join-Path $RepoRoot "scripts/nikami/deploy-fnv-vr-headset.ps1"

Assert-FileContains $SkyCpp "attachSkyNodeIfUnattached" "sky renderer preserves existing FNV wrapper parent"
Assert-FileContains $SkyCpp "FNV camera-relative sky mesh" "sky renderer creates camera-relative FNV wrapper"
Assert-FileContains $FlatProofScript "OPENMW_FNV_SKY_MISSING_LOG" "flat proof enables sky diagnostics"
foreach ($scriptPath in @($FlatScript, $VrDeployScript)) {
    Assert-FileContains $scriptPath "skyatmosphere = meshes/sky/atmosphere.nif" "FNV atmosphere setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skyclouds = meshes/sky/clouds.nif" "FNV cloud setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skynight01 = meshes/sky/stars.nif" "FNV stars setting in $(Split-Path $scriptPath -Leaf)"
    Assert-FileContains $scriptPath "skynight02 = meshes/sky/stars.nif" "FNV alternate stars setting in $(Split-Path $scriptPath -Leaf)"
}

$requiredLogPatterns = @(
    "FNV/ESM4: wrapped sky mesh day atmosphere \(meshes/sky/atmosphere\.nif\)",
    "FNV/ESM4: wrapped sky mesh night atmosphere \(meshes/sky/stars\.nif\)",
    "FNV/ESM4: wrapped sky mesh clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: wrapped sky mesh next clouds \(meshes/sky/clouds\.nif\)",
    "FNV/ESM4: enabled sun billboard using texture textures/sky/sun\.dds",
    "FNV/ESM4: enabled OpenMW Masser moon billboard with Fallout texture selection",
    "FNV/ESM4: enabled OpenMW Secunda moon billboard with Fallout texture selection"
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
Write-ProofLine ""
Write-ProofLine "Flat proof: $($latestFlatProof.FullName)"
Write-ProofLine "OpenMW log: $flatOpenMwLog"
Write-ProofLine "Settings: $flatSettings"

Assert-FileContains $flatSettings "skyatmosphere = meshes/sky/atmosphere.nif" "generated flat atmosphere setting"
Assert-FileContains $flatSettings "skyclouds = meshes/sky/clouds.nif" "generated flat cloud setting"
Assert-FileContains $flatSettings "skynight01 = meshes/sky/stars.nif" "generated flat stars setting"
Assert-FileContains $flatSettings "skynight02 = meshes/sky/stars.nif" "generated flat alternate stars setting"

Assert-FileNotContains $flatOpenMwLog "meshes/sky_atmosphere\.nif|meshes/sky_clouds_01\.nif|meshes/sky_night_01\.nif" "legacy OpenMW sky mesh path"
Assert-FileNotContains $flatOpenMwLog "marker_error|Failed to compile|failed to compile|linking failed|GLSL.*error|shader.*error" "sky shader/blocker line"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh day atmosphere \(meshes/sky/atmosphere\.nif\)" "runtime wrapped FNV atmosphere"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh clouds \(meshes/sky/clouds\.nif\)" "runtime wrapped FNV clouds"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh next clouds \(meshes/sky/clouds\.nif\)" "runtime wrapped FNV next clouds"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: wrapped sky mesh night atmosphere \(meshes/sky/stars\.nif\)" "runtime wrapped FNV stars"
Assert-FileContains $flatOpenMwLog "FNV/ESM4: enabled sun billboard using texture textures/sky/sun\.dds" "runtime FNV sun texture"

$result = [ordered]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    flatProofDir = $latestFlatProof.FullName
    classification = "runtime-supported"
    checked = @(
        "explicit FNV sky model settings",
        "camera-relative wrapped FNV atmosphere/cloud/star meshes",
        "FNV sun and moon texture selection",
        "no legacy OpenMW sky mesh fallback",
        "no shader/blocker log lines"
    )
}
$resultPath = Join-Path $ProofDir "fnv-sky-runtime-contract.json"
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Contract JSON: $resultPath"
Write-ProofLine "FNV sky runtime contract PASS"
