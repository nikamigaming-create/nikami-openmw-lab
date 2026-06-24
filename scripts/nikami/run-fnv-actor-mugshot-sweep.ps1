param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string[]]$Targets = @(
        "GSEasyPete",
        "GSSettlerCF",
        "GSSettlerAM",
        "GSSettlerAAM",
        "GSPGHM2",
        "GSPGAAM2",
        "GSPGHM",
        "GSPGAAM",
        "GSPGCM",
        "GSPGCM2"
    ),
    [int]$RunSeconds = 60,
    [double]$BootstrapHour = 3,
    [int]$ActorFrame = 600,
    [string]$ScreenshotFrames = "900,1200",
    [double]$ActorViewOffsetX = 140,
    [double]$ActorViewOffsetY = 0,
    [double]$ActorViewOffsetZ = 82,
    [double]$ActorViewTargetZ = 88,
    [int]$CropWidth = 280,
    [int]$CropHeight = 280,
    [int]$CropCenterOffsetX = 0,
    [int]$CropCenterOffsetY = -100,
    [int]$ZoomWidth = 900,
    [int]$ZoomHeight = 900,
    [int]$ContactSheetColumns = 4,
    [switch]$StageActor,
    [double]$ActorStageX = -67480,
    [double]$ActorStageY = 1500,
    [double]$ActorStageZ = 8425,
    [double]$ActorStageRotZ = 1.5708,
    [switch]$WorldSpaceActorViewOffset,
    [switch]$DisableNativeAnimationCallbacks,
    [switch]$NoSound,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
if (!(Test-Path -LiteralPath $FlatProof)) {
    throw "Missing wrapped proof script: $FlatProof"
}

$ProofBase = Join-Path $ProofRoot "fnv-flat-proof"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$SweepDir = Join-Path $ProofBase "actor-mugshot-sweep-$Stamp"
New-Item -ItemType Directory -Force -Path $SweepDir | Out-Null

Add-Type -AssemblyName System.Drawing

function ConvertTo-SafeFileName {
    param([string]$Name)

    $safe = $Name -replace '[^A-Za-z0-9_.-]', '_'
    if ([string]::IsNullOrWhiteSpace($safe)) { return "target" }
    return $safe
}

function ConvertTo-MarkdownCell {
    param([object]$Value)

    if ($null -eq $Value) { return "" }
    return ([string]$Value).Replace("|", "\|").Replace("`r", " ").Replace("`n", " ")
}

function Copy-IfPresent {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    if (![string]::IsNullOrWhiteSpace($SourcePath) -and (Test-Path -LiteralPath $SourcePath)) {
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
        return $DestinationPath
    }

    return $null
}

function Get-ProofDirectories {
    if (!(Test-Path -LiteralPath $ProofBase)) { return @() }
    return @(Get-ChildItem -LiteralPath $ProofBase -Directory -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName)
}

function Get-NewProofDirectory {
    param(
        [string[]]$Before,
        [datetime]$StartedAt
    )

    $beforeSet = New-Object "System.Collections.Generic.HashSet[string]"
    foreach ($path in $Before) {
        if (![string]::IsNullOrWhiteSpace($path)) {
            $null = $beforeSet.Add($path)
        }
    }

    $candidates = @(Get-ChildItem -LiteralPath $ProofBase -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            !$beforeSet.Contains($_.FullName) -and
            $_.FullName -ne $SweepDir -and
            $_.LastWriteTime -ge $StartedAt.AddSeconds(-5)
        } |
        Sort-Object LastWriteTime -Descending)

    if ($candidates.Count -gt 0) { return $candidates[0] }
    return $null
}

function Get-LatestScreenshot {
    param([string]$ProofDir)

    if ([string]::IsNullOrWhiteSpace($ProofDir) -or !(Test-Path -LiteralPath $ProofDir)) {
        return $null
    }

    $shots = @(Get-ChildItem -LiteralPath $ProofDir -Filter "*.png" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($shots.Count -gt 0) { return $shots[0] }
    return $null
}

function Get-ImageQuality {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) {
        return [pscustomobject]@{
            Width = 0
            Height = 0
            StdDev = 0.0
            Buckets = 0
            DarkPercent = 0.0
            MarkerPinkPercent = 0.0
            Status = "FAIL"
        }
    }

    $bmp = $null
    try {
        $bmp = [System.Drawing.Bitmap]::new($Path)
        $stepX = [Math]::Max(1, [int]($bmp.Width / 64))
        $stepY = [Math]::Max(1, [int]($bmp.Height / 64))
        $count = 0
        $sum = 0.0
        $sumSq = 0.0
        $dark = 0
        $markerPink = 0
        $buckets = New-Object "System.Collections.Generic.HashSet[int]"

        for ($y = [int]($stepY / 2); $y -lt $bmp.Height; $y += $stepY) {
            for ($x = [int]($stepX / 2); $x -lt $bmp.Width; $x += $stepX) {
                $color = $bmp.GetPixel($x, $y)
                $luma = (0.2126 * $color.R) + (0.7152 * $color.G) + (0.0722 * $color.B)
                $sum += $luma
                $sumSq += ($luma * $luma)
                if ($luma -lt 16) { $dark++ }
                if ($color.R -gt 180 -and $color.B -gt 160 -and $color.G -lt 80) { $markerPink++ }
                $bucket = (($color.R -shr 4) -shl 8) -bor (($color.G -shr 4) -shl 4) -bor ($color.B -shr 4)
                $null = $buckets.Add($bucket)
                $count++
            }
        }

        $mean = if ($count -gt 0) { $sum / $count } else { 0.0 }
        $variance = if ($count -gt 0) { [Math]::Max(0.0, ($sumSq / $count) - ($mean * $mean)) } else { 0.0 }
        $stddev = [Math]::Sqrt($variance)
        $darkPercent = if ($count -gt 0) { 100.0 * $dark / $count } else { 0.0 }
        $markerPinkPercent = if ($count -gt 0) { 100.0 * $markerPink / $count } else { 0.0 }
        $status = if ($stddev -ge 4.0 -and $buckets.Count -ge 16 -and $markerPinkPercent -lt 1.0) { "PASS" } else { "REVIEW" }

        return [pscustomobject]@{
            Width = $bmp.Width
            Height = $bmp.Height
            StdDev = [Math]::Round($stddev, 3)
            Buckets = $buckets.Count
            DarkPercent = [Math]::Round($darkPercent, 3)
            MarkerPinkPercent = [Math]::Round($markerPinkPercent, 3)
            Status = $status
        }
    }
    finally {
        if ($null -ne $bmp) { $bmp.Dispose() }
    }
}

