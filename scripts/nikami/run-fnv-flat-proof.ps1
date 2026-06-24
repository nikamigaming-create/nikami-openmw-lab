param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 16,
    [string]$StartCell = "Goodsprings",
    [switch]$WithMenu,
    [switch]$IncludeFnvrPlugin,
    [string]$ScreenshotFrames = "",
    [string[]]$RequireLogPattern = @(),
    [string]$TerrainProbePoints = "",
    [string]$TerrainProbeGrid = "",
    [double]$BootstrapHour = [double]::NaN,
    [switch]$RequireTerrainProbeFullSupport,
    [string]$BootstrapCell = "",
    [double]$BootstrapX = [double]::NaN,
    [double]$BootstrapY = [double]::NaN,
    [double]$BootstrapZ = [double]::NaN,
    [double]$BootstrapRotX = [double]::NaN,
    [double]$BootstrapRotY = [double]::NaN,
    [double]$BootstrapRotZ = [double]::NaN,
    [double]$BootstrapCameraDistance = [double]::NaN,
    [double]$FlatCameraPitch = [double]::NaN,
    [double]$FlatCameraYaw = [double]::NaN,
    [string]$ActorTarget = "",
    [int]$ActorFrame = 240,
    [switch]$StageActor,
    [double]$ActorStageX = [double]::NaN,
    [double]$ActorStageY = [double]::NaN,
    [double]$ActorStageZ = [double]::NaN,
    [double]$ActorStageRotX = [double]::NaN,
    [double]$ActorStageRotY = [double]::NaN,
    [double]$ActorStageRotZ = [double]::NaN,
    [double]$ActorViewOffsetX = [double]::NaN,
    [double]$ActorViewOffsetY = [double]::NaN,
    [double]$ActorViewOffsetZ = [double]::NaN,
    [double]$ActorViewTargetZ = [double]::NaN,
    [string]$ProofGuiMode = "",
    [int]$ProofGuiFrame = 240,
    [double]$WalkEndX = [double]::NaN,
    [double]$WalkEndY = [double]::NaN,
    [double]$WalkEndZ = [double]::NaN,
    [int]$WalkStartFrame = 120,
    [int]$WalkEndFrame = 540,
    [double]$WalkSpeed = [double]::NaN,
    [double]$WalkReachRadius = [double]::NaN,
    [double]$WalkMinZ = [double]::NaN,
    [switch]$RequirePlayerTerrainSupport,
    [switch]$RequireFlatCameraSettled,
    [switch]$RequireScreenshotStability,
    [double]$ScreenshotStabilityMaxMeanAbsDelta = 18.0,
    [double]$ScreenshotStabilityMaxHighDeltaRatio = 0.35,
    [double]$ScreenshotStabilityCropFraction = 0.65,
    [int]$ScreenshotTimingMinPostCameraFrames = 30,
    [int]$ScreenshotTimingMaxActorCameraAgeFrames = 90,
    [switch]$RequireActorVisibleHandGeometry,
    [double]$ActorVisibleHandMaxDistance = 30.0,
    [switch]$FnvPartMatrixAudit,
    [switch]$FnvDisableNativeAnimationCallbacks,
    [switch]$FnvDlodSettingsDiag,
    [switch]$FnvPsaDeathPoseDiag,
    [string]$TraceRawPendingRecord = "",
    [string]$ClassificationDir = "",
    [switch]$RequireSkyColorSanity,
    [switch]$RequireSkyPaletteMatch,
    [switch]$RequireSunVisible,
    [switch]$RequireSunDirectionRuntime,
    [string]$StartupScript = "",
    [string]$LoadSavegame = "",
    [string]$FnvQuestSaveLoadMode = "",
    [int]$FnvQuestSaveLoadFrame = 0,
    [switch]$FnvQuestObjectiveScriptTrace,
    [switch]$FnvQuestJournalScriptTrace,
    [switch]$FnvQuestStageFragmentTrace,
    [switch]$FnvQuestTargetTrace,
    [string]$FnvQuestTargetQuest = "",
    [int]$FnvQuestTargetObjective = -1,
    [int]$FnvQuestTargetIndex = -1,
    [int]$FnvQuestTargetFrame = 0,
    [switch]$FnvQuestConditionTrace,
    [int]$FnvQuestConditionFrame = 0,
    [switch]$FnvNonzeroProjectileBindingTrace,
    [switch]$FnvPlayerPerkTrace,
    [switch]$FnvActorValueTrace,
    [switch]$FnvProgressionTrace,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-flat-proof/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
$HarnessLog = Join-Path $ProofDir "harness.log"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Set-ProofEnv([hashtable]$Previous, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
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

function Add-GridProbePoints([System.Collections.Generic.List[string]]$Points, [string]$Grid) {
    if ([string]::IsNullOrWhiteSpace($Grid)) { return }
    foreach ($entry in ($Grid -split ';')) {
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        $parts = $entry.Split('=', 2)
        if ($parts.Count -ne 2) { throw "Invalid terrain grid entry: $entry" }
        $label = $parts[0]
        $values = @($parts[1].Split(',') | ForEach-Object { [double]($_.Trim()) })
        if ($values.Count -ne 6) { throw "Invalid terrain grid coordinates: $entry" }
        $x1 = $values[0]; $y1 = $values[1]; $x2 = $values[2]; $y2 = $values[3]; $z = $values[4]; $step = [Math]::Abs($values[5])
        if ($step -le 0) { throw "Invalid terrain grid step: $entry" }
        $row = 0
        for ($y = [Math]::Min($y1, $y2); $y -le [Math]::Max($y1, $y2) + 0.001; $y += $step) {
            $col = 0
            for ($x = [Math]::Min($x1, $x2); $x -le [Math]::Max($x1, $x2) + 0.001; $x += $step) {
                $Points.Add("${label}_${row}_${col}=$x,$y,$z")
                $col++
            }
            $row++
        }
    }
}

function Get-ProbePointString([string]$Points, [string]$Grid) {
    $result = [System.Collections.Generic.List[string]]::new()
    if (![string]::IsNullOrWhiteSpace($Points)) {
        foreach ($entry in ($Points -split ';')) {
            if (![string]::IsNullOrWhiteSpace($entry)) { $result.Add($entry.Trim()) }
        }
    }
    Add-GridProbePoints $result $Grid
    return ($result -join ';')
}

function Copy-IfPresent([string]$Source, [string]$Destination) {
    if (![string]::IsNullOrWhiteSpace($Source) -and (Test-Path -LiteralPath $Source)) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        return $Destination
    }
    return $null
}

function Count-LogMatches([string]$Path, [string]$Pattern) {
    if (!(Test-Path -LiteralPath $Path)) { return 0 }
    return @((Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue)).Count
}

function Get-FirstLogLine([string]$Path, [string]$Pattern) {
    if (!(Test-Path -LiteralPath $Path)) { return $null }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $match) { return $null }
    return $match.Line
}

