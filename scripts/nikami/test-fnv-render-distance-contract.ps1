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
. (Join-Path $PSScriptRoot "fnv-runtime-settings.ps1")
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-render-distance-contract/$Stamp"
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

Write-ProofLine "FNV render distance contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

if (!(Test-Path -LiteralPath $FnvData -PathType Container)) {
    throw "Missing FNV data directory: $FnvData"
}

$FnvRootFromData = Get-NikamiFnvRootFromData -FnvData $FnvData
$BlockDistance = Get-NikamiFnvIniNumericSetting -FnvRoot $FnvRootFromData -SettingName "fBlockLoadDistance"
$ExpectedViewingDistance = Get-NikamiFnvViewingDistance -FnvData $FnvData
if ($ExpectedViewingDistance -le 10000) {
    throw "Harvested viewing distance is not beyond the old low fallback: $ExpectedViewingDistance"
}

Write-ProofLine "Harvested fBlockLoadDistance: $($BlockDistance.value)"
Write-ProofLine "Harvest source: $($BlockDistance.source)"
Write-ProofLine "Expected generated OpenMW viewing distance: $ExpectedViewingDistance"
Write-ProofLine ""

$HelperScript = Join-Path $RepoRoot "scripts/nikami/fnv-runtime-settings.ps1"
$FlatScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat.ps1"
$FlatProofScript = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat-proof.ps1"
$VrDeployScript = Join-Path $RepoRoot "scripts/nikami/deploy-fnv-vr-headset.ps1"

Assert-FileContains $HelperScript "fBlockLoadDistance" "runtime settings helper harvests FNV block load distance"
Assert-FileContains $FlatScript "Get-NikamiFnvViewingDistance" "flat runner derives viewing distance from helper"
Assert-FileContains $VrDeployScript "Get-NikamiFnvViewingDistance" "VR/headset deploy derives viewing distance from helper"
Assert-FileNotContains $FlatScript "viewing distance = 10000" "flat runner hardcoded low viewing distance"
Assert-FileNotContains $VrDeployScript "ViewingDistance = 10000|viewing distance = 10000" "VR/headset hardcoded low viewing distance"

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
    -NoSound

$after = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)
$latestFlatProof = $after | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
if ($null -eq $latestFlatProof) {
    $latestFlatProof = $after | Select-Object -First 1
}
if ($null -eq $latestFlatProof) {
    throw "No FNV flat proof directory was produced"
}

$flatSettings = Join-Path $latestFlatProof.FullName "settings.cfg"
$flatOpenMwLog = Join-Path $latestFlatProof.FullName "openmw.log"
Write-ProofLine ""
Write-ProofLine "Flat proof: $($latestFlatProof.FullName)"
Write-ProofLine "Settings: $flatSettings"
Write-ProofLine "OpenMW log: $flatOpenMwLog"

Assert-FileContains $flatSettings "^viewing distance = $ExpectedViewingDistance$" "generated flat viewing distance"
Assert-FileNotContains $flatSettings "^viewing distance = 10000$" "generated low fallback viewing distance"
Assert-FileNotContains $flatOpenMwLog "Failed to compile|failed to compile|linking failed|GLSL.*error|shader.*error" "shader/blocker line"

$result = [ordered]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    flatProofDir = $latestFlatProof.FullName
    classification = "runtime-supported"
    harvestedSetting = "fBlockLoadDistance"
    harvestedSource = $BlockDistance.source
    harvestedValue = $BlockDistance.value
    generatedViewingDistance = $ExpectedViewingDistance
    checked = @(
        "retail FNV INI fBlockLoadDistance harvested from disk",
        "flat generated settings.cfg uses harvested viewing distance",
        "old 10000 viewing distance fallback absent",
        "shader/blocker log lines absent"
    )
}
$resultPath = Join-Path $ProofDir "fnv-render-distance-contract.json"
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Contract JSON: $resultPath"
Write-ProofLine "FNV render distance contract PASS"
