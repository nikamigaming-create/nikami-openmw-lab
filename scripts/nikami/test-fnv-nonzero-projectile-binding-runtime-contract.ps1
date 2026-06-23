param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 20
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

$AmmoEditorId = "Ammo40mmGrenadeIncendiary"
$ProjectileEditorId = "40mmGrenadeProjectileInc"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-nonzero-projectile-binding-runtime-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null
$RuntimeRunSeconds = [Math]::Max($RunSeconds, 20)

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
    Write-ProofLine "OK code anchor: $Description"
}

function Assert-FileContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing ${Description}: pattern=$Pattern path=$Path"
    }
    Write-ProofLine "OK ${Description}"
    return $match
}

function Assert-Equal([string]$Description, [object]$Actual, [object]$Expected) {
    if ($Actual -ne $Expected) {
        throw "Unexpected ${Description}: actual=$Actual expected=$Expected"
    }
    Write-ProofLine "OK ${Description}: $Actual"
}

function Assert-GreaterThan([string]$Description, [int]$Actual, [int]$Minimum) {
    if ($Actual -le $Minimum) {
        throw "Unexpected ${Description}: actual=$Actual minimumExclusive=$Minimum"
    }
    Write-ProofLine "OK ${Description}: $Actual"
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

Write-ProofLine "FNV nonzero projectile binding runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "RunSeconds: $RuntimeRunSeconds"
Write-ProofLine "AmmoEditorId: $AmmoEditorId"
Write-ProofLine "ProjectileEditorId: $ProjectileEditorId"
Write-ProofLine ""
Write-ProofLine "Runtime boundary: This proves one real nonzero FNV AMMO projectile FormID resolves through the runtime ESM4 store to a PROJ record and VFS-visible projectile mesh on PC-flat. It does not claim spawned projectile motion, collision, impact decals, explosions, tracer effects, sounds, or damage application."
Write-ProofLine ""

Assert-Text "components/esm4/loadammo.cpp" "mData.mProjectile" "AMMO loader captures projectile FormID"
Assert-Text "components/esm4/loadammo.cpp" 'case ESM::fourCC("DAT2")' "AMMO DAT2 projectile source"
Assert-Text "components/esm4/loadproj.hpp" "REC_PROJ4" "PROJ typed record id"
Assert-Text "components/esm4/loadproj.cpp" 'case ESM::fourCC("MODL")' "PROJ model loader"
Assert-Text "components/esm4/loadproj.cpp" "mData = readRawSubrecord" "PROJ DATA raw payload accounted"
Assert-Text "apps/openmw/mwworld/esmstore.hpp" "Store<ESM4::Projectile>" "PROJ store in runtime ESMStore"
Assert-Text "apps/openmw/mwworld/store.cpp" "TypedDynamicStore<ESM4::Projectile>" "PROJ dynamic store instantiation"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "case ESM::REC_PROJ4:" "PROJ FormID participates in generic runtime ESMStore lookup"
Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_NONZERO_PROJECTILE" "nonzero projectile runtime proof switch"
Assert-Text "apps/openmw/engine.cpp" "Ammo40mmGrenadeIncendiary" "selected nonzero projectile ammo"
Assert-Text "apps/openmw/engine.cpp" "40mmGrenadeProjectileInc" "selected nonzero projectile PROJ"
Assert-Text "apps/openmw/engine.cpp" "definition-model-binding-runtime-supported" "bounded projectile proof classification"
Assert-Text "apps/openmw/engine.cpp" "spawnedProjectileRuntime=loaded-pending-runtime" "spawned projectile behavior remains pending"
Assert-Text "components/misc/resourcehelpers.cpp" "correctMeshPath" "projectile model path is normalized under meshes"
Assert-Text "scripts/nikami/run-fnv-flat-proof.ps1" "FnvNonzeroProjectileBindingTrace" "flat proof can enable nonzero projectile binding trace"
Assert-Text "scripts/nikami/fnv_content_ledger.py" "projectileBindingClassification" "content ledger classifies AMMO projectile subfield"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"PROJ": "projectile records are source-backed and stored pending runtime projectile binding"' "PROJ remains loaded-pending runtime"

$ContentLedgerScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
& $ContentLedgerScript -FnvRoot $FnvRoot -FnvData $FnvData -ProofRoot $ProofRoot -Content $FlatContent
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}

$ContentLedgerDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-content-ledger") "FNV content ledger"
$GameplayRows = Read-JsonArray (Join-Path $ContentLedgerDir "gameplay-systems.json") "content ledger gameplay systems"
$AmmoRows = @($GameplayRows | Where-Object { [string]$_.recordType -eq "AMMO" })
$NonzeroAmmoRows = @($AmmoRows | Where-Object {
        ![string]::IsNullOrWhiteSpace([string]$_.projectile) -and [string]$_.projectile -ne "0x00000000"
    })
