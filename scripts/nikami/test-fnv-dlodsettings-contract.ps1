param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ProofRoot = "",
    [string]$HarvestDir = "",
    [int]$RunSeconds = 8
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
if ([string]::IsNullOrWhiteSpace($FnvData) -and ![string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvData = Join-Path $FnvRoot "Data"
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

function Count-LogMatches([string]$Path, [string]$Pattern) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return 0
    }
    return @((Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)).Count
}

function Get-LatestProofDir([string]$Root, [string[]]$Before) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing proof root: $Root"
    }
    $dirs = @(Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
    $newest = $dirs | Where-Object { $Before -notcontains $_.FullName } | Select-Object -First 1
    if ($null -ne $newest) {
        return $newest.FullName
    }
    if ($dirs.Count -gt 0) {
        return $dirs[0].FullName
    }
    throw "No proof directories found under $Root"
}

Write-ProofLine "FNV DLOD settings contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"loaded-pending-runtime" "distant-lod"' "harvest gate records DLOD settings as loaded-pending runtime data"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '".dlodsettings": ("loaded-pending-runtime"' "classifier records DLOD settings as loaded-pending runtime data"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_DLODSETTINGS_DIAG" "runtime DLOD settings diagnostic gate"
Assert-Text "apps/openmw/engine.cpp" "FNV/ESM4 proof: DLOD settings summary count=" "runtime DLOD settings summary log"
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

if ([string]::IsNullOrWhiteSpace($FnvData) -or !(Test-Path -LiteralPath $FnvData -PathType Container)) {
    throw "Missing FNV data directory for runtime DLOD probe: $FnvData"
}

$flatProofRoot = Join-Path $ProofRoot "fnv-flat-proof"
$beforeFlatProofs = @()
if (Test-Path -LiteralPath $flatProofRoot -PathType Container) {
    $beforeFlatProofs = @(Get-ChildItem -LiteralPath $flatProofRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -FnvDlodSettingsDiag `
    -NoSound

$flatProofDir = Get-LatestProofDir $flatProofRoot $beforeFlatProofs
$openMwLog = Join-Path $flatProofDir "openmw.log"
if (!(Test-Path -LiteralPath $openMwLog -PathType Leaf)) {
    throw "Missing OpenMW log from flat proof: $openMwLog"
}
Write-ProofLine ""
Write-ProofLine "Flat proof: $flatProofDir"
Write-ProofLine "OpenMW log: $openMwLog"

$loadFailureCount = Count-LogMatches $openMwLog "FNV/ESM4 proof: DLOD settings load failed"
if ($loadFailureCount -gt 0) {
    throw "Runtime DLOD settings load failures found: $loadFailureCount"
}
Write-ProofLine "OK no runtime DLOD settings load failures: 0"

$loadedPattern = 'FNV/ESM4 proof: DLOD settings loaded path=(?<path>[^ ]+) archive="(?<archive>[^"]*)" bytes=(?<bytes>[0-9]+)'
$runtimeRows = @()
foreach ($line in (Select-String -LiteralPath $openMwLog -Pattern $loadedPattern -AllMatches)) {
    foreach ($match in $line.Matches) {
        $runtimeRows += [pscustomobject]@{
            path = $match.Groups["path"].Value
            archive = $match.Groups["archive"].Value
            bytes = [int64]$match.Groups["bytes"].Value
        }
    }
}
if ($runtimeRows.Count -ne $expected.Count) {
    throw "Unexpected runtime DLOD settings load count: actual=$($runtimeRows.Count) expected=$($expected.Count)"
}

$runtimePathSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$totalRuntimeBytes = [int64]0
foreach ($row in $runtimeRows) {
    if ($row.bytes -le 0) {
        throw "Runtime DLOD settings row had no bytes: $($row.path)"
    }
    [void]$runtimePathSet.Add($row.path)
    $totalRuntimeBytes += $row.bytes
}
foreach ($expectedPath in $expected) {
    $normalizedExpected = $expectedPath.Replace("\", "/")
    if (!$runtimePathSet.Contains($normalizedExpected)) {
        throw "Runtime DLOD settings proof did not load expected path: $normalizedExpected"
    }
    Write-ProofLine "OK runtime DLOD settings load: $normalizedExpected"
}

$summaryPattern = 'FNV/ESM4 proof: DLOD settings summary count=(?<count>[0-9]+) totalBytes=(?<bytes>[0-9]+) pagingBinding=loaded-pending-runtime'
$summaryMatch = Select-String -LiteralPath $openMwLog -Pattern $summaryPattern | Select-Object -First 1
if ($null -eq $summaryMatch) {
    throw "Missing runtime DLOD settings summary line in $openMwLog"
}
$summaryCount = [int]$summaryMatch.Matches[0].Groups["count"].Value
$summaryBytes = [int64]$summaryMatch.Matches[0].Groups["bytes"].Value
if ($summaryCount -ne $expected.Count) {
    throw "Unexpected runtime DLOD summary count: actual=$summaryCount expected=$($expected.Count)"
}
if ($summaryBytes -ne $totalRuntimeBytes -or $summaryBytes -le 0) {
    throw "Unexpected runtime DLOD summary bytes: actual=$summaryBytes expected=$totalRuntimeBytes"
}
Write-ProofLine "OK runtime DLOD settings summary: count=$summaryCount totalBytes=$summaryBytes"

$metadataPath = Join-Path $ProofDir "dlodsettings-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    flatProofDir = $flatProofDir
    openMwLog = $openMwLog
    classification = "loaded-pending-runtime"
    subsystem = "distant-lod"
    expectedDlodSettingsPaths = $ExpectedDlodSettingsPaths
    archiveCounts = $archiveCounts
    dlodSettingsEntries = $dlodEntries
    runtimeRows = $runtimeRows
    runtimeTotalBytes = $totalRuntimeBytes
    pendingRuntimeGate = "Route parsed DLOD settings into terrain/object paging LOD decisions before promoting to runtime-supported."
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "DLOD settings entries: $($dlodEntries.Count)"
Write-ProofLine "Archive count rows: $($archiveCounts.Count)"
Write-ProofLine "Runtime loaded rows: $($runtimeRows.Count)"
Write-ProofLine "Runtime loaded bytes: $totalRuntimeBytes"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV DLOD settings contract PASS"
Write-ProofLine "DLOD settings are loaded-pending-runtime until parsed and routed into terrain/object paging runtime decisions."
