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
$ProofDir = Join-Path $ProofRoot "fnv-config-layering-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-Order([string]$Path, [string[]]$Needles, [string]$Description) {
    $text = Get-Content -LiteralPath $Path -Raw
    $last = -1
    foreach ($needle in $Needles) {
        $index = $text.IndexOf($needle, $last + 1, [System.StringComparison]::Ordinal)
        if ($index -lt 0) {
            throw "Missing layering marker '$needle' in $Path for $Description"
        }
        if ($index -le $last) {
            throw "Layering order failure for $Description at marker '$needle' in $Path"
        }
        $last = $index
    }
    Write-ProofLine "OK layering order: $Description"
}

Write-ProofLine "FNV config layering contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$FlatRunner = Join-Path $RepoRoot "scripts/nikami/run-fnv-flat.ps1"
$HeadsetDeploy = Join-Path $RepoRoot "scripts/nikami/deploy-fnv-vr-headset.ps1"

Assert-Order $FlatRunner @(
    'data=$((Join-Path $Resources "vfs-mw").Replace("\", "/"))',
    'data=$($FnvData.Replace("\", "/"))',
    '$OptionalDataLine'
) "desktop flat config resources < vanilla FNV < overlay"

Assert-Order $HeadsetDeploy @(
    'data=$DeviceRoot/resources/vfs-mw',
    'data=$FnvDeviceData',
    '$OptionalOverlayLine'
) "headset config resources < vanilla FNV < overlay"

Write-ProofLine ""
Write-ProofLine "FNV config layering contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
