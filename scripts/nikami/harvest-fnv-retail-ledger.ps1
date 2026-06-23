param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$BsaTool = $env:NIKAMI_BSATOOL,
    [string]$ProofRoot = "",
    [switch]$SkipBsaEntries
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($FnvRoot) -and [string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set -FnvRoot, -FnvData, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this harvest."
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvRoot = Split-Path $FnvData -Parent
}
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($BsaTool)) {
    $candidate = Join-Path $RepoRoot "build-clean/Release/bsatool.exe"
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $BsaTool = $candidate
    }
    elseif (Test-Path -LiteralPath "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe" -PathType Leaf) {
        $BsaTool = "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe"
    }
    else {
        $BsaTool = "bsatool.exe"
    }
}

$FnvRoot = (Resolve-Path $FnvRoot).Path
$FnvData = (Resolve-Path $FnvData).Path

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-retail-harvest/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
$BsaListDir = Join-Path $ProofDir "bsa-entry-lists"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null
New-Item -ItemType Directory -Force -Path $BsaListDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-RelativePath([string]$Base, [string]$Path) {
    $baseUri = [Uri]::new(($Base.TrimEnd("\") + "\"))
    $pathUri = [Uri]::new($Path)
    [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($pathUri).ToString()).Replace("/", "\")
}

function Get-FileLedgerRow([IO.FileInfo]$File, [string]$Root, [string]$Kind) {
    $hash = Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256
    [pscustomobject]@{
        kind = $Kind
        name = $File.Name
        relativePath = Get-RelativePath $Root $File.FullName
        bytes = $File.Length
        sha256 = $hash.Hash.ToLowerInvariant()
        lastWriteTimeUtc = $File.LastWriteTimeUtc.ToString("o")
    }
}

function Read-IniShape([IO.FileInfo]$File, [string]$Root) {
    $sections = @{}
    $current = ""
    foreach ($line in Get-Content -LiteralPath $File.FullName) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith(";") -or $trimmed.StartsWith("#")) {
            continue
        }
        $sectionMatch = [regex]::Match($trimmed, "^\[(?<name>[^\]]+)\]$")
        if ($sectionMatch.Success) {
            $current = $sectionMatch.Groups["name"].Value
            if (!$sections.ContainsKey($current)) {
                $sections[$current] = New-Object System.Collections.Generic.List[string]
            }
            continue
        }
        $keyMatch = [regex]::Match($trimmed, "^(?<key>[^=]+)=")
        if ($keyMatch.Success) {
            if (!$sections.ContainsKey($current)) {
                $sections[$current] = New-Object System.Collections.Generic.List[string]
            }
            $sections[$current].Add($keyMatch.Groups["key"].Value.Trim())
        }
    }

    $sectionRows = @()
    foreach ($section in ($sections.Keys | Sort-Object)) {
        $keys = @($sections[$section] | Sort-Object -Unique)
        $sectionRows += [pscustomobject]@{
            section = $section
            keyCount = $keys.Count
            keys = $keys
        }
    }

    $hash = Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256
    [pscustomobject]@{
        kind = "ini-shape"
        name = $File.Name
        relativePath = Get-RelativePath $Root $File.FullName
        bytes = $File.Length
        sha256 = $hash.Hash.ToLowerInvariant()
        sectionCount = $sectionRows.Count
        sections = $sectionRows
    }
}

function Write-Json([string]$Path, [object]$Value) {
    $Value | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Assert-NoPayloadOutputs([string]$Root) {
    $payloads = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in ".esm", ".esp", ".bsa", ".ini" } |
            Sort-Object FullName
    )
    if ($payloads.Count -gt 0) {
        foreach ($payload in $payloads) {
            Write-ProofLine "VIOLATION output payload: $($payload.FullName)"
        }
        throw "Harvest output contains retail/mod payload-looking files: $($payloads.Count)"
    }
}

$expectedPlugins = @(
    "FalloutNV.esm",
    "DeadMoney.esm",
    "HonestHearts.esm",
    "OldWorldBlues.esm",
    "LonesomeRoad.esm",
    "GunRunnersArsenal.esm",
    "CaravanPack.esm",
    "ClassicPack.esm",
    "MercenaryPack.esm",
    "TribalPack.esm"
)

$expectedBsaNames = @(
    "CaravanPack - Main.bsa",
    "ClassicPack - Main.bsa",
    "DeadMoney - Main.bsa",
    "DeadMoney - Sounds.bsa",
    "Fallout - Meshes.bsa",
    "Fallout - Misc.bsa",
    "Fallout - Sound.bsa",
    "Fallout - Textures.bsa",
    "Fallout - Textures2.bsa",
    "Fallout - Voices1.bsa",
    "GunRunnersArsenal - Main.bsa",
    "GunRunnersArsenal - Sounds.bsa",
    "HonestHearts - Main.bsa",
    "HonestHearts - Sounds.bsa",
    "LonesomeRoad - Main.bsa",
    "LonesomeRoad - Sounds.bsa",
    "MercenaryPack - Main.bsa",
    "OldWorldBlues - Main.bsa",
    "OldWorldBlues - Sounds.bsa",
    "TribalPack - Main.bsa",
    "Update.bsa"
)

$pluginFiles = @(
    Get-ChildItem -LiteralPath $FnvData -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in ".esm", ".esp" } |
        Sort-Object Name
)
$bsaFiles = @(
    Get-ChildItem -LiteralPath $FnvData -File -Filter *.bsa -ErrorAction SilentlyContinue |
        Sort-Object Name
)
$iniFiles = @(
    Get-ChildItem -LiteralPath $FnvRoot -File -Filter *.ini -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $FnvData -File -Filter *.ini -ErrorAction SilentlyContinue
) | Sort-Object FullName -Unique

