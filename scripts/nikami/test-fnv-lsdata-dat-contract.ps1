param(
    [string]$ProofRoot = "",
    [string]$HarvestDir = "",
    [string]$FnvRoot = "",
    [string]$BsaTool = "",
    [string]$SampleArchive = "Fallout - Misc.bsa"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ExpectedDatPaths = @(
    "lsdata\dtc6dal.dat",
    "lsdata\dtc6dl.dat",
    "lsdata\wt16m9bs.dat",
    "lsdata\wt16m9fs.dat",
    "lsdata\wt8s9bs.dat",
    "lsdata\wt8s9fs.dat"
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
$HarvestDir = (Resolve-Path $HarvestDir).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-lsdata-dat-contract/$Stamp"
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

function Read-FirstBytes([string]$Path, [int]$Count = 32) {
    [byte[]]$buffer = New-Object byte[] $Count
    $stream = [IO.File]::OpenRead($Path)
    try {
        $read = $stream.Read($buffer, 0, $buffer.Length)
    }
    finally {
        $stream.Dispose()
    }
    if ($read -lt $Count) {
        [Array]::Resize([ref]$buffer, $read)
    }
    return ,$buffer
}

function Read-UInt32LE([byte[]]$Bytes, [int]$Offset) {
    if ($Bytes.Length -lt $Offset + 4) {
        return 0
    }
    return [uint32](([uint32]$Bytes[$Offset]) -bor (([uint32]$Bytes[$Offset + 1]) -shl 8) -bor (([uint32]$Bytes[$Offset + 2]) -shl 16) -bor (([uint32]$Bytes[$Offset + 3]) -shl 24))
}

function Remove-TempExtract([string]$Path, [string]$AllowedRoot) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) {
        return
    }

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $AllowedRoot).Path
    if (!$resolvedPath.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove temp extract outside proof dir: $resolvedPath"
    }

    Remove-Item -LiteralPath $resolvedPath -Recurse -Force
}

Write-ProofLine "FNV LSDATA DAT contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"non-runtime-support-file" "lip-generation-support-tables"' "harvest gate records LSDATA DAT as accounted non-runtime support tables"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "loadVoiceLipSync" "runtime voice path consumes baked LIP sidecars"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "FNV/ESM4 diag: loaded LIP sync" "runtime LIP metadata is parsed"
Assert-Text "apps/openmw/mwbase/soundmanager.hpp" "getSaySoundLipValue" "sound interface exposes runtime mouth values"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "getSaySoundLipValue" "mouth animation consumes runtime LIP values"

$entryRoot = Join-Path $HarvestDir "bsa-entry-lists"
if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
    throw "Missing harvest BSA entry lists: $entryRoot"
}

$datEntries = @()
foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
    foreach ($entry in Get-Content -LiteralPath $list.FullName) {
        if ([IO.Path]::GetExtension($entry).Equals(".dat", [System.StringComparison]::OrdinalIgnoreCase)) {
            $datEntries += $entry
        }
    }
}
$datEntries = @($datEntries | Sort-Object)
$expected = @($ExpectedDatPaths | Sort-Object)
if ($datEntries.Count -ne $expected.Count) {
    throw "Unexpected DAT entry count: actual=$($datEntries.Count) expected=$($expected.Count)"
}
for ($i = 0; $i -lt $expected.Count; ++$i) {
    if (!$datEntries[$i].Equals($expected[$i], [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected DAT entry: actual=$($datEntries[$i]) expected=$($expected[$i])"
    }
    Write-ProofLine "OK DAT harvest path: $($datEntries[$i])"
}

$metadataPath = Join-Path $ProofDir "lsdata-dat-contract.json"
$retailProbe = @()
if (![string]::IsNullOrWhiteSpace($FnvRoot) -or ![string]::IsNullOrWhiteSpace($BsaTool)) {
    if ([string]::IsNullOrWhiteSpace($FnvRoot) -or [string]::IsNullOrWhiteSpace($BsaTool)) {
        throw "FnvRoot and BsaTool must be provided together for the retail DAT probe."
    }

    $archivePath = Join-Path (Join-Path $FnvRoot "Data") $SampleArchive
    if (!(Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw "Missing sample archive: $archivePath"
    }
    if (!(Test-Path -LiteralPath $BsaTool -PathType Leaf)) {
        throw "Missing bsatool: $BsaTool"
    }

    $tempDir = Join-Path $ProofDir "temp-extract"
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
    try {
        foreach ($sample in $ExpectedDatPaths) {
            & $BsaTool extract -f $archivePath $sample $tempDir | Out-Host
            $extracted = Join-Path $tempDir $sample
            if (!(Test-Path -LiteralPath $extracted -PathType Leaf)) {
                throw "bsatool did not produce expected DAT sample: $extracted"
            }

            $item = Get-Item -LiteralPath $extracted
            [byte[]]$bytes = Read-FirstBytes $extracted 32
            $retailProbe += [pscustomobject]@{
                archive = $SampleArchive
                path = $sample
                length = $item.Length
                firstUInt32 = Read-UInt32LE $bytes 0
                secondUInt32 = Read-UInt32LE $bytes 4
            }
            Write-ProofLine "OK retail DAT metadata: path=$sample length=$($item.Length) firstUInt32=$(Read-UInt32LE $bytes 0)"
        }
    }
    finally {
        Remove-TempExtract $tempDir $ProofDir
    }
}
else {
    Write-ProofLine "Retail DAT probe skipped: provide -FnvRoot and -BsaTool to verify local LSDATA table metadata."
}

[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    expectedDatPaths = $ExpectedDatPaths
    retailProbe = $retailProbe
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "DAT entries: $($datEntries.Count)"
Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV LSDATA DAT contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
