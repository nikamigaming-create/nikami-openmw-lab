param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($FnvRoot) -and [string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set -FnvRoot, -FnvData, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof."
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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-gameplay-record-store-contract/$Stamp"
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

function Assert-Count([object]$Counts, [string]$RecordType, [int]$Expected) {
    $actual = [int]($Counts.PSObject.Properties[$RecordType].Value)
    if ($actual -ne $Expected) {
        throw "Unexpected $RecordType gameplay ledger count: actual=$actual expected=$Expected"
    }
    Write-ProofLine "OK gameplay ledger count: $RecordType=$actual"
}

Write-ProofLine "FNV gameplay record store contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "components/esm4/loadperk.hpp" "REC_PERK4" "PERK loader declares ESM4 record id"
Assert-Text "components/esm4/loadperk.cpp" "mConditions" "PERK loader captures condition chunks"
Assert-Text "components/esm4/loadproj.hpp" "REC_PROJ4" "PROJ loader declares ESM4 record id"
Assert-Text "components/esm4/loadproj.cpp" "mData = readRawSubrecord" "PROJ loader captures opaque projectile DATA"
Assert-Text "components/esm4/loadexpl.hpp" "REC_EXPL4" "EXPL loader declares ESM4 record id"
Assert-Text "components/esm4/loadexpl.cpp" "mImpactDataSet" "EXPL loader captures impact data references"
Assert-Text "components/esm4/records.hpp" "loadperk.hpp" "PERK loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadproj.hpp" "PROJ loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadexpl.hpp" "EXPL loader is included in ESM4 record bundle"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Perk>" "PERK store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Projectile>" "PROJ store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Explosion>" "EXPL store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Perk>" "PERK dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Projectile>" "PROJ dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Explosion>" "EXPL dynamic store is explicitly instantiated"
Assert-Text "scripts/nikami/fnv_content_ledger.py" '"gameplaySystems"' "content ledger writes gameplay system artifact"

$ContentLedger = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedger -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$LedgerRoot = Join-Path $ProofRoot "fnv-content-ledger"
$LedgerDir = Get-ChildItem -LiteralPath $LedgerRoot -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($null -eq $LedgerDir) {
    throw "FNV content ledger did not produce a proof directory under $LedgerRoot"
}

$ResultPath = Join-Path $LedgerDir.FullName "result.json"
$GameplayPath = Join-Path $LedgerDir.FullName "gameplay-systems.json"
if (!(Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
    throw "Missing content ledger result: $ResultPath"
}
if (!(Test-Path -LiteralPath $GameplayPath -PathType Leaf)) {
    throw "Missing gameplay systems ledger: $GameplayPath"
}

$result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
$gameplay = @(Get-Content -LiteralPath $GameplayPath -Raw | ConvertFrom-Json)

Assert-Count $result.gameplaySystemCounts "PERK" 259
Assert-Count $result.gameplaySystemCounts "PROJ" 156
Assert-Count $result.gameplaySystemCounts "EXPL" 225

foreach ($needle in @("BuiltToDestroy", "WildWasteland", "SecuritronGrenadeProjectile", "SecuritronGrenadeExplosion")) {
    $match = $gameplay | Where-Object { $_.editorId -eq $needle } | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing gameplay ledger anchor editorId: $needle"
    }
    Write-ProofLine "OK gameplay ledger anchor: $needle"
}

$summary = [pscustomobject]@{
    status = "PASS"
    repoRoot = $RepoRoot
    fnvData = $FnvData
    ledgerDir = $LedgerDir.FullName
    gameplaySystems = $result.gameplaySystemCounts
    anchors = @("BuiltToDestroy", "WildWasteland", "SecuritronGrenadeProjectile", "SecuritronGrenadeExplosion")
}
$ContractPath = Join-Path $ProofDir "gameplay-record-store-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Gameplay ledger JSON: $GameplayPath"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV gameplay record store contract PASS"
Write-ProofLine "PERK/PROJ/EXPL are source-backed and stored; gameplay execution remains gated separately."
