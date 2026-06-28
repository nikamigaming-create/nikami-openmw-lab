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
    [string[]]$Modes = @(
        "bindCoreBindLowerRawUpper",
        "bindCoreBindLowerSplitUpper",
        "bindCoreBindLowerBindUpper",
        "bindCoreBindLowerBindArmsRawHands",
        "bindCoreBindLowerBindUpperRawForearmsHands",
        "bindCoreBindLowerRawUpperBindHands",
        "bindCoreBindLowerRawClavicleBindArms",
        "standingUpperBody"
    ),
    [string]$ActorKitAnimationSource = "mtidle",
    [string]$ActorKitAnimationGroup = "idle",
    [double]$ActorKitAnimationStartPoint = 1.0,
    [string[]]$Phases = @("body"),
    [string[]]$Angles = @("front"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "600,660,720",
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand",
    [switch]$FnvUseNativeAnimationCallbacks,
    [switch]$NoSound,
    [switch]$AllowMissingActorVisibleHandGeometry
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

function Get-FlatModes([string[]]$Values) {
    $flat = New-Object "System.Collections.Generic.List[string]"
    foreach ($value in $Values) {
        foreach ($mode in ([string]$value -split ",")) {
            $trimmed = $mode.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed) -and !$flat.Contains($trimmed)) {
                $flat.Add($trimmed)
            }
        }
    }
    return @($flat)
}

function Stop-RepoOpenMw {
    Get-Process openmw -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$RepoRoot\$BuildDir\$Configuration\openmw.exe" } |
        Stop-Process -Force
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
    if ($null -eq $property) { return $Default }
    if ($null -eq $property.Value) { return $Default }
    return $property.Value
}

$CharacterBuilder = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
if (!(Test-Path -LiteralPath $CharacterBuilder -PathType Leaf)) {
    throw "Missing character builder runner: $CharacterBuilder"
}

