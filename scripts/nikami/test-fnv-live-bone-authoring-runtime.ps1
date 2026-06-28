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
    [int]$RunSeconds = 60,
    [int]$WarmupSeconds = 18,
    [int]$TimeoutSeconds = 90,
    [string]$ScreenshotFrames = "1500,1800",
    [int]$ScreenshotTimeoutSeconds = 90,
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
    [string]$HandBone = "Bip01 R Hand",
    [string]$FingerBone = "Bip01 R Finger11",
    [double]$HandRotationZ = 25.0,
    [double]$FingerRotationZ = 15.0,
    [string]$NeutralActorPreviewProfile = "arms",
    [string]$FnvStaticizeRiggedHandParts = $env:OPENMW_FNV_STATICIZE_RIGGED_HAND_PARTS,
    [string]$FnvKeepRiggedHandParts = $env:OPENMW_FNV_KEEP_RIGGED_HAND_PARTS,
    [string]$PixelDiffBaseline = "",
    [double]$MinScreenshotMeanAbsDelta = 0.001,
    [int]$MinScreenshotChangedPixels = 1,
    [switch]$RequireScreenshotPixelDelta,
    [switch]$NoSound,
    [switch]$SkipVisualCapture,
    [switch]$KeepRuntime
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Add-Arg([System.Collections.Generic.List[string]]$List, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $List.Add($Name)
    $List.Add([string]$Value)
}

function Wait-Until([scriptblock]$Probe, [int]$TimeoutSec, [string]$Description) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $value = & $Probe
        if ($value) { return $value }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    throw "Timed out waiting for $Description"
}

function Find-RuntimeOpenMwLog([string]$Root, [string]$RuntimeTag) {
    $safeTag = $RuntimeTag -replace '[^A-Za-z0-9_.-]', '_'
    $liveConfigLog = Join-Path $Root "configs/fnv-flat-clean-$safeTag/openmw.log"
    if (Test-Path -LiteralPath $liveConfigLog -PathType Leaf) {
        return $liveConfigLog
    }

    $flatRoot = Join-Path $Root "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $flatRoot -PathType Container)) { return $null }
    $dirs = Get-ChildItem -LiteralPath $flatRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*$safeTag*" } |
        Sort-Object LastWriteTime -Descending
    foreach ($dir in $dirs) {
        $candidate = Join-Path $dir.FullName "openmw.log"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    return $null
}

function Find-RuntimeScreenshots([string]$Root, [string]$RuntimeTag) {
    $safeTag = $RuntimeTag -replace '[^A-Za-z0-9_.-]', '_'
    $screenshotDir = Join-Path $Root "runtime/fnv-flat-clean-$safeTag/screenshots"
    if (!(Test-Path -LiteralPath $screenshotDir -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $screenshotDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '^\.(png|jpg|jpeg|bmp|tga)$' } |
        Sort-Object LastWriteTime)
}

function Compare-ScreenshotPixels([string]$BaselinePath, [string]$CandidatePath) {
    if (!(Test-Path -LiteralPath $BaselinePath -PathType Leaf)) {
        throw "Missing screenshot pixel-diff baseline: $BaselinePath"
    }
    if (!(Test-Path -LiteralPath $CandidatePath -PathType Leaf)) {
        throw "Missing screenshot pixel-diff candidate: $CandidatePath"
    }

    Add-Type -AssemblyName System.Drawing
    function Open-BitmapWithRetry([string]$Path) {
        $lastError = $null
        for ($attempt = 0; $attempt -lt 20; ++$attempt) {
            try {
                return [System.Drawing.Bitmap]::FromFile($Path)
            }
            catch {
                $lastError = $_
                Start-Sleep -Milliseconds 250
            }
        }
        throw "Unable to open screenshot after retries: $Path ($($lastError.Exception.Message))"
    }

    $baseline = Open-BitmapWithRetry $BaselinePath
    $candidate = Open-BitmapWithRetry $CandidatePath
    try {
        if ($baseline.Width -ne $candidate.Width -or $baseline.Height -ne $candidate.Height) {
            throw "Screenshot dimensions differ: baseline=$($baseline.Width)x$($baseline.Height) candidate=$($candidate.Width)x$($candidate.Height)"
        }

        [double]$sumAbsDelta = 0.0
        [int]$changedPixels = 0
        for ($y = 0; $y -lt $baseline.Height; ++$y) {
            for ($x = 0; $x -lt $baseline.Width; ++$x) {
                $a = $baseline.GetPixel($x, $y)
                $b = $candidate.GetPixel($x, $y)
                $dr = [Math]::Abs([int]$a.R - [int]$b.R)
                $dg = [Math]::Abs([int]$a.G - [int]$b.G)
                $db = [Math]::Abs([int]$a.B - [int]$b.B)
                $sumAbsDelta += $dr + $dg + $db
                if (($dr + $dg + $db) -gt 0) {
                    ++$changedPixels
                }
            }
        }

        $channelSamples = [double]($baseline.Width * $baseline.Height * 3)
        return [pscustomobject][ordered]@{
            baseline = $BaselinePath
            candidate = $CandidatePath
            width = $baseline.Width
            height = $baseline.Height
            meanAbsDelta = $sumAbsDelta / $channelSamples
            changedPixels = $changedPixels
        }
    }
    finally {
        $candidate.Dispose()
        $baseline.Dispose()
    }
}

function Set-LiveControl([pscustomobject]$Doc, [string]$Name, [double]$Value) {
    $controls = $Doc.controls
    $existing = $controls.PSObject.Properties[$Name]
    if ($existing) {
        $existing.Value = $Value
    }
    else {
        $controls | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    }
}

function ConvertTo-FnvBoneControlPrefix([string]$BoneName) {
    $tokens = [regex]::Matches($BoneName, '[A-Za-z0-9]+') | ForEach-Object { $_.Value.ToUpperInvariant() }
    if (@($tokens).Count -eq 0) {
        throw "Unable to derive FNV live bone control prefix from empty/invalid bone name: '$BoneName'"
    }
    return "OPENMW_FNV_BONE_" + (@($tokens) -join "_")
}

function Stop-ProcessTree([int]$RootPid) {
    $children = Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq $RootPid }
    foreach ($child in $children) {
        Stop-ProcessTree ([int]$child.ProcessId)
    }
    $process = Get-Process -Id $RootPid -ErrorAction SilentlyContinue
    if ($process) {
        Stop-Process -Id $RootPid -Force
    }
}

