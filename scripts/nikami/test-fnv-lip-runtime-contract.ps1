param(
    [string]$ProofRoot = "",
    [string]$FnvRoot = "",
    [string]$BsaTool = "",
    [string]$SampleArchive = "Fallout - Voices1.bsa",
    [string]$SampleLipPath = "sound\voice\falloutnv.esm\maleuniquebenny\vdialoguebenny_greeting_00147866_1.lip"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-lip-runtime-contract/$Stamp"
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

function Read-UInt16LE([byte[]]$Bytes, [int]$Offset) {
    return [uint16](([uint16]$Bytes[$Offset]) -bor (([uint16]$Bytes[$Offset + 1]) -shl 8))
}

function Read-UInt32LE([byte[]]$Bytes, [int]$Offset) {
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

Write-ProofLine "FNV LIP runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/mwbase/soundmanager.hpp" "virtual float getSaySoundLipValue" "sound interface exposes LIP mouth values"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.hpp" "std::shared_ptr<const LipSyncData> mLipSync;" "say stream stores parsed LIP sidecar"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "loadVoiceLipSync" "voice path resolves LIP sidecars"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "durationMilliseconds" "LIP header duration parser"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "FNV/ESM4 diag: loaded LIP sync" "runtime LIP parse log"
Assert-Text "apps/openmw/mwsound/soundmanagerimp.cpp" "getStreamOffset" "LIP sampling follows active stream offset"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "getSaySoundLipValue" "FNV mouth animation consumes LIP value"
Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"loaded-pending-runtime" "voice-lip-sync"' "harvest gate records LIP as conditional runtime support"

$metadataPath = Join-Path $ProofDir "lip-runtime-contract.json"
$retailProbe = $null
if (![string]::IsNullOrWhiteSpace($FnvRoot) -or ![string]::IsNullOrWhiteSpace($BsaTool)) {
    if ([string]::IsNullOrWhiteSpace($FnvRoot) -or [string]::IsNullOrWhiteSpace($BsaTool)) {
        throw "FnvRoot and BsaTool must be provided together for the retail LIP probe."
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
        & $BsaTool extract -f $archivePath $SampleLipPath $tempDir | Out-Host
        $extracted = Join-Path $tempDir $SampleLipPath
        if (!(Test-Path -LiteralPath $extracted -PathType Leaf)) {
            throw "bsatool did not produce expected sample: $extracted"
        }

        [byte[]]$bytes = [IO.File]::ReadAllBytes($extracted)
        if ($bytes.Length -lt 17) {
            throw "LIP sample is too small for the runtime header: $($bytes.Length) bytes"
        }

        $version = Read-UInt32LE $bytes 0
        $durationMs = Read-UInt32LE $bytes 4
        $trackCount = Read-UInt32LE $bytes 8
        $keyCount = Read-UInt16LE $bytes 12
        $schema = Read-UInt16LE $bytes 14
        $payloadBytes = $bytes.Length - 16
        if ($version -ne 1 -or $durationMs -le 0 -or $trackCount -le 0 -or $keyCount -le 0 -or $payloadBytes -le 0) {
            throw "Unexpected LIP sample header version=$version durationMs=$durationMs trackCount=$trackCount keyCount=$keyCount payloadBytes=$payloadBytes"
        }

        $retailProbe = [pscustomobject]@{
            archive = $SampleArchive
            path = $SampleLipPath
            version = $version
            durationMilliseconds = $durationMs
            trackCount = $trackCount
            keyCount = $keyCount
            schema = $schema
            payloadBytes = $payloadBytes
            envelopeFramesAt30Hz = [int]([Math]::Ceiling(($durationMs / 1000.0) * 30.0) + 1)
        }
        Write-ProofLine "OK retail LIP header: version=$version durationMs=$durationMs trackCount=$trackCount keyCount=$keyCount schema=$schema payloadBytes=$payloadBytes"
    }
    finally {
        Remove-TempExtract $tempDir $ProofDir
    }
}
else {
    Write-ProofLine "Retail LIP probe skipped: provide -FnvRoot and -BsaTool to verify a local sidecar header."
}

[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    proofDir = $ProofDir
    retailProbe = $retailProbe
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV LIP runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