function Get-ActorProofPatterns([string]$Path, [string]$Target) {
    $patterns = [System.Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrWhiteSpace($Target)) { return @() }

    $patterns.Add([regex]::Escape($Target))
    $targetPattern = [regex]::Escape($Target)
    $matchLine = Get-FirstLogLine $Path "active-cell actor match target=`"$targetPattern`""
    if ($matchLine) {
        if ($matchLine -match "ref=([^ ]+)") { $patterns.Add([regex]::Escape($Matches[1])) }
        if ($matchLine -match "base=([^ ]+)") { $patterns.Add([regex]::Escape($Matches[1])) }
    }

    $seen = [System.Collections.Generic.HashSet[string]]::new()
    $result = [System.Collections.Generic.List[string]]::new()
    foreach ($pattern in $patterns) {
        if ($seen.Add($pattern)) { $result.Add($pattern) }
    }
    return @($result)
}

function Count-ActorPostureMatches([string]$Path, [string[]]$ActorPatterns, [string]$Kind, [string]$Verdict) {
    if (!(Test-Path -LiteralPath $Path) -or $ActorPatterns.Count -eq 0) { return 0 }

    $count = 0
    foreach ($pattern in $ActorPatterns) {
        $count += @((Select-String -LiteralPath $Path -Pattern "FNV/ESM4 diag: $Kind $pattern .* verdict=$Verdict" -ErrorAction SilentlyContinue)).Count
    }
    return $count
}

function Convert-LogFloat([string]$Text) {
    return [double]::Parse($Text, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Format-ProofNumber([object]$Value) {
    if ($null -eq $Value) { return "n/a" }
    return ([double]$Value).ToString("0.###", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-ActorVisibleHandGeometryProbe([string]$Path, [string[]]$ActorPatterns, [double]$MaxDistance) {
    if (!(Test-Path -LiteralPath $Path) -or $ActorPatterns.Count -eq 0) {
        return [pscustomobject]@{
            Status = "MISSING"
            SampleCount = 0
            LeftBestDistance = $null
            RightBestDistance = $null
            MaxDistance = $MaxDistance
            FailureLine = $null
        }
    }

    $seen = [System.Collections.Generic.HashSet[string]]::new()
    $samples = [System.Collections.Generic.List[object]]::new()
    foreach ($pattern in $ActorPatterns) {
        $matches = @(Select-String -LiteralPath $Path -Pattern "FNV/ESM4 ACTOR HAND GEOMETRY AUDIT $pattern " -ErrorAction SilentlyContinue)
        foreach ($match in $matches) {
            if (!$seen.Add($match.Line)) { continue }

            $side = "unknown"
            if ($match.Line -match " side=([^ ]+)") { $side = $Matches[1] }

            $renderValid = $false
            if ($match.Line -match " renderValid=([^ ]+)") {
                $renderValid = $Matches[1] -eq "1" -or $Matches[1] -eq "true"
            }

            $renderDistance = $null
            if ($match.Line -match " renderDistance=([-+0-9.eE]+)") {
                $renderDistance = Convert-LogFloat $Matches[1]
            }

            $samples.Add([pscustomobject]@{
                Side = $side
                RenderValid = $renderValid
                RenderDistance = $renderDistance
                Line = $match.Line
            })
        }
    }

    $bestLeft = $samples |
        Where-Object { $_.Side -eq "left" -and $_.RenderValid -and $null -ne $_.RenderDistance -and $_.RenderDistance -ge 0 } |
        Sort-Object RenderDistance |
        Select-Object -First 1
    $bestRight = $samples |
        Where-Object { $_.Side -eq "right" -and $_.RenderValid -and $null -ne $_.RenderDistance -and $_.RenderDistance -ge 0 } |
        Sort-Object RenderDistance |
        Select-Object -First 1

    $leftDistance = if ($null -ne $bestLeft) { [double]$bestLeft.RenderDistance } else { $null }
    $rightDistance = if ($null -ne $bestRight) { [double]$bestRight.RenderDistance } else { $null }
    $leftOk = $null -ne $leftDistance -and $leftDistance -le $MaxDistance
    $rightOk = $null -ne $rightDistance -and $rightDistance -le $MaxDistance
    $status = if ($leftOk -and $rightOk) { "PASS" } elseif ($samples.Count -gt 0) { "FAIL" } else { "MISSING" }

    $failureLine = $null
    if ($status -ne "PASS") {
        if ($null -ne $bestLeft -and ($null -eq $failureLine -or [double]$bestLeft.RenderDistance -gt $MaxDistance)) {
            $failureLine = $bestLeft.Line
        }
        if ($null -ne $bestRight -and [double]$bestRight.RenderDistance -gt $MaxDistance) {
            $failureLine = $bestRight.Line
        }
        if ($null -eq $failureLine -and $samples.Count -gt 0) {
            $failureLine = $samples[0].Line
        }
    }

    return [pscustomobject]@{
        Status = $status
        SampleCount = $samples.Count
        LeftBestDistance = $leftDistance
        RightBestDistance = $rightDistance
        MaxDistance = $MaxDistance
        FailureLine = $failureLine
    }
}

function Get-ScreenshotStabilityResult(
    [object[]]$Screenshots,
    [double]$MaxMeanAbsDelta,
    [double]$MaxHighDeltaRatio,
    [double]$CropFraction) {
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue

    $ordered = @($Screenshots | Sort-Object Name)
    $pairs = @()
    if ($ordered.Count -lt 2) {
        return [ordered]@{
            status = "MISSING"
            screenshotCount = $ordered.Count
            maxMeanAbsDelta = $MaxMeanAbsDelta
            maxHighDeltaRatio = $MaxHighDeltaRatio
            cropFraction = $CropFraction
            pairs = $pairs
        }
    }

    $allPass = $true
    for ($index = 1; $index -lt $ordered.Count; ++$index) {
        $previous = $ordered[$index - 1]
        $current = $ordered[$index]
        $previousBitmap = $null
        $currentBitmap = $null
        try {
            $previousBitmap = [System.Drawing.Bitmap]::FromFile($previous.FullName)
            $currentBitmap = [System.Drawing.Bitmap]::FromFile($current.FullName)
            $width = [Math]::Min($previousBitmap.Width, $currentBitmap.Width)
            $height = [Math]::Min($previousBitmap.Height, $currentBitmap.Height)
            $crop = [Math]::Max(0.10, [Math]::Min(1.0, $CropFraction))
            $cropWidth = [Math]::Max(1, [int][Math]::Round($width * $crop))
            $cropHeight = [Math]::Max(1, [int][Math]::Round($height * $crop))
            $startX = [Math]::Max(0, [int][Math]::Floor(($width - $cropWidth) / 2.0))
            $startY = [Math]::Max(0, [int][Math]::Floor(($height - $cropHeight) / 2.0))
            $stepX = [Math]::Max(1, [int][Math]::Floor($cropWidth / 96.0))
            $stepY = [Math]::Max(1, [int][Math]::Floor($cropHeight / 54.0))
            $samples = 0
            $sumDelta = 0.0
            $maxDelta = 0.0
            $highDelta = 0

            for ($y = $startY; $y -lt ($startY + $cropHeight); $y += $stepY) {
                for ($x = $startX; $x -lt ($startX + $cropWidth); $x += $stepX) {
                    $a = $previousBitmap.GetPixel($x, $y)
                    $b = $currentBitmap.GetPixel($x, $y)
                    $delta = ([Math]::Abs([double]$a.R - [double]$b.R) `
                        + [Math]::Abs([double]$a.G - [double]$b.G) `
                        + [Math]::Abs([double]$a.B - [double]$b.B)) / 3.0
                    $sumDelta += $delta
                    if ($delta -gt $maxDelta) { $maxDelta = $delta }
                    if ($delta -gt 45.0) { $highDelta++ }
                    $samples++
                }
            }

            $meanDelta = if ($samples -gt 0) { $sumDelta / $samples } else { [double]::PositiveInfinity }
            $highDeltaRatio = if ($samples -gt 0) { $highDelta / $samples } else { 1.0 }
            $pass = $meanDelta -le $MaxMeanAbsDelta -and $highDeltaRatio -le $MaxHighDeltaRatio
            if (!$pass) { $allPass = $false }
            $pairs += [pscustomobject][ordered]@{
                previous = $previous.Name
                current = $current.Name
                width = $width
                height = $height
                samples = $samples
                meanAbsDelta = [Math]::Round($meanDelta, 4)
                maxAbsDelta = [Math]::Round($maxDelta, 4)
                highDeltaRatio = [Math]::Round($highDeltaRatio, 6)
                pass = $pass
            }
        }
        finally {
            if ($null -ne $previousBitmap) { $previousBitmap.Dispose() }
            if ($null -ne $currentBitmap) { $currentBitmap.Dispose() }
        }
    }

    return [ordered]@{
        status = if ($allPass) { "PASS" } else { "FAIL" }
        screenshotCount = $ordered.Count
        maxMeanAbsDelta = $MaxMeanAbsDelta
        maxHighDeltaRatio = $MaxHighDeltaRatio
        cropFraction = $CropFraction
        pairs = $pairs
    }
}

function ConvertTo-ProofBool([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $false }
    return $Value -eq "1" -or $Value -ieq "true"
}

function Get-ScreenshotCaptureEvents([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) { return @() }

    $events = New-Object "System.Collections.Generic.List[object]"
    $lines = @(Select-String -LiteralPath $Path -Pattern "FNV/ESM4 proof: queuing native screenshot at frame" -ErrorAction SilentlyContinue)
    $order = 0
    foreach ($line in $lines) {
        $text = $line.Line
        if ($text -notmatch "queuing native screenshot at frame ([0-9]+)") { continue }
        $frame = [int]$Matches[1]
        $index = $order
        if ($text -match "index=([0-9]+)") { $index = [int]$Matches[1] }
        $flatCameraSettled = $false
        if ($text -match "flatCameraSettled=([^ ]+)") { $flatCameraSettled = ConvertTo-ProofBool $Matches[1] }
        $flatCameraSettledFrame = 0
        if ($text -match "flatCameraSettledFrame=([0-9]+)") { $flatCameraSettledFrame = [int]$Matches[1] }
        $actorTarget = ""
        if ($text -match "actorTarget=`"([^`"]*)`"") { $actorTarget = $Matches[1] }
        $actorCameraApplied = $false
        if ($text -match "actorCameraApplied=([^ ]+)") { $actorCameraApplied = ConvertTo-ProofBool $Matches[1] }
        $actorCameraFirstFrame = 0
        if ($text -match "actorCameraFirstFrame=([0-9]+)") { $actorCameraFirstFrame = [int]$Matches[1] }
        $actorCameraLastFrame = 0
        if ($text -match "actorCameraLastFrame=([0-9]+)") { $actorCameraLastFrame = [int]$Matches[1] }
        $actorStageLock = $false
        if ($text -match "actorStageLock=([^ ]+)") { $actorStageLock = ConvertTo-ProofBool $Matches[1] }

        $events.Add([pscustomobject][ordered]@{
            order = $order
            index = $index
            frame = $frame
            flatCameraSettled = $flatCameraSettled
            flatCameraSettledFrame = $flatCameraSettledFrame
            actorTarget = $actorTarget
            actorCameraApplied = $actorCameraApplied
            actorCameraFirstFrame = $actorCameraFirstFrame
            actorCameraLastFrame = $actorCameraLastFrame
            actorStageLock = $actorStageLock
            line = $text
        })
        ++$order
    }

    return @($events | Sort-Object order)
}

function Get-ScreenshotTimingResult(
    [object[]]$Screenshots,
    [string]$OpenMwLog,
    [string]$ActorTarget,
    [int]$MinPostCameraFrames,
    [int]$MaxActorCameraAgeFrames) {
    $ordered = @($Screenshots | Sort-Object Name)
    $events = @(Get-ScreenshotCaptureEvents $OpenMwLog)
    $items = @()

    if ($ordered.Count -eq 0) {
        return [ordered]@{
            status = "MISSING"
            screenshotCount = 0
            captureEventCount = $events.Count
            actorTarget = $ActorTarget
            minPostCameraFrames = $MinPostCameraFrames
            maxActorCameraAgeFrames = $MaxActorCameraAgeFrames
            screenshots = $items
        }
    }

    $allPass = $true
    for ($i = 0; $i -lt $ordered.Count; ++$i) {
        $screenshot = $ordered[$i]
        $event = if ($i -lt $events.Count) { $events[$i] } else { $null }
        $reason = "ok"
        $pass = $true
        $frame = 0
        $cameraSettleAge = $null
        $actorCameraFirstAge = $null
        $actorCameraLastAge = $null

        if ($null -eq $event) {
            $pass = $false
            $reason = "missing_capture_event"
        }
        else {
            $frame = $event.frame
            $cameraSettleAge = $event.frame - $event.flatCameraSettledFrame
            if (!$event.flatCameraSettled) {
                $pass = $false
                $reason = "flat_camera_not_settled_at_capture"
            }
            elseif ($event.flatCameraSettledFrame -le 0) {
                $pass = $false
                $reason = "missing_flat_camera_settle_frame"
            }
            elseif ($cameraSettleAge -lt $MinPostCameraFrames) {
                $pass = $false
                $reason = "capture_too_close_to_flat_camera_settle"
            }
            elseif (![string]::IsNullOrWhiteSpace($ActorTarget)) {
                $actorCameraFirstAge = $event.frame - $event.actorCameraFirstFrame
                $actorCameraLastAge = $event.frame - $event.actorCameraLastFrame
                if (!$event.actorCameraApplied) {
                    $pass = $false
                    $reason = "actor_camera_not_applied_at_capture"
                }
                elseif ($event.actorTarget -ine $ActorTarget) {
                    $pass = $false
                    $reason = "actor_target_mismatch"
                }
                elseif ($event.actorCameraFirstFrame -le 0) {
                    $pass = $false
                    $reason = "missing_actor_camera_first_frame"
                }
                elseif ($actorCameraFirstAge -lt $MinPostCameraFrames) {
                    $pass = $false
                    $reason = "capture_too_close_to_actor_camera_first_align"
                }
                elseif ($event.actorCameraLastFrame -le 0) {
                    $pass = $false
                    $reason = "missing_actor_camera_last_frame"
                }
                elseif ($actorCameraLastAge -lt 0 -or $actorCameraLastAge -gt $MaxActorCameraAgeFrames) {
                    $pass = $false
                    $reason = "actor_camera_alignment_stale_at_capture"
                }
            }
        }

        if (!$pass) { $allPass = $false }
        $items += [pscustomobject][ordered]@{
            screenshot = $screenshot.Name
            frame = $frame
            pass = $pass
            reason = $reason
            cameraSettleAgeFrames = $cameraSettleAge
            actorCameraFirstAgeFrames = $actorCameraFirstAge
            actorCameraLastAgeFrames = $actorCameraLastAge
            event = $event
        }
    }

    return [ordered]@{
        status = if ($allPass) { "PASS" } else { "FAIL" }
        screenshotCount = $ordered.Count
        captureEventCount = $events.Count
        actorTarget = $ActorTarget
        minPostCameraFrames = $MinPostCameraFrames
        maxActorCameraAgeFrames = $MaxActorCameraAgeFrames
        screenshots = $items
    }
}

function Assert-LogPattern([string]$Path, [string]$Pattern) {
    if ((Count-LogMatches $Path $Pattern) -eq 0) {
        throw "Missing required OpenMW log pattern: $Pattern"
    }
    Write-ProofLine "OK required log pattern: $Pattern"
}

function Assert-NoShaderBlockers([string[]]$Paths) {
    $shaderBlockerPattern = "failed initializing shader: sky|Failed to open shader .*sky\.(vert|frag)|Shader .*sky.* error|Failed to compile|failed to compile|linking failed|GLSL.*error|shader.*error"
    foreach ($path in $Paths) {
        if ([string]::IsNullOrWhiteSpace($path) -or !(Test-Path -LiteralPath $path -PathType Leaf)) {
            continue
        }
        if ((Count-LogMatches $path $shaderBlockerPattern) -gt 0) {
            throw "FNV flat proof saw sky shader/blocker lines in $path"
        }
        Write-ProofLine "OK absent shader blockers: $path"
    }
}

function Get-SkyColorSanityStats([string]$Path) {
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $samples = 0
        $redDominant = 0
        $blueDominant = 0
        $cyanDominant = 0
        $bright = 0
        $sumR = 0.0
        $sumG = 0.0
        $sumB = 0.0

        $startY = [int]($height * 0.05)
        $endY = [int]($height * 0.45)
        $startX = [int]($width * 0.10)
        $endX = [int]($width * 0.90)

        for ($y = $startY; $y -lt $endY; $y += 4) {
            for ($x = $startX; $x -lt $endX; $x += 4) {
                $pixel = $bitmap.GetPixel($x, $y)
                $r = [double]$pixel.R
                $g = [double]$pixel.G
                $b = [double]$pixel.B
                $luma = (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
                if ($luma -lt 25.0) {
                    continue
                }

                $samples++
                $sumR += $r
                $sumG += $g
                $sumB += $b
                if ($r -gt ($g + 35.0) -and $r -gt ($b + 35.0)) {
                    $redDominant++
                }
                if ($b -gt ($r + 45.0) -and $b -gt ($g + 20.0)) {
                    $blueDominant++
                }
                if ($g -gt ($r + 25.0) -and $b -gt ($r + 25.0)) {
                    $cyanDominant++
                }
                if ($luma -gt 140.0) {
                    $bright++
                }
            }
        }

        if ($samples -lt 1000) {
            throw "Sky color sanity had too few sampled pixels in ${Path}: $samples"
        }

        $avgR = $sumR / $samples
        $avgG = $sumG / $samples
        $avgB = $sumB / $samples
        $redDominantRatio = $redDominant / $samples
        $blueDominantRatio = $blueDominant / $samples
        $cyanDominantRatio = $cyanDominant / $samples
        $rawRedMaskLeak = $redDominantRatio -gt 0.55 -and $avgR -gt ($avgG + 35.0) -and $avgR -gt ($avgB + 35.0)
        $morrowindBluePaletteLeak = $cyanDominantRatio -gt 0.75 -and $avgB -gt ($avgR + 55.0) -and $avgB -gt ($avgG + 30.0)

        return [ordered]@{
            path = $Path
            width = $width
            height = $height
            samples = $samples
            avgR = [Math]::Round($avgR, 4)
            avgG = [Math]::Round($avgG, 4)
            avgB = [Math]::Round($avgB, 4)
            redDominantRatio = [Math]::Round($redDominantRatio, 6)
            blueDominantRatio = [Math]::Round($blueDominantRatio, 6)
            cyanDominantRatio = [Math]::Round($cyanDominantRatio, 6)
            brightRatio = [Math]::Round(($bright / $samples), 6)
            rawRedMaskLeak = $rawRedMaskLeak
            morrowindBluePaletteLeak = $morrowindBluePaletteLeak
        }
    }
    finally {
        $bitmap.Dispose()
    }
}

function Assert-SkyColorSanity([object[]]$Screenshots, [string]$ProofDir) {
    if ($Screenshots.Count -eq 0) {
        throw "FNV sky color sanity requires screenshots. Pass -ScreenshotFrames with at least one frame."
    }

    $stats = @()
    foreach ($screenshot in $Screenshots) {
        $stats += [pscustomobject](Get-SkyColorSanityStats $screenshot.FullName)
    }

    $statsPath = Join-Path $ProofDir "sky-color-sanity.json"
    $stats | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statsPath -Encoding UTF8
    Write-ProofLine "Sky color sanity JSON: $statsPath"

    $failures = @($stats | Where-Object { $_.rawRedMaskLeak -or $_.morrowindBluePaletteLeak })
    foreach ($stat in $stats) {
        Write-ProofLine ("Sky color sample {0}: avg=({1},{2},{3}) redDominant={4} blueDominant={5} cyanDominant={6} rawRedMaskLeak={7} morrowindBluePaletteLeak={8}" -f `
            (Split-Path $stat.path -Leaf), $stat.avgR, $stat.avgG, $stat.avgB, `
            $stat.redDominantRatio, $stat.blueDominantRatio, $stat.cyanDominantRatio, $stat.rawRedMaskLeak, `
            $stat.morrowindBluePaletteLeak)
    }

    if ($failures.Count -gt 0) {
        throw "FNV sky color sanity detected raw red/mask leakage or Morrowind blue-palette leakage. See $statsPath"
    }
}

