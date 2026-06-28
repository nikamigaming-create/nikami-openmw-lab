param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$ProofRoot = "",
    [string]$ActorTarget = "GSEasyPete",
    [ValidateSet("npc", "creature", "auto")]
    [string]$ActorKind = "npc",
    [ValidateSet("right-hand-close", "left-hand-close", "hands-close", "arms")]
    [string]$NeutralActorPreviewProfile = "right-hand-close",
    [string]$BaselineScreenshotFrames = "240",
    [string]$LiveScreenshotFrames = "360",
    [int]$BaselineRunSeconds = 42,
    [int]$LiveRunSeconds = 48,
    [int]$LiveWarmupSeconds = 12,
    [int]$TimeoutSeconds = 110,
    [string]$BootstrapCell = "FormId:0x10daeb9",
    [double]$BootstrapX = -67480,
    [double]$BootstrapY = 1500,
    [double]$BootstrapZ = 8425,
    [double]$BootstrapRotX = 0,
    [double]$BootstrapRotY = 0,
    [double]$BootstrapRotZ = 1.5708,
    [double]$BootstrapHour = 10,
    [double]$ActorStageX = -67480,
    [double]$ActorStageY = 1500,
    [double]$ActorStageZ = 8425,
    [double]$ActorStageRotX = 0,
    [double]$ActorStageRotY = 0,
    [double]$ActorStageRotZ = 1.5708,
    [double]$ActorViewOffsetZ = 108,
    [double]$ActorViewTargetZ = 108,
    [string[]]$BaselineAngles = @("front"),
    [string]$HandBone = "Bip01 R Hand",
    [string]$FingerBone = "Bip01 R Finger11",
    [double]$HandRotationZ = 25.0,
    [double]$FingerRotationZ = 15.0,
    [string]$FnvKeepRiggedHandParts = "1",
    [string]$WeightSelector = "all",
    [double]$MinScreenshotMeanAbsDelta = 0.001,
    [int]$MinScreenshotChangedPixels = 1,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Get-ProofDirectories([string]$Root, [string]$Child) {
    $path = Join-Path $Root $Child
    if (!(Test-Path -LiteralPath $path -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $path -Directory -ErrorAction SilentlyContinue)
}

function Get-NewestDirectoryAfter([string]$Root, [string]$Child, [datetime]$StartedAt) {
    $dirs = @(Get-ProofDirectories $Root $Child |
        Where-Object { $_.LastWriteTime -ge $StartedAt.AddSeconds(-2) } |
        Sort-Object LastWriteTime -Descending)
    if ($dirs.Count -eq 0) {
        return $null
    }
    return $dirs[0].FullName
}

function Add-Param([hashtable]$Params, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $Params[$Name] = $Value
}

$previousEnv = @{
    OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME = $env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME
    OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS = $env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS
    OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE
}

$baselineBuilder = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
$liveContract = Join-Path $PSScriptRoot "test-fnv-live-bone-authoring-runtime.ps1"
if (!(Test-Path -LiteralPath $baselineBuilder -PathType Leaf)) {
    throw "Missing baseline character builder: $baselineBuilder"
}
if (!(Test-Path -LiteralPath $liveContract -PathType Leaf)) {
    throw "Missing live bone authoring contract: $liveContract"
}

try {
    $env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME = "1"
    $env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS = "1"
    $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = $WeightSelector

    $baselineParams = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        ProofRoot = $ProofRoot
        ActorTarget = $ActorTarget
        ActorKind = $ActorKind
        Phases = @("t-pose")
        Angles = $BaselineAngles
        RunSeconds = $BaselineRunSeconds
        ScreenshotFrames = $BaselineScreenshotFrames
        BootstrapCell = $BootstrapCell
        BootstrapX = $BootstrapX
        BootstrapY = $BootstrapY
        BootstrapZ = $BootstrapZ
        BootstrapRotX = $BootstrapRotX
        BootstrapRotY = $BootstrapRotY
        BootstrapRotZ = $BootstrapRotZ
        BootstrapHour = $BootstrapHour
        ActorStageX = $ActorStageX
        ActorStageY = $ActorStageY
        ActorStageZ = $ActorStageZ
        ActorStageRotX = $ActorStageRotX
        ActorStageRotY = $ActorStageRotY
        ActorStageRotZ = $ActorStageRotZ
        ActorViewOffsetZ = $ActorViewOffsetZ
        ActorViewTargetZ = $ActorViewTargetZ
        NeutralActorPreviewProfile = $NeutralActorPreviewProfile
        FnvKeepRiggedHandParts = $FnvKeepRiggedHandParts
        AllowMissingActorVisibleHandGeometry = $true
    }
    Add-Param $baselineParams "FnvData" $FnvData
    Add-Param $baselineParams "FnvConfigData" $FnvConfigData
    Add-Param $baselineParams "VcpkgRoot" $VcpkgRoot
    Add-Param $baselineParams "ExtraOsgPluginDir" $ExtraOsgPluginDir
    if ($NoSound) { $baselineParams["NoSound"] = $true }

    $baselineStartedAt = Get-Date
    & $baselineBuilder @baselineParams | Out-Host
    $baselineSuite = Get-NewestDirectoryAfter $ProofRoot "fnv-character-builder" $baselineStartedAt
    if ([string]::IsNullOrWhiteSpace($baselineSuite)) {
        throw "Unable to find generated baseline character-builder suite."
    }

    $aggregateJson = Join-Path $baselineSuite "character-builder-suite.json"
    if (!(Test-Path -LiteralPath $aggregateJson -PathType Leaf)) {
        throw "Baseline suite did not write aggregate JSON: $aggregateJson"
    }
    $aggregate = @(Get-Content -LiteralPath $aggregateJson -Raw | ConvertFrom-Json)
    if ($aggregate.Count -eq 0) {
        throw "Baseline suite aggregate is empty: $aggregateJson"
    }
    $baselineCase = $aggregate[0]
    $screenshots = @($baselineCase.screenshots)
    if ($screenshots.Count -eq 0) {
        $screenshots = @(Get-ChildItem -LiteralPath ([string]$baselineCase.caseDir) -Filter "*.png" -File |
            Sort-Object Name | ForEach-Object { $_.Name })
    }
    if ($screenshots.Count -eq 0) {
        throw "Baseline case did not produce a screenshot: $($baselineCase.caseDir)"
    }
    $baselineScreenshot = Join-Path ([string]$baselineCase.caseDir) ([string]$screenshots[0])
    if (!(Test-Path -LiteralPath $baselineScreenshot -PathType Leaf)) {
        throw "Baseline screenshot is missing: $baselineScreenshot"
    }

    $liveParams = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        ProofRoot = $ProofRoot
        ActorTarget = $ActorTarget
        ActorKind = $ActorKind
        RunSeconds = $LiveRunSeconds
        WarmupSeconds = $LiveWarmupSeconds
        TimeoutSeconds = $TimeoutSeconds
        ScreenshotFrames = $LiveScreenshotFrames
        BootstrapCell = $BootstrapCell
        BootstrapX = $BootstrapX
        BootstrapY = $BootstrapY
        BootstrapZ = $BootstrapZ
        BootstrapRotX = $BootstrapRotX
        BootstrapRotY = $BootstrapRotY
        BootstrapRotZ = $BootstrapRotZ
        BootstrapHour = $BootstrapHour
        ActorStageX = $ActorStageX
        ActorStageY = $ActorStageY
        ActorStageZ = $ActorStageZ
        ActorStageRotX = $ActorStageRotX
        ActorStageRotY = $ActorStageRotY
        ActorStageRotZ = $ActorStageRotZ
        ActorViewOffsetZ = $ActorViewOffsetZ
        ActorViewTargetZ = $ActorViewTargetZ
        NeutralActorPreviewProfile = $NeutralActorPreviewProfile
        FnvKeepRiggedHandParts = $FnvKeepRiggedHandParts
        HandBone = $HandBone
        FingerBone = $FingerBone
        HandRotationZ = $HandRotationZ
        FingerRotationZ = $FingerRotationZ
        PixelDiffBaseline = $baselineScreenshot
        RequireScreenshotPixelDelta = $true
        MinScreenshotMeanAbsDelta = $MinScreenshotMeanAbsDelta
        MinScreenshotChangedPixels = $MinScreenshotChangedPixels
    }
    Add-Param $liveParams "FnvData" $FnvData
    Add-Param $liveParams "FnvConfigData" $FnvConfigData
    Add-Param $liveParams "VcpkgRoot" $VcpkgRoot
    Add-Param $liveParams "ExtraOsgPluginDir" $ExtraOsgPluginDir
    if ($NoSound) { $liveParams["NoSound"] = $true }

    $liveOutput = & $liveContract @liveParams
    $liveText = ($liveOutput | Out-String).Trim()
    $liveResult = $liveText | ConvertFrom-Json

    [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-bone-authoring-closeup-contract-result-v1"
        actorTarget = $ActorTarget
        neutralActorPreviewProfile = $NeutralActorPreviewProfile
        baselineAngles = @($BaselineAngles)
        handBone = $HandBone
        fingerBone = $FingerBone
        baselineSuite = $baselineSuite
        baselineCase = $baselineCase.case
        baselineScreenshot = $baselineScreenshot
        liveRuntimeTag = $liveResult.runtimeTag
        liveManifest = $liveResult.manifest
        liveOpenMwLog = $liveResult.openMwLog
        liveScreenshot = @($liveResult.screenshots)[0]
        pixelDiff = $liveResult.pixelDiff
        verdict = "PASS"
    } | ConvertTo-Json -Depth 8
}
finally {
    foreach ($entry in $previousEnv.GetEnumerator()) {
        if ($null -eq $entry.Value) {
            Remove-Item -Path "Env:$($entry.Key)" -ErrorAction SilentlyContinue
        }
        else {
            Set-Item -Path "Env:$($entry.Key)" -Value $entry.Value
        }
    }
}
