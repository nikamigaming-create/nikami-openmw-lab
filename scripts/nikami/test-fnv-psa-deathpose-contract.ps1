param(
    [string]$ProofRoot = "",
    [string]$HarvestDir = ""
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

Write-ProofLine "FNV PSA death-pose contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"known-blocked" "actor-deathpose-animation"' "harvest gate keeps PSA as an actor death-pose runtime blocker"
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

$metadataPath = Join-Path $ProofDir "psa-deathpose-contract.json"
[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    classification = "known-blocked"
    subsystem = "actor-deathpose-animation"
    expectedPsaPaths = $ExpectedPsaPaths
    archiveCounts = $archiveCounts
    psaEntries = $psaEntries
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "PSA entries: $($psaEntries.Count)"
Write-ProofLine "Archive count rows: $($archiveCounts.Count)"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV PSA death-pose contract PASS"
Write-ProofLine "PSA remains blocked until actor death-pose playback is parsed or explicitly bypassed with a runtime proof."
