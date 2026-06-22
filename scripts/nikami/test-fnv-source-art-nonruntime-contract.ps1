param(
    [string]$HarvestDir = "",
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) {
        throw "No FNV harvest proof root found. Run scripts/nikami/harvest-fnv-retail-ledger.ps1 first."
    }
    $latest = Get-ChildItem -LiteralPath $harvestRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -eq $latest) {
        throw "No FNV harvest proof directories found under $harvestRoot."
    }
    $HarvestDir = $latest.FullName
}
$HarvestDir = (Resolve-Path $HarvestDir).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-source-art-nonruntime/$Stamp"
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
    Write-ProofLine "OK contract: $Description"
}

Write-ProofLine "FNV PSD source-art non-runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"asset-harvested-non-runtime" "source-art-leftover"' "harvest gate records PSD as accounted non-runtime source art"
Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '".dds" = New-Rule "runtime-supported"' "DDS siblings are runtime-supported texture assets"
Assert-Text "components/resource/imagemanager.cpp" "ImageManager::getImage" "runtime texture path consumes DDS, not PSD source art"

$entryRoot = Join-Path $HarvestDir "bsa-entry-lists"
if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
    throw "Missing harvest BSA entry lists: $entryRoot"
}

$allEntries = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$psdRows = @()
foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
    foreach ($entry in Get-Content -LiteralPath $list.FullName) {
        [void]$allEntries.Add($entry)
        if ([IO.Path]::GetExtension($entry).Equals(".psd", [System.StringComparison]::OrdinalIgnoreCase)) {
            $psdRows += [pscustomobject]@{
                archiveList = $list.Name
                path = $entry
                ddsSibling = [IO.Path]::ChangeExtension($entry, ".dds")
            }
        }
    }
}

if ($psdRows.Count -eq 0) {
    throw "No PSD entries found in harvest; remove the PSD non-runtime rule or update this proof."
}

foreach ($row in $psdRows) {
    if (!$allEntries.Contains($row.ddsSibling)) {
        throw "PSD source art lacks a runtime DDS sibling: $($row.path) expected=$($row.ddsSibling)"
    }
    Write-ProofLine "OK PSD source art accounted: $($row.path) -> $($row.ddsSibling)"
}

$metadataPath = Join-Path $ProofDir "source-art-nonruntime.json"
$psdJsonRows = @($psdRows | ForEach-Object {
    [pscustomobject]@{
        archiveList = [string]$_.archiveList
        path = [string]$_.path
        ddsSibling = [string]$_.ddsSibling
    }
})
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    psdEntries = $psdJsonRows
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "PSD entries: $($psdRows.Count)"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV PSD source-art non-runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