function Resolve-FnvDataFromLatestHarvest([string]$ProofRootPath) {
    $harvestRoot = Join-Path $ProofRootPath "fnv-retail-harvest"
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

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Resolve-FnvDataFromLatestHarvest $ProofRoot
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runtimeTag = "live-bone-authoring-contract-$stamp"
$runDir = Join-Path $ProofRoot "fnv-live-bone-authoring-contract/$stamp"
$liveAuthoringFile = Join-Path $runDir "live-authoring.json"
$stdoutPath = Join-Path $runDir "flat-proof.stdout.log"
$stderrPath = Join-Path $runDir "flat-proof.stderr.log"
$manifestPath = Join-Path $runDir "runtime-contract.json"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

if ($RequireScreenshotPixelDelta -and [string]::IsNullOrWhiteSpace($PixelDiffBaseline)) {
    throw "-RequireScreenshotPixelDelta requires -PixelDiffBaseline"
}
if ($RequireScreenshotPixelDelta -and $SkipVisualCapture) {
    throw "-RequireScreenshotPixelDelta cannot be used with -SkipVisualCapture"
}

$handPrefix = ConvertTo-FnvBoneControlPrefix $HandBone
$fingerPrefix = ConvertTo-FnvBoneControlPrefix $FingerBone
$initialControls = [ordered]@{}
foreach ($prefix in @($handPrefix, $fingerPrefix)) {
    $initialControls["${prefix}_OFFSET_X"] = 0.0
    $initialControls["${prefix}_OFFSET_Y"] = 0.0
    $initialControls["${prefix}_OFFSET_Z"] = 0.0
    $initialControls["${prefix}_ROTATION_X"] = 0.0
    $initialControls["${prefix}_ROTATION_Y"] = 0.0
    $initialControls["${prefix}_ROTATION_Z"] = 0.0
}
[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-authoring-v1"
    schemaMarkers = @("runtime-live-authoring-v1", "bone-transform-controls-v1", "generated-control-file-only-v1")
    path = $liveAuthoringFile
    updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    controls = $initialControls
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        numericRuntimeControlsOnly = $true
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $liveAuthoringFile -Encoding UTF8

$flatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
if (!(Test-Path -LiteralPath $flatProof -PathType Leaf)) {
    throw "Missing flat proof runner: $flatProof"
}

$flatArgs = [System.Collections.Generic.List[string]]::new()
$flatArgs.Add("-NoProfile")
$flatArgs.Add("-ExecutionPolicy")
$flatArgs.Add("Bypass")
$flatArgs.Add("-File")
$flatArgs.Add($flatProof)
Add-Arg $flatArgs "-BuildDir" $BuildDir
Add-Arg $flatArgs "-Configuration" $Configuration
Add-Arg $flatArgs "-FnvData" $FnvData
Add-Arg $flatArgs "-FnvConfigData" $FnvConfigData
Add-Arg $flatArgs "-VcpkgRoot" $VcpkgRoot
Add-Arg $flatArgs "-ExtraOsgPluginDir" $ExtraOsgPluginDir
Add-Arg $flatArgs "-ProofRoot" $ProofRoot
Add-Arg $flatArgs "-FnvStaticizeRiggedHandParts" $FnvStaticizeRiggedHandParts
Add-Arg $flatArgs "-FnvKeepRiggedHandParts" $FnvKeepRiggedHandParts
Add-Arg $flatArgs "-RuntimeTag" $runtimeTag
Add-Arg $flatArgs "-RunSeconds" $RunSeconds
if (!$SkipVisualCapture) { Add-Arg $flatArgs "-ScreenshotFrames" $ScreenshotFrames }
Add-Arg $flatArgs "-BootstrapCell" $BootstrapCell
Add-Arg $flatArgs "-BootstrapX" $BootstrapX
Add-Arg $flatArgs "-BootstrapY" $BootstrapY
Add-Arg $flatArgs "-BootstrapZ" $BootstrapZ
Add-Arg $flatArgs "-BootstrapRotX" $BootstrapRotX
Add-Arg $flatArgs "-BootstrapRotY" $BootstrapRotY
Add-Arg $flatArgs "-BootstrapRotZ" $BootstrapRotZ
Add-Arg $flatArgs "-BootstrapHour" $BootstrapHour
Add-Arg $flatArgs "-ActorTarget" $ActorTarget
Add-Arg $flatArgs "-ActorKind" $ActorKind
$flatArgs.Add("-StageActor")
$flatArgs.Add("-NeutralActorPreview")
if ($ActorKind -ine "creature") {
    $flatArgs.Add("-NeutralActorPreviewStandingIdle")
    Add-Arg $flatArgs "-ActorKitAnimationSource" "hands-at-side"
}
Add-Arg $flatArgs "-NeutralActorPreviewProfile" $NeutralActorPreviewProfile
Add-Arg $flatArgs "-ActorStageX" $ActorStageX
Add-Arg $flatArgs "-ActorStageY" $ActorStageY
Add-Arg $flatArgs "-ActorStageZ" $ActorStageZ
Add-Arg $flatArgs "-ActorStageRotX" $ActorStageRotX
Add-Arg $flatArgs "-ActorStageRotY" $ActorStageRotY
Add-Arg $flatArgs "-ActorStageRotZ" $ActorStageRotZ
Add-Arg $flatArgs "-ActorViewOffsetZ" $ActorViewOffsetZ
Add-Arg $flatArgs "-ActorViewTargetZ" $ActorViewTargetZ
$flatArgs.Add("-ActorViewLocalOffset")
Add-Arg $flatArgs "-CharacterBuilderPhase" "t-pose"
Add-Arg $flatArgs "-LiveAuthoringFile" $liveAuthoringFile
if ($NoSound) { $flatArgs.Add("-NoSound") }

$runtimeCommand = "powershell " + (($flatArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) -join " ")
$process = Start-Process -FilePath "powershell" `
    -ArgumentList ($flatArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) `
    -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath

[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-bone-authoring-runtime-contract-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runtimeTag = $runtimeTag
    liveAuthoringFile = $liveAuthoringFile
    runtimeProcessId = $process.Id
    stdout = $stdoutPath
    stderr = $stderrPath
    runtimeCommand = $runtimeCommand
    actorTarget = $ActorTarget
    neutralActorPreviewProfile = $NeutralActorPreviewProfile
    handBone = $HandBone
    fingerBone = $FingerBone
    handPrefix = $handPrefix
    fingerPrefix = $fingerPrefix
    fnvStaticizeRiggedHandParts = $FnvStaticizeRiggedHandParts
    fnvKeepRiggedHandParts = $FnvKeepRiggedHandParts
    pixelDiffBaseline = $PixelDiffBaseline
    minScreenshotMeanAbsDelta = $MinScreenshotMeanAbsDelta
    minScreenshotChangedPixels = $MinScreenshotChangedPixels
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

try {
    $openMwLog = Wait-Until { Find-RuntimeOpenMwLog $ProofRoot $runtimeTag } $TimeoutSeconds "OpenMW log for $runtimeTag"
    Start-Sleep -Seconds $WarmupSeconds

    $doc = Get-Content -LiteralPath $liveAuthoringFile -Raw | ConvertFrom-Json
    Set-LiveControl $doc "${handPrefix}_ROTATION_Z" $HandRotationZ
    Set-LiveControl $doc "${fingerPrefix}_ROTATION_Z" $FingerRotationZ
    $doc.updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    $doc | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $liveAuthoringFile -Encoding UTF8

    $requiredPatterns = @(
        "FNV/ESM4 live authoring: bone-catalog-json",
        '"proofPreview":true',
        "FNV/ESM4 live authoring: frame-applied bone authoring actor=$ActorTarget",
        "bone=`"$HandBone`"",
        "prefix=$handPrefix",
        "rotation=(0,0,$([int]$HandRotationZ))",
        "bone=`"$FingerBone`"",
        "prefix=$fingerPrefix",
        "rotation=(0,0,$([int]$FingerRotationZ))",
        "gate=runtime-live-bone-authoring-mesh-consumers",
        "updatedSceneUtilBoneMatrices=1",
        "forcedRigGeometry=",
        "refreshedRigGeometry="
    )

    $null = Wait-Until {
        if (!(Test-Path -LiteralPath $openMwLog -PathType Leaf)) { return $false }
        $text = Get-Content -LiteralPath $openMwLog -Raw
        foreach ($pattern in $requiredPatterns) {
            if ($text -notlike "*$pattern*") { return $false }
        }
        return $true
    } $TimeoutSeconds "live bone authoring audit lines"

    $screenshots = @()
    if (!$SkipVisualCapture) {
        $queuedPatterns = @(
            "FNV/ESM4 proof: queuing native screenshot",
            "actorTarget=`"$ActorTarget`""
        )
        $null = Wait-Until {
            if (!(Test-Path -LiteralPath $openMwLog -PathType Leaf)) { return $false }
            $text = Get-Content -LiteralPath $openMwLog -Raw
            foreach ($pattern in $queuedPatterns) {
                if ($text -notlike "*$pattern*") { return $false }
            }
            return $true
        } $ScreenshotTimeoutSeconds "post-live-edit screenshot queue"

        $screenshots = @(Wait-Until {
            $items = @(Find-RuntimeScreenshots $ProofRoot $runtimeTag)
            if ($items.Count -gt 0) { return $items }
            return $false
        } $ScreenshotTimeoutSeconds "post-live-edit screenshot file")
    }

    $pixelDiff = $null
    if (![string]::IsNullOrWhiteSpace($PixelDiffBaseline)) {
        if ($screenshots.Count -eq 0) {
            throw "Pixel diff baseline was supplied, but no runtime screenshots were captured."
        }
        $pixelDiff = Compare-ScreenshotPixels $PixelDiffBaseline $screenshots[0].FullName
        if ($RequireScreenshotPixelDelta) {
            if ($pixelDiff.meanAbsDelta -lt $MinScreenshotMeanAbsDelta) {
                throw "Screenshot meanAbsDelta $($pixelDiff.meanAbsDelta) is below required minimum $MinScreenshotMeanAbsDelta"
            }
            if ($pixelDiff.changedPixels -lt $MinScreenshotChangedPixels) {
                throw "Screenshot changedPixels $($pixelDiff.changedPixels) is below required minimum $MinScreenshotChangedPixels"
            }
        }
    }

    if (!$KeepRuntime) {
        Stop-ProcessTree $process.Id
    }

    [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-bone-authoring-runtime-contract-result-v1"
        runtimeTag = $runtimeTag
        manifest = $manifestPath
        liveAuthoringFile = $liveAuthoringFile
        openMwLog = $openMwLog
        actorTarget = $ActorTarget
        handBone = $HandBone
        fingerBone = $FingerBone
        handPrefix = $handPrefix
        fingerPrefix = $fingerPrefix
        handRotationZ = $HandRotationZ
        fingerRotationZ = $FingerRotationZ
        screenshots = @($screenshots | ForEach-Object { $_.FullName })
        pixelDiff = $pixelDiff
        runtimeStopped = !$KeepRuntime
        verdict = "PASS"
    } | ConvertTo-Json -Depth 6
}
catch {
    if (!$KeepRuntime) {
        Stop-ProcessTree $process.Id
    }
    throw
}