function Copy-CenteredCrop {
    param(
        [string]$SourcePath,
        [string]$OutputPath
    )

    $bmp = [System.Drawing.Bitmap]::new($SourcePath)
    try {
        $cropW = [Math]::Min($CropWidth, $bmp.Width)
        $cropH = [Math]::Min($CropHeight, $bmp.Height)
        $centerX = ([double]$bmp.Width / 2.0) + $CropCenterOffsetX
        $centerY = ([double]$bmp.Height / 2.0) + $CropCenterOffsetY
        $x = [int][Math]::Round($centerX - ([double]$cropW / 2.0))
        $y = [int][Math]::Round($centerY - ([double]$cropH / 2.0))
        $x = [Math]::Max(0, [Math]::Min($x, $bmp.Width - $cropW))
        $y = [Math]::Max(0, [Math]::Min($y, $bmp.Height - $cropH))
        $crop = [System.Drawing.Rectangle]::new($x, $y, $cropW, $cropH)
        $cropped = $bmp.Clone($crop, $bmp.PixelFormat)
        try {
            $scaled = [System.Drawing.Bitmap]::new($ZoomWidth, $ZoomHeight)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($scaled)
                try {
                    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
                    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
                    $graphics.DrawImage($cropped, 0, 0, $ZoomWidth, $ZoomHeight)
                }
                finally {
                    $graphics.Dispose()
                }

                $scaled.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
            }
            finally {
                $scaled.Dispose()
            }
        }
        finally {
            $cropped.Dispose()
        }
    }
    finally {
        $bmp.Dispose()
    }
}

function Get-FirstLogMatch {
    param(
        [string]$LogPath,
        [string]$Pattern
    )

    if ([string]::IsNullOrWhiteSpace($LogPath) -or !(Test-Path -LiteralPath $LogPath)) {
        return $null
    }

    $match = Select-String -LiteralPath $LogPath -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $match) { return $null }
    return $match.Line
}

function Get-PartAssemblyProbe {
    param(
        [string]$LogPath,
        [string[]]$Patterns
    )

    $bestLine = $null
    $bestSuspectCount = $null
    $sawLine = $false

    if ([string]::IsNullOrWhiteSpace($LogPath) -or !(Test-Path -LiteralPath $LogPath)) {
        return [pscustomobject]@{
            Status = "MISSING"
            SuspectCount = $null
            Line = $null
        }
    }

    foreach ($pattern in $Patterns) {
        if ([string]::IsNullOrWhiteSpace($pattern)) { continue }

        $matches = @(Select-String -LiteralPath $LogPath -Pattern "runtime part audit summary $pattern" -ErrorAction SilentlyContinue)
        foreach ($line in $matches) {
            $sawLine = $true
            $suspectCount = $null
            if ($line.Line -match "suspect=([0-9]+)") {
                $suspectCount = [int]$Matches[1]
            }

            if ($null -eq $bestLine) {
                $bestLine = $line.Line
                $bestSuspectCount = $suspectCount
            }
            elseif ($null -ne $suspectCount -and ($null -eq $bestSuspectCount -or $suspectCount -gt $bestSuspectCount)) {
                $bestLine = $line.Line
                $bestSuspectCount = $suspectCount
            }
        }
    }

    $status = if ($null -ne $bestSuspectCount -and $bestSuspectCount -eq 0) { "PASS" } elseif ($null -ne $bestSuspectCount -and $bestSuspectCount -gt 0) { "FAIL" } elseif ($sawLine) { "REVIEW" } else { "MISSING" }

    return [pscustomobject]@{
        Status = $status
        SuspectCount = $bestSuspectCount
        Line = $bestLine
    }
}

function Get-FirstLogMatchForAnyPattern {
    param(
        [string]$LogPath,
        [string]$LinePrefix,
        [string[]]$Patterns,
        [string]$LineSuffix
    )

    foreach ($pattern in $Patterns) {
        if ([string]::IsNullOrWhiteSpace($pattern)) { continue }
        $line = Get-FirstLogMatch -LogPath $LogPath -Pattern "$LinePrefix$pattern$LineSuffix"
        if ($line) { return $line }
    }

    return $null
}

