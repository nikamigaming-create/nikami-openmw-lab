param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-runtime-bridge-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-CodeString([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source file for ${Description}: $RelativePath"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing code anchor for ${Description}: $Needle"
    }
    Write-ProofLine "OK code anchor: $Description"
}

Write-ProofLine "FNV runtime bridge contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "bridgeEsm4RuntimeStores" "bridge helper exists"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "store.get<ESM4::GameSetting>()" "reads ESM4 GMST store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "Store<ESM::GameSetting>" "writes ESM3 GMST runtime store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "store.get<ESM4::GlobalVariable>()" "reads ESM4 GLOB store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "Store<ESM::Global>" "writes ESM3 global runtime store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "store.get<ESM4::Script>()" "reads ESM4 SCPT store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "Store<ESM::Script>" "writes ESM3 script runtime store"
Assert-CodeString "apps/openmw/mwworld/esmstore.cpp" "FNV/ESM4 proof: bridged runtime records gmst=" "runtime bridge proof log"
Assert-CodeString "scripts/nikami/test-fnv-data-inventory.ps1" '"GMST" = "ESM4-to-runtime game setting bridge"' "GMST inventory runtime claim"
Assert-CodeString "scripts/nikami/test-fnv-data-inventory.ps1" '"GLOB" = "ESM4-to-runtime global variable bridge"' "GLOB inventory runtime claim"
Assert-CodeString "scripts/nikami/fnv_no_silent_skip_classification.py" '"SCPT": "script source is stored and bridged as evidence pending full FNV script VM/runtime semantics"' "SCPT loaded-pending runtime boundary"

Write-ProofLine ""
Write-ProofLine "FNV runtime bridge contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
