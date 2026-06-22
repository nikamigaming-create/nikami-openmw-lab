param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-proof-harness-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-File([string]$RelativePath) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing proof harness file: $RelativePath"
    }
    Write-ProofLine "OK file: $RelativePath"
    return $path
}

function Assert-Text([string]$Path, [string]$Needle, [string]$Description) {
    $text = Get-Content -LiteralPath $Path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing ${Description}: $Needle in $Path"
    }
    Write-ProofLine "OK contract: $Description"
}

Write-ProofLine "FNV proof harness contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$flat = Assert-File "scripts/nikami/run-fnv-flat-proof.ps1"
$doc = Assert-File "scripts/nikami/run-fnv-opening-doc-proof.ps1"
$walk = Assert-File "scripts/nikami/run-fnv-goodsprings-walk-replay-proof.ps1"
$ui = Assert-File "scripts/nikami/run-fnv-ui-baseline-proof.ps1"
$collision = Assert-File "scripts/nikami/run-fnv-goodsprings-collision-path-proof.ps1"
Assert-File "scripts/nikami/run-fnv-opening-vertical-slice.ps1" | Out-Null
Assert-File "scripts/nikami/run-fnv-actor-mugshot-sweep.ps1" | Out-Null
Assert-File "scripts/nikami/run-fnv-easy-pete-angle-sweep.ps1" | Out-Null

foreach ($needle in @(
    "[string]`$BuildDir",
    "[string]`$Configuration",
    "[string]`$FnvData",
    "[string]`$FnvConfigData",
    "[string]`$VcpkgRoot",
    "[string]`$ExtraOsgPluginDir",
    "[string]`$Triplet",
    "[string]`$ProofRoot",
    "[int]`$RunSeconds",
    "[string]`$ScreenshotFrames",
    "[string[]]`$RequireLogPattern",
    "[string]`$TerrainProbePoints",
    "[string]`$TerrainProbeGrid",
    "[switch]`$RequireTerrainProbeFullSupport",
    "[string]`$BootstrapCell",
    "[string]`$ActorTarget",
    "[switch]`$StageActor",
    "[switch]`$RequirePlayerTerrainSupport",
    "[switch]`$RequireFlatCameraSettled"
)) {
    Assert-Text $flat $needle "flat proof parameter $needle"
}

Assert-Text $doc "DocMitchellREF" "Doc Mitchell actor target"
Assert-Text $walk "FNV/ESM4 proof walk: summary reached=1 dropped=0" "walk replay completion assertion"
Assert-Text $ui "ProofGuiMode = `"data`"" "UI baseline DATA pane request"
Assert-Text $collision "Movable static physics classification lines:" "movable static removed-classification anchor"
Assert-Text $collision "captured removed MSTT collision surgery" "movable static removed-surgery anchor"

Write-ProofLine ""
Write-ProofLine "FNV proof harness contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
