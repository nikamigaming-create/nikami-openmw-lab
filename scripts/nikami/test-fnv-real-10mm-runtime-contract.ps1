param(
    [string]$FnvRoot = "D:\SteamLibrary\steamapps\common\Fallout New Vegas",
    [string]$FnvData = "",
    [string]$VcpkgRoot = "D:\code\c\FMODS\vcpkg",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 8,
    [int]$ProofFrame = 150
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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-real-10mm-runtime-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-Text([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing source file for ${Description}: $RelativePath"
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
        throw "Missing ${Description}: $Pattern in $Path"
    }
    Write-ProofLine "OK ${Description}: $($match.Line.Trim())"
    return $match.Matches[0]
}

function Assert-FileNotContains([string]$Path, [string]$Pattern, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing file for ${Description}: $Path"
    }
    $matches = @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)
    if ($matches.Count -gt 0) {
        throw "Unexpected ${Description}: $($matches[0].Line.Trim())"
    }
    Write-ProofLine "OK absent ${Description}: $Pattern"
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

function Set-ProofEnv([hashtable]$Previous, [string]$Name, [object]$Value) {
    if (!$Previous.ContainsKey($Name)) {
        $Previous[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
    }
    [Environment]::SetEnvironmentVariable($Name, [string]$Value, "Process")
}

function Restore-ProofEnv([hashtable]$Previous) {
    foreach ($name in $Previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $Previous[$name], "Process")
    }
}

Write-ProofLine "FNV real 10mm runtime contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

Assert-Text "apps/openmw/engine.cpp" "OPENMW_FNV_PROOF_REAL_10MM" "real 10mm proof environment hook"
Assert-Text "apps/openmw/engine.cpp" "Weap10mmPistol" "real 10mm weapon editor id"
Assert-Text "apps/openmw/engine.cpp" "Ammo10mm" "real 10mm ammo editor id"
Assert-Text "apps/openmw/engine.cpp" "weaponAmmoFound=" "real 10mm weapon-linked ammo proof"
Assert-Text "apps/openmw/engine.cpp" "real 10mm firing trace PASS" "real 10mm firing proof log"
Assert-Text "apps/openmw/engine.cpp" "real 10mm icon probe" "real 10mm icon resolver proof log"
Assert-Text "apps/openmw/mwclass/esm4base.hpp" "std::is_same_v<Record, ESM4::Weapon>" "ESM4 weapon equipment slot classification"
Assert-Text "apps/openmw/mwclass/esm4base.hpp" "MWWorld::InventoryStore::Slot_Ammunition" "ESM4 ammo equipment slot classification"
Assert-Text "apps/openmw/mwgui/hud.cpp" "getRefId().serializeText()" "HUD form-id-safe ammo fallback"
Assert-Text "components/misc/resourcehelpers.cpp" "textures/interface/icons" "Fallout interface icon root resolution"
Assert-Text "components/misc/resourcehelpers.cpp" "interface/icons" "Fallout relative interface icon root resolution"
Assert-Text "apps/openmw/mwworld/manualref.cpp" "case ESM::REC_WEAP4:" "ManualRef ESM4 weapon construction"
Assert-Text "apps/openmw/mwworld/manualref.cpp" "case ESM::REC_AMMO4:" "ManualRef ESM4 ammo construction"
Assert-Text "apps/openmw/mwworld/manualref.cpp" "case ESM::REC_ARMO4:" "ManualRef ESM4 armor construction"
Assert-Text "apps/openmw/mwworld/manualref.cpp" "case ESM::REC_MISC4:" "ManualRef ESM4 misc construction"
Assert-Text "apps/openmw/mwworld/manualref.cpp" "case ESM::REC_ALCH4:" "ManualRef ESM4 potion construction"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"WEAP": "weapon records feed inventory, actor, and HUD ammo paths"' "WEAP runtime-supported classification"
Assert-Text "scripts/nikami/fnv_no_silent_skip_classification.py" '"AMMO": "ammo records feed weapon/HUD ammo paths"' "AMMO runtime-supported classification"

