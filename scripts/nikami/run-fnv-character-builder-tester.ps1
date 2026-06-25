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
    [string[]]$Phases = @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"),
    [string[]]$ActorKitParts = @(),
    [string[]]$ActorKitPartModels = @(),
    [string[]]$ActorKitPropSlots = @(),
    [string[]]$ActorKitPropModels = @(),
    [string]$ActorKitAnimationSource = "",
    [double]$ActorKitAnimationStartPoint = [double]::NaN,
    [string]$ActorKitAnimationGroup = "",
    [string]$ActorKitDialogueMode = "",
    [string[]]$Angles = @("front", "front-left", "front-right"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "600,660,720,780,840",
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
    [double]$ActorViewDistance = 52,
    [double]$ActorViewOffsetZ = 108,
    [double]$ActorViewTargetZ = 108,
    [string]$NeutralActorPreviewProfile = "audit",
    [string]$FnvRotationMode = "bindCoreBindLowerRawUpper",
    [switch]$AllowMissingActorVisibleHandGeometry,
    [double]$ActorVisibleHandMaxDistance = 30.0,
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,HeadOld,HeadHuman",
    [string]$FnvHairEmissionStrength = "",
    [string]$LiveAuthoringFile = $env:OPENMW_FNV_LIVE_AUTHORING_FILE,
    [switch]$FnvUseNativeAnimationCallbacks,
    [switch]$CreatureDiagnostics,
    [switch]$NoSound,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Resolve-FnvDataFromLatestHarvest([string]$ProofRoot) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (!(Test-Path -LiteralPath $harvestRoot -PathType Container)) { return $null }
    $manifests = Get-ChildItem -LiteralPath $harvestRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "manifest.json" } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    foreach ($manifestPath in $manifests) {
        try {
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            $candidate = [string]$manifest.fnvData
            if (![string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Container)) {
                return [pscustomobject][ordered]@{
                    FnvData = (Resolve-Path -LiteralPath $candidate).Path
                    Manifest = $manifestPath
                }
            }
        }
        catch {
        }
    }
    return $null
}

function Resolve-VcpkgRootFromKnownPaths([string]$RepoRoot) {
    $candidates = @(
        $env:NIKAMI_VCPKG_ROOT,
        "D:\code\c\FMODS\vcpkg",
        (Join-Path $RepoRoot "vcpkg"),
        (Join-Path (Split-Path $RepoRoot -Parent) "vcpkg")
    )
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $toolchain = Join-Path $candidate "scripts/buildsystems/vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ""
}

$FnvDataProvenance = ""
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $harvestData = Resolve-FnvDataFromLatestHarvest $ProofRoot
    if ($null -ne $harvestData) {
        $FnvData = $harvestData.FnvData
        $FnvDataProvenance = $harvestData.Manifest
    }
}
$VcpkgRootProvenance = ""
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $detectedVcpkg = Resolve-VcpkgRootFromKnownPaths $RepoRoot
    if (![string]::IsNullOrWhiteSpace($detectedVcpkg)) {
        $VcpkgRoot = $detectedVcpkg
        $VcpkgRootProvenance = "verified local vcpkg toolchain"
    }
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$ReportScript = Join-Path $PSScriptRoot "fnv_character_builder_report.py"
if (!(Test-Path -LiteralPath $FlatProof)) { throw "Missing flat proof runner: $FlatProof" }
if (!(Test-Path -LiteralPath $ReportScript)) { throw "Missing character builder report parser: $ReportScript" }

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$SuiteDir = Join-Path $ProofRoot "fnv-character-builder/$Stamp"
$SummaryFile = Join-Path $SuiteDir "summary.txt"
New-Item -ItemType Directory -Force -Path $SuiteDir | Out-Null

function Write-SuiteLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Format-Double([double]$Value) {
    if ([double]::IsNaN($Value)) { return "" }
    return $Value.ToString("0.######", [Globalization.CultureInfo]::InvariantCulture)
}

function Get-ProofDirectories {
    $base = Join-Path $ProofRoot "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $base)) { return @() }
    return @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}

function Get-NewProofDirectory([string[]]$Before, [datetime]$StartedAt) {
    $base = Join-Path $ProofRoot "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $base)) { return $null }

    $beforeSet = New-Object "System.Collections.Generic.HashSet[string]"
    foreach ($path in $Before) {
        if (![string]::IsNullOrWhiteSpace($path)) { $null = $beforeSet.Add($path) }
    }

    $candidate = Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Where-Object { !$beforeSet.Contains($_.FullName) -and $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $candidate) { return $null }
    return $candidate.FullName
}