$KnownToleratedMissingMeshes = @(
    "meshes/sky_atmosphere.nif",
    "meshes/sky_night_01.nif",
    "meshes/sky_clouds_01.nif",
    "meshes/ashcloud.nif",
    "meshes/blightcloud.nif",
    "meshes/snow.nif",
    "meshes/blizzard.nif",
    "meshes/xbase_anim.nif",
    "meshes/xbase_anim.1st.nif",
    "meshes/xbase_anim_female.nif",
    "meshes/xargonian_swimkna.nif"
)

function Test-KnownToleratedLogLine {
    param([string]$Line)

    if ([string]::IsNullOrWhiteSpace($Line)) { return $false }
    foreach ($mesh in $KnownToleratedMissingMeshes) {
        if ($Line -like "*$mesh*") { return $true }
    }
    return $false
}

function Get-LogBlockerProbe {
    param([string]$LogPath)

    $realBlockerLine = $null
    $knownNoiseCount = 0

    if ([string]::IsNullOrWhiteSpace($LogPath) -or !(Test-Path -LiteralPath $LogPath)) {
        return [pscustomobject]@{
            RealBlockerLine = $null
            KnownNoiseCount = 0
        }
    }

    $candidateLines = @(Select-String -LiteralPath $LogPath -Pattern "Fatal error|Failed to start new game|unknown global|List of NPC classes|Failed to update LuaManager|Lua error|Resource 'meshes/base_anim|marker_error|markerPink|crash" -ErrorAction SilentlyContinue)
    foreach ($line in $candidateLines) {
        if (Test-KnownToleratedLogLine -Line $line.Line) {
            $knownNoiseCount++
            continue
        }

        if ($null -eq $realBlockerLine) {
            $realBlockerLine = $line.Line
        }
    }

    return [pscustomobject]@{
        RealBlockerLine = $realBlockerLine
        KnownNoiseCount = $knownNoiseCount
    }
}

function Test-LineHasAllTokens {
    param(
        [string]$Line,
        [string[]]$Tokens
    )

    if ([string]::IsNullOrWhiteSpace($Line)) { return $false }
    foreach ($token in $Tokens) {
        if ($Line -notlike "*$token*") { return $false }
    }
    return $true
}

function Get-LogProbe {
    param(
        [string]$LogPath,
        [string]$Target
    )

    $targetPattern = [regex]::Escape($Target)
    $matchLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "active-cell actor match target=`"$targetPattern`""
    $lookupFailLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "active-cell actor lookup failed target=`"$targetPattern`""
    $faceLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "FACE CHECK ${targetPattern}:"
    $animLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "actor controller audit result .* matched=[0-9]+ missing=0"
    if ($null -eq $animLine) {
        $animLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "actor controller audit result"
    }
    $partPatterns = @($targetPattern)
    if ($matchLine -and $matchLine -match "base=([^ ]+)") {
        $partPatterns += [regex]::Escape($Matches[1])
    }
    if ($matchLine -and $matchLine -match "ref=([^ ]+)") {
        $partPatterns += [regex]::Escape($Matches[1])
    }
    $worldPostureBadLine = Get-FirstLogMatchForAnyPattern -LogPath $LogPath -LinePrefix "world posture " -Patterns $partPatterns -LineSuffix " .* verdict=BAD"
    if ($null -eq $worldPostureBadLine) { $worldPostureBadLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "world posture .* verdict=BAD" }
    $worldPostureOkLine = Get-FirstLogMatchForAnyPattern -LogPath $LogPath -LinePrefix "world posture " -Patterns $partPatterns -LineSuffix " .* verdict=OK"
    $armPoseBadLine = Get-FirstLogMatchForAnyPattern -LogPath $LogPath -LinePrefix "standing arm pose " -Patterns $partPatterns -LineSuffix " .* verdict=BAD"
    if ($null -eq $armPoseBadLine) { $armPoseBadLine = Get-FirstLogMatch -LogPath $LogPath -Pattern "standing arm pose .* verdict=BAD" }
    $armPoseOkLine = Get-FirstLogMatchForAnyPattern -LogPath $LogPath -LinePrefix "standing arm pose " -Patterns $partPatterns -LineSuffix " .* verdict=OK"
    $partProbe = Get-PartAssemblyProbe -LogPath $LogPath -Patterns $partPatterns
    $blockerProbe = Get-LogBlockerProbe -LogPath $LogPath
    $blockerLine = $blockerProbe.RealBlockerLine

    $faceTokens = @("head=OK", "mouth=OK", "leftEye=OK", "rightEye=OK", "eyeTexture=OK", "hairRecord=OK", "hairAttached=OK")
    $faceStatus = if (Test-LineHasAllTokens -Line $faceLine -Tokens $faceTokens) { "PASS" } elseif ($faceLine) { "REVIEW" } else { "MISSING" }
    $animStatus = if ($worldPostureBadLine -or $armPoseBadLine) { "FAIL" } elseif ($animLine -and $animLine -match "missing=0" -and $worldPostureOkLine -and $armPoseOkLine) { "PASS" } elseif ($animLine -or $worldPostureOkLine -or $armPoseOkLine) { "REVIEW" } else { "MISSING" }
    $matchStatus = if ($matchLine) { "PASS" } elseif ($lookupFailLine) { "FAIL" } else { "MISSING" }
    if ($lookupFailLine -and !$blockerLine) { $blockerLine = $lookupFailLine }

    return [pscustomobject]@{
        ActorMatch = $matchStatus
        Face = $faceStatus
        Animation = $animStatus
        PartAssembly = $partProbe.Status
        PartAssemblySuspectCount = $partProbe.SuspectCount
        Blocker = if ($blockerLine) { "FAIL" } else { "PASS" }
        KnownNoiseCount = $blockerProbe.KnownNoiseCount
        MatchLine = $matchLine
        FaceLine = $faceLine
        AnimationLine = $animLine
        WorldPostureBadLine = $worldPostureBadLine
        WorldPostureOkLine = $worldPostureOkLine
        ArmPoseBadLine = $armPoseBadLine
        ArmPoseOkLine = $armPoseOkLine
        PartAssemblyLine = $partProbe.Line
        BlockerLine = $blockerLine
    }
}