$FlatProofScript = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$previousEnv = @{}
try {
    Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_REAL_10MM" "1"
    Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_REAL_10MM_FRAME" $ProofFrame
    & $FlatProofScript `
        -FnvData $FnvData `
        -VcpkgRoot $VcpkgRoot `
        -ProofRoot $ProofRoot `
        -RunSeconds $RunSeconds `
        -NoSound `
        -RequireLogPattern @(
            "FNV/ESM4 proof: real 10mm store scan .*weaponFound=1.*weaponAmmoFound=0.*namedAmmoFound=1.*ammoFound=1.*ammoSource=editorIdFallback",
            "FNV/ESM4 proof: real 10mm ammo reference classified known-blocked .*reason=weapon-ammo-reference-not-loaded-as-AMMO",
            "FNV/ESM4 proof: real 10mm icon probe .*rawIcon=`"interface/icons/pipboyimages/weapons/weapons_10mm_pistol.dds`".*resolvedIcon=`"textures/interface/icons/pipboyimages/weapons/weapons_10mm_pistol.dds`".*resolvedExists=1.*canonicalExists=1",
            "FNV/ESM4 proof: real 10mm equip PASS",
            "FNV/ESM4 proof: real 10mm muzzle ray origin=",
            "FNV/ESM4 proof: real 10mm firing trace request .*ammoBefore=48",
            "FNV/ESM4 proof: real 10mm firing trace PASS .*ammoBefore=48.*ammoAfter=47"
        )
}
finally {
    Restore-ProofEnv $previousEnv
}

$FlatProofDir = Get-LatestProofDir (Join-Path $ProofRoot "fnv-flat-proof") "FNV flat proof"
$OpenMwLog = Join-Path $FlatProofDir "openmw.log"
Assert-FileNotContains $OpenMwLog "FNV/ESM4 proof: real 10mm (equip|firing) BLOCKED|FNV/ESM4 proof: real 10mm equip FAIL" "real 10mm blocker/failure line"
Assert-FileNotContains $OpenMwLog "FNV/ESM4 proof: real 10mm ammo compatibility WARN|Error in frame: RefId is not a string" "real 10mm compatibility/frame id failure"
Assert-FileNotContains $OpenMwLog "Failed to open image: 'icons/pipboyimages/weapons/weapons_10mm_pistol.dds'" "10mm icon fallback"
Assert-FileContains $OpenMwLog "FNV/ESM4 proof: real 10mm ammo reference classified known-blocked .*weaponAmmo=FormId:0x11537e6.*selectedAmmo=.*selectedAmmoEdid=Ammo10mm" "real 10mm ammo reference classification" | Out-Null
$equipMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: real 10mm equip PASS .*weaponEdid=(?<weaponEdid>[^ ]+).*damage=(?<damage>[0-9]+).*clipSize=(?<clip>[0-9]+).*ammoEdid=(?<ammoEdid>[^ ]+).*ammoProjectile=(?<projectile>[^ ]+).*ammoDamage=(?<ammoDamage>[0-9.]+).*ammoCount=(?<ammoCount>[0-9]+)" "real 10mm equip proof"
$fireMatch = Assert-FileContains $OpenMwLog "FNV/ESM4 proof: real 10mm firing trace PASS ammoBefore=(?<before>[0-9]+) ammoAfter=(?<after>[0-9]+) raycastAvailable=(?<raycast>[01]) hit=(?<hit>[01])" "real 10mm firing trace proof"

$metadata = [ordered]@{
    status = "PASS"
    stamp = $Stamp
    repoRoot = $RepoRoot
    fnvData = $FnvData
    flatProofDir = $FlatProofDir
    weapon = [ordered]@{
        editorId = $equipMatch.Groups["weaponEdid"].Value
        damage = [int]$equipMatch.Groups["damage"].Value
        clipSize = [int]$equipMatch.Groups["clip"].Value
    }
    ammo = [ordered]@{
        editorId = $equipMatch.Groups["ammoEdid"].Value
        projectile = $equipMatch.Groups["projectile"].Value
        damage = [double]$equipMatch.Groups["ammoDamage"].Value
        equippedCount = [int]$equipMatch.Groups["ammoCount"].Value
        before = [int]$fireMatch.Groups["before"].Value
        after = [int]$fireMatch.Groups["after"].Value
    }
    raycastAvailable = [int]$fireMatch.Groups["raycast"].Value
    hit = [int]$fireMatch.Groups["hit"].Value
    runtimeBoundary = "This proves PC-flat ESM4 WEAP/AMMO store lookup, ManualRef construction, equip slots, HUD-safe form ids, retail icon resolution, ray trace request, and ammo decrement. The Weap10mmPistol mAmmo reference is explicitly known-blocked because it does not resolve to the AMMO store in this load order; full projectile visuals/effects remain separate PROJ/EXPL gates."
}
$metadataPath = Join-Path $ProofDir "fnv-real-10mm-runtime-contract.json"
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Flat proof: $FlatProofDir"
Write-ProofLine "Contract JSON: $metadataPath"
Write-ProofLine "FNV real 10mm runtime contract PASS"