function Copy-IfPresent([string]$Source, [string]$Destination) {
    if (![string]::IsNullOrWhiteSpace($Source) -and (Test-Path -LiteralPath $Source)) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        return $Destination
    }
    return $null
}

function ConvertTo-SafeName([string]$Text) {
    $safe = $Text -replace '[^A-Za-z0-9_.-]', '_'
    if ([string]::IsNullOrWhiteSpace($safe)) { return "item" }
    return $safe
}

function Join-OptionalSelectorList([string[]]$Values) {
    $items = New-Object "System.Collections.Generic.List[string]"
    foreach ($value in $Values) {
        foreach ($part in ($value -split ",")) {
            $trimmed = $part.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed)) {
                $items.Add($trimmed)
            }
        }
    }
    return ($items -join ",")
}

function Get-RegexValue([string]$Text, [string]$Pattern) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $match = [regex]::Match($Text, $Pattern)
    if (!$match.Success -or $match.Groups.Count -lt 2) { return $null }
    return $match.Groups[1].Value
}

function Get-FnvRuntimeEvidence([string]$ProofDir, [string]$FnvSkinningMatrixAudit) {
    $summaryPath = Join-Path $ProofDir "summary.txt"
    $openMwLog = Join-Path $ProofDir "openmw.log"
    $summaryText = if (Test-Path -LiteralPath $summaryPath -PathType Leaf) {
        Get-Content -LiteralPath $summaryPath -Raw
    } else {
        ""
    }

    $skinningModes = New-Object "System.Collections.Generic.List[object]"
    $seenSkinning = New-Object "System.Collections.Generic.HashSet[string]"
    if (Test-Path -LiteralPath $openMwLog -PathType Leaf) {
        Select-String -LiteralPath $openMwLog -Pattern "FNV/ESM4 diag: Fallout RigGeometry '.*' skinning mode delta" -ErrorAction SilentlyContinue |
            Select-Object -First 80 |
            ForEach-Object {
                $line = $_.Line
                if ($line -match "Fallout RigGeometry '([^']+)'.* selected=([^ ]+) inventoryPaperDoll=([^ ]+) sourceFallback=([^ ]+) hasSkinToSkel=([^ ]+) useSkinToSkel=([^ ]+)") {
                    $key = "$($Matches[1])|$($Matches[2])|$($Matches[3])"
                    if ($seenSkinning.Add($key)) {
                        $null = $skinningModes.Add([pscustomobject][ordered]@{
                            drawable = $Matches[1]
                            selected = $Matches[2]
                            inventoryPaperDoll = $Matches[3]
                            sourceFallback = $Matches[4]
                            hasSkinToSkel = $Matches[5]
                            useSkinToSkel = $Matches[6]
                            line = $line
                        })
                    }
                }
            }
    }

    $requireActorVisibleHandGeometry = Get-RegexValue $summaryText "RequireActorVisibleHandGeometry:\s+([^\r\n]+)"
    $actorVisibleHandMaxDistance = Get-RegexValue $summaryText "ActorVisibleHandMaxDistance:\s+([^\r\n]+)"
    $fnvHairEmissionStrength = Get-RegexValue $summaryText "FnvHairEmissionStrength:\s*([^\r\n]*)"
    $visibleHandGeometryStatus = Get-RegexValue $summaryText "Target visible hand geometry status:\s+([^\r\n]+)"
    $visibleHandGeometrySamples = Get-RegexValue $summaryText "Target visible hand geometry samples:\s+([^\r\n]+)"
    $visibleHandGeometryPoseSanityBadLines = Get-RegexValue $summaryText "Target visible hand geometry pose sanity BAD lines:\s+([^\r\n]+)"
    $visibleHandGeometryPoseSanityFailureLine = Get-RegexValue $summaryText "Target visible hand geometry pose sanity failure line:\s+([^\r\n]+)"
    $visibleHandGeometryFailureLine = Get-RegexValue $summaryText "Target visible hand geometry failure line:\s+([^\r\n]+)"

    return [pscustomobject][ordered]@{
        requireActorVisibleHandGeometry = $requireActorVisibleHandGeometry
        actorVisibleHandMaxDistance = $actorVisibleHandMaxDistance
        visibleHandGeometry = [pscustomobject][ordered]@{
            status = $visibleHandGeometryStatus
            samples = $visibleHandGeometrySamples
            poseSanityBadLines = $visibleHandGeometryPoseSanityBadLines
            poseSanityFailureLine = $visibleHandGeometryPoseSanityFailureLine
            failureLine = $visibleHandGeometryFailureLine
        }
        fnvSkinningMatrixAudit = $FnvSkinningMatrixAudit
        fnvHairEmissionStrength = $fnvHairEmissionStrength
        skinningModes = @($skinningModes.ToArray())
    }
}

