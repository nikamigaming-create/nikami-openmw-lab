param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$ProofRoot = "",
    [string]$HarvestDir = "",
    [string]$ContentLedgerDir = ""
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
$ProofDir = Join-Path $ProofRoot "fnv-extensionless-asset-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-LatestProofDir([string]$Root, [string]$Label) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Label proof root: $Root"
    }
    $dir = Get-ChildItem -LiteralPath $Root -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -eq $dir) {
        throw "Missing $Label proof directory under $Root"
    }
    return $dir.FullName
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

function Read-JsonArray([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Label}: $Path"
    }
    $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $value) {
        return @()
    }
    if ($value -is [System.Array]) {
        return @($value)
    }
    return @($value)
}

Write-ProofLine "FNV extensionless asset contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"<none>" = New-Rule "known-blocked" "extensionless-assets"' "future extensionless harvest entries stay path-owned"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" "archive_extension_totals" "classification emits extension rows from actual harvested entries"

if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
    $HarvestDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-retail-harvest") "FNV retail harvest"
}
else {
    $HarvestDir = (Resolve-Path $HarvestDir).Path
}
Write-ProofLine "HarvestDir: $HarvestDir"

$entryRoot = Join-Path $HarvestDir "bsa-entry-lists"
if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
    throw "Missing harvest BSA entry lists: $entryRoot"
}

$extensionlessEntries = @()
foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
    foreach ($entry in Get-Content -LiteralPath $list.FullName) {
        if ([string]::IsNullOrWhiteSpace([IO.Path]::GetExtension($entry))) {
            $extensionlessEntries += [pscustomobject]@{
                archiveList = $list.Name
                path = $entry.Replace("/", "\")
            }
        }
    }
}

if ($extensionlessEntries.Count -ne 0) {
    $extensionlessEntries | ForEach-Object {
        Write-ProofLine "FAIL extensionless archive entry: $($_.archiveList):$($_.path)"
    }
    throw "Found extensionless FNV archive entries without path-specific ownership: $($extensionlessEntries.Count)"
}
Write-ProofLine "OK extensionless archive entries absent: 0"

$ClassificationScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
$classificationArgs = @{
    ProofRoot = $ProofRoot
    HarvestDir = $HarvestDir
}
if (![string]::IsNullOrWhiteSpace($FnvRoot)) { $classificationArgs.FnvRoot = $FnvRoot }
if (![string]::IsNullOrWhiteSpace($FnvData)) { $classificationArgs.FnvData = $FnvData }
if (![string]::IsNullOrWhiteSpace($ContentLedgerDir)) { $classificationArgs.ContentLedgerDir = $ContentLedgerDir }
& $ClassificationScript @classificationArgs
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}

$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV no-silent-skip classification"
$classificationLedger = Join-Path $ClassificationDir "classification-ledger.json"
$entryClassification = Join-Path $ClassificationDir "archive-entry-classification.jsonl"
$resultPath = Join-Path $ClassificationDir "result.json"
$rows = Read-JsonArray $classificationLedger "classification ledger"
$noneRows = @($rows | Where-Object {
        [string]$_.itemType -eq "bsa-entry-extension" -and [string]$_.identifier -eq "<none>"
    })
if ($noneRows.Count -ne 0) {
    throw "Classification ledger emitted fake extensionless extension rows despite zero harvested entries: $($noneRows.Count)"
}
Write-ProofLine "OK no fake extensionless classification rows: 0"

$entryNoneRows = @()
if (Test-Path -LiteralPath $entryClassification -PathType Leaf) {
    foreach ($line in Get-Content -LiteralPath $entryClassification) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $row = $line | ConvertFrom-Json
        if ([string]$row.extension -eq "<none>") {
            $entryNoneRows += $row
        }
    }
}
if ($entryNoneRows.Count -ne 0) {
    throw "Archive-entry classification emitted extensionless rows despite zero harvested entries: $($entryNoneRows.Count)"
}
Write-ProofLine "OK no extensionless archive-entry classification rows: 0"

$result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
$knownBlocked = [int]$result.counts.knownBlocked
$extensionTotals = $result.PSObject.Properties["archiveExtensionTotals"]
if ($null -ne $extensionTotals -and $null -ne $extensionTotals.Value.PSObject.Properties["<none>"]) {
    throw "Classification result archiveExtensionTotals unexpectedly contains <none>"
}
Write-ProofLine "OK classification known-blocked count after extensionless cleanup: $knownBlocked"

$metadataPath = Join-Path $ProofDir "extensionless-asset-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    proofDir = $ProofDir
    harvestDir = $HarvestDir
    classificationDir = $ClassificationDir
    extensionlessEntries = 0
    classificationNoneRows = 0
    archiveEntryNoneRows = 0
    knownBlocked = $knownBlocked
    classification = "intentionally-excluded-with-proof"
    proof = "Current harvested FNV BSA entry lists contain no extensionless entries; classifier emits extension rows only for harvested extensions."
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "ClassificationDir: $ClassificationDir"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine "FNV extensionless asset contract PASS"
