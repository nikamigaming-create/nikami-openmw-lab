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
$ProofDir = Join-Path $ProofRoot "fnv-proof-harness-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-File([string]$RelativePath) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing proof harness file: $RelativePath"
    }
    Write-ProofLine "OK file: $RelativePath"
    return $path
}

function Assert-Text([string]$Path, [string]$Needle, [string]$Description) {
    $text = Get-Content -LiteralPath $Path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing ${Description}: $Needle in $Path"
    }
    Write-ProofLine "OK contract: $Description"
}

Write-ProofLine "FNV proof harness contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$flat = Assert-File "scripts/nikami/run-fnv-flat-proof.ps1"
$doc = Assert-File "scripts/nikami/run-fnv-opening-doc-proof.ps1"
$walk = Assert-File "scripts/nikami/run-fnv-goodsprings-walk-replay-proof.ps1"
$ui = Assert-File "scripts/nikami/run-fnv-ui-baseline-proof.ps1"
$collision = Assert-File "scripts/nikami/run-fnv-goodsprings-collision-path-proof.ps1"
Assert-File "scripts/nikami/run-fnv-opening-vertical-slice.ps1" | Out-Null
$mugshot = Assert-File "scripts/nikami/run-fnv-actor-mugshot-sweep.ps1"
Assert-File "scripts/nikami/run-fnv-easy-pete-angle-sweep.ps1" | Out-Null

foreach ($needle in @(
    "[string]`$BuildDir",
    "[string]`$Configuration",
    "[string]`$FnvData",
    "[string]`$FnvConfigData",
    "[string]`$VcpkgRoot",
    "[string]`$ExtraOsgPluginDir",
    "[string]`$Triplet",
    "[string]`$ProofRoot",
    "[int]`$RunSeconds",
    "[string]`$ScreenshotFrames",
    "[string[]]`$RequireLogPattern",
    "[string]`$TerrainProbePoints",
    "[string]`$TerrainProbeGrid",
    "[switch]`$RequireTerrainProbeFullSupport",
    "[string]`$BootstrapCell",
    "[string]`$ActorTarget",
    "[switch]`$StageActor",
    "[switch]`$RequirePlayerTerrainSupport",
    "[switch]`$RequireFlatCameraSettled",
    "[switch]`$RequireScreenshotStability",
    "[switch]`$RequireSkyColorSanity"
)) {
    Assert-Text $flat $needle "flat proof parameter $needle"
}

Assert-Text $flat "OPENMW_PROOF_POSTURE_TARGET" "flat proof asks runtime to audit targeted actor posture"
Assert-Text $flat "World posture BAD lines:" "flat proof reports bad world posture lines"
Assert-Text $flat "Standing arm pose BAD lines:" "flat proof reports standing arm bind/T-pose lines"
Assert-Text $flat "Target world posture BAD lines:" "flat proof reports target bad world posture lines"
Assert-Text $flat "Target standing arm pose BAD lines:" "flat proof reports target standing arm bind/T-pose lines"
Assert-Text $flat "Screenshot stability status:" "flat proof reports screenshot stability"
Assert-Text $flat "screenshot-stability.json" "flat proof writes screenshot stability JSON"
Assert-Text $flat "bad world posture" "flat proof fails targeted actor bad world posture"
Assert-Text $flat "standing arm bind/T-pose" "flat proof fails targeted actor bind/T-pose posture"
Assert-Text $flat "did not prove screenshot stability" "flat proof fails unstable screenshot capture"
Assert-Text $mugshot "[switch]`$DisableNativeAnimationCallbacks" "mugshot native callback disable is explicit opt-in"
Assert-Text $mugshot "RequireScreenshotStability = `$true" "mugshot requires screenshot stability"
Assert-Text $mugshot "world posture .* verdict=BAD" "mugshot parses bad world posture failures"
Assert-Text $mugshot "standing arm pose .* verdict=BAD" "mugshot parses standing arm bind/T-pose failures"
Assert-Text $mugshot "MachineWorldPostureBad" "mugshot human review includes machine world posture failure column"
Assert-Text $mugshot "MachineArmPoseBad" "mugshot human review includes machine arm pose failure column"
Assert-Text $mugshot "MachineScreenshotStability" "mugshot human review includes screenshot stability column"
Assert-Text $doc "ActorTarget = `"DocMitchell`"" "Doc Mitchell actor target"
Assert-Text $doc "FNV/ESM4 FACE CHECK DocMitchell:" "Doc Mitchell face asset assertion"
Assert-Text $doc "FNV/ESM4 diag: play matched FormId:0x1104c0c group 'idle'" "Doc Mitchell animation assertion"
Assert-Text $walk "FNV/ESM4 proof walk: summary reached=1 dropped=0" "walk replay completion assertion"
Assert-Text $ui "ProofGuiMode = `"data`"" "UI baseline DATA pane request"
Assert-Text $collision "Movable static physics classification lines:" "movable static removed-classification anchor"
Assert-Text $collision "captured removed MSTT collision surgery" "movable static removed-surgery anchor"

Write-ProofLine ""
Write-ProofLine "FNV proof harness contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
