param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 8
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvRoot = Split-Path $FnvData -Parent
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-weather-record-store-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-Text([string]$RelativePath, [string]$Pattern, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source file for ${Description}: $RelativePath"
    }
    $content = Get-Content -LiteralPath $path -Raw
    if (!$content.Contains($Pattern)) {
        throw "Missing ${Description}: $RelativePath does not contain [$Pattern]"
    }
    Write-ProofLine "OK code anchor: $Description -> $RelativePath"
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file for ${Label}: $Path" }
    if (!(Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "Missing ${Label}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Label}: $Pattern"
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file for ${Label}: $Path" }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Label}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Label}: $Pattern"
}

function Assert-RawPendingRuntimeTotal([string]$Path, [string]$RecordName, [int]$Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing OpenMW log for ${RecordName}: $Path" }
    $total = 0
    $lines = 0
    $pattern = "FNV/ESM4 inventory raw-loaded pending:\s+$([regex]::Escape($RecordName))4\s+count=(?<count>[0-9]+)"
    foreach ($match in (Select-String -LiteralPath $Path -Pattern $pattern -ErrorAction SilentlyContinue)) {
        $total += [int]$match.Matches[0].Groups["count"].Value
        ++$lines
    }
    if ($total -ne $Expected) {
        throw "Unexpected runtime raw-pending total for ${RecordName}: actual=$total expected=$Expected lines=$lines"
    }
    Write-ProofLine "OK runtime raw-pending total: $RecordName=$total lines=$lines"
}

function Get-LatestProofDir([string]$Root, [string]$Description) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Description proof root: $Root"
    }
    $latest = Get-ChildItem -LiteralPath $Root -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "Missing $Description proof directories under $Root"
    }
    return $latest.FullName
}

function Assert-RecordCount([object[]]$Records, [string]$RecordType, [int]$Expected) {
    $actual = 0
    foreach ($plugin in $Records) {
        foreach ($record in @($plugin.records)) {
            if ($record.type -eq $RecordType) {
                $actual += [int]$record.count
            }
        }
    }
    if ($actual -ne $Expected) {
        throw "Unexpected $RecordType ledger count: actual=$actual expected=$Expected"
    }
    Write-ProofLine "OK content ledger count: $RecordType=$actual"
}

function Assert-RecordClassification([string]$Path, [string]$RecordType, [string]$Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing classification ledger: $Path"
    }
    $rows = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $matches = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $rows) {
        $itemTypeProperty = $row.PSObject.Properties["itemType"]
        $recordTypeProperty = $row.PSObject.Properties["recordType"]
        if ($null -eq $itemTypeProperty -or $null -eq $recordTypeProperty) {
            continue
        }
        if ($itemTypeProperty.Value -eq "esm4-record-type" -and $recordTypeProperty.Value -eq $RecordType) {
            $matches.Add($row)
        }
    }
    if ($matches.Count -eq 0) {
        throw "Missing classification row for ESM4 record type $RecordType"
    }
    $classes = @($matches | ForEach-Object { $_.classification } | Select-Object -Unique)
    if (!($classes -contains $Expected)) {
        throw "Unexpected classification for ${RecordType}: actual=$($classes -join ',') expected=$Expected"
    }
    Write-ProofLine "OK classification: $RecordType=$Expected"
}

Write-ProofLine "FNV weather/visual record store contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "components/esm4/loadregn.cpp" "case ESM::fourCC(`"RDWT`")" "REGN legacy loader exposes why raw byte inventory is required before typed runtime support"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_CLMT4:" "CLMT raw-pending fallback is in ESMStore"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_WTHR4:" "WTHR raw-pending fallback is in ESMStore"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_IMGS4:" "IMGS raw-pending fallback is in ESMStore"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_REGN4:" "REGN raw-pending fallback is in ESMStore"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"WTHR": "weather bytes are inventoried pending WeatherManager binding"' "classifier declares WTHR loaded-pending intent"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"CLMT": "climate bytes are inventoried pending full FNV climate/weather runtime binding"' "classifier declares CLMT loaded-pending intent"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"REGN": "region bytes are inventoried pending full FNV weather/audio-region runtime binding"' "classifier declares REGN loaded-pending intent"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"IMGS": "image-space bytes are inventoried pending full post-process binding"' "classifier declares IMGS loaded-pending intent"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" 'esmstore.cpp' "classifier reads raw-pending fallback from esmstore.cpp"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
$RecordsPath = Join-Path $ContentLedgerDir "records.json"
if (!(Test-Path -LiteralPath $RecordsPath -PathType Leaf)) {
    throw "Missing content ledger records: $RecordsPath"
}
$records = Get-Content -LiteralPath $RecordsPath -Raw | ConvertFrom-Json
Assert-RecordCount $records "CLMT" 46
Assert-RecordCount $records "IMGS" 92
Assert-RecordCount $records "REGN" 319
Assert-RecordCount $records "WTHR" 98

$ClassificationScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
& $ClassificationScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -ContentLedgerDir $ContentLedgerDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}

$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV classification"
$ClassificationLedger = Join-Path $ClassificationDir "classification-ledger.json"
foreach ($recordType in @("CLMT", "IMGS", "REGN", "WTHR")) {
    Assert-RecordClassification -Path $ClassificationLedger -RecordType $recordType -Expected "loaded-pending-runtime"
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound `
    -ClassificationDir $ClassificationDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
Assert-RawPendingRuntimeTotal $OpenMwLog "CLMT" 46
Assert-RawPendingRuntimeTotal $OpenMwLog "IMGS" 92
Assert-RawPendingRuntimeTotal $OpenMwLog "REGN" 319
Assert-RawPendingRuntimeTotal $OpenMwLog "WTHR" 98
Assert-FileNotContains $OpenMwLog "FNV/ESM4 inventory skipped unsupported: (CLMT4|IMGS4|REGN4|WTHR4)" "weather/visual unsupported skip"

$summary = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    contentLedgerDir = $ContentLedgerDir
    classificationDir = $ClassificationDir
    flatProofDir = $FlatProofDir
    recordCounts = [ordered]@{
        CLMT = 46
        IMGS = 92
        REGN = 319
        WTHR = 98
    }
    classification = "loaded-pending-runtime"
    runtimeBoundary = "CLMT/WTHR/REGN/IMGS bytes are inventoried raw-pending. WeatherManager, region audio/weather, and post-process binding remain separate gates."
}
$ContractPath = Join-Path $ProofDir "fnv-weather-record-store-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Classification ledger: $ClassificationLedger"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV weather/visual record store contract PASS"
Write-ProofLine "CLMT/WTHR/REGN/IMGS are byte-inventoried loaded-pending-runtime."
