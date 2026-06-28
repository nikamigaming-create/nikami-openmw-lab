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
    [string]$HandBone = "Bip01 R Hand",
    [string[]]$FingerBones = @(
        "Bip01 R Thumb1",
        "Bip01 R Thumb11",
        "Bip01 R Thumb12",
        "Bip01 R Finger1",
        "Bip01 R Finger11",
        "Bip01 R Finger12",
        "Bip01 R Finger2",
        "Bip01 R Finger21",
        "Bip01 R Finger22",
        "Bip01 R Finger3",
        "Bip01 R Finger31",
        "Bip01 R Finger32",
        "Bip01 R Finger4",
        "Bip01 R Finger41",
        "Bip01 R Finger42"
    ),
    [double]$HandRotationZ = 25.0,
    [double]$FingerRotationZ = 15.0,
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
    [string]$FnvKeepRiggedHandParts = "1",
    [string]$WeightSelector = "all",
    [double]$MinScreenshotMeanAbsDelta = 0.001,
    [int]$MinScreenshotChangedPixels = 1,
    [bool]$RequireAnimatedMatrixDelta = $true,
    [double]$MinRiggedHandVertexDelta = 0.0001,
    [int]$MinLiveRigWeightVertices = 700,
    [bool]$RequireFabricNoTwist = $true,
    [switch]$NoSound,
    [switch]$ContinueOnFailure
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Add-Param([hashtable]$Params, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $Params[$Name] = $Value
}

function ConvertTo-SafeName([string]$Value) {
    return ($Value -replace '[^A-Za-z0-9_.-]', '_')
}

function ConvertTo-LogBoneName([string]$Value) {
    return ([string]$Value).ToLowerInvariant()
}

function Get-RiggedHandName([string]$BoneName) {
    if ($BoneName -match '(^|\s)L(\s|$)') {
        return "LeftHand:0"
    }
    return "RightHand:0"
}

function ConvertFrom-CloseupContractOutput([string]$Text) {
    $marker = '"nikami-fnv-live-bone-authoring-closeup-contract-result-v1"'
    $markerIndex = $Text.LastIndexOf($marker, [System.StringComparison]::Ordinal)
    if ($markerIndex -lt 0) {
        throw "Close-up contract output did not include the expected result schema marker."
    }
    $jsonStart = $Text.LastIndexOf("{", $markerIndex, [System.StringComparison]::Ordinal)
    if ($jsonStart -lt 0) {
        throw "Close-up contract output did not include a JSON object before the result schema marker."
    }
    $jsonText = $Text.Substring($jsonStart).Trim()
    return $jsonText | ConvertFrom-Json
}

function Test-LiveRigWeightDebug(
    [string]$LogText,
    [string]$RigName,
    [int]$MinWeightedVertices
) {
    $escapedRigName = [regex]::Escape($RigName)
    $matches = [regex]::Matches(
        $LogText,
        "live RigGeometry weight debug rig='$escapedRigName'.*weightedVertices=(?<vertices>[0-9]+)")
    foreach ($match in $matches) {
        if ([int]$match.Groups["vertices"].Value -ge $MinWeightedVertices) {
            return $true
        }
    }
    return $false
}

function Get-FabricNoTwistFailures([string]$LogText) {
    $failures = New-Object "System.Collections.Generic.List[string]"
    $matches = [regex]::Matches(
        $LogText,
        "FNV/ESM4 proof: Fallout RigGeometry '[^']+' fabric no-twist edge audit [^\r\n]* verdict=BAD[^\r\n]*gate=runtime-fnv-fabric-no-twist")
    foreach ($match in $matches) {
        $failures.Add($match.Value)
    }
    return @($failures.ToArray())
}

function Get-FabricNoTwistAuditLines([string]$LogText) {
    $lines = New-Object "System.Collections.Generic.List[string]"
    $matches = [regex]::Matches(
        $LogText,
        "FNV/ESM4 proof: Fallout RigGeometry '[^']+' fabric no-twist edge audit [^\r\n]*gate=runtime-fnv-fabric-no-twist")
    foreach ($match in $matches) {
        $lines.Add($match.Value)
    }
    return @($lines.ToArray())
}

