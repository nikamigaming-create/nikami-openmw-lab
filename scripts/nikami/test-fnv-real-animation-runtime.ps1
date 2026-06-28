param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$ActorTarget = "GSEasyPete",
    [ValidateSet("npc", "creature", "auto")]
    [string]$ActorKind = "npc",
    [string]$ActorKitAnimationSource = "mtidle",
    [string]$ActorKitAnimationGroup = "idle",
    [double]$ActorKitAnimationStartPoint = 1.0,
    [string[]]$Phases = @("body"),
    [string[]]$Angles = @("left", "right", "top"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "600,660,720",
    [string]$FnvRotationMode = "bindCoreBindLowerRawUpper",
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,upperbody",
    [int]$MinMatchedControllers = 1,
    [int]$MinAppliedControllers = 1,
    [switch]$RequireNoMissingControllers,
    [switch]$RequireReportPass,
    [switch]$RequireSemanticPoseOk,
    [switch]$FnvUseNativeAnimationCallbacks,
    [switch]$FnvWeaponIkAuthoringPose,
    [switch]$FnvEnableWeaponIkHandOrientation,
    [switch]$FnvSnapHandsToIkTargets,
    [switch]$AllowMissingActorVisibleHandGeometry,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Add-Arg([System.Collections.Generic.List[string]]$ArgumentList, [string]$Name, $Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $ArgumentList.Add($Name)
    $ArgumentList.Add([string]$Value)
}

function Add-Switch([System.Collections.Generic.List[string]]$ArgumentList, [string]$Name, [bool]$Enabled) {
    if ($Enabled) {
        $ArgumentList.Add($Name)
    }
}

function Get-LastRegexMatch([string]$Text, [string]$Pattern) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $matches = [regex]::Matches($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1]
}

function Get-JsonArray($Value) {
    if ($null -eq $Value) { return @() }
    return @($Value)
}