$pluginLedger = @($pluginFiles | ForEach-Object { Get-FileLedgerRow $_ $FnvRoot "plugin-metadata" })
$bsaLedger = @($bsaFiles | ForEach-Object { Get-FileLedgerRow $_ $FnvRoot "archive-metadata" })
$iniShapeLedger = @($iniFiles | ForEach-Object { Read-IniShape $_ $FnvRoot })

$missingPlugins = @($expectedPlugins | Where-Object { $name = $_; -not ($pluginFiles | Where-Object { $_.Name -ieq $name }) })
$missingBsas = @($expectedBsaNames | Where-Object { $name = $_; -not ($bsaFiles | Where-Object { $_.Name -ieq $name }) })

$bsaEntryLedgers = @()
if (!$SkipBsaEntries) {
    foreach ($bsa in $bsaFiles) {
        $safeName = ($bsa.Name -replace "[^A-Za-z0-9._-]", "_")
        $listPath = Join-Path $BsaListDir "$safeName.entries.txt"
        & $BsaTool list $bsa.FullName | Set-Content -LiteralPath $listPath -Encoding UTF8
        if ($LASTEXITCODE -ne 0) {
            throw "bsatool failed for $($bsa.FullName) exit=$LASTEXITCODE"
        }
        $entryCount = @(Get-Content -LiteralPath $listPath).Count
        $bsaEntryLedgers += [pscustomobject]@{
            archive = $bsa.Name
            entryList = Get-RelativePath $ProofDir $listPath
            entryCount = $entryCount
        }
        Write-ProofLine "OK listed BSA: $($bsa.Name) entries=$entryCount"
    }
}

$manifest = [pscustomobject]@{
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvRoot = $FnvRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    policy = "Generated evidence only. No retail ESM/ESP/BSA/INI payload files are copied into this repo or proof output."
    expectedRetailPlugins = $expectedPlugins
    expectedRetailArchives = $expectedBsaNames
    counts = [pscustomobject]@{
        plugins = $pluginLedger.Count
        archives = $bsaLedger.Count
        iniShapes = $iniShapeLedger.Count
        archiveEntryLists = $bsaEntryLedgers.Count
        missingExpectedPlugins = $missingPlugins.Count
        missingExpectedArchives = $missingBsas.Count
    }
    missingExpectedPlugins = $missingPlugins
    missingExpectedArchives = $missingBsas
}

Write-Json (Join-Path $ProofDir "manifest.json") $manifest
Write-Json (Join-Path $ProofDir "plugins-metadata.json") $pluginLedger
Write-Json (Join-Path $ProofDir "archives-metadata.json") $bsaLedger
Write-Json (Join-Path $ProofDir "ini-shape-ledger.json") $iniShapeLedger
Write-Json (Join-Path $ProofDir "archive-entry-ledger.json") $bsaEntryLedgers

Assert-NoPayloadOutputs $ProofDir

Write-ProofLine "FNV retail harvest ledger $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "Plugins discovered: $($pluginLedger.Count)"
Write-ProofLine "Archives discovered: $($bsaLedger.Count)"
Write-ProofLine "INI shapes discovered: $($iniShapeLedger.Count)"
Write-ProofLine "BSA entry lists generated: $($bsaEntryLedgers.Count)"
Write-ProofLine "Missing expected retail plugins: $($missingPlugins.Count)"
foreach ($name in $missingPlugins) {
    Write-ProofLine "MISSING plugin: $name"
}
Write-ProofLine "Missing expected retail archives: $($missingBsas.Count)"
foreach ($name in $missingBsas) {
    Write-ProofLine "MISSING archive: $name"
}

if ($missingPlugins.Count -gt 0 -or $missingBsas.Count -gt 0) {
    throw "Retail FNV harvest incomplete. See $SummaryFile"
}

Write-ProofLine ""
Write-ProofLine "FNV retail harvest ledger PASS"
Write-ProofLine "ProofDir: $ProofDir"
