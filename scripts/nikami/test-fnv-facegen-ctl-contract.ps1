param(
    [string]$ProofRoot = "",
    [string]$FnvRoot = "",
    [string]$BsaTool = "",
    [string]$SampleArchive = "Fallout - Misc.bsa",
    [string]$SampleCtlPath = "facegen\si.ctl"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-facegen-ctl-contract/$Stamp"
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

function Read-UInt32LE([byte[]]$Bytes, [int]$Offset) {
    return [uint32](([uint32]$Bytes[$Offset]) -bor (([uint32]$Bytes[$Offset + 1]) -shl 8) -bor (([uint32]$Bytes[$Offset + 2]) -shl 16) -bor (([uint32]$Bytes[$Offset + 3]) -shl 24))
}

function Read-CtlHeader([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    [byte[]]$header = New-Object byte[] 32
    $stream = [IO.File]::OpenRead($item.FullName)
    try {
        $read = $stream.Read($header, 0, $header.Length)
    }
    finally {
        $stream.Dispose()
    }
    if ($read -ne $header.Length) {
        throw "CTL header too short: $Path read=$read"
    }

    $magic = [Text.Encoding]::ASCII.GetString($header, 0, 8)
    $signature = Read-UInt32LE $header 8
    $basisVersion = Read-UInt32LE $header 12
    $symShape = Read-UInt32LE $header 16
    $asymShape = Read-UInt32LE $header 20
    $texture = Read-UInt32LE $header 24
    $reserved = Read-UInt32LE $header 28
    if ($magic -ne "FRCTL001" -or $basisVersion -le 0 -or $symShape -le 0 -or $asymShape -le 0 -or $texture -le 0) {
        throw "Unexpected CTL header for ${Path}: magic=$magic basis=$basisVersion sym=$symShape asym=$asymShape texture=$texture"
    }

    [pscustomobject]@{
        path = $Path
        length = $item.Length
        magic = $magic
        signature = ("0x{0:x8}" -f $signature)
        basisVersion = $basisVersion
        symmetricShapeModeCount = $symShape
        asymmetricShapeModeCount = $asymShape
        textureModeCount = $texture
        reserved = $reserved
        headerBytes = 32
        payloadBytes = $item.Length - 32
    }
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

Write-ProofLine "FNV FaceGen CTL contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenCtl" "FaceGen CTL VFS loader exists"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" '"FRCTL001"' "FaceGen CTL magic validation"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "validateFaceGenCtlBasis" "FaceGen CTL validates NPC coefficient basis"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: loaded FaceGen CTL" "runtime CTL parse log"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "FaceGen CTL basis validated" "runtime CTL basis validation log"
Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"vfs-readable-runtime-conditional" "facegen-control-basis"' "harvest gate records CTL as conditional runtime support"

$metadataPath = Join-Path $ProofDir "facegen-ctl-contract.json"
$retailProbe = $null
if (![string]::IsNullOrWhiteSpace($FnvRoot) -or ![string]::IsNullOrWhiteSpace($BsaTool)) {
    if ([string]::IsNullOrWhiteSpace($FnvRoot) -or [string]::IsNullOrWhiteSpace($BsaTool)) {
        throw "FnvRoot and BsaTool must be provided together for the retail CTL probe."
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
        & $BsaTool extract -f $archivePath $SampleCtlPath $tempDir | Out-Host
        $extracted = Join-Path $tempDir $SampleCtlPath
        if (!(Test-Path -LiteralPath $extracted -PathType Leaf)) {
            throw "bsatool did not produce expected CTL sample: $extracted"
        }

        $header = Read-CtlHeader $extracted
        if ($header.basisVersion -ne 81 -or $header.symmetricShapeModeCount -ne 50 -or $header.asymmetricShapeModeCount -ne 30 -or $header.textureModeCount -ne 50) {
            throw "Unexpected FNV FaceGen CTL basis: basis=$($header.basisVersion) sym=$($header.symmetricShapeModeCount) asym=$($header.asymmetricShapeModeCount) texture=$($header.textureModeCount)"
        }

        $retailProbe = [pscustomobject]@{
            archive = $SampleArchive
            path = $SampleCtlPath
            length = $header.length
            magic = $header.magic
            signature = $header.signature
            basisVersion = $header.basisVersion
            symmetricShapeModeCount = $header.symmetricShapeModeCount
            asymmetricShapeModeCount = $header.asymmetricShapeModeCount
            textureModeCount = $header.textureModeCount
            headerBytes = $header.headerBytes
            payloadBytes = $header.payloadBytes
        }
        Write-ProofLine "OK retail CTL header: basis=$($header.basisVersion) symShape=$($header.symmetricShapeModeCount) asymShape=$($header.asymmetricShapeModeCount) texture=$($header.textureModeCount) payloadBytes=$($header.payloadBytes)"
    }
    finally {
        Remove-TempExtract $tempDir $ProofDir
    }
}
else {
    Write-ProofLine "Retail CTL probe skipped: provide -FnvRoot and -BsaTool to verify local FaceGen control metadata."
}

[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    proofDir = $ProofDir
    retailProbe = $retailProbe
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV FaceGen CTL contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
