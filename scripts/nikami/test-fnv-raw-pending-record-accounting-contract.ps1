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

$FlatContent = @(
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

$RecordCounts = [ordered]@{
    ADDN = 38
    ALOC = 101
    AMEF = 80
    ANIO = 161
    CAMS = 276
    CCRD = 324
    CDCK = 13
    CHAL = 184
    CHIP = 6
    CMNY = 6
    CPTH = 418
    CSNO = 6
    DEBR = 6
    DEHY = 5
    DOBJ = 1
    ECZN = 28
    HUNG = 5
    LSCT = 1
    MICN = 12
    RADS = 5
    RCCT = 11
    RCPE = 291
    REPU = 13
    RGDL = 44
    SLPD = 5
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-raw-pending-record-accounting/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-LatestProofDir([string]$Root, [string]$Label) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Label proof root: $Root"
    }
    $dir = Get-ChildItem -LiteralPath $Root -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $dir) {
        throw "Missing $Label proof directory under $Root"
    }
    return $dir.FullName
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

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path)) { throw "Missing file for ${Label}: $Path" }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Label}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Label}: $Pattern"
}

function Read-JsonArray([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Label}: $Path"
    }
    $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $value) {
        return @()
    }
    if ($value -is [System.Array]) {
        return @($value)
    }
    return @($value)
}

function Get-ContentLedgerRecordCounts([string]$RecordsPath) {
    $plugins = Read-JsonArray $RecordsPath "content ledger records"
    $counts = @{}
    foreach ($plugin in $plugins) {
        foreach ($record in @($plugin.records)) {
            $type = [string]$record.type
            if (!$counts.ContainsKey($type)) {
                $counts[$type] = 0
            }
            $counts[$type] += [int]$record.count
        }
    }
    return $counts
}

function Assert-RecordCount([hashtable]$Counts, [string]$RecordType, [int]$Expected) {
    $actual = 0
    if ($Counts.ContainsKey($RecordType)) {
        $actual = [int]$Counts[$RecordType]
    }
    if ($actual -ne $Expected) {
        throw "Unexpected $RecordType content ledger count: actual=$actual expected=$Expected"
    }
    Write-ProofLine "OK content ledger count: $RecordType=$actual"
}

function Assert-RawPendingRuntimeTotal([string]$Path, [string]$RecordType, [int]$Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing OpenMW log: $Path"
    }
    $pattern = "FNV/ESM4 inventory raw-loaded pending: $([Regex]::Escape($RecordType))4 count=(\d+)"
    $matches = @(Select-String -LiteralPath $Path -Pattern $pattern -AllMatches)
    $total = 0
    foreach ($line in $matches) {
        foreach ($match in $line.Matches) {
            $total += [int]$match.Groups[1].Value
        }
    }
    if ($total -ne $Expected) {
        throw "Unexpected raw-pending runtime total for ${RecordType}: actual=$total expected=$Expected"
    }
    Write-ProofLine "OK runtime raw-pending total: $RecordType=$total"
}

Write-ProofLine "FNV raw-pending record accounting contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "components/esm4/readerutils.hpp" "valid final record may leave us at EOF" "ESM4 final-record reader guard"

foreach ($entry in $RecordCounts.GetEnumerator()) {
    $recordType = [string]$entry.Key
    Assert-Text "components/esm4/common.hpp" "REC_$recordType = fourCC(`"$recordType`")" "$recordType ESM4 record id"
    Assert-Text "components/esm/defs.hpp" "REC_${recordType}4 = esm4Recname(ESM4::REC_$recordType)" "$recordType OpenMW record id"
    Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_${recordType}4:" "$recordType raw-pending fallback"
    Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" "`"$recordType`": " "$recordType classifier loaded-pending reason"
}

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofDir -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofDir "fnv-content-ledger") "FNV content ledger"
$RecordsPath = Join-Path $ContentLedgerDir "records.json"
if (!(Test-Path -LiteralPath $RecordsPath -PathType Leaf)) {
    throw "Missing content ledger records: $RecordsPath"
}
$ContentCounts = Get-ContentLedgerRecordCounts $RecordsPath
foreach ($entry in $RecordCounts.GetEnumerator()) {
    Assert-RecordCount $ContentCounts ([string]$entry.Key) ([int]$entry.Value)
}

$ClassificationScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
& $ClassificationScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -ContentLedgerDir $ContentLedgerDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}

$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV classification"
$ClassificationLedger = Join-Path $ClassificationDir "classification-ledger.json"
$ClassificationRows = Read-JsonArray $ClassificationLedger "classification ledger"
$ClassificationByRecord = @{}
foreach ($row in $ClassificationRows) {
    $recordTypeProperty = $row.PSObject.Properties["recordType"]
    if (($row.itemType -ne "esm4-record-type") -or ($null -eq $recordTypeProperty)) {
        continue
    }
    $recordType = [string]$recordTypeProperty.Value
    if (!$ClassificationByRecord.ContainsKey($recordType)) {
        $ClassificationByRecord[$recordType] = @{}
    }
    $ClassificationByRecord[$recordType][[string]$row.classification] = $true
}
foreach ($entry in $RecordCounts.GetEnumerator()) {
    $recordType = [string]$entry.Key
    if (!$ClassificationByRecord.ContainsKey($recordType)) {
        throw "Missing classification row for ESM4 record type $recordType"
    }
    $actual = @($ClassificationByRecord[$recordType].Keys | Sort-Object) -join ","
    if ($actual -ne "loaded-pending-runtime") {
        throw "Unexpected classification for ${recordType}: actual=$actual expected=loaded-pending-runtime"
    }
    Write-ProofLine "OK classification: $recordType=loaded-pending-runtime"
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
foreach ($entry in $RecordCounts.GetEnumerator()) {
    Assert-RawPendingRuntimeTotal -Path $OpenMwLog -RecordType ([string]$entry.Key) -Expected ([int]$entry.Value)
}
$skipPattern = "FNV/ESM4 inventory skipped unsupported: (" + (($RecordCounts.Keys | ForEach-Object { [Regex]::Escape([string]$_ + "4") }) -join "|") + ")"
Assert-FileNotContains $OpenMwLog $skipPattern "raw-pending accounted record unsupported skip"

$summary = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    contentLedgerDir = $ContentLedgerDir
    classificationDir = $ClassificationDir
    flatProofDir = $FlatProofDir
    recordCounts = $RecordCounts
    classification = "loaded-pending-runtime"
    runtimeBoundary = "These FNV record bytes are inventoried raw-pending. Exact gameplay systems remain separate runtime-supported gates."
}
$ContractPath = Join-Path $ProofDir "fnv-raw-pending-record-accounting-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Classification ledger: $ClassificationLedger"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV raw-pending record accounting contract PASS"