function Get-LatestWeatherFallbackJson([string]$ProofRoot) {
    $weatherFallbackRoot = Join-Path $ProofRoot "fnv-weather-fallbacks"
    $latest = Get-ChildItem -LiteralPath $weatherFallbackRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "Missing generated FNV weather fallback proof under $weatherFallbackRoot"
    }

    $jsonPath = Join-Path $latest.FullName "fnv-weather-fallbacks.json"
    if (!(Test-Path -LiteralPath $jsonPath -PathType Leaf)) {
        throw "Missing generated FNV weather fallback JSON: $jsonPath"
    }

    return $jsonPath
}

function Convert-RgbTriplet([object]$Value, [string]$Label) {
    $items = @($Value)
    if ($items.Count -ne 3) {
        throw "Expected RGB triplet for ${Label}, got $($items.Count) values"
    }

    return @($items | ForEach-Object { [double]$_ })
}

function Get-NormalizedRgb([double]$R, [double]$G, [double]$B) {
    $sum = [Math]::Max(1.0, $R + $G + $B)
    return @(($R / $sum), ($G / $sum), ($B / $sum))
}

function Get-RgbLuma([double[]]$Rgb) {
    return (0.2126 * $Rgb[0]) + (0.7152 * $Rgb[1]) + (0.0722 * $Rgb[2])
}

function Get-NormalizedRgbDistance([double[]]$A, [double[]]$B) {
    return [Math]::Sqrt(
        [Math]::Pow($A[0] - $B[0], 2.0) +
        [Math]::Pow($A[1] - $B[1], 2.0) +
        [Math]::Pow($A[2] - $B[2], 2.0))
}

function Get-NearestExpectedSkyBand([double[]]$Normalized, [object]$Expected) {
    $nearestName = ""
    $nearestDistance = [double]::PositiveInfinity
    foreach ($band in $Expected.normalizedBands) {
        $distance = Get-NormalizedRgbDistance $Normalized @([double]$band.r, [double]$band.g, [double]$band.b)
        if ($distance -lt $nearestDistance) {
            $nearestDistance = $distance
            $nearestName = [string]$band.name
        }
    }

    return [ordered]@{
        name = $nearestName
        distance = $nearestDistance
    }
}

function Get-FnvSkyPaletteExpected([string]$WeatherFallbackJson) {
    $metadata = Get-Content -LiteralPath $WeatherFallbackJson -Raw | ConvertFrom-Json
    $clear = $metadata.selectedWeather.Clear
    if ($null -eq $clear) {
        throw "Generated weather fallback metadata has no Clear weather selection: $WeatherFallbackJson"
    }
    $day = $clear.skyColorGroups.Day
    if ($null -eq $day) {
        throw "Generated weather fallback metadata has no Clear/Day sky colors: $WeatherFallbackJson"
    }

    $skyUpper = Convert-RgbTriplet $day.SkyUpper "Clear/Day/SkyUpper"
    $skyLower = Convert-RgbTriplet $day.SkyLower "Clear/Day/SkyLower"
    $horizon = Convert-RgbTriplet $day.Horizon "Clear/Day/Horizon"
    $expectedLumas = @(
        (Get-RgbLuma $skyUpper)
        (Get-RgbLuma $skyLower)
        (Get-RgbLuma $horizon)
    )
    $normalizedSkyUpper = Get-NormalizedRgb -R ([double]$skyUpper[0]) -G ([double]$skyUpper[1]) -B ([double]$skyUpper[2])
    $normalizedSkyLower = Get-NormalizedRgb -R ([double]$skyLower[0]) -G ([double]$skyLower[1]) -B ([double]$skyLower[2])
    $normalizedHorizon = Get-NormalizedRgb -R ([double]$horizon[0]) -G ([double]$horizon[1]) -B ([double]$horizon[2])
    $normalizedBands = @(
        [ordered]@{ name = "SkyUpper"; r = $normalizedSkyUpper[0]; g = $normalizedSkyUpper[1]; b = $normalizedSkyUpper[2] }
        [ordered]@{ name = "SkyLower"; r = $normalizedSkyLower[0]; g = $normalizedSkyLower[1]; b = $normalizedSkyLower[2] }
        [ordered]@{ name = "Horizon"; r = $normalizedHorizon[0]; g = $normalizedHorizon[1]; b = $normalizedHorizon[2] }
    )

    return [ordered]@{
        weatherFallbackJson = $WeatherFallbackJson
        expectedWeather = "Clear"
        expectedTime = "Day"
        selectedEditorId = [string]$clear.selectedEditorId
        selectedPlugin = [string]$clear.selectedPlugin
        selectedFormId = [string]$clear.selectedFormId
        SkyUpper = $skyUpper
        SkyLower = $skyLower
        Horizon = $horizon
        minExpectedLuma = ($expectedLumas | Measure-Object -Minimum).Minimum
        maxExpectedLuma = ($expectedLumas | Measure-Object -Maximum).Maximum
        normalizedBands = $normalizedBands
    }
}

