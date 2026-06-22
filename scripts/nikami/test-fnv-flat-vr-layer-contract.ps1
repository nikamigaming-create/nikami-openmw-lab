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
$ProofDir = Join-Path $ProofRoot "fnv-flat-vr-layer-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-CodeString([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source file for ${Description}: $RelativePath"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing code anchor for ${Description}: $Needle"
    }
    Write-ProofLine "OK code anchor: $Description"
}

function Assert-CodeAbsent([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    $text = Get-Content -LiteralPath $path -Raw
    if ($text.Contains($Needle)) {
        throw "Unexpected code anchor for ${Description}: $Needle"
    }
    Write-ProofLine "OK absent anchor: $Description"
}

Write-ProofLine "FNV flat/VR layer contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-CodeString "scripts/nikami/run-fnv-flat.ps1" "[switch]`$IncludeFnvrPlugin" "flat runner exposes FNVR opt-in"
Assert-CodeString "scripts/nikami/run-fnv-flat.ps1" '$FnvrContentLine = "content=FNVR.esp"' "flat runner builds FNVR content line only through variable"
Assert-CodeString "scripts/nikami/run-fnv-flat.ps1" 'if ($IncludeFnvrPlugin)' "flat runner guards FNVR plugin"
Assert-CodeAbsent "scripts/nikami/run-fnv-flat.ps1" "`ncontent=FNVR.esp`n" "flat generated config has no unconditional FNVR content line"
Assert-CodeString "scripts/nikami/run-fnv-flat-proof.ps1" "[switch]`$IncludeFnvrPlugin" "flat proof exposes FNVR opt-in"
Assert-CodeString "scripts/nikami/run-fnv-flat-proof.ps1" '$flatArgs.IncludeFnvrPlugin = $true' "flat proof passes FNVR opt-in only when requested"
Assert-CodeString "scripts/nikami/deploy-fnv-vr-headset.ps1" '"FNVR.esp"' "headset/VR deploy still includes FNVR plugin"

Write-ProofLine ""
Write-ProofLine "FNV flat/VR layer contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