function Get-ReportValue($Report, [string]$Name, $Default) {
    if ($null -eq $Report) { return $Default }
    $property = $Report.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function Resolve-SuiteDirFromOutput([string]$Text) {
    $suiteMatch = Get-LastRegexMatch $Text '^SuiteDir:\s+(.+)$'
    if ($null -ne $suiteMatch) {
        return $suiteMatch.Groups[1].Value.Trim()
    }
    $builderMatch = Get-LastRegexMatch $Text '^FNV character builder suite:\s+(.+)$'
    if ($null -ne $builderMatch) {
        return $builderMatch.Groups[1].Value.Trim()
    }
    return ""
}

function Stop-RepoOpenMw {
    Get-Process openmw -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$RepoRoot\$BuildDir\$Configuration\openmw.exe" } |
        Stop-Process -Force
}

$CharacterBuilder = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
if (!(Test-Path -LiteralPath $CharacterBuilder -PathType Leaf)) {
    throw "Missing character builder runner: $CharacterBuilder"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $ProofRoot "fnv-real-animation-runtime\$stamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$outputLog = Join-Path $runDir "character-builder-output.log"
$manifestPath = Join-Path $runDir "real-animation-runtime.json"
$summaryPath = Join-Path $runDir "summary.md"

$previousEnv = @{
    OPENMW_FNV_SHOW_ALL_BONES = $env:OPENMW_FNV_SHOW_ALL_BONES
    OPENMW_FNV_ALL_BONE_DEBUG = $env:OPENMW_FNV_ALL_BONE_DEBUG
    OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS = $env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS
    OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE
    OPENMW_FNV_LIVE_RIG_WEIGHT_DEBUG = $env:OPENMW_FNV_LIVE_RIG_WEIGHT_DEBUG
    OPENMW_FNV_KEEP_RIGGED_HAND_PARTS = $env:OPENMW_FNV_KEEP_RIGGED_HAND_PARTS
    OPENMW_FNV_FABRIC_NO_TWIST_DETAIL = $env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL
    OPENMW_FNV_WEAPON_IK_AUTHORING_POSE = $env:OPENMW_FNV_WEAPON_IK_AUTHORING_POSE
    OPENMW_FNV_ENABLE_WEAPON_IK_HAND_ORIENTATION = $env:OPENMW_FNV_ENABLE_WEAPON_IK_HAND_ORIENTATION
    OPENMW_FNV_WEAPON_IK_SNAP_HANDS_TO_TARGETS = $env:OPENMW_FNV_WEAPON_IK_SNAP_HANDS_TO_TARGETS
}

try {
    $env:OPENMW_FNV_SHOW_ALL_BONES = "1"
    $env:OPENMW_FNV_ALL_BONE_DEBUG = "1"
    $env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS = "1"
    $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = "all"
    $env:OPENMW_FNV_LIVE_RIG_WEIGHT_DEBUG = "1"
    $env:OPENMW_FNV_KEEP_RIGGED_HAND_PARTS = "1"
    $env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL = "1"
    if ($FnvWeaponIkAuthoringPose) {
        $env:OPENMW_FNV_WEAPON_IK_AUTHORING_POSE = "1"
    }
    if ($FnvEnableWeaponIkHandOrientation) {
        $env:OPENMW_FNV_ENABLE_WEAPON_IK_HAND_ORIENTATION = "1"
    }
    if ($FnvSnapHandsToIkTargets) {
        $env:OPENMW_FNV_WEAPON_IK_SNAP_HANDS_TO_TARGETS = "1"
    }

    Stop-RepoOpenMw

    $args = [System.Collections.Generic.List[string]]::new()
    $args.Add("-NoProfile")
    $args.Add("-ExecutionPolicy")
    $args.Add("Bypass")
    $args.Add("-File")
    $args.Add($CharacterBuilder)
    Add-Arg $args "-BuildDir" $BuildDir
    Add-Arg $args "-Configuration" $Configuration
    Add-Arg $args "-FnvData" $FnvData
    Add-Arg $args "-FnvConfigData" $FnvConfigData
    Add-Arg $args "-VcpkgRoot" $VcpkgRoot
    Add-Arg $args "-ExtraOsgPluginDir" $ExtraOsgPluginDir
    Add-Arg $args "-Triplet" $Triplet
    Add-Arg $args "-ProofRoot" $ProofRoot
    Add-Arg $args "-ActorTarget" $ActorTarget
    Add-Arg $args "-ActorKind" $ActorKind
    Add-Arg $args "-Phases" ($Phases -join ",")
    Add-Arg $args "-Angles" ($Angles -join ",")
    Add-Arg $args "-RunSeconds" $RunSeconds
    Add-Arg $args "-ActorFrame" $ActorFrame
    Add-Arg $args "-ScreenshotFrames" $ScreenshotFrames
    Add-Arg $args "-ActorKitAnimationSource" $ActorKitAnimationSource
    Add-Arg $args "-ActorKitAnimationGroup" $ActorKitAnimationGroup
    Add-Arg $args "-ActorKitAnimationStartPoint" $ActorKitAnimationStartPoint
    Add-Arg $args "-FnvRotationMode" $FnvRotationMode
    Add-Arg $args "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
    Add-Switch $args "-FnvUseNativeAnimationCallbacks" ([bool]$FnvUseNativeAnimationCallbacks)
    Add-Switch $args "-AllowMissingActorVisibleHandGeometry" ([bool]$AllowMissingActorVisibleHandGeometry)
    Add-Switch $args "-NoSound" ([bool]$NoSound)
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & powershell @args 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $outputText = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    $outputText | Set-Content -LiteralPath $outputLog -Encoding UTF8

    $suiteDir = Resolve-SuiteDirFromOutput $outputText
    $suiteJsonPath = if (![string]::IsNullOrWhiteSpace($suiteDir)) { Join-Path $suiteDir "character-builder-suite.json" } else { "" }
    $suiteRows = @()
    if (![string]::IsNullOrWhiteSpace($suiteJsonPath) -and (Test-Path -LiteralPath $suiteJsonPath -PathType Leaf)) {
        $suiteRows = @(Get-Content -LiteralPath $suiteJsonPath -Raw | ConvertFrom-Json)
    }

    $caseResults = [System.Collections.Generic.List[object]]::new()
    foreach ($suiteRow in $suiteRows) {
        $caseDir = [string](Get-ReportValue $suiteRow "caseDir" "")
        $proofDir = [string](Get-ReportValue $suiteRow "proofDir" "")
        $reportJsonPath = if (![string]::IsNullOrWhiteSpace($caseDir)) { Join-Path $caseDir "character-builder-report.json" } else { "" }
        $openMwLogPath = if (![string]::IsNullOrWhiteSpace($caseDir)) { Join-Path $caseDir "openmw.log" } else { "" }
        $report = $null
        if (![string]::IsNullOrWhiteSpace($reportJsonPath) -and (Test-Path -LiteralPath $reportJsonPath -PathType Leaf)) {
            $report = Get-Content -LiteralPath $reportJsonPath -Raw | ConvertFrom-Json
        }
        $openMwText = ""
        if (![string]::IsNullOrWhiteSpace($openMwLogPath) -and (Test-Path -LiteralPath $openMwLogPath -PathType Leaf)) {
            $openMwText = Get-Content -LiteralPath $openMwLogPath -Raw
        }

        $animationSources = @(Get-JsonArray (Get-ReportValue $report "animationSources" @()))
        $animationPlayback = @(Get-JsonArray (Get-ReportValue $report "animationPlayback" @()))
        $playingPlayback = @($animationPlayback | Where-Object { [bool](Get-ReportValue $_ "playing" $false) })

        $matchedControllers = 0
        $missingControllers = 0
        $totalControllers = 0
        foreach ($source in $animationSources) {
            $sourceText = [string](Get-ReportValue $source "source" "")
            $sourceFilterRequested = ![string]::IsNullOrWhiteSpace($ActorKitAnimationSource)
            $sourceFilterMissed = $sourceFilterRequested -and !$sourceText.ToLowerInvariant().Contains(
                $ActorKitAnimationSource.ToLowerInvariant())
            if ($sourceFilterMissed) {
                continue
            }
            $matchedControllers += [int](Get-ReportValue $source "matchedControllers" 0)
            $missingControllers += [int](Get-ReportValue $source "missingControllers" 0)
            $totalControllers += [int](Get-ReportValue $source "totalControllers" 0)
        }

        $manualMatch = Get-LastRegexMatch $openMwText 'manually applied (?<applied>[0-9]+) active keyframe controller\(s\).*?matchedRigBoneTransforms=(?<matched>[0-9]+).*?missingRigBoneTransforms=(?<missing>[0-9]+).*?falloutRotationMode=(?<mode>[^ ]+).*?maxMatrixDelta=(?<maxMatrix>[-+0-9.eE]+).*?maxDeltaBone=(?<maxBone>[^ ]+).*?maxArmDelta=(?<maxArm>[-+0-9.eE]+).*?maxArmBone=(?<maxArmBone>[^ ]+)'
        $semanticMatch = Get-LastRegexMatch $openMwText 'semantic pose .*?maxMajorDeg=(?<maxMajor>[-+0-9.eE]+).*?maxMajorBone=(?<maxMajorBone>[^\s]+).*?verdict=(?<verdict>[^\s]+).*?reason=(?<reason>[^\s]+)'

        $fabricNoTwistAudited = $openMwText -like "*gate=runtime-fnv-fabric-no-twist*"
        $fabricNoTwistBad = $openMwText -match "fabric no-twist edge audit[^\r\n]*verdict=BAD[^\r\n]*gate=runtime-fnv-fabric-no-twist"
        $allBoneOverlayAudited = $openMwText -like "*gate=runtime-fnv-all-bone-overlay*"
        $liveRigWeightAudited = $openMwText -like "*gate=runtime-fnv-live-rig-weight-debug*"
        $sourceNeedle = $ActorKitAnimationSource.ToLowerInvariant()
        $sourceMatched = [string]::IsNullOrWhiteSpace($sourceNeedle) -or
            (($animationSources | ForEach-Object { [string](Get-ReportValue $_ "source" "") }) -join "`n").ToLowerInvariant().Contains($sourceNeedle) -or
            (($playingPlayback | ForEach-Object { [string](Get-ReportValue $_ "source" "") }) -join "`n").ToLowerInvariant().Contains($sourceNeedle)

        $caseResults.Add([pscustomobject][ordered]@{
            case = [string](Get-ReportValue $suiteRow "case" "")
            phase = [string](Get-ReportValue $suiteRow "phase" "")
            angle = [string](Get-ReportValue $suiteRow "angle" "")
            runtimeGateStatus = [string](Get-ReportValue $suiteRow "runtimeGateStatus" "")
            runtimeGateError = [string](Get-ReportValue $suiteRow "runtimeGateError" "")
            reportStatus = [string](Get-ReportValue $report "status" "MISSING")
            caseDir = $caseDir
            proofDir = $proofDir
            reportJson = $reportJsonPath
            openMwLog = $openMwLogPath
            sourceMatched = $sourceMatched
            animationSourceCount = $animationSources.Count
            animationPlaybackCount = $animationPlayback.Count
            animationPlayingPlaybackCount = $playingPlayback.Count
            matchedControllers = $matchedControllers
            missingControllers = $missingControllers
            totalControllers = $totalControllers
            manuallyAppliedControllers = if ($null -ne $manualMatch) { [int]$manualMatch.Groups["applied"].Value } else { 0 }
            matchedRigBoneTransforms = if ($null -ne $manualMatch) { [int]$manualMatch.Groups["matched"].Value } else { 0 }
            missingRigBoneTransforms = if ($null -ne $manualMatch) { [int]$manualMatch.Groups["missing"].Value } else { 0 }
            loggedRotationMode = if ($null -ne $manualMatch) { $manualMatch.Groups["mode"].Value } else { "" }
            maxMatrixDelta = if ($null -ne $manualMatch) { [double]$manualMatch.Groups["maxMatrix"].Value } else { [double]::NaN }
            maxMatrixDeltaBone = if ($null -ne $manualMatch) { $manualMatch.Groups["maxBone"].Value } else { "" }
            semanticVerdict = if ($null -ne $semanticMatch) { $semanticMatch.Groups["verdict"].Value } else { "MISSING" }
            semanticReason = if ($null -ne $semanticMatch) { $semanticMatch.Groups["reason"].Value } else { "" }
            maxMajorDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["maxMajor"].Value } else { [double]::NaN }
            maxMajorBone = if ($null -ne $semanticMatch) { $semanticMatch.Groups["maxMajorBone"].Value } else { "" }
            fabricNoTwistAudited = $fabricNoTwistAudited
            fabricNoTwistBad = $fabricNoTwistBad
            allBoneOverlayAudited = $allBoneOverlayAudited
            liveRigWeightAudited = $liveRigWeightAudited
        })
    }

    $best = @($caseResults |
        Sort-Object @{ Expression = { if ($_.sourceMatched) { 0 } else { 1 } } },
            @{ Expression = { if ($_.animationPlayingPlaybackCount -gt 0) { 0 } else { 1 } } },
            @{ Expression = { if ($_.manuallyAppliedControllers -ge $MinAppliedControllers) { 0 } else { 1 } } },
            @{ Expression = { if ($_.fabricNoTwistAudited -and !$_.fabricNoTwistBad) { 0 } else { 1 } } },
            @{ Expression = { if ($_.allBoneOverlayAudited) { 0 } else { 1 } } },
            @{ Expression = { if ($_.liveRigWeightAudited) { 0 } else { 1 } } },
            @{ Expression = { $_.case } } |
        Select-Object -First 1)

    if ($best.Count -gt 0) {
        $best = $best[0]
    }
    else {
        $best = $null
    }

    $failures = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $best) { $failures.Add("missing generated character-builder case rows") }
    elseif ($best.reportStatus -eq "MISSING") { $failures.Add("missing character-builder report JSON") }
    if ($RequireReportPass -and $null -ne $best -and $best.reportStatus -ne "PASS") { $failures.Add("character-builder report did not PASS") }
    if ($null -ne $best -and !$best.sourceMatched) { $failures.Add("selected animation source '$ActorKitAnimationSource' was not present in animation source/playback evidence") }
    if ($null -ne $best -and $best.animationSourceCount -le 0) { $failures.Add("missing animation source evidence") }
    if ($null -ne $best -and $best.animationPlayingPlaybackCount -le 0) { $failures.Add("missing active playing animation playback evidence") }
    if ($null -ne $best -and $best.matchedControllers -lt $MinMatchedControllers) { $failures.Add("matched controllers $($best.matchedControllers) < $MinMatchedControllers") }
    if ($RequireNoMissingControllers -and $null -ne $best -and $best.missingControllers -ne 0) { $failures.Add("missing controllers $($best.missingControllers)") }
    if (!$FnvUseNativeAnimationCallbacks) {
        if ($null -eq $best -or $best.manuallyAppliedControllers -le 0) {
            $failures.Add("missing manual active keyframe controller application audit")
        }
        elseif ($best.manuallyAppliedControllers -lt $MinAppliedControllers) {
            $failures.Add("applied controllers $($best.manuallyAppliedControllers) < $MinAppliedControllers")
        }
    }
    if ($null -ne $best -and $best.semanticVerdict -eq "MISSING") { $failures.Add("missing semantic pose audit") }
    elseif ($RequireSemanticPoseOk -and $null -ne $best -and $best.semanticVerdict -ne "OK") { $failures.Add("semantic pose verdict $($best.semanticVerdict):$($best.semanticReason)") }
    if ($null -ne $best -and !$best.fabricNoTwistAudited) { $failures.Add("missing runtime-fnv-fabric-no-twist audit") }
    if ($null -ne $best -and $best.fabricNoTwistBad) { $failures.Add("fabric no-twist BAD audit line present") }
    if ($null -ne $best -and !$best.allBoneOverlayAudited) { $failures.Add("missing runtime-fnv-all-bone-overlay audit") }
    if ($null -ne $best -and !$best.liveRigWeightAudited) { $failures.Add("missing runtime-fnv-live-rig-weight-debug audit") }

    $doc = [pscustomobject][ordered]@{
        schema = "nikami-fnv-real-animation-runtime-contract-v1"
        createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        verdict = if ($failures.Count -eq 0) { "PASS" } else { "FAIL" }
        failures = @($failures)
        runDir = $runDir
        repoRoot = $RepoRoot
        actorTarget = $ActorTarget
        actorKind = $ActorKind
        animationSource = $ActorKitAnimationSource
        animationGroup = $ActorKitAnimationGroup
        animationStartPoint = $ActorKitAnimationStartPoint
        phases = @($Phases)
        angles = @($Angles)
        suiteDir = $suiteDir
        suiteJson = $suiteJsonPath
        selectedCase = $best
        cases = @($caseResults)
        caseDir = if ($null -ne $best) { $best.caseDir } else { "" }
        proofDir = if ($null -ne $best) { $best.proofDir } else { "" }
        reportJson = if ($null -ne $best) { $best.reportJson } else { "" }
        openMwLog = if ($null -ne $best) { $best.openMwLog } else { "" }
        outputLog = $outputLog
        reportStatus = if ($null -ne $best) { $best.reportStatus } else { "MISSING" }
        animationSourceCount = if ($null -ne $best) { $best.animationSourceCount } else { 0 }
        animationPlaybackCount = if ($null -ne $best) { $best.animationPlaybackCount } else { 0 }
        animationPlayingPlaybackCount = if ($null -ne $best) { $best.animationPlayingPlaybackCount } else { 0 }
        matchedControllers = if ($null -ne $best) { $best.matchedControllers } else { 0 }
        missingControllers = if ($null -ne $best) { $best.missingControllers } else { 0 }
        totalControllers = if ($null -ne $best) { $best.totalControllers } else { 0 }
        manuallyAppliedControllers = if ($null -ne $best) { $best.manuallyAppliedControllers } else { 0 }
        matchedRigBoneTransforms = if ($null -ne $best) { $best.matchedRigBoneTransforms } else { 0 }
        missingRigBoneTransforms = if ($null -ne $best) { $best.missingRigBoneTransforms } else { 0 }
        loggedRotationMode = if ($null -ne $best) { $best.loggedRotationMode } else { "" }
        maxMatrixDelta = if ($null -ne $best) { $best.maxMatrixDelta } else { [double]::NaN }
        maxMatrixDeltaBone = if ($null -ne $best) { $best.maxMatrixDeltaBone } else { "" }
        semanticVerdict = if ($null -ne $best) { $best.semanticVerdict } else { "MISSING" }
        semanticReason = if ($null -ne $best) { $best.semanticReason } else { "" }
        maxMajorDeg = if ($null -ne $best) { $best.maxMajorDeg } else { [double]::NaN }
        maxMajorBone = if ($null -ne $best) { $best.maxMajorBone } else { "" }
        fabricNoTwistAudited = if ($null -ne $best) { $best.fabricNoTwistAudited } else { $false }
        fabricNoTwistBad = if ($null -ne $best) { $best.fabricNoTwistBad } else { $false }
        allBoneOverlayAudited = if ($null -ne $best) { $best.allBoneOverlayAudited } else { $false }
        liveRigWeightAudited = if ($null -ne $best) { $best.liveRigWeightAudited } else { $false }
        requireNoMissingControllers = [bool]$RequireNoMissingControllers
        requireReportPass = [bool]$RequireReportPass
        requireSemanticPoseOk = [bool]$RequireSemanticPoseOk
        nativeAnimationCallbacks = [bool]$FnvUseNativeAnimationCallbacks
        weaponIkAuthoringPose = [bool]$FnvWeaponIkAuthoringPose
        weaponIkHandOrientation = [bool]$FnvEnableWeaponIkHandOrientation
        weaponIkSnapHandsToTargets = [bool]$FnvSnapHandsToIkTargets
        payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
    }

    $doc | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    $summary = [System.Collections.Generic.List[string]]::new()
    $summary.Add("# FNV Real Animation Runtime Contract")
    $summary.Add("")
    $summary.Add("Verdict: ``$($doc.verdict)``")
    $summary.Add("Actor: ``$ActorTarget``")
    $summary.Add("Animation: ``$ActorKitAnimationSource`` / ``$ActorKitAnimationGroup`` @ ``$ActorKitAnimationStartPoint``")
    $summary.Add("Selected case: ``$($doc.selectedCase.case)``")
    $summary.Add("Controllers: matched ``$($doc.matchedControllers)`` / total ``$($doc.totalControllers)`` / missing ``$($doc.missingControllers)``")
    $summary.Add("Playback rows: ``$($doc.animationPlayingPlaybackCount)`` playing of ``$($doc.animationPlaybackCount)``")
    $summary.Add("Applied controllers: ``$($doc.manuallyAppliedControllers)``")
    $summary.Add("Semantic pose: ``$($doc.semanticVerdict):$($doc.semanticReason)``")
    $summary.Add("Fabric no-twist audited: ``$($doc.fabricNoTwistAudited)`` bad: ``$($doc.fabricNoTwistBad)``")
    $summary.Add("All-bone overlay audited: ``$($doc.allBoneOverlayAudited)``")
    $summary.Add("Live rig weight audited: ``$($doc.liveRigWeightAudited)``")
    $summary.Add("Manifest: ``$manifestPath``")
    $summary.Add("OpenMW log: ``$openMwLogPath``")
    $summary.Add("")
    $summary.Add("## Camera Cases")
    $summary.Add("| case | status | semantic | max major | openmw log |")
    $summary.Add("| --- | --- | --- | --- | --- |")
    foreach ($case in @($doc.cases)) {
        $summary.Add("| ``$($case.case)`` | ``$($case.runtimeGateStatus)/$($case.reportStatus)`` | ``$($case.semanticVerdict):$($case.semanticReason)`` | ``$($case.maxMajorDeg) $($case.maxMajorBone)`` | ``$($case.openMwLog)`` |")
    }
    if ($failures.Count -gt 0) {
        $summary.Add("")
        $summary.Add("## Failures")
        foreach ($failure in $failures) {
            $summary.Add("- $failure")
        }
    }
    $summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

    if ($failures.Count -gt 0) {
        throw "FNV real animation runtime contract failed. See $manifestPath"
    }

    Write-Host "FNV real animation runtime contract: $runDir"
    Write-Host "Manifest: $manifestPath"
    Write-Host "Summary: $summaryPath"
}
finally {
    Stop-RepoOpenMw
    foreach ($entry in $previousEnv.GetEnumerator()) {
        if ($null -eq $entry.Value) {
            Remove-Item -Path "Env:$($entry.Key)" -ErrorAction SilentlyContinue
        }
        else {
            Set-Item -Path "Env:$($entry.Key)" -Value $entry.Value
        }
    }
}