function Get-SkyPaletteBandStats(
    [System.Drawing.Bitmap]$Bitmap,
    [string]$Name,
    [double]$StartYFraction,
    [double]$EndYFraction,
    [object]$Expected) {

    $width = $Bitmap.Width
    $height = $Bitmap.Height
    $samples = 0
    $sumR = 0.0
    $sumG = 0.0
    $sumB = 0.0
    $nearestCounts = [ordered]@{
        SkyUpper = 0
        SkyLower = 0
        Horizon = 0
    }
    $skyUpperBand = $Expected.normalizedBands | Where-Object { $_.name -eq "SkyUpper" } | Select-Object -First 1
    if ($null -eq $skyUpperBand) {
        throw "FNV sky palette band '$Name' has no generated SkyUpper expected band"
    }
    $skyUpperNormalized = @([double]$skyUpperBand.r, [double]$skyUpperBand.g, [double]$skyUpperBand.b)
    $bestSkyUpperDistance = [double]::PositiveInfinity
    $bestSkyUpperRgb = $null

    $startY = [int]($height * $StartYFraction)
    $endY = [int]($height * $EndYFraction)
    $startX = [int]($width * 0.10)
    $endX = [int]($width * 0.90)

    for ($y = $startY; $y -lt $endY; $y += 4) {
        for ($x = $startX; $x -lt $endX; $x += 4) {
            $pixel = $Bitmap.GetPixel($x, $y)
            $r = [double]$pixel.R
            $g = [double]$pixel.G
            $b = [double]$pixel.B
            $luma = (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
            if ($luma -lt 25.0 -or $luma -gt 252.0) {
                continue
            }

            $samples++
            $sumR += $r
            $sumG += $g
            $sumB += $b
            $pixelNormalized = Get-NormalizedRgb $r $g $b
            $pixelNearest = Get-NearestExpectedSkyBand $pixelNormalized $Expected
            $nearestCounts[$pixelNearest.name] = [int]$nearestCounts[$pixelNearest.name] + 1
            $skyUpperDistance = Get-NormalizedRgbDistance $pixelNormalized $skyUpperNormalized
            if ($skyUpperDistance -lt $bestSkyUpperDistance) {
                $bestSkyUpperDistance = $skyUpperDistance
                $bestSkyUpperRgb = @($r, $g, $b)
            }
        }
    }

    if ($samples -lt 1000) {
        throw "FNV sky palette band '$Name' had too few sampled pixels: $samples"
    }

    $avgR = $sumR / $samples
    $avgG = $sumG / $samples
    $avgB = $sumB / $samples
    $avgRgb = @($avgR, $avgG, $avgB)
    $avgLuma = Get-RgbLuma $avgRgb
    $normalized = Get-NormalizedRgb $avgR $avgG $avgB
    $nearest = Get-NearestExpectedSkyBand $normalized $Expected
    $skyUpperPixelFraction = [double]$nearestCounts.SkyUpper / [double]$samples

    return [ordered]@{
        name = $Name
        startYFraction = $StartYFraction
        endYFraction = $EndYFraction
        samples = $samples
        avgR = [Math]::Round($avgR, 4)
        avgG = [Math]::Round($avgG, 4)
        avgB = [Math]::Round($avgB, 4)
        avgLuma = [Math]::Round($avgLuma, 4)
        normalizedR = [Math]::Round($normalized[0], 6)
        normalizedG = [Math]::Round($normalized[1], 6)
        normalizedB = [Math]::Round($normalized[2], 6)
        blueMinusGreen = [Math]::Round(($avgB - $avgG), 4)
        greenMinusRed = [Math]::Round(($avgG - $avgR), 4)
        nearestExpectedBand = $nearest.name
        nearestExpectedNormalizedDistance = [Math]::Round($nearest.distance, 6)
        nearestExpectedBandCounts = $nearestCounts
        skyUpperPixelFraction = [Math]::Round($skyUpperPixelFraction, 6)
        bestSkyUpperRgb = @(
            [Math]::Round([double]$bestSkyUpperRgb[0], 4)
            [Math]::Round([double]$bestSkyUpperRgb[1], 4)
            [Math]::Round([double]$bestSkyUpperRgb[2], 4)
        )
        bestSkyUpperNormalizedDistance = [Math]::Round($bestSkyUpperDistance, 6)
        skyUpperVisiblePass = $skyUpperPixelFraction -ge 0.05 -and $bestSkyUpperDistance -le 0.03
        channelOrderMatches = $avgB -gt ($avgG + 8.0) -and $avgG -gt ($avgR + 8.0)
        normalizedDistancePass = $nearest.distance -le 0.12
    }
}

function Get-SkyPaletteMatchStats([string]$Path, [object]$Expected) {
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $samples = 0
        $sumR = 0.0
        $sumG = 0.0
        $sumB = 0.0

        $startY = [int]($height * 0.05)
        $endY = [int]($height * 0.45)
        $startX = [int]($width * 0.10)
        $endX = [int]($width * 0.90)

        for ($y = $startY; $y -lt $endY; $y += 4) {
            for ($x = $startX; $x -lt $endX; $x += 4) {
                $pixel = $bitmap.GetPixel($x, $y)
                $r = [double]$pixel.R
                $g = [double]$pixel.G
                $b = [double]$pixel.B
                $luma = (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
                if ($luma -lt 25.0 -or $luma -gt 252.0) {
                    continue
                }

                $samples++
                $sumR += $r
                $sumG += $g
                $sumB += $b
            }
        }

        if ($samples -lt 1000) {
            throw "FNV sky palette match had too few sampled pixels in ${Path}: $samples"
        }

        $avgR = $sumR / $samples
        $avgG = $sumG / $samples
        $avgB = $sumB / $samples
        $avgRgb = @($avgR, $avgG, $avgB)
        $avgLuma = Get-RgbLuma $avgRgb
        $normalized = Get-NormalizedRgb $avgR $avgG $avgB
        $nearest = Get-NearestExpectedSkyBand $normalized $Expected
        $nearestDistance = $nearest.distance

        $channelOrderMatches = $avgB -gt ($avgG + 8.0) -and $avgG -gt ($avgR + 8.0)
        $brightnessMatches = $avgLuma -ge ($Expected.minExpectedLuma * 0.45) `
            -and $avgLuma -le (($Expected.maxExpectedLuma * 1.15) + 20.0)
        $normalizedDistancePass = $nearestDistance -le 0.10
        $verticalBands = @(
            [pscustomobject](Get-SkyPaletteBandStats $bitmap "top" 0.05 0.18 $Expected)
            [pscustomobject](Get-SkyPaletteBandStats $bitmap "upperMid" 0.18 0.32 $Expected)
            [pscustomobject](Get-SkyPaletteBandStats $bitmap "mid" 0.32 0.45 $Expected)
            [pscustomobject](Get-SkyPaletteBandStats $bitmap "lower" 0.45 0.60 $Expected)
        )
        $observedExpectedBands = @($verticalBands | Select-Object -ExpandProperty nearestExpectedBand -Unique)
        $topBand = $verticalBands | Where-Object { $_.name -eq "top" } | Select-Object -First 1
        $lowerBand = $verticalBands | Where-Object { $_.name -eq "lower" } | Select-Object -First 1
        $skyUpperBandPass = [bool]$topBand.skyUpperVisiblePass
        $topCloudCompositeBandPass = @("SkyUpper", "SkyLower") -contains $topBand.nearestExpectedBand
        $lowerLooksLikeHorizon = $lowerBand.nearestExpectedBand -eq "Horizon"
        $upperBlueDominance = [double]$topBand.blueMinusGreen
        $lowerBlueDominance = [double]$lowerBand.blueMinusGreen
        $verticalBlueGradientPass = ($upperBlueDominance - $lowerBlueDominance) -ge 12.0
        $verticalBandOrderMatches = $skyUpperBandPass -and $topCloudCompositeBandPass -and $lowerLooksLikeHorizon -and $verticalBlueGradientPass
        $distinctVerticalBandsPass = $observedExpectedBands.Count -ge 2
        $verticalBandDistancePass = @($verticalBands | Where-Object { !$_.normalizedDistancePass }).Count -eq 0

        return [ordered]@{
            path = $Path
            width = $width
            height = $height
            samples = $samples
            avgR = [Math]::Round($avgR, 4)
            avgG = [Math]::Round($avgG, 4)
            avgB = [Math]::Round($avgB, 4)
            avgLuma = [Math]::Round($avgLuma, 4)
            normalizedR = [Math]::Round($normalized[0], 6)
            normalizedG = [Math]::Round($normalized[1], 6)
            normalizedB = [Math]::Round($normalized[2], 6)
            nearestExpectedBand = $nearest.name
            nearestExpectedNormalizedDistance = [Math]::Round($nearestDistance, 6)
            channelOrderMatches = $channelOrderMatches
            brightnessMatches = $brightnessMatches
            normalizedDistancePass = $normalizedDistancePass
            verticalBands = $verticalBands
            observedExpectedBands = $observedExpectedBands
            skyUpperBandPass = $skyUpperBandPass
            topCloudCompositeBandPass = $topCloudCompositeBandPass
            verticalBandOrderMatches = $verticalBandOrderMatches
            verticalBlueGradientPass = $verticalBlueGradientPass
            distinctVerticalBandsPass = $distinctVerticalBandsPass
            verticalBandDistancePass = $verticalBandDistancePass
            paletteMatches = $channelOrderMatches -and $brightnessMatches -and $normalizedDistancePass `
                -and $verticalBandOrderMatches -and $distinctVerticalBandsPass -and $verticalBandDistancePass
        }
    }
    finally {
        $bitmap.Dispose()
    }
}

