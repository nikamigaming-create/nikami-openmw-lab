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

$ExpectedPsaPaths = @(
    "meshes\characters\_male\idleanims\deathposes.psa",
    "meshes\creatures\centaur\deathposes.psa",
    "meshes\creatures\deathclaw\deathposes.psa",
    "meshes\creatures\ghoul\deathposes.psa",
    "meshes\creatures\mirelurk\deathposes.psa",
    "meshes\creatures\nvsporecarrier\deathposes.psa",
    "meshes\creatures\sentrybot\deathposes.psa",
    "meshes\creatures\smbehemoth\deathposes.psa",
    "meshes\creatures\smspinebreaker\deathposes.psa",
    "meshes\dlc04\creatures\hillfolk2and3\anims\deathposes.psa",
    "meshes\dlc05\creatures\alien\deathposes.psa",
    "meshes\dlcpitt\creatures\streettrog\deathposes.psa",
    "meshes\nvdlc01\creatures\ghosts\deathposes.psa",
    "meshes\nvdlc02\creatures\yaoguai\idleanims\deathpose.psa"
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
$ProofDir = Join-Path $ProofRoot "fnv-psa-deathpose-contract/$Stamp"
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

Write-ProofLine "FNV PSA death-pose contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"loaded-pending-runtime" "actor-deathpose-animation"' "harvest gate records PSA as loaded-pending death-pose runtime data"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '".psa": ("loaded-pending-runtime"' "classifier records PSA as loaded-pending death-pose runtime data"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PSA_DEATHPOSE_DIAG" "runtime PSA death-pose diagnostic gate"
Assert-Text "apps/openmw/engine.cpp" "FNV/ESM4 proof: PSA death-pose summary count=" "runtime PSA death-pose summary log"
Assert-Text "apps/niftest/niftest.cpp" 'extension == ".psa"' "niftest recognizes PSA as NIF-adjacent tooling input"
Assert-NoText "components/resource/scenemanager.cpp" 'extension == "psa"' "scene manager has no PSA runtime load branch"
Assert-NoText "apps/openmw/mwrender/animation.cpp" ".psa" "main animation runtime has no PSA reader"
Assert-NoText "apps/openmw/mwrender/creatureanimation.cpp" ".psa" "creature animation runtime has no PSA reader"
Assert-NoText "apps/openmw/mwrender/npcanimation.cpp" ".psa" "NPC animation runtime has no PSA reader"

$entryRoot = Join-Path $HarvestDir "bsa-entry-lists"
if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
    throw "Missing harvest BSA entry lists: $entryRoot"
}

$psaEntries = @()
foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
    foreach ($entry in Get-Content -LiteralPath $list.FullName) {
        if ([IO.Path]::GetExtension($entry).Equals(".psa", [System.StringComparison]::OrdinalIgnoreCase)) {
            $normalized = $entry.Replace("/", "\").ToLowerInvariant()
            $psaEntries += [pscustomobject]@{
                archiveList = $list.Name
                path = $normalized
            }
        }
    }
}