function Get-SummaryProbe {
    param([string]$SummaryPath)

    if ([string]::IsNullOrWhiteSpace($SummaryPath) -or !(Test-Path -LiteralPath $SummaryPath)) {
        return [pscustomobject]@{
            TerrainSupport = "MISSING"
            Airborne = "MISSING"
            Camera = "MISSING"
            CameraFailure = "MISSING"
            ScreenshotStability = "MISSING"
            ScreenshotTiming = "MISSING"
            Blocker = "MISSING"
            TerrainSupportMissLine = $null
            AirborneLine = $null
            CameraSettledLine = $null
            CameraFailureLine = $null
            ScreenshotStabilityLine = $null
            ScreenshotTimingLine = $null
            BlockerLine = $null
        }
    }

    $terrainSupportMissLine = Select-String -LiteralPath $SummaryPath -Pattern "Player terrain support miss lines: [1-9]" -ErrorAction SilentlyContinue | Select-Object -First 1
    $airborneLine = Select-String -LiteralPath $SummaryPath -Pattern "Player terrain airborne lines: [1-9]" -ErrorAction SilentlyContinue | Select-Object -First 1
    $cameraSettledLine = Select-String -LiteralPath $SummaryPath -Pattern "Flat camera settled lines: [1-9]" -ErrorAction SilentlyContinue | Select-Object -First 1
    $cameraFailureLine = Select-String -LiteralPath $SummaryPath -Pattern "Flat camera failure lines: [1-9]" -ErrorAction SilentlyContinue | Select-Object -First 1
    $screenshotStabilityPassLine = Select-String -LiteralPath $SummaryPath -Pattern "Screenshot stability status: PASS" -ErrorAction SilentlyContinue | Select-Object -First 1
    $screenshotStabilityFailLine = Select-String -LiteralPath $SummaryPath -Pattern "Screenshot stability status: (FAIL|MISSING)" -ErrorAction SilentlyContinue | Select-Object -First 1
    $screenshotTimingPassLine = Select-String -LiteralPath $SummaryPath -Pattern "Screenshot timing status: PASS" -ErrorAction SilentlyContinue | Select-Object -First 1
    $screenshotTimingFailLine = Select-String -LiteralPath $SummaryPath -Pattern "Screenshot timing status: (FAIL|MISSING)" -ErrorAction SilentlyContinue | Select-Object -First 1
    $blockerLine = Select-String -LiteralPath $SummaryPath -Pattern "Real fatal/blocker lines: [1-9]" -ErrorAction SilentlyContinue | Select-Object -First 1

    return [pscustomobject]@{
        TerrainSupport = if ($terrainSupportMissLine) { "FAIL" } else { "PASS" }
        Airborne = if ($airborneLine) { "FAIL" } else { "PASS" }
        Camera = if ($cameraSettledLine) { "PASS" } else { "FAIL" }
        CameraFailure = if ($cameraFailureLine) { "FAIL" } else { "PASS" }
        ScreenshotStability = if ($screenshotStabilityPassLine) { "PASS" } elseif ($screenshotStabilityFailLine) { "FAIL" } else { "MISSING" }
        ScreenshotTiming = if ($screenshotTimingPassLine) { "PASS" } elseif ($screenshotTimingFailLine) { "FAIL" } else { "MISSING" }
        Blocker = if ($blockerLine) { "FAIL" } else { "PASS" }
        TerrainSupportMissLine = if ($terrainSupportMissLine) { $terrainSupportMissLine.Line } else { $null }
        AirborneLine = if ($airborneLine) { $airborneLine.Line } else { $null }
        CameraSettledLine = if ($cameraSettledLine) { $cameraSettledLine.Line } else { $null }
        CameraFailureLine = if ($cameraFailureLine) { $cameraFailureLine.Line } else { $null }
        ScreenshotStabilityLine = if ($screenshotStabilityPassLine) { $screenshotStabilityPassLine.Line } elseif ($screenshotStabilityFailLine) { $screenshotStabilityFailLine.Line } else { $null }
        ScreenshotTimingLine = if ($screenshotTimingPassLine) { $screenshotTimingPassLine.Line } elseif ($screenshotTimingFailLine) { $screenshotTimingFailLine.Line } else { $null }
        BlockerLine = if ($blockerLine) { $blockerLine.Line } else { $null }
    }
}