function Assert-SkyPaletteMatch([object[]]$Screenshots, [string]$ProofDir, [string]$ProofRoot) {
    if ($Screenshots.Count -eq 0) {
        throw "FNV sky palette match requires screenshots. Pass -ScreenshotFrames with at least one frame."
    }

    $weatherFallbackJson = Get-LatestWeatherFallbackJson $ProofRoot
    $expected = Get-FnvSkyPaletteExpected $weatherFallbackJson
    $stats = @()
    foreach ($screenshot in $Screenshots) {
        $stats += [pscustomobject](Get-SkyPaletteMatchStats $screenshot.FullName $expected)
    }

    $failures = @($stats | Where-Object { !$_.paletteMatches })
    $result = [ordered]@{
        status = if ($failures.Count -eq 0) { "PASS" } else { "FAIL" }
        expected = $expected
        samples = $stats
    }
    $statsPath = Join-Path $ProofDir "sky-palette-match.json"
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statsPath -Encoding UTF8
    Write-ProofLine "Sky palette match JSON: $statsPath"

    foreach ($stat in $stats) {
        Write-ProofLine ("Sky palette sample {0}: avg=({1},{2},{3}) normalized=({4},{5},{6}) nearestBand={7} nearestDistance={8} channelOrderMatches={9} brightnessMatches={10} normalizedDistancePass={11} verticalBandOrderMatches={12} distinctVerticalBandsPass={13}" -f `
            (Split-Path $stat.path -Leaf), $stat.avgR, $stat.avgG, $stat.avgB, `
            $stat.normalizedR, $stat.normalizedG, $stat.normalizedB, `
            $stat.nearestExpectedBand, $stat.nearestExpectedNormalizedDistance, $stat.channelOrderMatches, `
            $stat.brightnessMatches, $stat.normalizedDistancePass, $stat.verticalBandOrderMatches, `
            $stat.distinctVerticalBandsPass)
        foreach ($band in $stat.verticalBands) {
            Write-ProofLine ("  band {0}: avg=({1},{2},{3}) nearestBand={4} nearestDistance={5} blueMinusGreen={6} greenMinusRed={7} skyUpperPixelFraction={8} bestSkyUpperDistance={9}" -f `
                $band.name, $band.avgR, $band.avgG, $band.avgB, $band.nearestExpectedBand, `
                $band.nearestExpectedNormalizedDistance, $band.blueMinusGreen, $band.greenMinusRed, `
                $band.skyUpperPixelFraction, $band.bestSkyUpperNormalizedDistance)
        }
    }

    if ($failures.Count -gt 0) {
        throw "FNV sky palette match failed against generated WTHR Clear/Day colors or vertical band ordering. See $statsPath"
    }
}

function Get-SunVisibilityStats([string]$Path) {
    Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $width = $bitmap.Width
        $height = $bitmap.Height
        $samples = 0
        $core = 0
        $maxLuma = 0.0
        $sumX = 0.0
        $sumY = 0.0

        $endY = [int]($height * 0.70)
        for ($y = 0; $y -lt $endY; $y += 2) {
            for ($x = 0; $x -lt $width; $x += 2) {
                $pixel = $bitmap.GetPixel($x, $y)
                $r = [double]$pixel.R
                $g = [double]$pixel.G
                $b = [double]$pixel.B
                $luma = (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
                $samples++
                if ($luma -gt $maxLuma) {
                    $maxLuma = $luma
                }
                if ($r -ge 245.0 -and $g -ge 245.0 -and $b -ge 235.0 -and $luma -ge 245.0) {
                    $core++
                    $sumX += $x
                    $sumY += $y
                }
            }
        }

        $coreRatio = if ($samples -gt 0) { $core / $samples } else { 0.0 }
        return [ordered]@{
            path = $Path
            width = $width
            height = $height
            samples = $samples
            sunCoreSamples = $core
            sunCoreRatio = [Math]::Round($coreRatio, 6)
            maxLuma = [Math]::Round($maxLuma, 4)
            centroidX = if ($core -gt 0) { [Math]::Round($sumX / $core, 2) } else { $null }
            centroidY = if ($core -gt 0) { [Math]::Round($sumY / $core, 2) } else { $null }
            sunVisible = ($core -ge 250 -and $coreRatio -ge 0.0005 -and $maxLuma -ge 250.0)
        }
    }
    finally {
        $bitmap.Dispose()
    }
}

function Assert-SunVisible([object[]]$Screenshots, [string]$ProofDir) {
    if ($Screenshots.Count -eq 0) {
        throw "FNV sun visibility requires screenshots. Pass -ScreenshotFrames with at least one frame."
    }

    $stats = @()
    foreach ($screenshot in $Screenshots) {
        $stats += [pscustomobject](Get-SunVisibilityStats $screenshot.FullName)
    }

    $statsPath = Join-Path $ProofDir "sun-visibility.json"
    $stats | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statsPath -Encoding UTF8
    Write-ProofLine "Sun visibility JSON: $statsPath"
    foreach ($stat in $stats) {
        Write-ProofLine ("Sun visibility sample {0}: core={1} ratio={2} maxLuma={3} centroid=({4},{5}) visible={6}" -f `
            (Split-Path $stat.path -Leaf), $stat.sunCoreSamples, $stat.sunCoreRatio, $stat.maxLuma, `
            $stat.centroidX, $stat.centroidY, $stat.sunVisible)
    }

    if (@($stats | Where-Object { $_.sunVisible }).Count -eq 0) {
        throw "FNV sun visibility did not find a bright sun/glare core in generated screenshots. See $statsPath"
    }
}

function Get-MatchDouble([System.Text.RegularExpressions.Match]$Match, [string]$Name) {
    return [double]$Match.Groups[$Name].Value
}

function Get-VectorDistance([double[]]$A, [double[]]$B) {
    return [Math]::Sqrt(
        [Math]::Pow($A[0] - $B[0], 2.0) +
        [Math]::Pow($A[1] - $B[1], 2.0) +
        [Math]::Pow($A[2] - $B[2], 2.0))
}

function Assert-SunDirectionRuntime([string]$OpenMwLog, [string]$ProofDir) {
    $number = "[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?"
    $weatherPattern = "FNV/ESM4 proof: sun orbit hour=(?<hour>$number) sunrise=(?<sunrise>$number) nightStart=(?<nightStart>$number) dayDuration=(?<dayDuration>$number) nightDuration=(?<nightDuration>$number) orbit=(?<orbit>$number) isNight=(?<isNight>[01]) rawDirection=\((?<rawX>$number),(?<rawY>$number),(?<rawZ>$number)\) expectedSkyPosition=\((?<skyX>$number),(?<skyY>$number),(?<skyZ>$number)\)"
    $renderPattern = "FNV/ESM4 proof: render sun direction raw=\((?<rawX>$number),(?<rawY>$number),(?<rawZ>$number)\) skyPosition=\((?<skyX>$number),(?<skyY>$number),(?<skyZ>$number)\) sunlightPosition=\((?<lightX>$number),(?<lightY>$number),(?<lightZ>$number)\) normalizedSky=\((?<normX>$number),(?<normY>$number),(?<normZ>$number)\) night=(?<night>[01]) matchSunlightToSun=(?<match>[01])"

    $weatherLines = @(Select-String -LiteralPath $OpenMwLog -Pattern $weatherPattern -AllMatches -ErrorAction SilentlyContinue)
    $renderLines = @(Select-String -LiteralPath $OpenMwLog -Pattern $renderPattern -AllMatches -ErrorAction SilentlyContinue)
    if ($weatherLines.Count -eq 0) {
        throw "FNV sun direction proof did not find weather sun orbit logs. See $OpenMwLog"
    }
    if ($renderLines.Count -eq 0) {
        throw "FNV sun direction proof did not find render sun direction logs. See $OpenMwLog"
    }

    $weatherMatch = $weatherLines[0].Matches[0]
    $renderMatch = $renderLines[0].Matches[0]
    $weatherSky = @(
        (Get-MatchDouble $weatherMatch "skyX"),
        (Get-MatchDouble $weatherMatch "skyY"),
        (Get-MatchDouble $weatherMatch "skyZ")
    )
    $renderSky = @(
        (Get-MatchDouble $renderMatch "skyX"),
        (Get-MatchDouble $renderMatch "skyY"),
        (Get-MatchDouble $renderMatch "skyZ")
    )
    $renderRaw = @(
        (Get-MatchDouble $renderMatch "rawX"),
        (Get-MatchDouble $renderMatch "rawY"),
        (Get-MatchDouble $renderMatch "rawZ")
    )
    $weatherRaw = @(
        (Get-MatchDouble $weatherMatch "rawX"),
        (Get-MatchDouble $weatherMatch "rawY"),
        (Get-MatchDouble $weatherMatch "rawZ")
    )
    $norm = @(
        (Get-MatchDouble $renderMatch "normX"),
        (Get-MatchDouble $renderMatch "normY"),
        (Get-MatchDouble $renderMatch "normZ")
    )
    $chainDistance = Get-VectorDistance $weatherSky $renderSky
    $normLength = [Math]::Sqrt(($norm[0] * $norm[0]) + ($norm[1] * $norm[1]) + ($norm[2] * $norm[2]))
    $dayDuration = Get-MatchDouble $weatherMatch "dayDuration"
    $nightDuration = Get-MatchDouble $weatherMatch "nightDuration"
    $orbit = Get-MatchDouble $weatherMatch "orbit"
    $hour = Get-MatchDouble $weatherMatch "hour"
    $sunrise = Get-MatchDouble $weatherMatch "sunrise"
    $nightStart = Get-MatchDouble $weatherMatch "nightStart"
    $adjustedHour = $hour
    $adjustedNightStart = $nightStart
    if ($hour -lt $sunrise) { $adjustedHour += 24.0 }
    if ($nightStart -lt $sunrise) { $adjustedNightStart += 24.0 }
    $isNight = $adjustedHour -ge $adjustedNightStart
    if (!$isNight) {
        $expectedOrbit = 1.0 - (2.0 * (($adjustedHour - $sunrise) / $dayDuration))
    }
    else {
        $expectedOrbit = 2.0 * (($adjustedHour - $adjustedNightStart) / $nightDuration) - 1.0
    }
    $expectedRaw = @((-400.0 * $expectedOrbit), 75.0, -100.0)
    $expectedSkyFromFormula = @(-$expectedRaw[0], -$expectedRaw[1], (400.0 - [Math]::Abs(-$expectedRaw[0])))
    $skyPositionZPositive = $renderSky[2] -gt 0.0
    $chainMatches = $chainDistance -le 0.01
    $orbitMatches = [Math]::Abs($orbit - $expectedOrbit) -le 0.001
    $rawDirectionMatches = (Get-VectorDistance $renderRaw $expectedRaw) -le 0.05 -and (Get-VectorDistance $weatherRaw $expectedRaw) -le 0.05
    $skyFormulaMatches = (Get-VectorDistance $renderSky $expectedSkyFromFormula) -le 0.05
    $normalizedSkyUnitLength = [Math]::Abs($normLength - 1.0) -le 0.01

    $result = [ordered]@{
        status = if ($chainMatches -and $orbitMatches -and $rawDirectionMatches -and $skyFormulaMatches -and $normalizedSkyUnitLength -and $skyPositionZPositive -and $dayDuration -gt 0.0 -and $nightDuration -gt 0.0) { "PASS" } else { "FAIL" }
        classification = "runtime-supported"
        fnvOrbitParity = "loaded-pending-runtime"
        runtimeBoundary = "Current OpenMW sun orbit and renderer vector chain are proved for PC-flat; this does not claim retail FNV sun-orbit parity."
        weatherLogCount = $weatherLines.Count
        renderLogCount = $renderLines.Count
        sample = [ordered]@{
            hour = $hour
            sunrise = $sunrise
            nightStart = $nightStart
            dayDuration = $dayDuration
            nightDuration = $nightDuration
            orbit = $orbit
            expectedOrbit = [Math]::Round($expectedOrbit, 6)
            orbitMatches = $orbitMatches
            weatherIsNight = [int]$weatherMatch.Groups["isNight"].Value
            renderNight = [int]$renderMatch.Groups["night"].Value
            renderMatchSunlightToSun = [int]$renderMatch.Groups["match"].Value
            rawDirection = $renderRaw
            expectedRawDirection = $expectedRaw
            rawDirectionMatches = $rawDirectionMatches
            expectedSkyPosition = $weatherSky
            expectedSkyPositionFromFormula = $expectedSkyFromFormula
            renderSkyPosition = $renderSky
            chainDistance = [Math]::Round($chainDistance, 6)
            chainMatches = $chainMatches
            skyFormulaMatches = $skyFormulaMatches
            normalizedSky = $norm
            normalizedSkyLength = [Math]::Round($normLength, 6)
            normalizedSkyUnitLength = $normalizedSkyUnitLength
            skyPositionZPositive = $skyPositionZPositive
        }
    }

    $statsPath = Join-Path $ProofDir "sun-direction.json"
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statsPath -Encoding UTF8
    Write-ProofLine "Sun direction JSON: $statsPath"
    Write-ProofLine ("Sun direction sample: hour={0} orbit={1} sky=({2},{3},{4}) chainDistance={5} normLength={6} zPositive={7}" -f `
        $result.sample.hour, $result.sample.orbit, $renderSky[0], $renderSky[1], $renderSky[2], `
        $result.sample.chainDistance, $result.sample.normalizedSkyLength, $result.sample.skyPositionZPositive)

    if ($result.status -ne "PASS") {
        throw "FNV sun direction proof failed. See $statsPath"
    }
}

