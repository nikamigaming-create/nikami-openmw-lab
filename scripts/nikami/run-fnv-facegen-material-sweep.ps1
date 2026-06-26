param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ProofRoot = "",
    [string]$ActorTarget = "GSEasyPete",
    [ValidateSet("npc", "creature", "auto")]
    [string]$ActorKind = "npc",
    [int]$RunSeconds = 45,
    [int]$ActorFrame = 900,
    [string]$ScreenshotFrames = "900",
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function New-ProofRunStamp {
    $now = Get-Date
    $processId = [System.Diagnostics.Process]::GetCurrentProcess().Id
    $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
    return "{0}_{1}_{2}" -f $now.ToString("yyyyMMdd_HHmmss_fff"), $processId, $suffix
}

function Resolve-FnvDataFromLatestHarvest([string]$Root) {
    $harvestRoot = Join-Path $Root "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) { return "" }
    $manifests = Get-ChildItem -LiteralPath $harvestRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "manifest.json" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    foreach ($manifestPath in $manifests) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            $candidate = [string]$manifest.fnvData
            if (![string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
        catch {
        }
    }
    return ""
}

function Write-ManifestLine([string]$Text) {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Get-GeneratedFaceGenLine([string]$LogPath) {
    if (!(Test-Path -LiteralPath $LogPath -PathType Leaf)) { return "" }
    $lines = Select-String -LiteralPath $LogPath -Pattern "generated NPC FaceGen diffuse" -SimpleMatch
    if ($null -eq $lines -or $lines.Count -eq 0) { return "" }
    return [string]$lines[-1].Line
}

function Get-MatchValue([string]$Text, [string]$Pattern) {
    $match = [regex]::Match($Text, $Pattern)
    if (!$match.Success) { return "" }
    return $match.Groups[1].Value
}

$BuilderRunner = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
if (!(Test-Path -LiteralPath $BuilderRunner -PathType Leaf)) {
    throw "Missing character builder runner: $BuilderRunner"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Resolve-FnvDataFromLatestHarvest $ProofRoot
}
if ([string]::IsNullOrWhiteSpace($FnvData) -or !(Test-Path -LiteralPath $FnvData -PathType Container)) {
    throw "Set NIKAMI_FNV_DATA or pass -FnvData; sweep cases use isolated proof roots and cannot infer retail data from nested output roots."
}

$Stamp = New-ProofRunStamp
$SweepDir = Join-Path $ProofRoot "fnv-facegen-material-sweep/$Stamp"
New-Item -ItemType Directory -Force -Path $SweepDir | Out-Null
$SummaryFile = Join-Path $SweepDir "summary.txt"
$ManifestPath = Join-Path $SweepDir "facegen-material-sweep.json"

$variants = @(
    [pscustomobject][ordered]@{
        Name = "current"
        CompositeScale = ""
        EgtDiffuseScale = ""
        BiasR = ""
        BiasG = ""
        BiasB = ""
        DisableEgt = ""
    },
    [pscustomobject][ordered]@{
        Name = "egt-half"
        CompositeScale = ""
        EgtDiffuseScale = "0.5"
        BiasR = ""
        BiasG = ""
        BiasB = ""
        DisableEgt = ""
    },
    [pscustomobject][ordered]@{
        Name = "egt-off"
        CompositeScale = ""
        EgtDiffuseScale = ""
        BiasR = ""
        BiasG = ""
        BiasB = ""
        DisableEgt = "1"
    },
    [pscustomobject][ordered]@{
        Name = "warmer-green"
        CompositeScale = "2"
        EgtDiffuseScale = "0.5"
        BiasR = "-0.02"
        BiasG = "0.04"
        BiasB = "0.01"
        DisableEgt = ""
    }
)

Write-ManifestLine "FNV FaceGen material sweep $Stamp"
Write-ManifestLine "RepoRoot: $RepoRoot"
Write-ManifestLine "SweepDir: $SweepDir"
Write-ManifestLine "ActorTarget: $ActorTarget"
Write-ManifestLine "FnvData: $FnvData"
Write-ManifestLine "Policy: no retail assets copied into repo; generated proof output only"

$results = @()
foreach ($variant in $variants) {
    Write-ManifestLine ""
    Write-ManifestLine "CASE $($variant.Name) compositeScale=$($variant.CompositeScale) egtDiffuseScale=$($variant.EgtDiffuseScale) bias=$($variant.BiasR),$($variant.BiasG),$($variant.BiasB) disableEgt=$($variant.DisableEgt)"
    $startedAt = Get-Date
    $caseRoot = Join-Path $SweepDir $variant.Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null

    $args = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        ProofRoot = $caseRoot
        ActorTarget = $ActorTarget
        ActorKind = $ActorKind
        Phases = @("headgear")
        Angles = @("front")
        RunSeconds = $RunSeconds
        ActorFrame = $ActorFrame
        ScreenshotFrames = $ScreenshotFrames
        AllowMissingActorVisibleHandGeometry = $true
        FnvFaceGenCompositeScale = $variant.CompositeScale
        FnvEgtDiffuseScale = $variant.EgtDiffuseScale
        FnvFaceGenBiasR = $variant.BiasR
        FnvFaceGenBiasG = $variant.BiasG
        FnvFaceGenBiasB = $variant.BiasB
        FnvDisableEgtDiffuseSynthesis = $variant.DisableEgt
    }
    if (![string]::IsNullOrWhiteSpace($VcpkgRoot)) { $args.VcpkgRoot = $VcpkgRoot }
    if ($NoSound) { $args.NoSound = $true }

    $status = "PASS"
    $errorText = ""
    try {
        & $BuilderRunner @args | Out-Host
    }
    catch {
        $status = "FAIL"
        $errorText = $_.Exception.Message
        Write-Warning "Variant $($variant.Name) failed: $errorText"
    }

    $suite = Get-ChildItem -LiteralPath (Join-Path $caseRoot "fnv-character-builder") -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    $reportPath = ""
    $proofDir = ""
    $screenshot = ""
    $faceCrop = ""
    $facegenPng = ""
    $facegenLine = ""
    if ($null -ne $suite) {
        $reportPath = Join-Path $suite.FullName "headgear_front/character-builder-report.json"
        if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            $proofDir = [string]$report.proofDir
        }
        $screenshotCandidate = Join-Path $suite.FullName "headgear_front/screenshot000.png"
        if (Test-Path -LiteralPath $screenshotCandidate -PathType Leaf) { $screenshot = $screenshotCandidate }
        $faceCropCandidate = Join-Path $suite.FullName "headgear_front/review-crops/screenshot000-face-hat-review.png"
        if (Test-Path -LiteralPath $faceCropCandidate -PathType Leaf) { $faceCrop = $faceCropCandidate }
    }
    if (![string]::IsNullOrWhiteSpace($proofDir)) {
        $facegen = Get-ChildItem -LiteralPath (Join-Path $proofDir "facegen-proof") -Filter "*.png" -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $facegen) { $facegenPng = $facegen.FullName }
        $facegenLine = Get-GeneratedFaceGenLine (Join-Path $proofDir "openmw.log")
    }

    $result = [pscustomobject][ordered]@{
        name = $variant.Name
        status = $status
        error = $errorText
        startedAt = $startedAt.ToString("o")
        compositeScale = $variant.CompositeScale
        egtDiffuseScale = $variant.EgtDiffuseScale
        biasR = $variant.BiasR
        biasG = $variant.BiasG
        biasB = $variant.BiasB
        disableEgt = $variant.DisableEgt
        suiteDir = if ($null -ne $suite) { $suite.FullName } else { "" }
        proofDir = $proofDir
        screenshot = $screenshot
        faceCrop = $faceCrop
        facegenProofImage = $facegenPng
        generatedAverage = Get-MatchValue $facegenLine "generatedAverage=\(([^)]*)\)"
        egtDeltaAverage = Get-MatchValue $facegenLine "egtDeltaAverage=\(([^)]*)\)"
        facegenLogLine = $facegenLine
    }
    $results += $result
    Write-ManifestLine "RESULT $($variant.Name) status=$status proof=$proofDir faceCrop=$faceCrop facegen=$facegenPng generatedAverage=$($result.generatedAverage)"
}

$manifest = [pscustomobject][ordered]@{
    schema = "nikami-fnv-facegen-material-sweep-v1"
    generatedAt = (Get-Date).ToString("o")
    repoRoot = $RepoRoot
    sweepDir = $SweepDir
    actorTarget = $ActorTarget
    policy = "generated proof output only; no retail assets copied into repo"
    results = $results
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
Write-ManifestLine ""
Write-ManifestLine "Manifest: $ManifestPath"
Write-ManifestLine "SweepDir: $SweepDir"