$diagonal = $ActorViewDistance * 0.7071067811865476
$AllAngles = @(
    [pscustomobject]@{ Name = "front"; OffsetX = $ActorViewDistance; OffsetY = 0.0 },
    [pscustomobject]@{ Name = "front-left"; OffsetX = $diagonal; OffsetY = -$diagonal },
    [pscustomobject]@{ Name = "front-right"; OffsetX = $diagonal; OffsetY = $diagonal }
)

$RequestedAngles = New-Object "System.Collections.Generic.List[string]"
foreach ($angleValue in $Angles) {
    foreach ($anglePart in ($angleValue -split ",")) {
        $trimmed = $anglePart.Trim()
        if (![string]::IsNullOrWhiteSpace($trimmed)) {
            $RequestedAngles.Add($trimmed)
        }
    }
}
if ($RequestedAngles.Count -eq 0) {
    throw "No character builder camera angles selected."
}
$KnownAngleNames = @($AllAngles | ForEach-Object { $_.Name })
$unknownAngles = @($RequestedAngles | Where-Object { $KnownAngleNames -notcontains $_ })
if ($unknownAngles.Count -gt 0) {
    throw "Unknown character builder camera angle(s): $($unknownAngles -join ','). Valid angles: $($KnownAngleNames -join ',')"
}
$CameraAngles = @($AllAngles | Where-Object { $RequestedAngles -contains $_.Name })

if ($ActorKind -ieq "creature" -and !$PSBoundParameters.ContainsKey("Phases")) {
    $Phases = @("creature-model", "creature-body", "creature-animation", "creature-full")
}

$NormalizedPhases = New-Object "System.Collections.Generic.List[string]"
foreach ($phaseValue in $Phases) {
    foreach ($phasePart in ($phaseValue -split ",")) {
        $trimmed = $phasePart.Trim()
        if (![string]::IsNullOrWhiteSpace($trimmed)) {
            $NormalizedPhases.Add($trimmed)
        }
    }
}
if ($NormalizedPhases.Count -eq 0) {
    throw "No character builder phases selected."
}
$Phases = @($NormalizedPhases)
$ActorKitPartsCsv = Join-OptionalSelectorList $ActorKitParts
$ActorKitPartModelsCsv = Join-OptionalSelectorList $ActorKitPartModels
$ActorKitPropSlotsCsv = Join-OptionalSelectorList $ActorKitPropSlots
$ActorKitPropModelsCsv = Join-OptionalSelectorList $ActorKitPropModels

