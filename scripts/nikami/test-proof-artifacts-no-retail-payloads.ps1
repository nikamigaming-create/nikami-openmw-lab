param(
    [string]$ProofRoot = "",
    [string]$RepoRoot = ""
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
$ProofRoot = (Resolve-Path $ProofRoot).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "proof-artifact-payload-safety/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

$PayloadExtensions = @(
    ".bsa", ".esm", ".esp", ".ini",
    ".egt", ".egm", ".tri", ".lip", ".ogg", ".wav", ".mp3", ".dds", ".nif", ".kf",
    ".psd", ".tai", ".ctl", ".dat", ".spt", ".dlodsettings", ".psa"
)

$RawByteTextPatterns = @(
    '"hex32"\s*:',
    '"rawBytes"\s*:',
    '"sampleBytes"\s*:',
    '"firstBytes"\s*:',
    '"base64"\s*:',
    '"contentBase64"\s*:',
    '"bytesBase64"\s*:'
)

$ContentLedgerTextPayloadPatterns = @(
    '"source"\s*:',
    '"questName"\s*:',
    '"topicName"\s*:',
    '"response"\s*:',
    '"notes"\s*:',
    '"edits"\s*:',
    '"text"\s*:',
    '"fullName"\s*:',
    '"value"\s*:\s*"'
)

Write-ProofLine "Proof artifact payload safety $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofRoot: $ProofRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$payloadFiles = @()
foreach ($file in Get-ChildItem -LiteralPath $ProofRoot -Recurse -File -Force) {
    if ($PayloadExtensions -contains $file.Extension.ToLowerInvariant()) {
        $payloadFiles += $file.FullName
    }
}

$tempExtractDirs = @(
    Get-ChildItem -LiteralPath $ProofRoot -Recurse -Directory -Force |
        Where-Object { $_.Name -ieq "temp-extract" } |
        Select-Object -ExpandProperty FullName
)

$rawByteMatches = @()
$textFiles = Get-ChildItem -LiteralPath $ProofRoot -Recurse -File -Force |
    Where-Object { @(".json", ".jsonl", ".txt", ".log", ".csv", ".md") -contains $_.Extension.ToLowerInvariant() }
foreach ($file in $textFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($pattern in $RawByteTextPatterns) {
        if ($text -match $pattern) {
            $rawByteMatches += "$($file.FullName) pattern=$pattern"
        }
    }
}

$contentLedgerTextMatches = @()
$contentLedgerRoot = Join-Path $ProofRoot "fnv-content-ledger"
if (Test-Path -LiteralPath $contentLedgerRoot -PathType Container) {
    $contentLedgerFiles = Get-ChildItem -LiteralPath $contentLedgerRoot -Recurse -File -Force |
        Where-Object { $_.Extension.ToLowerInvariant() -eq ".json" }
    foreach ($file in $contentLedgerFiles) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        foreach ($pattern in $ContentLedgerTextPayloadPatterns) {
            if ($text -match $pattern) {
                $contentLedgerTextMatches += "$($file.FullName) pattern=$pattern"
            }
        }
    }
}

Write-ProofLine "Payload extension files: $($payloadFiles.Count)"
Write-ProofLine "Temp extract dirs: $($tempExtractDirs.Count)"
Write-ProofLine "Raw byte text matches: $($rawByteMatches.Count)"
Write-ProofLine "Content ledger text payload matches: $($contentLedgerTextMatches.Count)"

if ($payloadFiles.Count -gt 0) {
    $payloadFiles | ForEach-Object { Write-ProofLine "FAIL payload file: $_" }
}
if ($tempExtractDirs.Count -gt 0) {
    $tempExtractDirs | ForEach-Object { Write-ProofLine "FAIL temp extract dir: $_" }
}
if ($rawByteMatches.Count -gt 0) {
    $rawByteMatches | ForEach-Object { Write-ProofLine "FAIL raw byte text: $_" }
}
if ($contentLedgerTextMatches.Count -gt 0) {
    $contentLedgerTextMatches | ForEach-Object { Write-ProofLine "FAIL content ledger text payload: $_" }
}

if ($payloadFiles.Count -gt 0 -or $tempExtractDirs.Count -gt 0 -or $rawByteMatches.Count -gt 0 -or $contentLedgerTextMatches.Count -gt 0) {
    throw "Proof artifact payload safety failed. See $SummaryFile."
}

Write-ProofLine ""
Write-ProofLine "Proof artifact payload safety PASS"
Write-ProofLine "ProofDir: $ProofDir"