function Get-LogTextAfterBoneAuthoring([string]$LogText, [string]$BoneName) {
    $appliedNeedle = "bone=`"$BoneName`""
    $appliedIndex = $LogText.IndexOf($appliedNeedle, [System.StringComparison]::Ordinal)
    if ($appliedIndex -lt 0) {
        return ""
    }
    return $LogText.Substring($appliedIndex)
}

function Test-RiggedHandConsumption(
    [string]$LogText,
    [string]$HandBone,
    [string]$FingerBone,
    [double]$MinVertexDelta,
    [int]$MinWeightedVertices
) {
    $appliedNeedle = "bone=`"$FingerBone`""
    $appliedIndex = $LogText.IndexOf($appliedNeedle, [System.StringComparison]::Ordinal)
    if ($appliedIndex -lt 0) {
        return $false
    }

    $riggedHandName = Get-RiggedHandName $HandBone
    if (!(Test-LiveRigWeightDebug $LogText $riggedHandName $MinWeightedVertices)) {
        return $false
    }

    $tail = $LogText.Substring($appliedIndex)
    $exactMatrixNeedle = "observed animated bone matrix delta"
    $exactBoneNeedle = "bone=$(ConvertTo-LogBoneName $FingerBone)"
    if ($tail -like "*$exactMatrixNeedle*" -and $tail -like "*$exactBoneNeedle*") {
        return $true
    }

    $matches = [regex]::Matches(
        $tail,
        "Fallout RigGeometry '$([regex]::Escape($riggedHandName))' skinned vertices this frame maxVertexSkinDelta=(?<delta>[-+0-9.eE]+)")
    foreach ($match in $matches) {
        if ([double]$match.Groups["delta"].Value -ge $MinVertexDelta) {
            return $true
        }
    }
    return $false
}

$closeupContract = Join-Path $PSScriptRoot "test-fnv-live-bone-authoring-closeup.ps1"
if (!(Test-Path -LiteralPath $closeupContract -PathType Leaf)) {
    throw "Missing close-up contract runner: $closeupContract"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $ProofRoot "fnv-live-finger-closeup-sweep\$stamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$flatFingerBones = New-Object "System.Collections.Generic.List[string]"
foreach ($boneValue in $FingerBones) {
    foreach ($bone in ([string]$boneValue -split ",")) {
        $trimmed = $bone.Trim()
        if (![string]::IsNullOrWhiteSpace($trimmed) -and !$flatFingerBones.Contains($trimmed)) {
            $flatFingerBones.Add($trimmed)
        }
    }
}
if ($flatFingerBones.Count -eq 0) {
    throw "No finger bones requested."
}

$results = New-Object "System.Collections.Generic.List[object]"
foreach ($fingerBone in $flatFingerBones) {
    $safeBone = ConvertTo-SafeName $fingerBone
    $outputLog = Join-Path $runDir "$safeBone-output.log"
    $params = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        ProofRoot = $ProofRoot
        ActorTarget = $ActorTarget
        ActorKind = $ActorKind
        NeutralActorPreviewProfile = $NeutralActorPreviewProfile
        HandBone = $HandBone
        FingerBone = $fingerBone
        HandRotationZ = $HandRotationZ
        FingerRotationZ = $FingerRotationZ
        BaselineScreenshotFrames = $BaselineScreenshotFrames
        LiveScreenshotFrames = $LiveScreenshotFrames
        BaselineRunSeconds = $BaselineRunSeconds
        LiveRunSeconds = $LiveRunSeconds
        LiveWarmupSeconds = $LiveWarmupSeconds
        TimeoutSeconds = $TimeoutSeconds
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
        BaselineAngles = $BaselineAngles
        FnvKeepRiggedHandParts = $FnvKeepRiggedHandParts
        WeightSelector = $WeightSelector
        MinScreenshotMeanAbsDelta = $MinScreenshotMeanAbsDelta
        MinScreenshotChangedPixels = $MinScreenshotChangedPixels
    }
    Add-Param $params "FnvData" $FnvData
    Add-Param $params "FnvConfigData" $FnvConfigData
    Add-Param $params "VcpkgRoot" $VcpkgRoot
    Add-Param $params "ExtraOsgPluginDir" $ExtraOsgPluginDir
    if ($NoSound) { $params["NoSound"] = $true }

    Write-Host "Running live close-up finger proof: $fingerBone"
    try {
        $output = & $closeupContract @params 2>&1
        $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        $text | Set-Content -LiteralPath $outputLog -Encoding UTF8
        $result = ConvertFrom-CloseupContractOutput $text
        if ($RequireAnimatedMatrixDelta) {
            $logPath = [string]$result.liveOpenMwLog
            if ([string]::IsNullOrWhiteSpace($logPath) -or !(Test-Path -LiteralPath $logPath -PathType Leaf)) {
                throw "Close-up contract result did not expose a readable live OpenMW log for $fingerBone."
            }
            $logText = Get-Content -LiteralPath $logPath -Raw
            $postAuthoringLogText = Get-LogTextAfterBoneAuthoring $logText $fingerBone
            $fabricAuditLines = @(Get-FabricNoTwistAuditLines $postAuthoringLogText)
            if ($RequireFabricNoTwist -and $fabricAuditLines.Count -eq 0) {
                throw "Live proof did not record post-authoring fabric no-twist audit lines while moving $fingerBone in $logPath."
            }
            $fabricFailures = @(Get-FabricNoTwistFailures $postAuthoringLogText)
            if ($RequireFabricNoTwist -and $fabricFailures.Count -gt 0) {
                throw "Live proof recorded post-authoring fabric no-twist BAD lines while moving $fingerBone. First failure: $($fabricFailures[0])"
            }
            if (!(Test-RiggedHandConsumption $logText $HandBone $fingerBone $MinRiggedHandVertexDelta $MinLiveRigWeightVertices)) {
                $riggedHandName = Get-RiggedHandName $HandBone
                throw "Live proof did not record selected-bone application, live $riggedHandName weight debug with at least $MinLiveRigWeightVertices weighted vertices, and rigged hand vertex/matrix movement for $fingerBone in $logPath."
            }
        }
        $result | Add-Member -NotePropertyName outputLog -NotePropertyValue $outputLog
        $result | Add-Member -NotePropertyName status -NotePropertyValue "PASS"
        $result | Add-Member -NotePropertyName riggedHandConsumptionAudited -NotePropertyValue ([bool]$RequireAnimatedMatrixDelta)
        $result | Add-Member -NotePropertyName liveRigWeightDebugAudited -NotePropertyValue ([bool]$RequireAnimatedMatrixDelta)
        $result | Add-Member -NotePropertyName fabricNoTwistAudited -NotePropertyValue ([bool]$RequireFabricNoTwist)
        $result | Add-Member -NotePropertyName fabricNoTwistAuditLines -NotePropertyValue ([int]$fabricAuditLines.Count)
        $results.Add($result)
    }
    catch {
        $message = $_.Exception.Message
        $errorLog = Join-Path $runDir "$safeBone-error.log"
        $message | Set-Content -LiteralPath $errorLog -Encoding UTF8
        $failure = [pscustomobject][ordered]@{
            schema = "nikami-fnv-live-finger-closeup-sweep-failure-v1"
            actorTarget = $ActorTarget
            handBone = $HandBone
            fingerBone = $fingerBone
            outputLog = $outputLog
            errorLog = $errorLog
            status = "FAIL"
            error = $message
        }
        $results.Add($failure)
        if (!$ContinueOnFailure) {
            break
        }
    }
}

