param(
    [string]$ProofRoot = "",
    [string]$FnvRoot = "",
    [string]$BsaTool = "",
    [string]$SampleArchive = "Fallout - Meshes.bsa",
    [string[]]$SampleEgtPaths = @(
        "meshes\characters\head\headhuman.egt",
        "meshes\characters\head\eyelefthuman.egt",
        "meshes\characters\_male\upperbodyhumanmale.egt",
        "meshes\characters\_male\righthandmale.egt"
    )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-egt-runtime-contract/$Stamp"
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

function Read-EgtHeader([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    [byte[]]$header = New-Object byte[] 64
    $stream = [IO.File]::OpenRead($item.FullName)
    try {
        $read = $stream.Read($header, 0, $header.Length)
    }
    finally {
        $stream.Dispose()
    }
    if ($read -ne $header.Length) {
        throw "EGT header too short: $Path read=$read"
    }

    $magic = [Text.Encoding]::ASCII.GetString($header, 0, 8)
    $width = Read-UInt32LE $header 8
    $height = Read-UInt32LE $header 12
    $symmetricModeCount = Read-UInt32LE $header 16
    $asymmetricModeCount = Read-UInt32LE $header 20
    $basisVersion = Read-UInt32LE $header 24
    $modeCount = $symmetricModeCount + $asymmetricModeCount
    $expectedLength = 64L + ((4L + ([int64]$width * [int64]$height * 3L)) * [int64]$modeCount)

    if ($magic -ne "FREGT003" -or $width -le 0 -or $height -le 0 -or $modeCount -le 0) {
        throw "Unexpected EGT header for ${Path}: magic=$magic width=$width height=$height modes=$modeCount"
    }
    if ($item.Length -ne $expectedLength) {
        throw "Unexpected EGT length for ${Path}: length=$($item.Length) expected=$expectedLength"
    }

    [pscustomobject]@{
        path = $Path
        length = $item.Length
        magic = $magic
        width = $width
        height = $height
        symmetricModeCount = $symmetricModeCount
        asymmetricModeCount = $asymmetricModeCount
        modeCount = $modeCount
        basisVersion = $basisVersion
        headerBytes = 64
        perModeBytes = 4 + ($width * $height * 3)
        payloadBytes = $item.Length - 64
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

Write-ProofLine "FNV EGT runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "loadFaceGenEgt" "FaceGen EGT VFS loader exists"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" '"FREGT003"' "EGT magic validation"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "headerSize = 64" "EGT documented fixed header size contract"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "symmetricTextureModeCount" "EGT parser reads symmetric texture mode count"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "asymmetricTextureModeCount" "EGT parser reads asymmetric texture mode count"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "std::int8_t" "EGT parser treats mode planes as signed bytes"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "deriveFaceGenEgtMaterialTint" "EGT texture coefficients produce a material tint"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "mSymTextureModeCoefficients" "NPC FGTS coefficients drive EGT complexion"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "applyFaceGenEgtTint" "EGT tint is applied to actor skin parts"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: loaded FaceGen EGT" "runtime EGT parse log"
Assert-Text "apps/openmw/mwrender/esm4npcanimation.cpp" "FNV/ESM4 diag: applied FaceGen EGT complexion" "runtime EGT application log"
Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"loaded-pending-runtime" "facegen-tint-maps"' "harvest gate records EGT as conditional runtime support"

$metadataPath = Join-Path $ProofDir "egt-runtime-contract.json"
$retailProbe = @()
if (![string]::IsNullOrWhiteSpace($FnvRoot) -or ![string]::IsNullOrWhiteSpace($BsaTool)) {
    if ([string]::IsNullOrWhiteSpace($FnvRoot) -or [string]::IsNullOrWhiteSpace($BsaTool)) {
        throw "FnvRoot and BsaTool must be provided together for the retail EGT probe."
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
        foreach ($sample in $SampleEgtPaths) {
            & $BsaTool extract -f $archivePath $sample $tempDir | Out-Host
            $extracted = Join-Path $tempDir $sample
            if (!(Test-Path -LiteralPath $extracted -PathType Leaf)) {
                throw "bsatool did not produce expected EGT sample: $extracted"
            }

            $header = Read-EgtHeader $extracted
            $retailProbe += [pscustomobject]@{
                archive = $SampleArchive
                path = $sample
                length = $header.length
                magic = $header.magic
                width = $header.width
                height = $header.height
                symmetricModeCount = $header.symmetricModeCount
                asymmetricModeCount = $header.asymmetricModeCount
                modeCount = $header.modeCount
                basisVersion = $header.basisVersion
                headerBytes = $header.headerBytes
                perModeBytes = $header.perModeBytes
                payloadBytes = $header.payloadBytes
            }
            Write-ProofLine "OK retail EGT header: path=$sample size=$($header.width)x$($header.height) modes=$($header.modeCount) basis=$($header.basisVersion) length=$($header.length)"
        }
    }
    finally {
        Remove-TempExtract $tempDir $ProofDir
    }
}
else {
    Write-ProofLine "Retail EGT probe skipped: provide -FnvRoot and -BsaTool to verify local FaceGen tint-map headers."
}

[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    proofDir = $ProofDir
    retailProbe = $retailProbe
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV EGT runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