function Get-OverallStatus {
    param([pscustomobject]$Result)

    if ($Result.InvocationStatus -eq "FAIL") { return "FAIL" }
    if ([string]::IsNullOrWhiteSpace($Result.ProofDir)) { return "FAIL" }
    if ([string]::IsNullOrWhiteSpace($Result.SourceImage)) { return "FAIL" }
    if ($Result.LogProbe.Blocker -eq "FAIL") { return "FAIL" }
    if ($Result.SummaryProbe.TerrainSupport -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.Airborne -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.Camera -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.CameraFailure -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.ScreenshotStability -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.ScreenshotTiming -ne "PASS") { return "FAIL" }
    if ($Result.SummaryProbe.Blocker -ne "PASS") { return "FAIL" }
    if ($Result.LogProbe.ActorMatch -ne "PASS") { return "FAIL" }
    if ($Result.LogProbe.PartAssembly -eq "FAIL") { return "FAIL" }
    if ($Result.LogProbe.PartAssembly -ne "PASS") { return "REVIEW" }
    if ($Result.ImageQuality.Status -ne "PASS") { return "REVIEW" }
    if ($Result.LogProbe.Face -ne "PASS") { return "REVIEW" }
    if ($Result.LogProbe.Animation -ne "PASS") { return "REVIEW" }
    if ($Result.VisualReview -ne "PASS") { return "REVIEW" }
    return "PASS"
}

function Invoke-MugshotTarget {
    param([string]$Target)

    $safeTarget = ConvertTo-SafeFileName $Target
    $harnessLog = Join-Path $SweepDir "$safeTarget`_harness_output.log"
    $before = @(Get-ProofDirectories)
    $startedAt = Get-Date
    $invocationStatus = "PASS"
    $invocationError = ""

    $proofArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        RunSeconds = $RunSeconds
        BootstrapHour = $BootstrapHour
        ScreenshotFrames = $ScreenshotFrames
        BootstrapCell = "FormId:0x10daeb9"
        BootstrapX = -67735
        BootstrapY = 3204
        BootstrapZ = 8425
        BootstrapRotX = 0
        BootstrapRotZ = -0.6981317
        ActorTarget = $Target
        ActorFrame = $ActorFrame
        ActorViewOffsetX = $ActorViewOffsetX
        ActorViewOffsetY = $ActorViewOffsetY
        ActorViewOffsetZ = $ActorViewOffsetZ
        ActorViewTargetZ = $ActorViewTargetZ
        RequirePlayerTerrainSupport = $true
        RequireFlatCameraSettled = $true
        RequireScreenshotStability = $true
        RequireActorVisibleHandGeometry = $true
        FnvPartMatrixAudit = $true
    }
    if (!$WorldSpaceActorViewOffset) { $proofArgs.ActorViewLocalOffset = $true }
    if ($DisableNativeAnimationCallbacks) { $proofArgs.FnvDisableNativeAnimationCallbacks = $true }
    if ($StageActor) {
        $proofArgs.StageActor = $true
        $proofArgs.ActorStageX = $ActorStageX
        $proofArgs.ActorStageY = $ActorStageY
        $proofArgs.ActorStageZ = $ActorStageZ
        $proofArgs.ActorStageRotZ = $ActorStageRotZ
    }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if ($NoSound) { $proofArgs.NoSound = $true }

    Write-Host ""
    Write-Host "=== FNV actor mugshot target: $Target ==="
    try {
        & $FlatProof @proofArgs 2>&1 | Tee-Object -FilePath $harnessLog | Out-Host
    }
    catch {
        $invocationStatus = "FAIL"
        $invocationError = $_.Exception.Message
        Add-Content -LiteralPath $harnessLog -Value ""
        Add-Content -LiteralPath $harnessLog -Value "HARNESS ERROR: $invocationError"
        Write-Warning "Target $Target failed while running wrapped proof: $invocationError"
    }

    $latest = Get-NewProofDirectory -Before $before -StartedAt $startedAt
    $proofDir = if ($null -ne $latest) { $latest.FullName } else { $null }
    $shot = Get-LatestScreenshot -ProofDir $proofDir

    $sourcePath = $null
    $zoomPath = $null
    $summaryPath = $null
    $openmwLogPath = $null
    $myGuiLogPath = $null
    $stdoutPath = $null
    $stderrPath = $null

    if ($null -ne $shot) {
        $sourcePath = Join-Path $SweepDir "$safeTarget`_source.png"
        $zoomPath = Join-Path $SweepDir "$safeTarget`_mugshot.png"
        Copy-Item -LiteralPath $shot.FullName -Destination $sourcePath -Force
        Copy-CenteredCrop -SourcePath $shot.FullName -OutputPath $zoomPath
    }

    if (![string]::IsNullOrWhiteSpace($proofDir)) {
        $summaryPath = Copy-IfPresent -SourcePath (Join-Path $proofDir "summary.txt") -DestinationPath (Join-Path $SweepDir "$safeTarget`_summary.txt")
        $openmwLogPath = Copy-IfPresent -SourcePath (Join-Path $proofDir "openmw.log") -DestinationPath (Join-Path $SweepDir "$safeTarget`_openmw.log")
        $myGuiLogPath = Copy-IfPresent -SourcePath (Join-Path $proofDir "MyGUI.log") -DestinationPath (Join-Path $SweepDir "$safeTarget`_MyGUI.log")
        $stdoutPath = Copy-IfPresent -SourcePath (Join-Path $proofDir "openmw-proof.stdout.log") -DestinationPath (Join-Path $SweepDir "$safeTarget`_stdout.log")
        $stderrPath = Copy-IfPresent -SourcePath (Join-Path $proofDir "openmw-proof.stderr.log") -DestinationPath (Join-Path $SweepDir "$safeTarget`_stderr.log")
    }

    $qualityPath = if (![string]::IsNullOrWhiteSpace($zoomPath)) { $zoomPath } else { $sourcePath }
    $quality = Get-ImageQuality -Path $qualityPath
    $logProbe = Get-LogProbe -LogPath $openmwLogPath -Target $Target
    $summaryProbe = Get-SummaryProbe -SummaryPath $summaryPath

    $result = [pscustomobject]@{
        Target = $Target
        SafeTarget = $safeTarget
        Overall = "PENDING"
        InvocationStatus = $invocationStatus
        InvocationError = $invocationError
        ProofDir = $proofDir
        SourceImage = $sourcePath
        MugshotImage = $zoomPath
        HarnessLog = $harnessLog
        OpenMwLog = $openmwLogPath
        MyGuiLog = $myGuiLogPath
        StdoutLog = $stdoutPath
        StderrLog = $stderrPath
        Summary = $summaryPath
        ImageQuality = $quality
        LogProbe = $logProbe
        SummaryProbe = $summaryProbe
        VisualReview = "REVIEW_REQUIRED"
        VisualPassPlaceholder = "[ ] PASS"
        VisualFailPlaceholder = "[ ] FAIL"
    }
    $result.Overall = Get-OverallStatus -Result $result
    return $result
}