$failures = @($results | Where-Object { $_.status -ne "PASS" })
$passCount = [int]($results.Count - $failures.Count)
$requestedFingerBoneArray = @($flatFingerBones.ToArray())
$resultArray = @($results.ToArray())
$doc = [pscustomobject][ordered]@{
    schema = "nikami-fnv-live-finger-closeup-sweep-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $runDir
    actorTarget = $ActorTarget
    actorKind = $ActorKind
    neutralActorPreviewProfile = $NeutralActorPreviewProfile
    handBone = $HandBone
    requestedFingerBones = $requestedFingerBoneArray
    requireRiggedHandConsumption = $RequireAnimatedMatrixDelta
    minRiggedHandVertexDelta = $MinRiggedHandVertexDelta
    minLiveRigWeightVertices = $MinLiveRigWeightVertices
    requireFabricNoTwist = $RequireFabricNoTwist
    passCount = $passCount
    failCount = [int]$failures.Count
    payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
    results = $resultArray
}

$jsonPath = Join-Path $runDir "live-finger-closeup-sweep.json"
$doc | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

$summaryPath = Join-Path $runDir "summary.md"
$summary = New-Object "System.Collections.Generic.List[string]"
$summary.Add("# FNV Live Finger Close-Up Sweep")
$summary.Add("")
$summary.Add("Actor: ``$ActorTarget``")
$summary.Add("Profile: ``$NeutralActorPreviewProfile``")
$summary.Add("Hand bone: ``$HandBone``")
$summary.Add("Baseline angles: ``$($BaselineAngles -join ',')``")
$summary.Add("Rigged hand consumption required: ``$RequireAnimatedMatrixDelta``")
$summary.Add("Minimum rigged hand vertex delta: ``$MinRiggedHandVertexDelta``")
$summary.Add("")
$summary.Add("| Finger bone | Status | Mean delta | Changed pixels | Live screenshot |")
$summary.Add("| --- | --- | ---: | ---: | --- |")
foreach ($result in $results) {
    $mean = ""
    $changed = ""
    $screenshot = ""
    if ($null -ne $result.PSObject.Properties["pixelDiff"] -and $null -ne $result.pixelDiff) {
        $mean = [string]$result.pixelDiff.meanAbsDelta
        $changed = [string]$result.pixelDiff.changedPixels
    }
    if ($null -ne $result.PSObject.Properties["liveScreenshot"] -and $null -ne $result.liveScreenshot) {
        $screenshot = [string]$result.liveScreenshot
    }
    $summary.Add("| $($result.fingerBone) | $($result.status) | $mean | $changed | $screenshot |")
}
$summary.Add("")
$summary.Add("Sweep JSON: ``$jsonPath``")
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host "FNV live finger close-up sweep: $runDir"
Write-Host "Sweep JSON: $jsonPath"
Write-Host "Summary: $summaryPath"

if ($failures.Count -gt 0) {
    throw "FNV live finger close-up sweep failed $($failures.Count) of $($results.Count) case(s). See $jsonPath"
}
