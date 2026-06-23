param(
    [string]$ProofRoot = "",
    [string]$HarvestDir = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ExpectedDlodSettingsPaths = @(
    "lodsettings\bouldercityworld.dlodsettings",
    "lodsettings\freesidefortworld.dlodsettings",
    "lodsettings\freesidenorthworld.dlodsettings",
    "lodsettings\freesideworld.dlodsettings",
    "lodsettings\gamorrahworld.dlodsettings",
    "lodsettings\lucky38world.dlodsettings",
    "lodsettings\nvdlc01easttownn.dlodsettings",
    "lodsettings\nvdlc01easttowns.dlodsettings",
    "lodsettings\nvdlc01villa.dlodsettings",
    "lodsettings\nvdlc01villachristine.dlodsettings",
    "lodsettings\nvdlc01villadean.dlodsettings",
    "lodsettings\nvdlc01westtownn.dlodsettings",
    "lodsettings\nvdlc01westtowns.dlodsettings",
    "lodsettings\nvdlc02zioncanyon.dlodsettings",
    "lodsettings\nvdlc03bigmt.dlodsettings",
    "lodsettings\nvdlc04dividevistaworld.dlodsettings",
    "lodsettings\nvdlc04divideworld.dlodsettings",
    "lodsettings\nvdlc04nukelegion.dlodsettings",
    "lodsettings\nvdlc04nukencr.dlodsettings",
    "lodsettings\nvdlc04nukesilo2.dlodsettings",
    "lodsettings\nvdlc04road01world.dlodsettings",
    "lodsettings\nvdlc04road02world.dlodsettings",
    "lodsettings\thestripworldnew.dlodsettings",
    "lodsettings\wastelandnv.dlodsettings",
    "lodsettings\wastelandnvmini.dlodsettings"
)

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
$ProofRoot = (Resolve-Path $ProofRoot).Path
$HarvestDir = (Resolve-Path $HarvestDir).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-dlodsettings-contract/$Stamp"
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

function Assert-NoText([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing file for ${Description}: $RelativePath"
    }

    $text = Get-Content -LiteralPath $path -Raw
    if ($text.Contains($Needle)) {
        throw "Unexpected ${Description}: $Needle in $RelativePath"
    }
    Write-ProofLine "OK negative contract: $Description"
}

Write-ProofLine "FNV DLOD settings contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"blocked-runtime-support" "distant-lod"' "harvest gate keeps DLOD settings as a runtime blocker"
Assert-Text "apps/openmw/mwrender/renderingmanager.cpp" "QuadTreeWorld" "terrain quad tree runtime exists"
Assert-Text "apps/openmw/mwrender/objectpaging.cpp" "getLODMeshName" "object paging currently derives LOD mesh names"
Assert-Text "components/misc/resourcehelpers.cpp" "getLODMeshName" "resource helper uses filename-pattern LOD lookup"
Assert-Text "components/esm4/loadstat.cpp" "mLOD" "STAT loader captures ESM4 LOD model strings"
Assert-Text "components/esm4/loadrefr.cpp" "XLOD" "REFR loader sees XLOD data"
Assert-NoText "apps/openmw/mwrender/objectpaging.cpp" ".dlodsettings" "object paging does not consume DLOD settings files"
Assert-NoText "apps/openmw/mwrender/renderingmanager.cpp" ".dlodsettings" "rendering manager does not consume DLOD settings files"
Assert-NoText "components/misc/resourcehelpers.cpp" ".dlodsettings" "resource helper does not consume DLOD settings files"

$entryRoot = Join-Path $HarvestDir "bsa-entry-lists"
if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
    throw "Missing harvest BSA entry lists: $entryRoot"
}

$dlodEntries = @()
foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
    foreach ($entry in Get-Content -LiteralPath $list.FullName) {
        if ([IO.Path]::GetExtension($entry).Equals(".dlodsettings", [System.StringComparison]::OrdinalIgnoreCase)) {
            $normalized = $entry.Replace("/", "\").ToLowerInvariant()
            $dlodEntries += [pscustomobject]@{
                archiveList = $list.Name
                path = $normalized
                worldspaceName = [IO.Path]::GetFileNameWithoutExtension($normalized)
            }
        }
    }
}

$actual = @($dlodEntries | ForEach-Object { $_.path } | Sort-Object)
$expected = @($ExpectedDlodSettingsPaths | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object)
if ($actual.Count -ne $expected.Count) {
    throw "Unexpected DLOD settings entry count: actual=$($actual.Count) expected=$($expected.Count)"
}
for ($i = 0; $i -lt $expected.Count; ++$i) {
    if (!$actual[$i].Equals($expected[$i], [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected DLOD settings entry: actual=$($actual[$i]) expected=$($expected[$i])"
    }
    if (!$actual[$i].StartsWith("lodsettings\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "DLOD settings path is not under lodsettings: $($actual[$i])"
    }
    Write-ProofLine "OK DLOD settings harvest path: $($actual[$i])"
}

$archiveCounts = @{}
foreach ($entry in $dlodEntries) {
    if (!$archiveCounts.ContainsKey($entry.archiveList)) {
        $archiveCounts[$entry.archiveList] = 0
    }
    $archiveCounts[$entry.archiveList] += 1
}

$metadataPath = Join-Path $ProofDir "dlodsettings-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    classification = "blocked-runtime-support"
    subsystem = "distant-lod"
    expectedDlodSettingsPaths = $ExpectedDlodSettingsPaths
    archiveCounts = $archiveCounts
    dlodSettingsEntries = $dlodEntries
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "DLOD settings entries: $($dlodEntries.Count)"
Write-ProofLine "Archive count rows: $($archiveCounts.Count)"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV DLOD settings contract PASS"
Write-ProofLine "DLOD settings remain blocked until parsed and routed into terrain/object paging runtime decisions."