Write-SuiteLine "FNV character builder tester $Stamp"
Write-SuiteLine "RepoRoot: $RepoRoot"
Write-SuiteLine "SuiteDir: $SuiteDir"
Write-SuiteLine "ActorTarget: $ActorTarget"
Write-SuiteLine "FnvData: $FnvData"
if (![string]::IsNullOrWhiteSpace($FnvDataProvenance)) {
    Write-SuiteLine "FnvDataProvenance: latest generated harvest manifest $FnvDataProvenance"
}
Write-SuiteLine "VcpkgRoot: $VcpkgRoot"
if (![string]::IsNullOrWhiteSpace($VcpkgRootProvenance)) {
    Write-SuiteLine "VcpkgRootProvenance: $VcpkgRootProvenance"
}
Write-SuiteLine "ActorKind: $ActorKind"
Write-SuiteLine "CreatureDiagnostics: $($CreatureDiagnostics -or $ActorKind -ieq 'creature')"
Write-SuiteLine "Phases: $($Phases -join ',')"
Write-SuiteLine "ActorKitParts: $ActorKitPartsCsv"
Write-SuiteLine "ActorKitPartModels: $ActorKitPartModelsCsv"
Write-SuiteLine "ActorKitPropSlots: $ActorKitPropSlotsCsv"
Write-SuiteLine "ActorKitPropModels: $ActorKitPropModelsCsv"
Write-SuiteLine "ActorKitAnimationSource: $ActorKitAnimationSource"
Write-SuiteLine "ActorKitAnimationStartPoint: $(Format-Double $ActorKitAnimationStartPoint)"
Write-SuiteLine "ActorKitAnimationGroup: $ActorKitAnimationGroup"
Write-SuiteLine "ActorKitDialogueMode: $ActorKitDialogueMode"
Write-SuiteLine "FnvRotationMode: $FnvRotationMode"
Write-SuiteLine "AllowMissingActorVisibleHandGeometry: $AllowMissingActorVisibleHandGeometry"
Write-SuiteLine "ActorVisibleHandMaxDistance: $ActorVisibleHandMaxDistance"
Write-SuiteLine "FnvSkinningMatrixAudit: $FnvSkinningMatrixAudit"
Write-SuiteLine "FnvHairEmissionStrength: $FnvHairEmissionStrength"
Write-SuiteLine "LiveAuthoringFile: $LiveAuthoringFile"
Write-SuiteLine "FnvUseNativeAnimationCallbacks: $FnvUseNativeAnimationCallbacks"
Write-SuiteLine "Angles: $(@($CameraAngles | ForEach-Object { $_.Name }) -join ',')"
Write-SuiteLine "BootstrapCell: $BootstrapCell"
Write-SuiteLine "BootstrapPosition: $BootstrapX,$BootstrapY,$BootstrapZ"
Write-SuiteLine "BootstrapRotation: $BootstrapRotX,$BootstrapRotY,$BootstrapRotZ"
Write-SuiteLine "ActorStagePosition: $ActorStageX,$ActorStageY,$ActorStageZ"
Write-SuiteLine "ActorStageRotation: $ActorStageRotX,$ActorStageRotY,$ActorStageRotZ"
Write-SuiteLine "NeutralActorPreviewProfile: $NeutralActorPreviewProfile"
Write-SuiteLine "Policy: no retail assets copied into repo; generated proof output only"
Write-SuiteLine ""

$ActorKitAnimationStartPointValue = if (![double]::IsNaN($ActorKitAnimationStartPoint)) { $ActorKitAnimationStartPoint } else { $null }

$Results = New-Object "System.Collections.Generic.List[object]"