$actual = @($psaEntries | ForEach-Object { $_.path } | Sort-Object)
$expected = @($ExpectedPsaPaths | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object)
if ($actual.Count -ne $expected.Count) {
    throw "Unexpected PSA entry count: actual=$($actual.Count) expected=$($expected.Count)"
}
for ($i = 0; $i -lt $expected.Count; ++$i) {
    if (!$actual[$i].Equals($expected[$i], [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected PSA entry: actual=$($actual[$i]) expected=$($expected[$i])"
    }
    if (!$actual[$i].StartsWith("meshes\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "PSA path is not under meshes: $($actual[$i])"
    }
    if ($actual[$i].IndexOf("deathpose", [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "PSA path is not a death-pose asset: $($actual[$i])"
    }
    Write-ProofLine "OK PSA harvest path: $($actual[$i])"
}

$archiveCounts = @{}
foreach ($entry in $psaEntries) {
    if (!$archiveCounts.ContainsKey($entry.archiveList)) {
        $archiveCounts[$entry.archiveList] = 0
    }
    $archiveCounts[$entry.archiveList] += 1
}

if ([string]::IsNullOrWhiteSpace($FnvData) -or !(Test-Path -LiteralPath $FnvData -PathType Container)) {
    throw "Missing FNV data directory for runtime PSA probe: $FnvData"
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
    -FnvPsaDeathPoseDiag `
    -NoSound

$flatProofDir = Get-LatestProofDir $flatProofRoot $beforeFlatProofs
$openMwLog = Join-Path $flatProofDir "openmw.log"
if (!(Test-Path -LiteralPath $openMwLog -PathType Leaf)) {
    throw "Missing OpenMW log from flat proof: $openMwLog"
}
Write-ProofLine ""
Write-ProofLine "Flat proof: $flatProofDir"
Write-ProofLine "OpenMW log: $openMwLog"

$loadFailureCount = Count-LogMatches $openMwLog "FNV/ESM4 proof: PSA death-pose load failed"
if ($loadFailureCount -gt 0) {
    throw "Runtime PSA death-pose load failures found: $loadFailureCount"
}
Write-ProofLine "OK no runtime PSA death-pose load failures: 0"

$loadedPattern = 'FNV/ESM4 proof: PSA death-pose loaded path=(?<path>[^ ]+) archive="(?<archive>[^"]*)" bytes=(?<bytes>[0-9]+)'
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
    throw "Unexpected runtime PSA death-pose load count: actual=$($runtimeRows.Count) expected=$($expected.Count)"
}

$runtimePathSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$totalRuntimeBytes = [int64]0
foreach ($row in $runtimeRows) {
    if ($row.bytes -le 0) {
        throw "Runtime PSA death-pose row had no bytes: $($row.path)"
    }
    [void]$runtimePathSet.Add($row.path)
    $totalRuntimeBytes += $row.bytes
}
foreach ($expectedPath in $expected) {
    $normalizedExpected = $expectedPath.Replace("\", "/")
    if (!$runtimePathSet.Contains($normalizedExpected)) {
        throw "Runtime PSA death-pose proof did not load expected path: $normalizedExpected"
    }
    Write-ProofLine "OK runtime PSA death-pose load: $normalizedExpected"
}

$summaryPattern = 'FNV/ESM4 proof: PSA death-pose summary count=(?<count>[0-9]+) totalBytes=(?<bytes>[0-9]+) playbackBinding=loaded-pending-runtime'
$summaryMatch = Select-String -LiteralPath $openMwLog -Pattern $summaryPattern | Select-Object -First 1
if ($null -eq $summaryMatch) {
    throw "Missing runtime PSA death-pose summary line in $openMwLog"
}
$summaryCount = [int]$summaryMatch.Matches[0].Groups["count"].Value
$summaryBytes = [int64]$summaryMatch.Matches[0].Groups["bytes"].Value
if ($summaryCount -ne $expected.Count) {
    throw "Unexpected runtime PSA summary count: actual=$summaryCount expected=$($expected.Count)"
}
if ($summaryBytes -ne $totalRuntimeBytes -or $summaryBytes -le 0) {
    throw "Unexpected runtime PSA summary bytes: actual=$summaryBytes expected=$totalRuntimeBytes"
}
Write-ProofLine "OK runtime PSA death-pose summary: count=$summaryCount totalBytes=$summaryBytes"

$metadataPath = Join-Path $ProofDir "psa-deathpose-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    flatProofDir = $flatProofDir
    openMwLog = $openMwLog
    classification = "loaded-pending-runtime"
    subsystem = "actor-deathpose-animation"
    expectedPsaPaths = $ExpectedPsaPaths
    archiveCounts = $archiveCounts
    psaEntries = $psaEntries
    runtimeRows = $runtimeRows
    runtimeTotalBytes = $totalRuntimeBytes
    pendingRuntimeGate = "Parse PSA death-pose animation semantics and bind actor/creature death playback before promoting to runtime-supported."
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "PSA entries: $($psaEntries.Count)"
Write-ProofLine "Archive count rows: $($archiveCounts.Count)"
Write-ProofLine "Runtime loaded rows: $($runtimeRows.Count)"
Write-ProofLine "Runtime loaded bytes: $totalRuntimeBytes"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV PSA death-pose contract PASS"
Write-ProofLine "PSA death poses are loaded-pending-runtime until actor/creature death playback is parsed and bound."