function New-ContactSheet {
    param(
        [object[]]$Results,
        [string]$OutputPath
    )

    if ($Results.Count -eq 0) { return }

    $cols = [Math]::Max(1, $ContactSheetColumns)
    $rows = [int][Math]::Ceiling($Results.Count / $cols)
    $thumbW = 360
    $thumbH = 360
    $labelH = 118
    $sheet = [System.Drawing.Bitmap]::new($cols * $thumbW, $rows * ($thumbH + $labelH))

    try {
        $graphics = [System.Drawing.Graphics]::FromImage($sheet)
        try {
            $graphics.Clear([System.Drawing.Color]::FromArgb(18, 18, 18))
            $titleFont = [System.Drawing.Font]::new("Consolas", 18, [System.Drawing.FontStyle]::Bold)
            $smallFont = [System.Drawing.Font]::new("Consolas", 13, [System.Drawing.FontStyle]::Regular)
            $placeholderFont = [System.Drawing.Font]::new("Consolas", 24, [System.Drawing.FontStyle]::Bold)
            $whiteBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
            $mutedBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(190, 190, 190))
            $passBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(80, 230, 120))
            $reviewBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 210, 80))
            $failBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 95, 95))
            $borderPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(70, 70, 70), 1)
            $format = [System.Drawing.StringFormat]::new()
            $format.Trimming = [System.Drawing.StringTrimming]::EllipsisCharacter

            try {
                for ($i = 0; $i -lt $Results.Count; $i++) {
                    $result = $Results[$i]
                    $col = $i % $cols
                    $row = [Math]::Floor($i / $cols)
                    $x = $col * $thumbW
                    $y = $row * ($thumbH + $labelH)
                    $imageRect = [System.Drawing.Rectangle]::new($x, $y, $thumbW, $thumbH)
                    $labelRect = [System.Drawing.RectangleF]::new($x + 8, $y + $thumbH + 8, $thumbW - 16, $labelH - 8)

                    if (![string]::IsNullOrWhiteSpace($result.MugshotImage) -and (Test-Path -LiteralPath $result.MugshotImage)) {
                        $img = [System.Drawing.Bitmap]::new($result.MugshotImage)
                        try {
                            $graphics.DrawImage($img, $imageRect)
                        }
                        finally {
                            $img.Dispose()
                        }
                    }
                    else {
                        $graphics.FillRectangle([System.Drawing.Brushes]::Black, $imageRect)
                        $graphics.DrawString("NO IMAGE", $placeholderFont, $failBrush, $x + 102, $y + 160)
                    }

                    $graphics.DrawRectangle($borderPen, $imageRect)
                    $statusBrush = $reviewBrush
                    if ($result.Overall -eq "PASS") { $statusBrush = $passBrush }
                    if ($result.Overall -eq "FAIL") { $statusBrush = $failBrush }

                    $graphics.DrawString($result.Target, $titleFont, $whiteBrush, $labelRect, $format)
                    $graphics.DrawString("overall=$($result.Overall) visual=$($result.VisualReview)", $smallFont, $statusBrush, $x + 8, $y + $thumbH + 38)
                    $graphics.DrawString("face=$($result.LogProbe.Face) anim=$($result.LogProbe.Animation) parts=$($result.LogProbe.PartAssembly) suspect=$($result.LogProbe.PartAssemblySuspectCount)", $smallFont, $mutedBrush, $x + 8, $y + $thumbH + 60)
                    $graphics.DrawString("[ ] PASS  [ ] FAIL", $smallFont, $mutedBrush, $x + 8, $y + $thumbH + 82)
                }
            }
            finally {
                $titleFont.Dispose()
                $smallFont.Dispose()
                $placeholderFont.Dispose()
                $whiteBrush.Dispose()
                $mutedBrush.Dispose()
                $passBrush.Dispose()
                $reviewBrush.Dispose()
                $failBrush.Dispose()
                $borderPen.Dispose()
                $format.Dispose()
            }

            $sheet.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Jpeg)
        }
        finally {
            $graphics.Dispose()
        }
    }
    finally {
        $sheet.Dispose()
    }
}