$requestedModes = Get-FlatModes $Modes
if ($requestedModes.Count -eq 0) {
    throw "No rotation modes requested."
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ProofRoot "fnv-animation-rotation-sweep\$stamp"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$results = New-Object "System.Collections.Generic.List[object]"
try {
    foreach ($mode in $requestedModes) {
        Stop-RepoOpenMw
        $modeSafe = ($mode -replace '[^A-Za-z0-9_.-]', '_')
        $outputLog = Join-Path $RunDir "$modeSafe-output.log"

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
        Add-Arg $args "-FnvRotationMode" $mode
        Add-Arg $args "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
        Add-Switch $args "-FnvUseNativeAnimationCallbacks" ([bool]$FnvUseNativeAnimationCallbacks)
        Add-Switch $args "-AllowMissingActorVisibleHandGeometry" ([bool]$AllowMissingActorVisibleHandGeometry)
        Add-Switch $args "-NoSound" ([bool]$NoSound)

        Write-Host "Running FNV rotation mode '$mode'..."
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & powershell @args 2>&1
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }

        $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        $text | Set-Content -LiteralPath $outputLog -Encoding UTF8

        $suiteDir = ""
        $suiteMatch = Get-LastRegexMatch $text '^SuiteDir:\s+(.+)$'
        if ($null -ne $suiteMatch) {
            $suiteDir = $suiteMatch.Groups[1].Value.Trim()
        }
        if ([string]::IsNullOrWhiteSpace($suiteDir)) {
            $suiteMatches = [regex]::Matches($text, '^FNV character builder suite:\s+(.+)$', [System.Text.RegularExpressions.RegexOptions]::Multiline)
            if ($suiteMatches.Count -gt 0) {
                $suiteDir = $suiteMatches[$suiteMatches.Count - 1].Groups[1].Value.Trim()
            }
        }

        $suiteJsonPath = ""
        $caseDir = ""
        $proofDir = ""
        $reportJsonPath = ""
        $openMwLogPath = ""
        $report = $null
        if (![string]::IsNullOrWhiteSpace($suiteDir)) {
            $suiteJsonPath = Join-Path $suiteDir "character-builder-suite.json"
            if (Test-Path -LiteralPath $suiteJsonPath -PathType Leaf) {
                $suiteRows = @(Get-Content -LiteralPath $suiteJsonPath -Raw | ConvertFrom-Json)
                if ($suiteRows.Count -gt 0) {
                    $row = $suiteRows[0]
                    $caseDir = [string]$row.caseDir
                    $proofDir = [string]$row.proofDir
                }
            }
            if ([string]::IsNullOrWhiteSpace($caseDir)) {
                $caseDir = Get-ChildItem -LiteralPath $suiteDir -Directory -ErrorAction SilentlyContinue |
                    Select-Object -First 1 -ExpandProperty FullName
            }
        }
        if (![string]::IsNullOrWhiteSpace($caseDir)) {
            $reportJsonPath = Join-Path $caseDir "character-builder-report.json"
            $openMwLogPath = Join-Path $caseDir "openmw.log"
            if (Test-Path -LiteralPath $reportJsonPath -PathType Leaf) {
                $report = Get-Content -LiteralPath $reportJsonPath -Raw | ConvertFrom-Json
            }
        }

        $openMwText = ""
        if (![string]::IsNullOrWhiteSpace($openMwLogPath) -and (Test-Path -LiteralPath $openMwLogPath -PathType Leaf)) {
            $openMwText = Get-Content -LiteralPath $openMwLogPath -Raw
        }

        $manualMatch = Get-LastRegexMatch $openMwText 'manually applied (?<applied>[0-9]+) active keyframe controller\(s\).*?matchedRigBoneTransforms=(?<matched>[0-9]+).*?missingRigBoneTransforms=(?<missing>[0-9]+).*?falloutRotationMode=(?<mode>[^ ]+).*?maxMatrixDelta=(?<maxMatrix>[-+0-9.eE]+).*?maxDeltaBone=(?<maxBone>[^ ]+).*?maxArmDelta=(?<maxArm>[-+0-9.eE]+).*?maxArmBone=(?<maxArmBone>[^ ]+)'
        $semanticMatch = Get-LastRegexMatch $openMwText 'semantic pose .*?headDeg=(?<head>[-+0-9.eE]+).*?spine2Deg=(?<spine2>[-+0-9.eE]+).*?lUpperArmDeg=(?<lUpperArm>[-+0-9.eE]+).*?rUpperArmDeg=(?<rUpperArm>[-+0-9.eE]+).*?lForearmDeg=(?<lForearm>[-+0-9.eE]+).*?rForearmDeg=(?<rForearm>[-+0-9.eE]+).*?maxMajorDeg=(?<maxMajor>[-+0-9.eE]+).*?maxMajorBone=(?<maxMajorBone>[^ ]+).*?verdict=(?<verdict>[^ ]+).*?reason=(?<reason>[^ ]+)'

        $animationSources = @(Get-JsonArray (Get-ReportValue $report "animationSources" @()))
        $animationPlayback = @(Get-JsonArray (Get-ReportValue $report "animationPlayback" @()))
        $failures = @(Get-JsonArray (Get-ReportValue $report "failures" @()))
        $screenshots = @(Get-JsonArray (Get-ReportValue $report "screenshots" @()))
        $handSummary = Get-ReportValue $report "handRuntimeSummary" $null

        $matchedControllers = 0
        $missingControllers = 0
        $totalControllers = 0
        foreach ($source in $animationSources) {
            $matchedControllers += [int](Get-ReportValue $source "matchedControllers" 0)
            $missingControllers += [int](Get-ReportValue $source "missingControllers" 0)
            $totalControllers += [int](Get-ReportValue $source "totalControllers" 0)
        }
        $playingPlayback = @($animationPlayback | Where-Object { [bool](Get-ReportValue $_ "playing" $false) })

        $result = [pscustomobject][ordered]@{
            mode = $mode
            exitCode = $exitCode
            suiteDir = $suiteDir
            caseDir = $caseDir
            proofDir = $proofDir
            outputLog = $outputLog
            openmwLog = $openMwLogPath
            reportJson = $reportJsonPath
            reportStatus = [string](Get-ReportValue $report "status" "MISSING")
            failureCount = $failures.Count
            failures = $failures
            screenshotCount = $screenshots.Count
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
            maxArmDelta = if ($null -ne $manualMatch) { [double]$manualMatch.Groups["maxArm"].Value } else { [double]::NaN }
            maxArmDeltaBone = if ($null -ne $manualMatch) { $manualMatch.Groups["maxArmBone"].Value } else { "" }
            semanticVerdict = if ($null -ne $semanticMatch) { $semanticMatch.Groups["verdict"].Value } else { "MISSING" }
            semanticReason = if ($null -ne $semanticMatch) { $semanticMatch.Groups["reason"].Value } else { "" }
            maxMajorDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["maxMajor"].Value } else { [double]::NaN }
            maxMajorBone = if ($null -ne $semanticMatch) { $semanticMatch.Groups["maxMajorBone"].Value } else { "" }
            lUpperArmDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["lUpperArm"].Value } else { [double]::NaN }
            rUpperArmDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["rUpperArm"].Value } else { [double]::NaN }
            lForearmDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["lForearm"].Value } else { [double]::NaN }
            rForearmDeg = if ($null -ne $semanticMatch) { [double]$semanticMatch.Groups["rForearm"].Value } else { [double]::NaN }
            staticGripVertices = if ($null -ne $handSummary) { [int](Get-ReportValue $handSummary "staticGripVertices" 0) } else { 0 }
            visibleLimbShapeBadLines = if ($null -ne $handSummary) { [int](Get-ReportValue $handSummary "visibleLimbShapeBadLines" 0) } else { 0 }
            payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
        }
        $results.Add($result)
    }
} finally {
    Stop-RepoOpenMw
}

