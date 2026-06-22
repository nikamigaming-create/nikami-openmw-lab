param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$ProofRoot = "",
    [string]$BsaTool = "D:\Modlists\fnv\openmw-source\MSVC2022_64\Release\bsatool.exe"
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

$Rg = Get-Command rg -ErrorAction SilentlyContinue
if ($null -eq $Rg) {
    throw "Missing rg. Install ripgrep or run this proof in the Codex lab shell."
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-movable-static-data/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Write-Section([string]$Name) {
    Write-ProofLine ""
    Write-ProofLine "[$Name]"
}

function Assert-File([string]$Path, [string]$Description) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Description}: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    Write-ProofLine "OK file: $Description -> $Path ($($item.Length) bytes)"
}

function Assert-EsmString([string]$Needle, [string]$Description) {
    & $Rg.Source -a -q --fixed-strings -- $Needle $EsmPath
    if ($LASTEXITCODE -ne 0) {
        throw "Missing ${Description}: $Needle"
    }
    Write-ProofLine "OK FNV ESM anchor: $Description -> $Needle"
}

function Assert-CodeString([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    Assert-File $path $Description
    & $Rg.Source -q --fixed-strings -- $Needle $path
    if ($LASTEXITCODE -ne 0) {
        throw "Missing code anchor in ${RelativePath}: $Needle"
    }
    Write-ProofLine "OK code anchor: $Description -> $Needle"
}

function Assert-CodeAbsent([string]$RelativePath, [string]$Needle, [string]$Description) {
    $path = Join-Path $RepoRoot $RelativePath
    Assert-File $path $Description
    & $Rg.Source -q --fixed-strings -- $Needle $path
    if ($LASTEXITCODE -eq 0) {
        throw "Unexpected code anchor in ${RelativePath}: $Needle"
    }
    Write-ProofLine "OK code absent: $Description -> $Needle"
}

function Assert-BsaEntry([string]$Entry, [string]$Description) {
    $match = $MeshList | Where-Object { $_.Equals($Entry, [System.StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
    if ($null -eq $match) {
        throw "Missing BSA entry ${Description}: $Entry"
    }
    Write-ProofLine "OK mesh BSA entry: $Description -> $match"
}

$EsmPath = Join-Path $FnvData "FalloutNV.esm"
$MeshesBsa = Join-Path $FnvData "Fallout - Meshes.bsa"

Write-ProofLine "FNV movable static/tumbleweed data proof $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine "BsaTool: $BsaTool"
Write-ProofLine ""

Write-Section "Files"
Assert-File $EsmPath "main ESM"
Assert-File $MeshesBsa "mesh BSA"
Assert-File $BsaTool "BSA listing tool"

Write-Section "Engine record and physics anchors"
Assert-CodeString "components/esm/defs.hpp" "REC_MSTT4 = esm4Recname(ESM4::REC_MSTT)" "MSTT record id"
Assert-CodeString "components/esm4/loadmstt.hpp" "struct MovableStatic" "ESM4 MSTT loader declaration"
Assert-CodeString "components/esm4/loadmstt.cpp" "void ESM4::MovableStatic::load" "ESM4 MSTT loader implementation"
Assert-CodeString "apps/openmw/mwclass/classes.cpp" "ESM4Named<ESM4::MovableStatic>::registerSelf();" "MSTT class registration"
Assert-CodeString "apps/openmw/mwclass/esm4base.cpp" "physics.addObject(ptr, VFS::Path::toNormalized(model), rotation, MWPhysics::CollisionType_World);" "generic ESM4 static collision insertion"
Assert-CodeAbsent "apps/openmw/mwclass/esm4base.cpp" "FNV/ESM4 movable static physics:" "runtime MSTT physics classification log removed"
Assert-CodeAbsent "apps/openmw/mwclass/esm4base.cpp" "effect-no-world-collision" "FNV MSTT effect collision skip removed"
Assert-CodeAbsent "apps/openmw/mwclass/esm4base.cpp" "tumbleweed-needs-movable-physics" "FNV tumbleweed collision skip removed"
Assert-CodeString "scripts/nikami/run-fnv-goodsprings-collision-path-proof.ps1" "Movable static physics classification lines:" "Goodsprings proof removed-classification gate"
Assert-CodeString "scripts/nikami/run-fnv-goodsprings-collision-path-proof.ps1" "captured removed MSTT collision surgery" "Goodsprings removed-MSTT-surgery failure gate"

Write-Section "BSA assets"
$MeshList = & $BsaTool list $MeshesBsa
if ($LASTEXITCODE -ne 0) {
    throw "BSA listing failed for $MeshesBsa"
}
Assert-BsaEntry "meshes\clutter\tumbleweednv.nif" "MSTT clutter tumbleweed model"
Assert-BsaEntry "meshes\effects\nv\sanddust\sanddust02.nif" "MSTT sand dust effect model"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\skeleton.nif" "creature tumbleweed skeleton"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\nvtumbleweed.nif" "creature tumbleweed skin"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\nvradioactivetumbleweed.nif" "radioactive creature tumbleweed skin"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\mtidle.kf" "creature idle animation"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\mtforward.kf" "creature forward animation"
Assert-BsaEntry "meshes\creatures\nvtumbleweed\mtfastforward.kf" "creature fast-forward animation"

Write-Section "ESM records and scripts"
Assert-EsmString "TumbleweedNV" "movable static editor id"
Assert-EsmString "clutter\TumbleweedNV.NIF" "movable static model"
Assert-EsmString "FXDust02" "sand dust movable static editor id"
Assert-EsmString "effects\NV\SandDust\SandDust02.NIF" "sand dust effect model"
Assert-EsmString "NVCRTumbleweed" "creature tumbleweed editor id"
Assert-EsmString "NVCRTumbleweedBaby" "baby creature tumbleweed editor id"
Assert-EsmString "NVCRTumbleweedRad" "radioactive creature tumbleweed editor id"
Assert-EsmString "creatures\NVTumbleweed\skeleton.nif" "creature tumbleweed skeleton model"
Assert-EsmString "NVCRTumbleweedScript" "creature tumbleweed script"
Assert-EsmString "NVCRTumbleweedRadSCRIPT" "radioactive creature tumbleweed script"
Assert-EsmString "TumbleweedRagdoll" "creature tumbleweed ragdoll"

Write-ProofLine ""
Write-ProofLine "FNV movable static/tumbleweed data proof PASS"
Write-ProofLine "ProofDir: $ProofDir"