function Write-Report {
    param(
        [object[]]$Results,
        [string]$ReportPath,
        [string]$ContactSheetPath,
        [string]$ResultsJsonPath,
        [string]$HumanReviewCsvPath
    )

    $lines = New-Object "System.Collections.Generic.List[string]"
    $lines.Add("# FNV Actor Mugshot Sweep")
    $lines.Add("")
    $lines.Add("- Sweep directory: ``$SweepDir``")
    $lines.Add("- Contact sheet: ``$ContactSheetPath``")
    $lines.Add("- Machine-readable results: ``$ResultsJsonPath``")
    $lines.Add("- Human review CSV: ``$HumanReviewCsvPath``")
    $lines.Add("- Targets: $($Results.Count)")
    $lines.Add("- Run seconds: $RunSeconds")
    $lines.Add("- Bootstrap hour: $BootstrapHour")
    $lines.Add("- Actor frame: $ActorFrame")
    $lines.Add("- Screenshot frames: $ScreenshotFrames")
    $lines.Add("- Crop: center ${CropWidth}x${CropHeight} offset=(${CropCenterOffsetX},${CropCenterOffsetY}) -> ${ZoomWidth}x${ZoomHeight}")
    $lines.Add("- Require pass: $RequirePass")
    $lines.Add("")
    $lines.Add("![Mugshot contact sheet](mugshot_contact_sheet.jpg)")
    $lines.Add("")
    $lines.Add("## Summary")
    $lines.Add("")
    $lines.Add("| Target | Overall | Visual | Actor | Face | Anim | Parts | Stability | Screenshot | Mugshot image | OpenMW log |")
    $lines.Add("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")

    foreach ($result in $Results) {
        $imageStatus = "$($result.ImageQuality.Status) $($result.ImageQuality.Width)x$($result.ImageQuality.Height)"
        $stabilityStatus = "terrain=$($result.SummaryProbe.TerrainSupport) airborne=$($result.SummaryProbe.Airborne) camera=$($result.SummaryProbe.Camera) screenshot=$($result.SummaryProbe.ScreenshotStability) timing=$($result.SummaryProbe.ScreenshotTiming)"
        $lines.Add("| $(ConvertTo-MarkdownCell $result.Target) | $($result.Overall) | $($result.VisualReview) | $($result.LogProbe.ActorMatch) | $($result.LogProbe.Face) | $($result.LogProbe.Animation) | $($result.LogProbe.PartAssembly) | $(ConvertTo-MarkdownCell $stabilityStatus) | $(ConvertTo-MarkdownCell $imageStatus) | ``$(ConvertTo-MarkdownCell $result.MugshotImage)`` | ``$(ConvertTo-MarkdownCell $result.OpenMwLog)`` |")
    }

    $lines.Add("")
    $lines.Add("## Target Details")
    foreach ($result in $Results) {
        $lines.Add("")
        $lines.Add("### $($result.Target)")
        $lines.Add("")
        $lines.Add("- Overall: $($result.Overall)")
        $lines.Add("- Visual review: $($result.VisualReview) $($result.VisualPassPlaceholder) $($result.VisualFailPlaceholder)")
        $lines.Add("- Actor match: $($result.LogProbe.ActorMatch)")
        $lines.Add("- Face check: $($result.LogProbe.Face)")
        $lines.Add("- Animation audit: $($result.LogProbe.Animation)")
        $lines.Add("- Part assembly audit: $($result.LogProbe.PartAssembly) suspect=$($result.LogProbe.PartAssemblySuspectCount)")
        $lines.Add("- Stability: terrain=$($result.SummaryProbe.TerrainSupport) airborne=$($result.SummaryProbe.Airborne) camera=$($result.SummaryProbe.Camera) cameraFailure=$($result.SummaryProbe.CameraFailure) screenshot=$($result.SummaryProbe.ScreenshotStability) timing=$($result.SummaryProbe.ScreenshotTiming) blockers=$($result.SummaryProbe.Blocker)")
        $lines.Add("- Known tolerated missing mesh lines: $($result.LogProbe.KnownNoiseCount)")
        $lines.Add("- Screenshot auto-check: $($result.ImageQuality.Status) $($result.ImageQuality.Width)x$($result.ImageQuality.Height) stddev=$($result.ImageQuality.StdDev) buckets=$($result.ImageQuality.Buckets) dark=$($result.ImageQuality.DarkPercent)% markerPink=$($result.ImageQuality.MarkerPinkPercent)%")
        $lines.Add("- Proof dir: ``$($result.ProofDir)``")
        $lines.Add("- Source image: ``$($result.SourceImage)``")
        $lines.Add("- Mugshot image: ``$($result.MugshotImage)``")
        $lines.Add("- Harness output: ``$($result.HarnessLog)``")
        $lines.Add("- OpenMW log: ``$($result.OpenMwLog)``")
        $lines.Add("- MyGUI log: ``$($result.MyGuiLog)``")
        $lines.Add("- Stdout log: ``$($result.StdoutLog)``")
        $lines.Add("- Stderr log: ``$($result.StderrLog)``")
        $lines.Add("- Summary: ``$($result.Summary)``")
        if (![string]::IsNullOrWhiteSpace($result.InvocationError)) {
            $lines.Add("- Invocation error: ``$($result.InvocationError)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.BlockerLine)) {
            $lines.Add("- Blocker line: ``$(ConvertTo-MarkdownCell $result.LogProbe.BlockerLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.TerrainSupportMissLine)) {
            $lines.Add("- Terrain support miss line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.TerrainSupportMissLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.AirborneLine)) {
            $lines.Add("- Airborne line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.AirborneLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.CameraSettledLine)) {
            $lines.Add("- Camera settled line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.CameraSettledLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.CameraFailureLine)) {
            $lines.Add("- Camera failure line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.CameraFailureLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.ScreenshotStabilityLine)) {
            $lines.Add("- Screenshot stability line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.ScreenshotStabilityLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.ScreenshotTimingLine)) {
            $lines.Add("- Screenshot timing line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.ScreenshotTimingLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.SummaryProbe.BlockerLine)) {
            $lines.Add("- Summary blocker line: ``$(ConvertTo-MarkdownCell $result.SummaryProbe.BlockerLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.MatchLine)) {
            $lines.Add("- Match line: ``$(ConvertTo-MarkdownCell $result.LogProbe.MatchLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.FaceLine)) {
            $lines.Add("- Face line: ``$(ConvertTo-MarkdownCell $result.LogProbe.FaceLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.AnimationLine)) {
            $lines.Add("- Animation line: ``$(ConvertTo-MarkdownCell $result.LogProbe.AnimationLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.WorldPostureBadLine)) {
            $lines.Add("- World posture BAD line: ``$(ConvertTo-MarkdownCell $result.LogProbe.WorldPostureBadLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.WorldPostureOkLine)) {
            $lines.Add("- World posture OK line: ``$(ConvertTo-MarkdownCell $result.LogProbe.WorldPostureOkLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.ArmPoseBadLine)) {
            $lines.Add("- Arm pose BAD line: ``$(ConvertTo-MarkdownCell $result.LogProbe.ArmPoseBadLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.ArmPoseOkLine)) {
            $lines.Add("- Arm pose OK line: ``$(ConvertTo-MarkdownCell $result.LogProbe.ArmPoseOkLine)``")
        }
        if (![string]::IsNullOrWhiteSpace($result.LogProbe.PartAssemblyLine)) {
            $lines.Add("- Part assembly line: ``$(ConvertTo-MarkdownCell $result.LogProbe.PartAssemblyLine)``")
        }
    }

    Set-Content -LiteralPath $ReportPath -Value $lines -Encoding UTF8
}

function Write-HumanReviewCsv {
    param(
        [object[]]$Results,
        [string]$OutputPath
    )

    $rows = foreach ($result in $Results) {
        [pscustomobject]@{
            Target = $result.Target
            MachineOverall = $result.Overall
            MachineFace = $result.LogProbe.Face
            MachineAnimation = $result.LogProbe.Animation
            MachineWorldPostureBad = if ($result.LogProbe.WorldPostureBadLine) { "YES" } else { "NO" }
            MachineArmPoseBad = if ($result.LogProbe.ArmPoseBadLine) { "YES" } else { "NO" }
            MachineParts = $result.LogProbe.PartAssembly
            MachineImage = $result.ImageQuality.Status
            MachineScreenshotStability = $result.SummaryProbe.ScreenshotStability
            MachineScreenshotTiming = $result.SummaryProbe.ScreenshotTiming
            VisualReview = "REVIEW_REQUIRED"
            HumanReview = ""
            HumanNotes = ""
            MugshotImage = $result.MugshotImage
            SourceImage = $result.SourceImage
            ProofDir = $result.ProofDir
            OpenMwLog = $result.OpenMwLog
        }
    }

    $rows | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding UTF8
}

$results = New-Object "System.Collections.Generic.List[object]"
foreach ($target in $Targets) {
    $results.Add((Invoke-MugshotTarget -Target $target))
}

$resultArray = $results.ToArray()
$contactSheet = Join-Path $SweepDir "mugshot_contact_sheet.jpg"
$report = Join-Path $SweepDir "mugshot_report.md"
$resultsJson = Join-Path $SweepDir "mugshot_results.json"
$humanReviewCsv = Join-Path $SweepDir "human-review.csv"

New-ContactSheet -Results $resultArray -OutputPath $contactSheet
$resultArray | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsJson -Encoding UTF8
Write-HumanReviewCsv -Results $resultArray -OutputPath $humanReviewCsv
Write-Report -Results $resultArray -ReportPath $report -ContactSheetPath $contactSheet -ResultsJsonPath $resultsJson -HumanReviewCsvPath $humanReviewCsv

Write-Host ""
Write-Host "FNV actor mugshot sweep:"
Write-Host "  $SweepDir"
Write-Host "  Report: $report"
Write-Host "  Contact sheet: $contactSheet"
Write-Host "  Results JSON: $resultsJson"
Write-Host "  Human review CSV: $humanReviewCsv"
Get-ChildItem -LiteralPath $SweepDir | Select-Object FullName, Length

if ($RequirePass) {
    $notPassing = @($resultArray | Where-Object { $_.Overall -ne "PASS" })
    if ($notPassing.Count -gt 0) {
        $summary = ($notPassing | ForEach-Object { "$($_.Target)=$($_.Overall)" }) -join ", "
        throw "Actor mugshot sweep did not pass all targets: $summary. See $report"
    }
}