function Get-LatestClassificationDir([string]$Root) {
    $classificationRoot = Join-Path $Root "fnv-no-silent-skip-classification"
    if (!(Test-Path -LiteralPath $classificationRoot -PathType Container)) {
        return ""
    }
    $latest = Get-ChildItem -LiteralPath $classificationRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($null -eq $latest) { return "" }
    return $latest.FullName
}

function Get-Esm4ClassificationMap([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        $Dir = Get-LatestClassificationDir $ProofRoot
    }
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        throw "Missing FNV no-silent-skip classification proof. Run scripts/nikami/test-fnv-no-silent-skip-classification.ps1 first."
    }
    $ledgerPath = Join-Path $Dir "classification-ledger.json"
    if (!(Test-Path -LiteralPath $ledgerPath -PathType Leaf)) {
        throw "Missing FNV no-silent-skip classification ledger: $ledgerPath"
    }
    $rows = @(Get-Content -LiteralPath $ledgerPath -Raw | ConvertFrom-Json)
    $map = @{}
    foreach ($row in ($rows | Where-Object { $_.itemType -eq "esm4-record-type" })) {
        $recordTypeProperty = $row.PSObject.Properties["recordType"]
        $classificationProperty = $row.PSObject.Properties["classification"]
        if ($null -eq $recordTypeProperty -or $null -eq $classificationProperty) { continue }
        $recordType = [string]$recordTypeProperty.Value
        if ([string]::IsNullOrWhiteSpace($recordType)) { continue }
        if (!$map.ContainsKey($recordType)) {
            $map[$recordType] = [System.Collections.Generic.List[string]]::new()
        }
        if (!$map[$recordType].Contains([string]$classificationProperty.Value)) {
            $map[$recordType].Add([string]$classificationProperty.Value)
        }
    }
    Write-ProofLine "ESM4 classification ledger: $ledgerPath"
    return $map
}

function Get-UnsupportedEsm4Skips([string]$Path) {
    $result = [System.Collections.Generic.List[object]]::new()
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $result
    }
    foreach ($match in (Select-String -LiteralPath $Path -Pattern "FNV/ESM4 inventory skipped unsupported:\s+(?<type>[A-Z0-9_]+)\s+count=(?<count>[0-9]+)" -ErrorAction SilentlyContinue)) {
        $recordName = $match.Matches[0].Groups["type"].Value
        $recordType = $recordName
        if ($recordType.EndsWith("4", [System.StringComparison]::Ordinal)) {
            $recordType = $recordType.Substring(0, $recordType.Length - 1)
        }
        $result.Add([pscustomobject]@{
            recordName = $recordName
            recordType = $recordType
            count = [int]$match.Matches[0].Groups["count"].Value
            line = $match.Line
        })
    }
    return $result
}

function Assert-UnsupportedEsm4SkipsClassified([object[]]$Skips, [string]$Dir) {
    if ($Skips.Count -eq 0) {
        return
    }
    $map = Get-Esm4ClassificationMap $Dir
    $failures = [System.Collections.Generic.List[string]]::new()
    foreach ($skip in $Skips) {
        if (!$map.ContainsKey($skip.recordType)) {
            $failures.Add("unclassified $($skip.recordName) count=$($skip.count)")
            continue
        }
        $classes = @($map[$skip.recordType])
        if ($classes -contains "runtime-supported" -or $classes -contains "loaded-pending-runtime") {
            $failures.Add("classification-mismatch $($skip.recordName) count=$($skip.count) classification=$($classes -join ',')")
            continue
        }
        if ($classes -contains "known-blocked") {
            Write-ProofLine "OK known-blocked ESM4 skip: $($skip.recordName) count=$($skip.count)"
            continue
        }
        $failures.Add("unsupported classification $($skip.recordName) count=$($skip.count) classification=$($classes -join ',')")
    }
    if ($failures.Count -gt 0) {
        foreach ($failure in $failures) {
            Write-ProofLine "FAIL ESM4 skip classification: $failure"
        }
        throw "FNV flat proof saw unclassified or mismatched unsupported ESM4 skips. See $OpenMwLog"
    }
}

$FlatScript = Join-Path $PSScriptRoot "run-fnv-flat.ps1"
if (!(Test-Path -LiteralPath $FlatScript)) {
    throw "Missing base flat runner: $FlatScript"
}

$ConfigDir = Join-Path $ProofRoot "configs/fnv-flat-clean"
$RuntimeDir = Join-Path $ProofRoot "runtime/fnv-flat-clean"
$ScreenshotDir = Join-Path $RuntimeDir "screenshots"
if (Test-Path -LiteralPath $ScreenshotDir) {
    Get-ChildItem -LiteralPath $ScreenshotDir -File -ErrorAction SilentlyContinue | Remove-Item -Force
}

