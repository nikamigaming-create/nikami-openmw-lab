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

function Assert-Equal([string]$Description, [int]$Actual, [int]$Expected) {
    if ($Actual -ne $Expected) {
        throw "Unexpected ${Description}: actual=$Actual expected=$Expected"
    }
    Write-ProofLine "OK ${Description}: $Actual"
}

function Get-FlatLedgerRecordCounts([string]$RecordsPath) {
    $flatContentSet = @{}
    foreach ($content in $FlatContent) {
        $flatContentSet[$content] = $true
    }

    $counts = @{}
    foreach ($plugin in (Read-JsonArray $RecordsPath "content ledger records")) {
        if (!$flatContentSet.ContainsKey([string]$plugin.plugin)) {
            continue
        }
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

function Get-RecordCount([hashtable]$Counts, [string]$RecordType) {
    if ($Counts.ContainsKey($RecordType)) {
        return [int]$Counts[$RecordType]
    }
    return 0
}

function Get-RuntimeLoadedRecordTotal([string]$OpenMwLog, [string]$RecordType) {
    $pattern = "FNV/ESM4 inventory loaded: $([Regex]::Escape($RecordType))4 count=(\d+)"
    $total = 0
    foreach ($line in @(Select-String -LiteralPath $OpenMwLog -Pattern $pattern -AllMatches)) {
        foreach ($match in $line.Matches) {
            $total += [int]$match.Groups[1].Value
        }
    }
    return $total
}

function Assert-NoUnsupportedRecordSkip([string]$OpenMwLog, [string]$RecordType) {
    $pattern = "FNV/ESM4 inventory skipped unsupported:\s+$([Regex]::Escape($RecordType))4\s+count="
    $matches = @(Select-String -LiteralPath $OpenMwLog -Pattern $pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -ne 0) {
        throw "Unexpected unsupported $RecordType runtime skip count in $OpenMwLog"
    }
    Write-ProofLine "OK no unsupported runtime skip: $RecordType"
}

function Assert-RuntimeSupportedGameplayRows([object[]]$Rows, [string]$RecordType, [int]$ExpectedCount) {
    $matches = @($Rows | Where-Object { [string]$_.recordType -eq $RecordType })
    Assert-Equal "$RecordType gameplay row count" $matches.Count $ExpectedCount
    foreach ($row in $matches) {
        if ([string]$row.classification -ne "runtime-supported") {
            throw "Unexpected $RecordType gameplay classification for $($row.editorId): $($row.classification)"
        }
        if ([string]$row.readiness -ne "runtime-supported") {
            throw "Unexpected $RecordType gameplay readiness for $($row.editorId): $($row.readiness)"
        }
        if ([string]$row.runtimeProofGate -ne "fnv-real-10mm-runtime-contract") {
            throw "Unexpected $RecordType runtime proof gate for $($row.editorId): $($row.runtimeProofGate)"
        }
        if (@($row.unprovenGameplayGates).Count -eq 0) {
            throw "Missing bounded unproven gameplay gates for $RecordType $($row.editorId)"
        }
    }
    Write-ProofLine "OK bounded runtime-supported gameplay rows: $RecordType=$($matches.Count)"
}

function Assert-AmmoProjectileBindingRows([object[]]$Rows) {
    $ammoRows = @($Rows | Where-Object { [string]$_.recordType -eq "AMMO" })
    Assert-Equal "AMMO projectile binding row count" $ammoRows.Count 145

    $nullProjectileRows = @($ammoRows | Where-Object {
            [string]$_.projectile -eq "0x00000000" -and
            [string]$_.projectileBindingClassification -eq "intentionally-excluded-with-proof"
        })
    $nonzeroProjectileRows = @($ammoRows | Where-Object {
            ![string]::IsNullOrWhiteSpace([string]$_.projectile) -and
            [string]$_.projectile -ne "0x00000000" -and
            [string]$_.projectileBindingClassification -eq "loaded-pending-runtime"
        })
    $undecodedProjectileRows = @($ammoRows | Where-Object {
            [string]::IsNullOrWhiteSpace([string]$_.projectile) -and
            [string]$_.projectileBindingClassification -eq "loaded-pending-runtime"
        })

    Assert-Equal "AMMO null projectile rows" $nullProjectileRows.Count 101
    Assert-Equal "AMMO nonzero projectile pending rows" $nonzeroProjectileRows.Count 28
    Assert-Equal "AMMO undecoded projectile pending rows" $undecodedProjectileRows.Count 16

    $tenMillimeter = $ammoRows | Where-Object { [string]$_.editorId -eq "Ammo10mm" } | Select-Object -First 1
    if ($null -eq $tenMillimeter) {
        throw "Missing Ammo10mm gameplay ledger row"
    }
    if ([string]$tenMillimeter.projectile -ne "0x00000000") {
        throw "Unexpected Ammo10mm projectile field: $($tenMillimeter.projectile)"
    }
    if ([string]$tenMillimeter.projectileBindingClassification -ne "intentionally-excluded-with-proof") {
        throw "Unexpected Ammo10mm projectile binding classification: $($tenMillimeter.projectileBindingClassification)"
    }
    if ([string]$tenMillimeter.projectileBindingBoundary -notmatch "no PROJ record to bind") {
        throw "Ammo10mm projectile boundary does not prove null-projectile exclusion"
    }

    Write-ProofLine "OK AMMO projectile binding classifications: null=$($nullProjectileRows.Count) nonzeroPending=$($nonzeroProjectileRows.Count) undecodedPending=$($undecodedProjectileRows.Count)"
}

function Assert-LoadedPendingGameplayRows([object[]]$Rows, [string]$RecordType, [int]$ExpectedCount, [string]$ExpectedGate) {
    $matches = @($Rows | Where-Object { [string]$_.recordType -eq $RecordType })
    Assert-Equal "$RecordType gameplay row count" $matches.Count $ExpectedCount
    foreach ($row in $matches) {
        if ([string]$row.classification -ne "loaded-pending-runtime") {
            throw "Unexpected $RecordType gameplay classification for $($row.editorId): $($row.classification)"
        }
        if ([string]$row.readiness -ne "loaded-pending-runtime") {
            throw "Unexpected $RecordType gameplay readiness for $($row.editorId): $($row.readiness)"
        }
        if ([string]$row.firstFailingGate -ne $ExpectedGate) {
            throw "Unexpected $RecordType first failing gate for $($row.editorId): $($row.firstFailingGate)"
        }
    }
    Write-ProofLine "OK loaded-pending gameplay rows: $RecordType=$($matches.Count) gate=$ExpectedGate"
}

function Get-UniqueGameplayFormCount([object[]]$Rows, [string]$RecordType) {
    return @($Rows |
        Where-Object { [string]$_.recordType -eq $RecordType } |
        Select-Object -ExpandProperty formId -Unique).Count
}

Write-ProofLine "FNV gameplay record store contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "components/esm4/loadweap.hpp" "REC_WEAP4" "WEAP loader declares ESM4 record id"
Assert-Text "components/esm4/loadweap.cpp" "mSoundRefs" "WEAP loader captures weapon sound references"
Assert-Text "components/esm4/loadammo.hpp" "REC_AMMO4" "AMMO loader declares ESM4 record id"
Assert-Text "components/esm4/loadammo.cpp" "mData.mProjectile" "AMMO loader captures projectile reference"
Assert-Text "components/esm4/loadperk.hpp" "REC_PERK4" "PERK loader declares ESM4 record id"
Assert-Text "components/esm4/loadperk.cpp" "mConditions" "PERK loader captures condition chunks"
Assert-Text "components/esm4/loadproj.hpp" "REC_PROJ4" "PROJ loader declares ESM4 record id"
Assert-Text "components/esm4/loadproj.cpp" "mData = readRawSubrecord" "PROJ loader captures opaque projectile DATA"
Assert-Text "components/esm4/loadexpl.hpp" "REC_EXPL4" "EXPL loader declares ESM4 record id"
Assert-Text "components/esm4/loadexpl.cpp" "mImpactDataSet" "EXPL loader captures impact data references"
Assert-Text "components/esm4/records.hpp" "loadweap.hpp" "WEAP loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadammo.hpp" "AMMO loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadperk.hpp" "PERK loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadproj.hpp" "PROJ loader is included in ESM4 record bundle"
Assert-Text "components/esm4/records.hpp" "loadexpl.hpp" "EXPL loader is included in ESM4 record bundle"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Weapon>" "WEAP store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Ammunition>" "AMMO store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Perk>" "PERK store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Projectile>" "PROJ store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Explosion>" "EXPL store is in ESMStore tuple"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Weapon>" "WEAP dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Ammunition>" "AMMO dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Perk>" "PERK dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Projectile>" "PROJ dynamic store is explicitly instantiated"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Explosion>" "EXPL dynamic store is explicitly instantiated"
Assert-Text "scripts/nikami/fnv_content_ledger.py" '"gameplaySystems"' "content ledger writes gameplay system artifact"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "def weapon_row" "content ledger writes WEAP gameplay rows"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "def ammo_row" "content ledger writes AMMO gameplay rows"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "unprovenGameplayGates" "content ledger bounds runtime-supported gameplay rows"

$ContentLedger = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedger -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
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
$gameplay = Get-Content -LiteralPath $GameplayPath -Raw | ConvertFrom-Json
$flatCounts = Get-FlatLedgerRecordCounts (Join-Path $LedgerDir.FullName "records.json")

Assert-Count $result.gameplaySystemCounts "WEAP" 506
Assert-Count $result.gameplaySystemCounts "AMMO" 145
Assert-Count $result.gameplaySystemCounts "PERK" 259
Assert-Count $result.gameplaySystemCounts "PROJ" 156
Assert-Count $result.gameplaySystemCounts "EXPL" 225
Assert-RuntimeSupportedGameplayRows -Rows $gameplay -RecordType "WEAP" -ExpectedCount 506
Assert-RuntimeSupportedGameplayRows -Rows $gameplay -RecordType "AMMO" -ExpectedCount 145
Assert-AmmoProjectileBindingRows -Rows $gameplay
Assert-LoadedPendingGameplayRows -Rows $gameplay -RecordType "PROJ" -ExpectedCount 156 -ExpectedGate "runtime-projectile-definition-binding"
Assert-LoadedPendingGameplayRows -Rows $gameplay -RecordType "EXPL" -ExpectedCount 225 -ExpectedGate "runtime-explosion-effect-binding"
$uniqueRuntimeExpected = @{}
foreach ($recordType in @("WEAP", "AMMO", "PERK", "PROJ", "EXPL")) {
    $raw = Get-RecordCount $flatCounts $recordType
    $unique = Get-UniqueGameplayFormCount -Rows $gameplay -RecordType $recordType
    Assert-Equal "$recordType gameplay raw row count" (@($gameplay | Where-Object { [string]$_.recordType -eq $recordType }).Count) $raw
    if ($unique -le 0) {
        throw "Unexpected empty unique gameplay FormID set for $recordType"
    }
    $uniqueRuntimeExpected[$recordType] = $unique
    Write-ProofLine "OK gameplay source-local unique FormIDs: $recordType=$unique raw=$raw duplicateDelta=$($raw - $unique)"
}
$runtimeExpected = @{
    WEAP = 506
    AMMO = 145
    PERK = 259
    PROJ = 156
    EXPL = 225
}
Assert-Equal "WEAP raw-to-runtime-inventory delta" ((Get-RecordCount $flatCounts "WEAP") - [int]$runtimeExpected["WEAP"]) 0

foreach ($needle in @("Weap10mmPistol", "Ammo10mm", "BuiltToDestroy", "WildWasteland", "SecuritronGrenadeProjectile", "SecuritronGrenadeExplosion")) {
    $match = $gameplay | Where-Object { $_.editorId -eq $needle } | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing gameplay ledger anchor editorId: $needle"
    }
    Write-ProofLine "OK gameplay ledger anchor: $needle"
}

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RunSeconds `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
if (!(Test-Path -LiteralPath $OpenMwLog -PathType Leaf)) {
    throw "Missing FNV flat OpenMW log: $OpenMwLog"
}

$runtimeLoaded = @{}
foreach ($recordType in @("WEAP", "AMMO", "PERK", "PROJ", "EXPL")) {
    $expected = [int]$runtimeExpected[$recordType]
    $actual = Get-RuntimeLoadedRecordTotal $OpenMwLog $recordType
    Assert-Equal "runtime loaded $recordType count" $actual $expected
    Assert-NoUnsupportedRecordSkip $OpenMwLog $recordType
    $runtimeLoaded[$recordType] = $actual
}

$summary = [pscustomobject]@{
    status = "PASS"
    repoRoot = $RepoRoot
    fnvData = $FnvData
    ledgerDir = $LedgerDir.FullName
    flatProofDir = $FlatProofDir
    gameplaySystems = $result.gameplaySystemCounts
    uniqueRuntimeExpected = $uniqueRuntimeExpected
    runtimeExpected = $runtimeExpected
    runtimeLoaded = $runtimeLoaded
    runtimeBoundary = "WEAP/AMMO rows are runtime-supported only for bounded store, ManualRef, inventory, HUD, icon, null-projectile hitscan/raycast accounting, and real 10mm ammo-decrement paths. AMMO projectile subfields are separately classified: null projectile rows are intentionally-excluded-with-proof, nonzero/undecoded projectile rows remain loaded-pending-runtime. PROJ/EXPL rows remain loaded-pending-runtime until spawned projectile visuals, impacts, and explosion effects have runtime proof gates."
    anchors = @("Weap10mmPistol", "Ammo10mm", "BuiltToDestroy", "WildWasteland", "SecuritronGrenadeProjectile", "SecuritronGrenadeExplosion")
}
$ContractPath = Join-Path $ProofDir "gameplay-record-store-contract.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ContractPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Gameplay ledger JSON: $GameplayPath"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $ContractPath"
Write-ProofLine "FNV gameplay record store contract PASS"
Write-ProofLine "WEAP/AMMO/PERK/PROJ/EXPL are source-backed, stored, and matched to PC-flat runtime inventory; WEAP/AMMO runtime scope is bounded and PROJ/EXPL gameplay execution remains gated separately."
