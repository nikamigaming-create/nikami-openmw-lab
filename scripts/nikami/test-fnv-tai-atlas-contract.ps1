param(
    [string]$ProofRoot = "",
    [string]$HarvestDir = "",
    [string]$FnvRoot = "",
    [string]$BsaTool = "",
    [string]$SampleArchive = "Fallout - Textures2.bsa",
    [string]$SampleTaiPath = "textures\interface\interfaceshared.tai"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (Test-Path -LiteralPath $harvestRoot -PathType Container) {
        $latest = Get-ChildItem -LiteralPath $harvestRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($null -ne $latest) {
            $HarvestDir = $latest.FullName
        }
    }
}
if (![string]::IsNullOrWhiteSpace($HarvestDir)) {
    $HarvestDir = (Resolve-Path $HarvestDir).Path
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-tai-atlas-contract/$Stamp"
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

function Get-HarvestEntries([string]$Dir) {
    $entries = [System.Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        return @()
    }

    $entryRoot = Join-Path $Dir "bsa-entry-lists"
    if (!(Test-Path -LiteralPath $entryRoot -PathType Container)) {
        return @($entries.ToArray())
    }

    foreach ($list in Get-ChildItem -LiteralPath $entryRoot -Filter "*.entries.txt" -File) {
        foreach ($entry in Get-Content -LiteralPath $list.FullName) {
            $entries.Add($entry)
        }
    }
    return @($entries.ToArray())
}

function Parse-Tai([string]$Path, [string[]]$HarvestEntries) {
    $entrySet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $baseNameIndex = @{}
    foreach ($entry in $HarvestEntries) {
        $normalizedEntry = $entry.Replace("/", "\").ToLowerInvariant()
        [void]$entrySet.Add($normalizedEntry)
        $base = [IO.Path]::GetFileName($normalizedEntry)
        if (!$baseNameIndex.ContainsKey($base)) {
            $baseNameIndex[$base] = [System.Collections.Generic.List[string]]::new()
        }
        $baseNameIndex[$base].Add($entry)
    }

    $rows = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith("#")) {
            continue
        }

        $parts = $trimmed -split "\s+", 2
        if ($parts.Count -ne 2) {
            throw "Malformed TAI row: $line"
        }
        $sourceName = $parts[0]
        $fields = @($parts[1].Split(",") | ForEach-Object { $_.Trim() })
        if ($fields.Count -lt 8) {
            throw "Malformed TAI atlas fields: $line"
        }

        $atlasName = $fields[0]
        $atlasHarvestPath = "textures\interface\$($atlasName.ToLowerInvariant())"
        $sourceHarvestPath = "textures\interface\$($sourceName.ToLowerInvariant())"
        $sourceBasenameMatches = @()
        $sourceKey = $sourceName.ToLowerInvariant()
        if ($baseNameIndex.ContainsKey($sourceKey)) {
            $sourceBasenameMatches = @($baseNameIndex[$sourceKey])
        }
        $directSourcePresent = $entrySet.Contains($sourceHarvestPath)

        $rows += [pscustomobject]@{
            source = $sourceName
            atlas = $atlasName
            atlasPresent = $entrySet.Contains($atlasHarvestPath)
            directSourceCount = if ($directSourcePresent) { 1 } else { 0 }
            directSourceSample = if ($directSourcePresent) { $sourceHarvestPath } else { "" }
            basenameSourceCount = $sourceBasenameMatches.Count
            u = [double]$fields[3]
            v = [double]$fields[4]
            width = [double]$fields[6]
            height = [double]$fields[7]
        }
    }
    return @($rows)
}

Write-ProofLine "FNV TAI atlas contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "HarvestDir: $HarvestDir"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '"runtime-supported" "interface-texture-atlas-index"' "harvest gate records TAI as runtime-supported atlas metadata"
Assert-Text "scripts/nikami/test-fnv-harvest-action-coverage.ps1" '".dds" = New-Rule "runtime-supported"' "DDS atlas and source textures are runtime-supported"
Assert-Text "components/myguiplatform/myguitexture.cpp" "mImageManager->getImage" "MyGUI texture path consumes DDS images"
Assert-Text "components/myguiplatform/myguitexture.cpp" "loadFalloutTaiAtlas" "runtime parses InterfaceShared TAI metadata"
Assert-Text "components/myguiplatform/myguitexture.cpp" "cropFalloutTaiAtlasImage" "runtime crops atlas-only TAI textures"
Assert-Text "components/myguiplatform/myguitexture.cpp" "FNV/ESM4 diag: resolved TAI atlas texture" "runtime logs TAI atlas resolution"

$metadataPath = Join-Path $ProofDir "tai-atlas-contract.json"
$retailProbe = $null
if (![string]::IsNullOrWhiteSpace($FnvRoot) -or ![string]::IsNullOrWhiteSpace($BsaTool)) {
    if ([string]::IsNullOrWhiteSpace($FnvRoot) -or [string]::IsNullOrWhiteSpace($BsaTool)) {
        throw "FnvRoot and BsaTool must be provided together for the retail TAI probe."
    }
    if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
        throw "HarvestDir is required for the retail TAI probe so atlas rows can be checked against shipped DDS entries."
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
        & $BsaTool extract -f $archivePath $SampleTaiPath $tempDir | Out-Host
        $extracted = Join-Path $tempDir $SampleTaiPath
        if (!(Test-Path -LiteralPath $extracted -PathType Leaf)) {
            throw "bsatool did not produce expected TAI sample: $extracted"
        }

        $harvestEntries = @(Get-HarvestEntries $HarvestDir)
        $rows = @(Parse-Tai $extracted $harvestEntries)
        if ($rows.Count -eq 0) {
            throw "TAI sample did not contain any atlas rows: $SampleTaiPath"
        }

        $missingAtlas = @($rows | Where-Object { !$_.atlasPresent })
        $missingDirectSources = @($rows | Where-Object { $_.directSourceCount -le 0 })
        if ($missingAtlas.Count -gt 0) {
            throw "TAI rows refer to missing atlas DDS entries: $($missingAtlas[0].atlas)"
        }
        if ($missingDirectSources.Count -eq 0) {
            throw "TAI sample has no atlas-only rows; runtime resolver proof no longer exercises missing direct DDS textures."
        }

        $retailProbe = [pscustomobject]@{
            archive = $SampleArchive
            path = $SampleTaiPath
            rows = $rows.Count
            atlasFiles = @($rows | Select-Object -ExpandProperty atlas -Unique)
            missingAtlasRows = $missingAtlas.Count
            missingDirectSourceRows = $missingDirectSources.Count
            firstRows = @($rows | Select-Object -First 8)
        }
        Write-ProofLine "OK retail TAI parse: rows=$($rows.Count) atlasFiles=$(@($retailProbe.atlasFiles).Count) missingAtlas=0 atlasOnlyRows=$($missingDirectSources.Count)"
    }
    finally {
        Remove-TempExtract $tempDir $ProofDir
    }
}
else {
    Write-ProofLine "Retail TAI probe skipped: provide -FnvRoot, -BsaTool, and -HarvestDir to parse local atlas metadata."
}

[pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    harvestDir = $HarvestDir
    proofDir = $ProofDir
    retailProbe = $retailProbe
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine "Metadata JSON: $metadataPath"
Write-ProofLine ""
Write-ProofLine "FNV TAI atlas contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