$previousEnv = @{}
$probePoints = Get-ProbePointString $TerrainProbePoints $TerrainProbeGrid
try {
    Set-ProofEnv $previousEnv "OPENMW_PROOF_SCREENSHOT_FRAME" $ScreenshotFrames
    Set-ProofEnv $previousEnv "OPENMW_FNV_TERRAIN_PROBE_POINTS" $probePoints
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_CELL" $BootstrapCell
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_POS_X" $BootstrapX
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_POS_Y" $BootstrapY
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_POS_Z" $BootstrapZ
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_ROT_X" $BootstrapRotX
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_ROT_Y" $BootstrapRotY
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_ROT_Z" $BootstrapRotZ
    Set-ProofEnv $previousEnv "OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE" $BootstrapCameraDistance
    Set-ProofEnv $previousEnv "OPENMW_FNV_FLAT_CAMERA_PITCH" $FlatCameraPitch
    Set-ProofEnv $previousEnv "OPENMW_FNV_FLAT_CAMERA_YAW" $FlatCameraYaw
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_TARGET" $ActorTarget
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_FRAME" $ActorFrame
    if ($StageActor) { Set-ProofEnv $previousEnv "OPENMW_PROOF_STAGE_ACTOR" "1" }
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_X" $ActorStageX
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_Y" $ActorStageY
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_Z" $ActorStageZ
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_ROT_X" $ActorStageRotX
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_ROT_Y" $ActorStageRotY
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_STAGE_ROT_Z" $ActorStageRotZ
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_VIEW_OFFSET_X" $ActorViewOffsetX
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y" $ActorViewOffsetY
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z" $ActorViewOffsetZ
    Set-ProofEnv $previousEnv "OPENMW_PROOF_ACTOR_VIEW_TARGET_Z" $ActorViewTargetZ
    if (![string]::IsNullOrWhiteSpace($ActorTarget)) {
        Set-ProofEnv $previousEnv "OPENMW_PROOF_POSTURE_TARGET" $ActorTarget
        Set-ProofEnv $previousEnv "OPENMW_PROOF_POSTURE_LABEL" "fnv-actor-proof"
    }
    Set-ProofEnv $previousEnv "OPENMW_PROOF_GUI_MODE" $ProofGuiMode
    Set-ProofEnv $previousEnv "OPENMW_PROOF_GUI_FRAME" $ProofGuiFrame
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_END_X" $WalkEndX
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_END_Y" $WalkEndY
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_END_Z" $WalkEndZ
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_START_FRAME" $WalkStartFrame
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_END_FRAME" $WalkEndFrame
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_SPEED" $WalkSpeed
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_REACH_RADIUS" $WalkReachRadius
    Set-ProofEnv $previousEnv "OPENMW_PROOF_WALK_MIN_Z" $WalkMinZ
    if ($RequirePlayerTerrainSupport) { Set-ProofEnv $previousEnv "OPENMW_FNV_FLOOR_WATCHDOG" "1" }
    if ($FnvPartMatrixAudit) { Set-ProofEnv $previousEnv "OPENMW_FNV_PART_MATRIX_AUDIT" "1" }
    if ($FnvDisableNativeAnimationCallbacks) { Set-ProofEnv $previousEnv "OPENMW_FNV_DISABLE_NATIVE_ANIMATION_CALLBACKS" "1" }
    if ($FnvDlodSettingsDiag) { Set-ProofEnv $previousEnv "OPENMW_FNV_DLODSETTINGS_DIAG" "1" }
    if ($FnvPsaDeathPoseDiag) { Set-ProofEnv $previousEnv "OPENMW_FNV_PSA_DEATHPOSE_DIAG" "1" }
    Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_STARTUP_SCRIPT" $StartupScript
    if ($FnvQuestObjectiveScriptTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_OBJECTIVE_SCRIPT_TRACE" "1"
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_CONSOLE_SCRIPT_TRACE" "1"
    }
    if ($FnvQuestJournalScriptTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_JOURNAL_SCRIPT_TRACE" "1"
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_CONSOLE_SCRIPT_TRACE" "1"
    }
    if ($FnvQuestStageFragmentTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_STAGE_FRAGMENT_TRACE" "1"
    }
    if ($FnvQuestTargetTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_TARGETS" "1"
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_TARGET_QUEST" $FnvQuestTargetQuest
        if ($FnvQuestTargetObjective -ge 0) {
            Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_TARGET_OBJECTIVE" $FnvQuestTargetObjective
        }
        if ($FnvQuestTargetIndex -ge 0) {
            Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_TARGET_INDEX" $FnvQuestTargetIndex
        }
        if ($FnvQuestTargetFrame -gt 0) {
            Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_TARGET_FRAME" $FnvQuestTargetFrame
        }
    }
    if ($FnvQuestConditionTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_CONDITIONS" "1"
        if ($FnvQuestConditionFrame -gt 0) {
            Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_CONDITION_FRAME" $FnvQuestConditionFrame
        }
    }
    if ($FnvNonzeroProjectileBindingTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_NONZERO_PROJECTILE" "1"
    }
    if ($FnvPlayerPerkTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_PLAYER_PERKS" "1"
    }
    if ($FnvActorValueTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_ACTOR_VALUES" "1"
    }
    if ($FnvProgressionTrace) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_PROGRESSION" "1"
    }
    Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_SAVELOAD" $FnvQuestSaveLoadMode
    if ($FnvQuestSaveLoadFrame -gt 0) {
        Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_QUEST_SAVELOAD_FRAME" $FnvQuestSaveLoadFrame
    }
    Set-ProofEnv $previousEnv "OPENMW_FNV_TRACE_RAW_PENDING_RECORD" $TraceRawPendingRecord
    Set-ProofEnv $previousEnv "OPENMW_FNV_RENDER_DISTANCE_DIAG" "1"
    Set-ProofEnv $previousEnv "OPENMW_FNV_SKY_MISSING_LOG" "1"
    Set-ProofEnv $previousEnv "OPENMW_FNV_PROOF_WEATHER_ID" "1"

    $flatArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        StartCell = $StartCell
        MaxRunSeconds = $RunSeconds
    }
    if (![double]::IsNaN($BootstrapHour)) { $flatArgs.BootstrapHour = $BootstrapHour }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $flatArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $flatArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if (![string]::IsNullOrWhiteSpace($StartupScript)) { $flatArgs.StartupScript = $StartupScript }
    if (![string]::IsNullOrWhiteSpace($LoadSavegame)) { $flatArgs.LoadSavegame = $LoadSavegame }
    if ($WithMenu) { $flatArgs.WithMenu = $true }
    if ($IncludeFnvrPlugin) { $flatArgs.IncludeFnvrPlugin = $true }
    if ($NoSound) { $flatArgs.NoSound = $true }

    Write-ProofLine "FNV flat runtime proof $Stamp"
    Write-ProofLine "Runtime mode: pc-flat"
    Write-ProofLine "Runtime priority: pc-flat-first pcvr-second android-last"
    Write-ProofLine "IncludeFnvrPlugin: $IncludeFnvrPlugin"
    Write-ProofLine "RepoRoot: $RepoRoot"
    Write-ProofLine "ProofDir: $ProofDir"
    Write-ProofLine "RunSeconds: $RunSeconds"
    Write-ProofLine "ScreenshotFrames: $ScreenshotFrames"
    Write-ProofLine "WithMenu: $WithMenu"
    Write-ProofLine "RequireSkyColorSanity: $RequireSkyColorSanity"
    Write-ProofLine "RequireSkyPaletteMatch: $RequireSkyPaletteMatch"
    Write-ProofLine "RequireSunVisible: $RequireSunVisible"
    Write-ProofLine "RequireSunDirectionRuntime: $RequireSunDirectionRuntime"
    Write-ProofLine "RequireScreenshotStability: $RequireScreenshotStability"
    Write-ProofLine "ScreenshotStabilityMaxMeanAbsDelta: $ScreenshotStabilityMaxMeanAbsDelta"
    Write-ProofLine "ScreenshotStabilityMaxHighDeltaRatio: $ScreenshotStabilityMaxHighDeltaRatio"
    Write-ProofLine "ScreenshotStabilityCropFraction: $ScreenshotStabilityCropFraction"
    Write-ProofLine "ScreenshotTimingMinPostCameraFrames: $ScreenshotTimingMinPostCameraFrames"
    Write-ProofLine "ScreenshotTimingMaxActorCameraAgeFrames: $ScreenshotTimingMaxActorCameraAgeFrames"
    Write-ProofLine "RequireActorVisibleHandGeometry: $RequireActorVisibleHandGeometry"
    Write-ProofLine "ActorVisibleHandMaxDistance: $ActorVisibleHandMaxDistance"
    Write-ProofLine "BootstrapCell: $BootstrapCell"
    Write-ProofLine "FlatCameraPitch: $FlatCameraPitch"
    Write-ProofLine "FlatCameraYaw: $FlatCameraYaw"
    Write-ProofLine "FnvDlodSettingsDiag: $FnvDlodSettingsDiag"
    Write-ProofLine "FnvPsaDeathPoseDiag: $FnvPsaDeathPoseDiag"
    Write-ProofLine "FnvPlayerPerkTrace: $FnvPlayerPerkTrace"
    Write-ProofLine "FnvActorValueTrace: $FnvActorValueTrace"
    Write-ProofLine "FnvProgressionTrace: $FnvProgressionTrace"
    Write-ProofLine "StartupScript: $StartupScript"
    Write-ProofLine "LoadSavegame: $LoadSavegame"
    Write-ProofLine "FnvQuestSaveLoadMode: $FnvQuestSaveLoadMode"
    Write-ProofLine "FnvQuestSaveLoadFrame: $FnvQuestSaveLoadFrame"
    Write-ProofLine "FnvQuestObjectiveScriptTrace: $FnvQuestObjectiveScriptTrace"
    Write-ProofLine "FnvQuestJournalScriptTrace: $FnvQuestJournalScriptTrace"
    Write-ProofLine "FnvQuestStageFragmentTrace: $FnvQuestStageFragmentTrace"
    Write-ProofLine "FnvQuestTargetTrace: $FnvQuestTargetTrace"
    Write-ProofLine "FnvQuestTargetQuest: $FnvQuestTargetQuest"
    Write-ProofLine "FnvQuestTargetObjective: $FnvQuestTargetObjective"
    Write-ProofLine "FnvQuestTargetIndex: $FnvQuestTargetIndex"
    Write-ProofLine "FnvQuestTargetFrame: $FnvQuestTargetFrame"
    Write-ProofLine "FnvQuestConditionTrace: $FnvQuestConditionTrace"
    Write-ProofLine "FnvQuestConditionFrame: $FnvQuestConditionFrame"
    Write-ProofLine "FnvNonzeroProjectileBindingTrace: $FnvNonzeroProjectileBindingTrace"
    Write-ProofLine "TraceRawPendingRecord: $TraceRawPendingRecord"
    Write-ProofLine "TerrainProbePoints: $probePoints"
    Write-ProofLine "BootstrapHour: $BootstrapHour"
    Write-ProofLine ""

    & $FlatScript @flatArgs 2>&1 | Tee-Object -FilePath $HarnessLog | Out-Host
}
finally {
    Restore-ProofEnv $previousEnv
}

$OpenMwLog = Copy-IfPresent (Join-Path $ConfigDir "openmw.log") (Join-Path $ProofDir "openmw.log")
$MyGuiLog = Copy-IfPresent (Join-Path $ConfigDir "MyGUI.log") (Join-Path $ProofDir "MyGUI.log")
$StdoutLog = Copy-IfPresent (Join-Path $ConfigDir "openmw-process.stdout.log") (Join-Path $ProofDir "openmw-proof.stdout.log")
$StderrLog = Copy-IfPresent (Join-Path $ConfigDir "openmw-process.stderr.log") (Join-Path $ProofDir "openmw-proof.stderr.log")
Copy-IfPresent (Join-Path $ConfigDir "openmw.cfg") (Join-Path $ProofDir "openmw.cfg") | Out-Null
Copy-IfPresent (Join-Path $ConfigDir "settings.cfg") (Join-Path $ProofDir "settings.cfg") | Out-Null
if (Test-Path -LiteralPath $ScreenshotDir) {
    Get-ChildItem -LiteralPath $ScreenshotDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $ProofDir $_.Name) -Force
    }
}

if ([string]::IsNullOrWhiteSpace($OpenMwLog)) {
    throw "OpenMW log was not produced. Harness log: $HarnessLog"
}