$best = $results |
    Sort-Object @{ Expression = { if ($_.semanticVerdict -eq "OK") { 0 } else { 1 } } },
        @{ Expression = { if ([double]::IsNaN($_.maxMajorDeg)) { [double]::PositiveInfinity } else { $_.maxMajorDeg } } },
        @{ Expression = { if ($_.animationPlayingPlaybackCount -gt 0) { 0 } else { 1 } } },
        failureCount,
        mode |
    Select-Object -First 1

$doc = [pscustomobject][ordered]@{
    schema = "nikami-fnv-animation-rotation-sweep-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $RunDir
    repoRoot = $RepoRoot
    actorTarget = $ActorTarget
    actorKind = $ActorKind
    animationSource = $ActorKitAnimationSource
    animationGroup = $ActorKitAnimationGroup
    animationStartPoint = $ActorKitAnimationStartPoint
    modes = $requestedModes
    selectedBestMode = if ($null -ne $best) { [string]$best.mode } else { "" }
    promotionPolicy = "Only promote after a mode keeps controller binding/playback intact and improves semantic/visual gates across representative NPCs."
    payloadPolicy = "generated proof metadata/log references only; no retail assets are committed"
    results = @($results)
}

$jsonPath = Join-Path $RunDir "animation-rotation-sweep.json"
$doc | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

$summaryPath = Join-Path $RunDir "summary.md"
$summary = New-Object "System.Collections.Generic.List[string]"
$summary.Add("# FNV Animation Rotation Sweep")
$summary.Add("")
$summary.Add("Actor: ``$ActorTarget``")
$summary.Add("Animation: ``$ActorKitAnimationSource`` / ``$ActorKitAnimationGroup`` @ ``$ActorKitAnimationStartPoint``")
$summary.Add("Best observed mode: ``$($doc.selectedBestMode)``")
$summary.Add("")
$summary.Add("| Mode | Report | Exit | Ctrl | Playback | Semantic | Max deg | Max bone | L upper | R upper | L fore | R fore | Screens | Failures |")
$summary.Add("| --- | --- | ---: | ---: | ---: | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |")
foreach ($result in $results) {
    $ctrl = "{0}/{1}" -f $result.matchedControllers, $result.totalControllers
    $playback = "{0}/{1}" -f $result.animationPlayingPlaybackCount, $result.animationPlaybackCount
    $summary.Add("| $($result.mode) | $($result.reportStatus) | $($result.exitCode) | $ctrl | $playback | $($result.semanticVerdict):$($result.semanticReason) | $($result.maxMajorDeg) | $($result.maxMajorBone) | $($result.lUpperArmDeg) | $($result.rUpperArmDeg) | $($result.lForearmDeg) | $($result.rForearmDeg) | $($result.screenshotCount) | $($result.failureCount) |")
}
$summary.Add("")
$summary.Add("No retail payload bytes are written; proof rows reference generated logs only.")
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host "FNV animation rotation sweep: $RunDir"
Write-Host "Sweep JSON: $jsonPath"
Write-Host "Summary: $summaryPath"
