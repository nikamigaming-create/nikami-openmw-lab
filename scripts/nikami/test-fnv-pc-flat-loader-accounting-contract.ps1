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

$AllowedLoadedClasses = @("runtime-supported", "loaded-pending-runtime")

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-pc-flat-loader-accounting/$Stamp"
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

function Add-Count([hashtable]$Counts, [string]$RecordType, [int]$Count) {
    if (!$Counts.ContainsKey($RecordType)) {
        $Counts[$RecordType] = 0
    }
    $Counts[$RecordType] += $Count
}

function Get-Count([hashtable]$Counts, [string]$RecordType) {
    if ($Counts.ContainsKey($RecordType)) {
        return [int]$Counts[$RecordType]
    }
    return 0
}

function Get-LedgerRecordCounts([string]$RecordsPath) {
    $counts = @{}
    foreach ($plugin in (Read-JsonArray $RecordsPath "content ledger records")) {
        foreach ($record in @($plugin.records)) {
            Add-Count $counts ([string]$record.type) ([int]$record.count)
        }
    }
    return $counts
}

function Get-RuntimeInventoryCounts([string]$OpenMwLog, [string]$Pattern) {
    $counts = @{}
    foreach ($line in @(Select-String -LiteralPath $OpenMwLog -Pattern $Pattern -AllMatches -ErrorAction SilentlyContinue)) {
        foreach ($match in $line.Matches) {
            $recordName = [string]$match.Groups["type"].Value
            $recordType = $recordName
            if ($recordType.EndsWith("4", [System.StringComparison]::Ordinal)) {
                $recordType = $recordType.Substring(0, $recordType.Length - 1)
            }
            Add-Count $counts $recordType ([int]$match.Groups["count"].Value)
        }
    }
    return $counts
}

function Get-ClassificationByRecord([string]$ClassificationLedger) {
    $result = @{}
    foreach ($row in (Read-JsonArray $ClassificationLedger "classification ledger")) {
        if ([string]$row.itemType -ne "esm4-record-type") {
            continue
        }
        $recordTypeProperty = $row.PSObject.Properties["recordType"]
        if ($null -eq $recordTypeProperty) {
            continue
        }
        $recordType = [string]$recordTypeProperty.Value
        if ([string]::IsNullOrWhiteSpace($recordType)) {
            continue
        }
        if (!$result.ContainsKey($recordType)) {
            $result[$recordType] = @{}
        }
        $result[$recordType][[string]$row.classification] = $true
    }
    return $result
}

function Assert-FlatContentLines([string]$OpenMwCfg) {
    if (!(Test-Path -LiteralPath $OpenMwCfg -PathType Leaf)) {
        throw "Missing generated openmw.cfg: $OpenMwCfg"
    }
    $contentLines = @(Select-String -LiteralPath $OpenMwCfg -Pattern "^content=" | ForEach-Object {
            $_.Line.Substring("content=".Length)
        })
    if ($contentLines.Count -ne $FlatContent.Count) {
        throw "Unexpected PC-flat content line count: actual=$($contentLines.Count) expected=$($FlatContent.Count)"
    }
    for ($i = 0; $i -lt $FlatContent.Count; $i++) {
        if ($contentLines[$i] -ne $FlatContent[$i]) {
            throw "Unexpected PC-flat content line ${i}: actual=$($contentLines[$i]) expected=$($FlatContent[$i])"
        }
    }
    if ($contentLines -contains "FNVR.esp") {
        throw "PC-flat openmw.cfg unexpectedly includes FNVR.esp"
    }
    Write-ProofLine "OK PC-flat content order excludes FNVR.esp: $($contentLines -join ', ')"
}

Write-ProofLine "FNV PC-flat loader accounting contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat content ledger proof failed with exit code $LASTEXITCODE."
}
$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV flat content ledger"
$RecordsPath = Join-Path $ContentLedgerDir "records.json"
$LedgerCounts = Get-LedgerRecordCounts $RecordsPath
Write-ProofLine "Flat content ledger: $ContentLedgerDir"
Write-ProofLine "Flat record types: $($LedgerCounts.Count)"