$fatalCount = Count-LogMatches $OpenMwLog "Fatal error|Failed to start new game|unknown global|Failed to update LuaManager|Lua error|marker_error"
$terrainProbeLines = Count-LogMatches $OpenMwLog "Nikami FNV player terrain probe:"
$namedProbeLines = Count-LogMatches $OpenMwLog "Nikami FNV named terrain probe:"
$namedProbeMissLines = Count-LogMatches $OpenMwLog "Nikami FNV named terrain probe:.*(rayHit=0|misses=[1-9])"
$playerSupportMissLines = Count-LogMatches $OpenMwLog "Nikami FNV player terrain probe:.*supportSamples\(radius=.*misses=[1-9]"
$airborneLines = Count-LogMatches $OpenMwLog "FNV/ESM4 proof walk: summary .*airborneFrames=[1-9]"
$flatCameraSettledLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: settled flat startup camera"
$flatCameraFailureLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: flat startup camera did not attach"
$worldPostureBadLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: world posture .* verdict=BAD"
$worldPostureOkLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: world posture .* verdict=OK"
$standingArmPoseBadLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: standing arm pose .* verdict=BAD"
$standingArmPoseOkLines = Count-LogMatches $OpenMwLog "FNV/ESM4 diag: standing arm pose .* verdict=OK"
$actorProofPatterns = @(Get-ActorProofPatterns $OpenMwLog $ActorTarget)
$targetWorldPostureBadLines = Count-ActorPostureMatches $OpenMwLog $actorProofPatterns "world posture" "BAD"
$targetWorldPostureOkLines = Count-ActorPostureMatches $OpenMwLog $actorProofPatterns "world posture" "OK"
$targetStandingArmPoseBadLines = Count-ActorPostureMatches $OpenMwLog $actorProofPatterns "standing arm pose" "BAD"
$targetStandingArmPoseOkLines = Count-ActorPostureMatches $OpenMwLog $actorProofPatterns "standing arm pose" "OK"
$targetVisibleHandGeometry = Get-ActorVisibleHandGeometryProbe $OpenMwLog $actorProofPatterns $ActorVisibleHandMaxDistance
$unsupportedEsm4Skips = @(Get-UnsupportedEsm4Skips $OpenMwLog)
$screenshots = @(Get-ChildItem -LiteralPath $ProofDir -Filter "*.png" -File -ErrorAction SilentlyContinue)
$screenshotStability = [pscustomobject](Get-ScreenshotStabilityResult `
    $screenshots `
    $ScreenshotStabilityMaxMeanAbsDelta `
    $ScreenshotStabilityMaxHighDeltaRatio `
    $ScreenshotStabilityCropFraction)
$screenshotStabilityPath = Join-Path $ProofDir "screenshot-stability.json"
$screenshotStability | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $screenshotStabilityPath -Encoding UTF8
$screenshotTiming = [pscustomobject](Get-ScreenshotTimingResult `
    $screenshots `
    $OpenMwLog `
    $ActorTarget `
    $ScreenshotTimingMinPostCameraFrames `
    $ScreenshotTimingMaxActorCameraAgeFrames)
$screenshotTimingPath = Join-Path $ProofDir "screenshot-timing.json"
$screenshotTiming | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $screenshotTimingPath -Encoding UTF8

Write-ProofLine ""
Write-ProofLine "Artifacts:"
Write-ProofLine "OpenMW log: $OpenMwLog"
Write-ProofLine "MyGUI log: $MyGuiLog"
Write-ProofLine "Stdout log: $StdoutLog"
Write-ProofLine "Stderr log: $StderrLog"
Write-ProofLine "Screenshots: $($screenshots.Count)"
Write-ProofLine "Screenshot stability JSON: $screenshotStabilityPath"
Write-ProofLine "Screenshot timing JSON: $screenshotTimingPath"
Write-ProofLine ""
Write-ProofLine "Runtime probes:"
Write-ProofLine "Real fatal/blocker lines: $fatalCount"
Write-ProofLine "Player terrain probe lines: $terrainProbeLines"
Write-ProofLine "Named terrain probe lines: $namedProbeLines"
Write-ProofLine "Named terrain support miss lines: $namedProbeMissLines"
Write-ProofLine "Player terrain support miss lines: $playerSupportMissLines"
Write-ProofLine "Player terrain airborne lines: $airborneLines"
Write-ProofLine "Flat camera settled lines: $flatCameraSettledLines"
Write-ProofLine "Flat camera failure lines: $flatCameraFailureLines"
Write-ProofLine "World posture OK lines: $worldPostureOkLines"
Write-ProofLine "World posture BAD lines: $worldPostureBadLines"
Write-ProofLine "Standing arm pose OK lines: $standingArmPoseOkLines"
Write-ProofLine "Standing arm pose BAD lines: $standingArmPoseBadLines"
Write-ProofLine "Target actor posture patterns: $($actorProofPatterns -join ',')"
Write-ProofLine "Target world posture OK lines: $targetWorldPostureOkLines"
Write-ProofLine "Target world posture BAD lines: $targetWorldPostureBadLines"
Write-ProofLine "Target standing arm pose OK lines: $targetStandingArmPoseOkLines"
Write-ProofLine "Target standing arm pose BAD lines: $targetStandingArmPoseBadLines"
Write-ProofLine "Target visible hand geometry status: $($targetVisibleHandGeometry.Status)"
Write-ProofLine ("Target visible hand geometry samples: {0} leftBest={1} rightBest={2} maxDistance={3}" -f `
    $targetVisibleHandGeometry.SampleCount, `
    (Format-ProofNumber $targetVisibleHandGeometry.LeftBestDistance), `
    (Format-ProofNumber $targetVisibleHandGeometry.RightBestDistance), `
    (Format-ProofNumber $targetVisibleHandGeometry.MaxDistance))
if (![string]::IsNullOrWhiteSpace($targetVisibleHandGeometry.FailureLine)) {
    Write-ProofLine "Target visible hand geometry failure line: $($targetVisibleHandGeometry.FailureLine)"
}
Write-ProofLine "Screenshot stability status: $($screenshotStability.status)"
Write-ProofLine "Screenshot timing status: $($screenshotTiming.status)"
foreach ($pair in $screenshotStability.pairs) {
    Write-ProofLine ("Screenshot stability pair {0}->{1}: meanAbsDelta={2} highDeltaRatio={3} pass={4}" -f `
        $pair.previous, $pair.current, $pair.meanAbsDelta, $pair.highDeltaRatio, $pair.pass)
}
foreach ($timed in $screenshotTiming.screenshots) {
    Write-ProofLine ("Screenshot timing {0}: frame={1} cameraSettleAgeFrames={2} actorCameraFirstAgeFrames={3} actorCameraLastAgeFrames={4} pass={5} reason={6}" -f `
        $timed.screenshot, $timed.frame, $timed.cameraSettleAgeFrames, $timed.actorCameraFirstAgeFrames, `
        $timed.actorCameraLastAgeFrames, $timed.pass, $timed.reason)
}
Write-ProofLine "Unsupported ESM4 skip lines: $($unsupportedEsm4Skips.Count)"

if ($fatalCount -gt 0) { throw "FNV flat proof saw fatal/blocker log lines. See $OpenMwLog" }
Assert-NoShaderBlockers @($OpenMwLog, $MyGuiLog, $StdoutLog, $StderrLog, $HarnessLog)
if ($RequireSkyColorSanity) { Assert-SkyColorSanity $screenshots $ProofDir }
if ($RequireSkyPaletteMatch) { Assert-SkyPaletteMatch $screenshots $ProofDir $ProofRoot }
if ($RequireSunVisible) { Assert-SunVisible $screenshots $ProofDir }
if ($RequireSunDirectionRuntime) { Assert-SunDirectionRuntime $OpenMwLog $ProofDir }
Assert-UnsupportedEsm4SkipsClassified $unsupportedEsm4Skips $ClassificationDir
if ($RequireFlatCameraSettled -and $flatCameraSettledLines -eq 0) { throw "FNV flat proof did not prove flat camera settlement. See $OpenMwLog" }
if ($flatCameraFailureLines -gt 0) { throw "FNV flat proof saw flat camera failure lines. See $OpenMwLog" }
if ($RequireScreenshotStability -and $screenshotStability.status -ne "PASS") { throw "FNV flat proof did not prove screenshot stability. See $screenshotStabilityPath" }
if ($RequireScreenshotStability -and $screenshotTiming.status -ne "PASS") { throw "FNV flat proof did not prove post-settle screenshot timing. See $screenshotTimingPath" }
if (![string]::IsNullOrWhiteSpace($ActorTarget) -and $actorProofPatterns.Count -eq 0) { throw "FNV actor proof did not resolve target actor patterns for $ActorTarget. See $OpenMwLog" }
if (![string]::IsNullOrWhiteSpace($ActorTarget) -and $targetWorldPostureBadLines -gt 0) { throw "FNV actor proof saw target bad world posture lines. See $OpenMwLog" }
if (![string]::IsNullOrWhiteSpace($ActorTarget) -and $targetStandingArmPoseBadLines -gt 0) { throw "FNV actor proof saw target standing arm bind/T-pose lines. See $OpenMwLog" }
if (![string]::IsNullOrWhiteSpace($ActorTarget) -and $targetWorldPostureOkLines -eq 0) { throw "FNV actor proof did not log target world posture lines. See $OpenMwLog" }
if (![string]::IsNullOrWhiteSpace($ActorTarget) -and $targetStandingArmPoseOkLines -eq 0) { throw "FNV actor proof did not log target standing arm pose lines. See $OpenMwLog" }
if ($RequireActorVisibleHandGeometry -and [string]::IsNullOrWhiteSpace($ActorTarget)) { throw "FNV actor visible hand geometry proof requires ActorTarget." }
if ($RequireActorVisibleHandGeometry -and !$FnvPartMatrixAudit) { throw "FNV actor visible hand geometry proof requires FnvPartMatrixAudit." }
if ($RequireActorVisibleHandGeometry -and $targetVisibleHandGeometry.Status -ne "PASS") { throw "FNV actor proof did not prove visible skinned hand geometry follows animated hand anchors. See $OpenMwLog" }
if ($RequirePlayerTerrainSupport -and $playerSupportMissLines -gt 0) { throw "FNV flat proof saw player terrain support misses. See $OpenMwLog" }
if ($RequirePlayerTerrainSupport -and $airborneLines -gt 0) { throw "FNV flat proof saw walk airborne frames. See $OpenMwLog" }
if ($RequireTerrainProbeFullSupport -and $namedProbeLines -eq 0) { throw "FNV flat proof did not log named terrain probe lines. See $OpenMwLog" }
if ($RequireTerrainProbeFullSupport -and $namedProbeMissLines -gt 0) { throw "FNV flat proof saw named terrain probe misses. See $OpenMwLog" }

foreach ($pattern in $RequireLogPattern) {
    Assert-LogPattern $OpenMwLog $pattern
}

Write-ProofLine ""
Write-ProofLine "FNV flat runtime proof PASS"
Write-ProofLine "ProofDir: $ProofDir"