$AmmoRow = $AmmoRows | Where-Object { [string]$_.editorId -eq $AmmoEditorId } | Select-Object -First 1
if ($null -eq $AmmoRow) {
    throw "Missing expected AMMO row in gameplay ledger: $AmmoEditorId"
}
if ([string]::IsNullOrWhiteSpace([string]$AmmoRow.projectile) -or [string]$AmmoRow.projectile -eq "0x00000000") {
    throw "Expected selected AMMO to carry a nonzero projectile FormID: $AmmoEditorId projectile=$($AmmoRow.projectile)"
}
if ([string]$AmmoRow.projectileBindingClassification -ne "loaded-pending-runtime") {
    throw "Expected selected AMMO projectile subfield to remain pending spawned runtime proof before this gate: $($AmmoRow.projectileBindingClassification)"
}
$ProjectileRow = $GameplayRows | Where-Object {
    [string]$_.recordType -eq "PROJ" -and [string]$_.formId -eq [string]$AmmoRow.projectile
} | Select-Object -First 1
if ($null -eq $ProjectileRow) {
    throw "Missing PROJ row referenced by selected AMMO: ammo=$AmmoEditorId projectile=$($AmmoRow.projectile)"
}
Assert-Equal "selected projectile editor id" ([string]$ProjectileRow.editorId) $ProjectileEditorId
Assert-GreaterThan "nonzero AMMO projectile rows" $NonzeroAmmoRows.Count 0
Assert-GreaterThan "selected PROJ model length" ([string]$ProjectileRow.model).Length 0
Write-ProofLine "Selected AMMO source: plugin=$($AmmoRow.plugin) formId=$($AmmoRow.formId) projectile=$($AmmoRow.projectile)"
Write-ProofLine "Selected PROJ source: plugin=$($ProjectileRow.plugin) formId=$($ProjectileRow.formId) model=$($ProjectileRow.model)"

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
& $FlatProofScript `
    -FnvData $FnvData `
    -VcpkgRoot $VcpkgRoot `
    -ProofRoot $ProofRoot `
    -RunSeconds $RuntimeRunSeconds `
    -FnvNonzeroProjectileBindingTrace `
    -RequireLogPattern @(
        "FNV/ESM4 proof: nonzero projectile binding PASS .*ammoEdid=$AmmoEditorId .*ammoProjectileSet=1 .*projectileRecordType=0x[1-9a-fA-F][0-9a-fA-F]* .*projectileFound=1 .*projectileEdid=$ProjectileEditorId .*projectileModelExists=1 .*projectileDataBytes=[1-9][0-9]* .*runtimeBoundary=definition-model-binding-runtime-supported .*spawnedProjectileRuntime=loaded-pending-runtime"
    ) `
    -NoSound
if ($LASTEXITCODE -ne 0) {
    throw "FNV flat proof failed with exit code $LASTEXITCODE."
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
$summary = Join-Path $FlatProofDir "summary.txt"
$runtimeMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: nonzero projectile binding PASS" "runtime nonzero projectile binding proof"
Assert-FileContains $summary "^FnvNonzeroProjectileBindingTrace: True$" "flat proof required nonzero projectile trace" | Out-Null

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    contentLedgerDir = $ContentLedgerDir
    flatProofDir = $FlatProofDir
    ammoEditorId = $AmmoEditorId
    ammoFormId = $AmmoRow.formId
    ammoProjectileFormId = $AmmoRow.projectile
    projectileEditorId = $ProjectileEditorId
    projectileFormId = $ProjectileRow.formId
    projectileModel = $ProjectileRow.model
    nonzeroAmmoProjectileRows = $NonzeroAmmoRows.Count
    runtimeLog = $runtimeMatch.Line
    classifications = @(
        [ordered]@{
            system = "AMMO.mProjectile"
            item = "$AmmoEditorId -> $ProjectileEditorId"
            classification = "runtime-supported"
            proof = "Selected nonzero AMMO projectile FormID resolves to a typed PROJ record and VFS-visible mesh in PC-flat runtime."
            notProven = "Projectile spawning, flight, impact, explosion, tracer, sound, and damage behavior remain separate runtime gates."
        },
        [ordered]@{
            system = "PROJ"
            item = $ProjectileEditorId
            classification = "loaded-pending-runtime"
            proof = "PROJ definition and model binding are proved for this selected row only."
            notProven = "Full PROJ gameplay execution remains loaded-pending-runtime until spawned projectile behavior is proven."
        }
    )
    runtimeBoundary = "Definition/model binding is runtime-supported for the selected nonzero AMMO->PROJ path; spawned projectile gameplay remains loaded-pending-runtime."
}
$metadataPath = Join-Path $ProofDir "fnv-nonzero-projectile-binding-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Content ledger: $ContentLedgerDir"
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV nonzero projectile binding runtime contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