$ClassificationScript = Join-Path $PSScriptRoot "test-fnv-no-silent-skip-classification.ps1"
& $ClassificationScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -ContentLedgerDir $ContentLedgerDir
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification failed with exit code $LASTEXITCODE."
}
$ClassificationDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-no-silent-skip-classification") "FNV classification"
$ClassificationLedger = Join-Path $ClassificationDir "classification-ledger.json"
$ClassificationByRecord = Get-ClassificationByRecord $ClassificationLedger
Write-ProofLine "Classification ledger: $ClassificationLedger"

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
$OpenMwCfg = Join-Path $FlatProofDir "openmw.cfg"
if (!(Test-Path -LiteralPath $OpenMwLog -PathType Leaf)) {
    throw "Missing FNV flat OpenMW log: $OpenMwLog"
}
Assert-FlatContentLines $OpenMwCfg

$LoadedCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory loaded:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"
$RawPendingCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory raw-loaded pending:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"
$SkippedCounts = Get-RuntimeInventoryCounts $OpenMwLog "FNV/ESM4 inventory skipped unsupported:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)"

$runtimeTypes = @{}
foreach ($table in @($LoadedCounts, $RawPendingCounts, $SkippedCounts)) {
    foreach ($recordType in $table.Keys) {
        $runtimeTypes[$recordType] = $true
    }
}
foreach ($recordType in ($runtimeTypes.Keys | Sort-Object)) {
    if (!$LedgerCounts.ContainsKey($recordType)) {
        throw "Runtime inventory logged record type absent from PC-flat ledger: $recordType"
    }
}

$Rows = @()
foreach ($recordType in ($LedgerCounts.Keys | Sort-Object)) {
    $expected = [int]$LedgerCounts[$recordType]
    $loaded = Get-Count $LoadedCounts $recordType
    $rawPending = Get-Count $RawPendingCounts $recordType
    $skipped = Get-Count $SkippedCounts $recordType
    $headerAccounted = 0
    if ($recordType -eq "TES4" -and ($loaded + $rawPending + $skipped) -eq 0) {
        $headerAccounted = $expected
    }
    if ($rawPending -gt 0 -and $rawPending -ne $expected) {
        throw "Raw-pending subset mismatch for ${recordType}: ledger=$expected rawPending=$rawPending"
    }
    $actual = $loaded + $skipped + $headerAccounted
    if ($actual -ne $expected) {
        throw "Record accounting mismatch for ${recordType}: ledger=$expected loaded=$loaded rawPending=$rawPending skipped=$skipped header=$headerAccounted actual=$actual"
    }
    if (!$ClassificationByRecord.ContainsKey($recordType)) {
        throw "Missing classification row for PC-flat record type $recordType"
    }
    $classes = @($ClassificationByRecord[$recordType].Keys | Sort-Object)
    $classText = $classes -join ","
    $allowsRuntimeLoad = $false
    foreach ($class in $classes) {
        if ($AllowedLoadedClasses -contains $class) {
            $allowsRuntimeLoad = $true
        }
    }
    if ($allowsRuntimeLoad -and $skipped -ne 0) {
        throw "Loaded-supported PC-flat record type was skipped: $recordType classification=$classText skipped=$skipped"
    }
    if (!$allowsRuntimeLoad -and $skipped -eq 0 -and $recordType -ne "TES4") {
        throw "PC-flat record type has no runtime/load-pending classification but did not appear as skipped: $recordType classification=$classText"
    }

    Write-ProofLine "OK record accounting: $recordType ledger=$expected loaded=$loaded rawPending=$rawPending skipped=$skipped header=$headerAccounted classification=$classText"
    $Rows += [ordered]@{
        recordType = $recordType
        ledger = $expected
        loaded = $loaded
        rawPending = $rawPending
        skipped = $skipped
        headerAccounted = $headerAccounted
        classification = $classes
    }
}

$summary = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    proofDir = $ProofDir
    contentLedgerDir = $ContentLedgerDir
    classificationDir = $ClassificationDir
    flatProofDir = $FlatProofDir
    flatContent = $FlatContent
    recordTypeCount = $Rows.Count
    rows = $Rows
}
$ContractPath = Join-Path $ProofDir "fnv-pc-flat-loader-accounting-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV PC-flat loader accounting contract PASS"
