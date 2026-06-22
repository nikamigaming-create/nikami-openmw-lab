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

$Rg = Get-Command rg -ErrorAction SilentlyContinue
if ($null -eq $Rg) {
    throw "Missing rg. Install ripgrep or run this proof in the Codex lab shell."
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-opening-data/$Stamp"
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
    Write-ProofLine "OK ESM opening anchor: $Description -> $Needle"
}

$EsmPath = Join-Path $FnvData "FalloutNV.esm"
$DefaultIni = Join-Path $FnvRoot "Fallout_default.ini"
$IntroMovie = Join-Path $FnvData "Video/FNVIntro.bik"

Write-ProofLine "FNV opening data proof $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "FnvRoot: $FnvRoot"
Write-ProofLine "FnvData: $FnvData"
Write-ProofLine ""

Write-Section "Files"
Assert-File $EsmPath "main ESM"
Assert-File $DefaultIni "default INI"
Assert-File $IntroMovie "loose FNV intro movie"

Write-Section "Movie and cemetery/grave spine"
Assert-EsmString "MusIntroMusic" "intro music sound"
Assert-EsmString "FNVIntro" "intro movie record/string anchor"
Assert-EsmString "NVCemeteryIntro" "cemetery intro record anchor"
Assert-EsmString "NVCemeteryIntroFX" "cemetery intro image-space modifier"
Assert-EsmString "musCtrlGSCemetery" "Goodsprings cemetery music controller"
Assert-EsmString "GSCemetery" "Goodsprings cemetery anchor"
Assert-EsmString "GoodspringsCemeteryMapMarker" "Goodsprings cemetery map marker"
Assert-EsmString "EMTGrave" "grave ambient emitter"
Assert-EsmString "NV_OpenGrave" "open grave placed/static anchor"
Assert-EsmString "NVOpenGrave0000" "open grave scene anchor"
Assert-EsmString "Grave01a" "grave static anchor"
Assert-EsmString "GameExamineGrave01a" "grave examine prompt/message anchor"

Write-Section "Doc Mitchell wake-up spine"
Assert-EsmString "VCG01" "opening quest anchor"
Assert-EsmString "GSDocMitchellScript" "Doc Mitchell house script"
Assert-EsmString "GSDocMitchellDoorScript" "Doc Mitchell door script"
Assert-EsmString "GSDocMitchellExitTriggerScript" "Doc Mitchell exit trigger script"
Assert-EsmString "GSDocMitchellHouse" "Doc Mitchell house cell"
Assert-EsmString "DocMitchellREF" "placed Doc Mitchell reference"
Assert-EsmString "MaleUniqueDocMitchell" "Doc Mitchell race/body anchor"
Assert-EsmString "VCG01DocMarkerFirstPositionREF" "Doc Mitchell first-position marker ref"
Assert-EsmString "VCG01DocExamMarkerREF" "Doc Mitchell exam marker ref"
Assert-EsmString "VCG01DocMitchellChairTriggerSCRIPT" "Doc Mitchell chair trigger script"
Assert-EsmString "VCG01DocMitchellCouchTriggerSCRIPT" "Doc Mitchell couch trigger script"
Assert-EsmString "VCG01DocMitchellBedsideStandingPackage" "Doc Mitchell bedside package"
Assert-EsmString "VCG01DocMitchellFirstPosition" "Doc Mitchell first-position package"
Assert-EsmString "VCG01DocMitchellDialogueStart" "Doc Mitchell dialogue package"
Assert-EsmString "VCG01DocMitchellTravelToExamSpot" "Doc Mitchell exam travel package"
Assert-EsmString "VCG01DocMitchellTravelToPlayerAtTester" "Doc Mitchell tester travel package"
Assert-EsmString "VCG01DocMitchellFarewellDialogueStart" "Doc Mitchell farewell dialogue package"
Assert-EsmString "VCG01DocMitchellTravelToExit" "Doc Mitchell exit travel package"
Assert-EsmString "VCG01PlayerSection0" "player wake-up package section 0"
Assert-EsmString "VCG01PlayerSection1" "player wake-up package section 1"

Write-Section "Doc Mitchell wake-up audio and exit"
Assert-EsmString "NPCDocMPlyrEyesOpen" "Doc Mitchell wake-up eyes-open audio"
Assert-EsmString "NPCDocMPlyrSitUp" "Doc Mitchell wake-up sit-up audio"
Assert-EsmString "NPCDocMPlyrStand" "Doc Mitchell wake-up stand audio"
Assert-EsmString "NPCDocMWoahThere" "Doc Mitchell wake-up dialogue audio"
Assert-EsmString "GSDocMitchellDoor" "Doc Mitchell house door"
Assert-EsmString "GSDocMitchellExitTrigger" "Doc Mitchell exit trigger"
Assert-EsmString "VendorContainerDocMitchell" "Doc Mitchell vendor container"

Write-ProofLine ""
Write-ProofLine "FNV opening data proof PASS"
Write-ProofLine "ProofDir: $ProofDir"
