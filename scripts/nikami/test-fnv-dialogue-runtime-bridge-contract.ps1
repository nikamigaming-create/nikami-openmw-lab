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
$ProofDir = Join-Path $ProofRoot "fnv-dialogue-runtime-bridge-contract/$Stamp"
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

Write-ProofLine "FNV dialogue runtime bridge contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/mwworld/store.hpp" "ESM::Dialogue* insert(const ESM::Dialogue& dialogue);" "dialogue store runtime insert"
Assert-Text "apps/openmw/mwworld/store.hpp" "void rebuildRuntimeIndex();" "dialogue store runtime index rebuild"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "bridgeEsm4QuestDialogueStores" "ESM4 quest/dialogue bridge helper"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questDialogues=" "runtime bridge quest dialogue count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "topicDialogues=" "runtime bridge topic dialogue count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "questInfos=" "runtime bridge quest INFO count log"
Assert-Text "components/esm4/loadinfo.cpp" "mSound = ESM::FormId::fromUint32(mResponseData.sound);" "FNV TRDT INFO sound loader"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "resolveEsm4SoundFile" "ESM4 INFO sound FormId resolver"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mSound = infoSound;" "INFO sound path transfer"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "resolvedInfoSounds=" "runtime bridge resolved INFO sound count log"
Assert-Text "apps/openmw/mwworld/esmstore.cpp" "info.mResultScript = source.mScript.scriptSource;" "INFO result script transfer"
Assert-Text "scripts/nikami/test-fnv-data-inventory.ps1" '"INFO" = "ESM4-to-runtime quest INFO response bridge"' "INFO runtime inventory classification"

Write-ProofLine ""
Write-ProofLine "FNV dialogue runtime bridge contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
