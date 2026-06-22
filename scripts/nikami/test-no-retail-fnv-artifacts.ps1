param(
    [string]$RepoRoot = "",
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
}
else {
    $RepoRoot = (Resolve-Path $RepoRoot).Path
}

if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$GateProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
$ProofDir = Join-Path $GateProofRoot "no-retail-fnv-artifacts/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-RetailArtifactFiles([string]$Root) {
    if ([string]::IsNullOrWhiteSpace($Root) -or !(Test-Path -LiteralPath $Root -PathType Container)) {
        return @()
    }

    $resolved = (Resolve-Path $Root).Path
    return @(
        Get-ChildItem -LiteralPath $resolved -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $path = $_.FullName
                $_.Extension -in ".bsa", ".esm", ".esp", ".ini" -and
                $path -notmatch '\\\.git\\' -and
                $path -notmatch '\\build[^\\]*\\' -and
                $path -notmatch '\\CMakeFiles\\'
            } |
            Sort-Object FullName
    )
}

Write-ProofLine "No retail FNV artifact gate $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofRoot: $ProofRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""
Write-ProofLine "Policy: Nikami publish/proof outputs must not contain retail/mod Fallout New Vegas .bsa, .esm, .esp, or .ini payload files."
Write-ProofLine ""

$violations = @()
foreach ($root in @($RepoRoot, $ProofRoot)) {
    $files = @(Get-RetailArtifactFiles $root)
    Write-ProofLine "Scanned: $root"
    Write-ProofLine "Retail artifact matches: $($files.Count)"
    foreach ($file in $files) {
        $violations += $file
        Write-ProofLine "VIOLATION: $($file.FullName) ($($file.Length) bytes)"
    }
}

if ($violations.Count -gt 0) {
    throw "Retail FNV artifacts found: $($violations.Count). See $SummaryFile"
}

Write-ProofLine ""
Write-ProofLine "No retail FNV artifact gate PASS"
Write-ProofLine "ProofDir: $ProofDir"