foreach ($phase in $Phases) {
    foreach ($angle in $CameraAngles) {
        $safePhase = ConvertTo-SafeName $phase
        $safeAngle = ConvertTo-SafeName $angle.Name
        $caseName = "${safePhase}_${safeAngle}"
        $runtimeTag = "character-builder-$Stamp-$caseName"
        $CaseDir = Join-Path $SuiteDir $caseName
        New-Item -ItemType Directory -Force -Path $CaseDir | Out-Null

        Write-SuiteLine "CASE $caseName runtimeTag=$runtimeTag"
        $before = @(Get-ProofDirectories)
        $startedAt = Get-Date
        $runtimeGateStatus = "PASS"
        $runtimeGateError = ""

        $proofArgs = @{
            BuildDir = $BuildDir
            Configuration = $Configuration
            FnvData = $FnvData
            VcpkgRoot = $VcpkgRoot
            Triplet = $Triplet
            ProofRoot = $ProofRoot
            RuntimeTag = $runtimeTag
            RunSeconds = $RunSeconds
            ScreenshotFrames = $ScreenshotFrames
            BootstrapCell = $BootstrapCell
            BootstrapX = $BootstrapX
            BootstrapY = $BootstrapY
            BootstrapZ = $BootstrapZ
            BootstrapRotX = $BootstrapRotX
            BootstrapRotY = $BootstrapRotY
            BootstrapRotZ = $BootstrapRotZ
            BootstrapHour = $BootstrapHour
            ActorTarget = $ActorTarget
            ActorKind = $ActorKind
            StageActor = $true
            ActorFrame = $ActorFrame
            NeutralActorPreview = $($ActorKind -ine "creature")
            NeutralActorPreviewStandingIdle = $($ActorKind -ine "creature")
            NeutralActorPreviewProfile = $NeutralActorPreviewProfile
            ActorStageX = $ActorStageX
            ActorStageY = $ActorStageY
            ActorStageZ = $ActorStageZ
            ActorStageRotX = $ActorStageRotX
            ActorStageRotY = $ActorStageRotY
            ActorStageRotZ = $ActorStageRotZ
            ActorViewOffsetX = [double]$angle.OffsetX
            ActorViewOffsetY = [double]$angle.OffsetY
            ActorViewOffsetZ = $ActorViewOffsetZ
            ActorViewTargetZ = $ActorViewTargetZ
            ActorViewLocalOffset = $true
            FnvPartMatrixAudit = $true
            FnvSkinningMatrixAudit = $FnvSkinningMatrixAudit
            FnvHairEmissionStrength = $FnvHairEmissionStrength
            LiveAuthoringFile = $LiveAuthoringFile
            FnvRotationMode = $FnvRotationMode
            CharacterBuilderPhase = $phase
        }
        $requireVisibleHandGeometry = !$AllowMissingActorVisibleHandGeometry -and $ActorKind -ine "creature"
        if ($requireVisibleHandGeometry) {
            $proofArgs.RequireActorVisibleHandGeometry = $true
            $proofArgs.ActorVisibleHandMaxDistance = $ActorVisibleHandMaxDistance
        }
        if (![string]::IsNullOrWhiteSpace($ActorKitPartsCsv)) { $proofArgs.ActorKitParts = $ActorKitPartsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPartModelsCsv)) { $proofArgs.ActorKitPartModels = $ActorKitPartModelsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPropSlotsCsv)) { $proofArgs.ActorKitPropSlots = $ActorKitPropSlotsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPropModelsCsv)) { $proofArgs.ActorKitPropModels = $ActorKitPropModelsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitAnimationSource)) { $proofArgs.ActorKitAnimationSource = $ActorKitAnimationSource }
        if (![double]::IsNaN($ActorKitAnimationStartPoint)) { $proofArgs.ActorKitAnimationStartPoint = $ActorKitAnimationStartPoint }
        if (![string]::IsNullOrWhiteSpace($ActorKitAnimationGroup)) { $proofArgs.ActorKitAnimationGroup = $ActorKitAnimationGroup }
        if (![string]::IsNullOrWhiteSpace($ActorKitDialogueMode)) { $proofArgs.ActorKitDialogueMode = $ActorKitDialogueMode }
        if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
        if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
        if (!$FnvUseNativeAnimationCallbacks) { $proofArgs.FnvDisableNativeAnimationCallbacks = $true }
        if ($CreatureDiagnostics -or $ActorKind -ieq "creature") { $proofArgs.CreatureDiagnostics = $true }
        if ($phase -ieq "talk" -or $phase -ieq "dialogue") { $proofArgs.CharacterBuilderTalk = $true }
        if ($NoSound) { $proofArgs.NoSound = $true }

        try {
            & $FlatProof @proofArgs | Out-Host
        }
        catch {
            $runtimeGateStatus = "FAIL"
            $runtimeGateError = $_.Exception.Message
            Write-Warning "Character builder runtime gate failed for ${caseName}: $runtimeGateError"
        }

        $ProofDir = Get-NewProofDirectory $before $startedAt
        if ([string]::IsNullOrWhiteSpace($ProofDir)) {
            throw "Unable to find generated flat proof directory for $caseName"
        }

        Copy-IfPresent (Join-Path $ProofDir "openmw.log") (Join-Path $CaseDir "openmw.log") | Out-Null
        Copy-IfPresent (Join-Path $ProofDir "summary.txt") (Join-Path $CaseDir "flat-summary.txt") | Out-Null
        Copy-IfPresent (Join-Path $ProofDir "screenshot-timing.json") (Join-Path $CaseDir "screenshot-timing.json") | Out-Null
        Copy-IfPresent (Join-Path $ProofDir "screenshot-stability.json") (Join-Path $CaseDir "screenshot-stability.json") | Out-Null
        Copy-IfPresent (Join-Path $ProofDir "fnv-data-provenance.json") (Join-Path $CaseDir "fnv-data-provenance.json") | Out-Null
        Get-ChildItem -LiteralPath $ProofDir -Filter "*.png" -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $CaseDir $_.Name) -Force
        }

        $reportJson = Join-Path $CaseDir "character-builder-report.json"
        $reportMd = Join-Path $CaseDir "character-builder-report.md"
        & python $ReportScript --proof-dir $ProofDir --actor $ActorTarget --actor-kind $ActorKind --phase $phase --out-json $reportJson --out-md $reportMd | Out-Host
        $reportExit = $LASTEXITCODE
        $reportStatus = if ($reportExit -eq 0) { "PASS" } else { "FAIL" }
        $reportData = $null
        if (Test-Path -LiteralPath $reportJson) {
            $reportData = Get-Content -LiteralPath $reportJson -Raw | ConvertFrom-Json
        }
        $runtimeEvidence = Get-FnvRuntimeEvidence $ProofDir $FnvSkinningMatrixAudit

        $result = [pscustomobject][ordered]@{
            case = $caseName
            phase = $phase
            angle = $angle.Name
            runtimeGateStatus = $runtimeGateStatus
            runtimeGateError = $runtimeGateError
            reportStatus = $reportStatus
            proofDir = $ProofDir
            caseDir = $CaseDir
            bootstrap = [pscustomobject][ordered]@{
                cell = $BootstrapCell
                x = $BootstrapX
                y = $BootstrapY
                z = $BootstrapZ
                rotX = $BootstrapRotX
                rotY = $BootstrapRotY
                rotZ = $BootstrapRotZ
                hour = $BootstrapHour
            }
            actorStage = [pscustomobject][ordered]@{
                x = $ActorStageX
                y = $ActorStageY
                z = $ActorStageZ
                rotX = $ActorStageRotX
                rotY = $ActorStageRotY
                rotZ = $ActorStageRotZ
            }
            actorCamera = [pscustomobject][ordered]@{
                angle = $angle.Name
                neutralPreviewProfile = $NeutralActorPreviewProfile
                offsetX = [double]$angle.OffsetX
                offsetY = [double]$angle.OffsetY
                offsetZ = $ActorViewOffsetZ
                targetZ = $ActorViewTargetZ
                localOffset = $true
            }
            actorKitSelection = [pscustomobject][ordered]@{
                parts = $ActorKitPartsCsv
                partModels = $ActorKitPartModelsCsv
                propSlots = $ActorKitPropSlotsCsv
                propModels = $ActorKitPropModelsCsv
                animationSource = $ActorKitAnimationSource
                animationStartPoint = $ActorKitAnimationStartPointValue
                animationGroup = $ActorKitAnimationGroup
                dialogueMode = $ActorKitDialogueMode
                neutralPreviewProfile = $NeutralActorPreviewProfile
                fnvRotationMode = $FnvRotationMode
                requireVisibleHandGeometry = [bool]$requireVisibleHandGeometry
                actorVisibleHandMaxDistance = $ActorVisibleHandMaxDistance
                fnvSkinningMatrixAudit = $FnvSkinningMatrixAudit
                fnvHairEmissionStrength = $FnvHairEmissionStrength
                fnvUseNativeAnimationCallbacks = [bool]$FnvUseNativeAnimationCallbacks
            }
            runtimeEvidence = $runtimeEvidence
            failures = if ($null -ne $reportData) { @($reportData.failures) } else { @("report parser did not produce JSON") }
            screenshots = if ($null -ne $reportData) { @($reportData.screenshots) } else { @() }
        }
        $Results.Add($result)

        Write-SuiteLine ("  runtime={0} report={1} proof={2}" -f $runtimeGateStatus, $reportStatus, $ProofDir)
        foreach ($failure in $result.failures) {
            Write-SuiteLine "  failure: $failure"
        }
    }
}

$AggregateJson = Join-Path $SuiteDir "character-builder-suite.json"
$Results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $AggregateJson -Encoding UTF8

$AggregateMd = Join-Path $SuiteDir "character-builder-suite.md"
$md = New-Object "System.Collections.Generic.List[string]"
$md.Add("# FNV Character Builder Suite")
$md.Add("")
$md.Add("Actor: ``$ActorTarget``")
$md.Add("")
$md.Add("| Case | Runtime | Report | Failures |")
$md.Add("|---|---|---|---|")
foreach ($result in $Results) {
    $failureText = (@($result.failures) -join "<br>")
    $md.Add("| $($result.case) | $($result.runtimeGateStatus) | $($result.reportStatus) | $failureText |")
}
$md.Add("")
$md.Add("Suite: ``$SuiteDir``")
$md | Set-Content -LiteralPath $AggregateMd -Encoding UTF8

Write-SuiteLine ""
Write-SuiteLine "Aggregate JSON: $AggregateJson"
Write-SuiteLine "Aggregate Markdown: $AggregateMd"
Write-SuiteLine "SuiteDir: $SuiteDir"

$failed = @($Results | Where-Object { $_.runtimeGateStatus -ne "PASS" -or $_.reportStatus -ne "PASS" })
if ($RequirePass -and $failed.Count -gt 0) {
    throw "FNV character builder tester failed $($failed.Count) case(s). See $AggregateMd"
}
