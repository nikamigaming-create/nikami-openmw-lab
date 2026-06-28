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
    [string]$FnvProofWeaponEdid = "",
    [string[]]$Angles = @("left", "right", "top"),
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
    [double]$NeutralActorPreviewYawOffsetDeg = [double]::NaN,
    [string]$FnvRotationMode = "bindCoreBindLowerRawUpper",
    [switch]$AllowMissingActorVisibleHandGeometry,
    [double]$ActorVisibleHandMaxDistance = 30.0,
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,HeadOld,HeadHuman",
    [string]$FnvHairEmissionStrength = "",
    [string]$FnvFaceGenTextureMode = $env:OPENMW_FNV_FACEGEN_TEXTURE_MODE,
    [string]$FnvFaceGenCompositeScale = $env:OPENMW_FNV_FACEGEN_COMPOSITE_SCALE,
    [string]$FnvEgtDiffuseScale = $env:OPENMW_FNV_EGT_DIFFUSE_SCALE,
    [string]$FnvFaceGenBiasR = $env:OPENMW_FNV_FACEGEN_BIAS_R,
    [string]$FnvFaceGenBiasG = $env:OPENMW_FNV_FACEGEN_BIAS_G,
    [string]$FnvFaceGenBiasB = $env:OPENMW_FNV_FACEGEN_BIAS_B,
    [string]$FnvDisableEgtDiffuseSynthesis = $env:OPENMW_FNV_DISABLE_EGT_DIFFUSE_SYNTHESIS,
    [string]$FnvUseEgtMaterialTint = $env:OPENMW_FNV_USE_EGT_MATERIAL_TINT,
    [string]$FnvUseRawBodyTintSwatch = $env:OPENMW_FNV_USE_RAW_BODY_TINT_SWATCH,
    [string]$FnvDisableEgtMaterialTint = $env:OPENMW_FNV_DISABLE_EGT_MATERIAL_TINT,
    [string]$FnvDisableRawBodyTintSwatch = $env:OPENMW_FNV_DISABLE_RAW_BODY_TINT_SWATCH,
    [string]$FnvSkinningMode = $env:OPENMW_FNV_SKINNING_MODE,
    [string]$FnvVrHandSkinningMode = $env:OPENMW_FNV_VR_HAND_SKINNING_MODE,
    [string]$FnvStaticizeRiggedHandParts = $env:OPENMW_FNV_STATICIZE_RIGGED_HAND_PARTS,
    [string]$FnvKeepRiggedHandParts = $env:OPENMW_FNV_KEEP_RIGGED_HAND_PARTS,
    [string]$FnvHandBindFrameAttach = $env:OPENMW_FNV_HAND_BIND_FRAME_ATTACH,
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

function New-ProofRunStamp {
    $now = Get-Date
    $processId = [System.Diagnostics.Process]::GetCurrentProcess().Id
    $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
    return "{0}_{1}_{2}" -f $now.ToString("yyyyMMdd_HHmmss_fff"), $processId, $suffix
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

$Stamp = New-ProofRunStamp
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

function Get-NewProofDirectory([string[]]$Before, [datetime]$StartedAt, [string]$RuntimeTag = "") {
    $base = Join-Path $ProofRoot "fnv-flat-proof"
    if (!(Test-Path -LiteralPath $base)) { return $null }

    $beforeSet = New-Object "System.Collections.Generic.HashSet[string]"
    foreach ($path in $Before) {
        if (![string]::IsNullOrWhiteSpace($path)) { $null = $beforeSet.Add($path) }
    }

    $candidates = @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Where-Object { !$beforeSet.Contains($_.FullName) -and $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) })
    if (![string]::IsNullOrWhiteSpace($RuntimeTag)) {
        $safeRuntimeTag = $RuntimeTag -replace '[^A-Za-z0-9_.-]', '_'
        $tagged = @($candidates | Where-Object { $_.Name -like "*$safeRuntimeTag*" })
        if ($tagged.Count -gt 0) {
            $candidates = $tagged
        }
    }

    $candidate = $candidates |
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

function Test-ActorKitWeaponActionGroup([string]$Group) {
    if ([string]::IsNullOrWhiteSpace($Group)) { return $false }
    return $Group -match '(?i)(attack|fire|shoot|reload|aim)'
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
    $visibleLimbShapeBadLines = Get-RegexValue $summaryText "Target visible limb shape BAD lines:\s+([^\r\n]+)"
    $visibleLimbShapeFailureLine = Get-RegexValue $summaryText "Target visible limb shape failure line:\s+([^\r\n]+)"
    $fabricNoTwistBadLines = Get-RegexValue $summaryText "Target fabric no-twist BAD lines:\s+([^\r\n]+)"
    $fabricNoTwistFailureLine = Get-RegexValue $summaryText "Target fabric no-twist failure line:\s+([^\r\n]+)"
    $visibleHandGeometryFailureLine = Get-RegexValue $summaryText "Target visible hand geometry failure line:\s+([^\r\n]+)"
    $fnvShowIkBones = Get-RegexValue $summaryText "FnvShowIkBones:\s+([^\r\n]+)"
    $fnvArmBaselinePose = Get-RegexValue $summaryText "FnvArmBaselinePose:\s+([^\r\n]+)"
    $armBaselinePoseLines = Get-RegexValue $summaryText "Arm baseline pose proof lines:\s+([^\r\n]+)"
    $armBaselineChainLines = Get-RegexValue $summaryText "Arm baseline chain proof lines:\s+([^\r\n]+)"
    $weaponIkSolverLines = Get-RegexValue $summaryText "Weapon IK solver proof lines:\s+([^\r\n]+)"
    $weaponIkAuthoringPoseLines = Get-RegexValue $summaryText "Weapon IK authoring pose proof lines:\s+([^\r\n]+)"
    $weaponIkEndpointCcdLines = Get-RegexValue $summaryText "Weapon IK chain endpoint CCD proof lines:\s+([^\r\n]+)"
    $weaponIkFabrikPoleLines = Get-RegexValue $summaryText "Weapon IK FABRIK pole proof lines:\s+([^\r\n]+)"
    $weaponIkShoulderTargetLines = Get-RegexValue $summaryText "Weapon IK shoulder target proof lines:\s+([^\r\n]+)"
    $weaponIkUncrossedHandsLines = Get-RegexValue $summaryText "Weapon IK uncrossed hands proof lines:\s+([^\r\n]+)"
    $weaponIkWeaponAimLines = Get-RegexValue $summaryText "Weapon IK weapon aim proof lines:\s+([^\r\n]+)"
    $weaponIkVrArcadeHandLines = Get-RegexValue $summaryText "Weapon IK VR arcade hand solver proof lines:\s+([^\r\n]+)"
    $weaponIkBoneOverlayLines = Get-RegexValue $summaryText "Weapon IK bone overlay proof lines:\s+([^\r\n]+)"
    $staticHandGripLines = Get-RegexValue $summaryText "Target static hand grip proof lines:\s+([^\r\n]+)"

    return [pscustomobject][ordered]@{
        requireActorVisibleHandGeometry = $requireActorVisibleHandGeometry
        actorVisibleHandMaxDistance = $actorVisibleHandMaxDistance
        visibleHandGeometry = [pscustomobject][ordered]@{
            status = $visibleHandGeometryStatus
            samples = $visibleHandGeometrySamples
            poseSanityBadLines = $visibleHandGeometryPoseSanityBadLines
            poseSanityFailureLine = $visibleHandGeometryPoseSanityFailureLine
            limbShapeBadLines = $visibleLimbShapeBadLines
            limbShapeFailureLine = $visibleLimbShapeFailureLine
            fabricNoTwistBadLines = $fabricNoTwistBadLines
            fabricNoTwistFailureLine = $fabricNoTwistFailureLine
            failureLine = $visibleHandGeometryFailureLine
        }
        fnvSkinningMatrixAudit = $FnvSkinningMatrixAudit
        fnvHairEmissionStrength = $fnvHairEmissionStrength
        fnvShowIkBones = $fnvShowIkBones
        fnvArmBaselinePose = $fnvArmBaselinePose
        armBaselinePoseProofLines = $armBaselinePoseLines
        armBaselineChainProofLines = $armBaselineChainLines
        weaponIkSolverProofLines = $weaponIkSolverLines
        weaponIkAuthoringPoseProofLines = $weaponIkAuthoringPoseLines
        weaponIkEndpointCcdProofLines = $weaponIkEndpointCcdLines
        weaponIkFabrikPoleProofLines = $weaponIkFabrikPoleLines
        weaponIkShoulderTargetProofLines = $weaponIkShoulderTargetLines
        weaponIkUncrossedHandsProofLines = $weaponIkUncrossedHandsLines
        weaponIkWeaponAimProofLines = $weaponIkWeaponAimLines
        weaponIkVrArcadeHandSolverProofLines = $weaponIkVrArcadeHandLines
        weaponIkBoneOverlayProofLines = $weaponIkBoneOverlayLines
        staticHandGripProofLines = $staticHandGripLines
        skinningModes = @($skinningModes.ToArray())
    }
}

$diagonal = $ActorViewDistance * 0.7071067811865476
$AllAngles = @(
    [pscustomobject]@{ Name = "front"; OffsetX = $ActorViewDistance; OffsetY = 0.0; OffsetZ = $ActorViewOffsetZ; TargetZ = $ActorViewTargetZ; NeutralYawDeg = 0.0 },
    [pscustomobject]@{ Name = "front-left"; OffsetX = $diagonal; OffsetY = -$diagonal; OffsetZ = $ActorViewOffsetZ; TargetZ = $ActorViewTargetZ; NeutralYawDeg = -45.0 },
    [pscustomobject]@{ Name = "front-right"; OffsetX = $diagonal; OffsetY = $diagonal; OffsetZ = $ActorViewOffsetZ; TargetZ = $ActorViewTargetZ; NeutralYawDeg = 45.0 },
    [pscustomobject]@{ Name = "left"; OffsetX = 0.0; OffsetY = -$ActorViewDistance; OffsetZ = $ActorViewOffsetZ; TargetZ = $ActorViewTargetZ; NeutralYawDeg = -90.0 },
    [pscustomobject]@{ Name = "right"; OffsetX = 0.0; OffsetY = $ActorViewDistance; OffsetZ = $ActorViewOffsetZ; TargetZ = $ActorViewTargetZ; NeutralYawDeg = 90.0 },
    [pscustomobject]@{ Name = "top"; OffsetX = 0.0; OffsetY = 24.0; OffsetZ = ($ActorViewOffsetZ + 210.0); TargetZ = ($ActorViewTargetZ - 8.0); NeutralYawDeg = 0.0 }
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
$HasArmBaselinePhase = $false
foreach ($phase in $Phases) {
    if ($phase -ieq "arm-baseline" -or $phase -ieq "t-pose") {
        $HasArmBaselinePhase = $true
        break
    }
}
$ResolvedActorKitAnimationSource = $ActorKitAnimationSource
$ActorKitAnimationSourceDefaulted = $false
if (!$HasArmBaselinePhase -and !($Phases -contains "weapon") -and $ActorKind -ine "creature" -and [string]::IsNullOrWhiteSpace($ResolvedActorKitAnimationSource) -and !(Test-ActorKitWeaponActionGroup $ActorKitAnimationGroup)) {
    $ResolvedActorKitAnimationSource = "mtidle"
    $ActorKitAnimationSourceDefaulted = $true
}
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
Write-SuiteLine "ActorKitAnimationSource: $ResolvedActorKitAnimationSource"
Write-SuiteLine "ActorKitAnimationSourceRequested: $ActorKitAnimationSource"
Write-SuiteLine "ActorKitAnimationSourceDefaulted: $ActorKitAnimationSourceDefaulted"
Write-SuiteLine "ActorKitAnimationStartPoint: $(Format-Double $ActorKitAnimationStartPoint)"
Write-SuiteLine "ActorKitAnimationGroup: $ActorKitAnimationGroup"
Write-SuiteLine "ActorKitDialogueMode: $ActorKitDialogueMode"
Write-SuiteLine "FnvProofWeaponEdid: $FnvProofWeaponEdid"
Write-SuiteLine "FnvRotationMode: $FnvRotationMode"
Write-SuiteLine "AllowMissingActorVisibleHandGeometry: $AllowMissingActorVisibleHandGeometry"
Write-SuiteLine "ActorVisibleHandMaxDistance: $ActorVisibleHandMaxDistance"
Write-SuiteLine "FnvSkinningMatrixAudit: $FnvSkinningMatrixAudit"
Write-SuiteLine "FnvHairEmissionStrength: $FnvHairEmissionStrength"
Write-SuiteLine "FnvFaceGenTextureMode: $FnvFaceGenTextureMode"
Write-SuiteLine "FnvFaceGenCompositeScale: $FnvFaceGenCompositeScale"
Write-SuiteLine "FnvEgtDiffuseScale: $FnvEgtDiffuseScale"
Write-SuiteLine "FnvFaceGenBias: $FnvFaceGenBiasR,$FnvFaceGenBiasG,$FnvFaceGenBiasB"
Write-SuiteLine "FnvDisableEgtDiffuseSynthesis: $FnvDisableEgtDiffuseSynthesis"
Write-SuiteLine "FnvUseEgtMaterialTint: $FnvUseEgtMaterialTint"
Write-SuiteLine "FnvUseRawBodyTintSwatch: $FnvUseRawBodyTintSwatch"
Write-SuiteLine "FnvDisableEgtMaterialTint: $FnvDisableEgtMaterialTint"
Write-SuiteLine "FnvDisableRawBodyTintSwatch: $FnvDisableRawBodyTintSwatch"
Write-SuiteLine "FnvSkinningMode: $FnvSkinningMode"
Write-SuiteLine "FnvVrHandSkinningMode: $FnvVrHandSkinningMode"
Write-SuiteLine "FnvStaticizeRiggedHandParts: $FnvStaticizeRiggedHandParts"
Write-SuiteLine "FnvKeepRiggedHandParts: $FnvKeepRiggedHandParts"
Write-SuiteLine "FnvHandBindFrameAttach: $FnvHandBindFrameAttach"
Write-SuiteLine "LiveAuthoringFile: $LiveAuthoringFile"
Write-SuiteLine "FnvUseNativeAnimationCallbacks: $FnvUseNativeAnimationCallbacks"
Write-SuiteLine "Angles: $(@($CameraAngles | ForEach-Object { $_.Name }) -join ',')"
Write-SuiteLine "BootstrapCell: $BootstrapCell"
Write-SuiteLine "BootstrapPosition: $BootstrapX,$BootstrapY,$BootstrapZ"
Write-SuiteLine "BootstrapRotation: $BootstrapRotX,$BootstrapRotY,$BootstrapRotZ"
Write-SuiteLine "ActorStagePosition: $ActorStageX,$ActorStageY,$ActorStageZ"
Write-SuiteLine "ActorStageRotation: $ActorStageRotX,$ActorStageRotY,$ActorStageRotZ"
Write-SuiteLine "NeutralActorPreviewProfile: $NeutralActorPreviewProfile"
Write-SuiteLine "NeutralActorPreviewYawOffsetDeg: $(Format-Double $NeutralActorPreviewYawOffsetDeg)"
Write-SuiteLine "Policy: no retail assets copied into repo; generated proof output only"
Write-SuiteLine ""

$ActorKitAnimationStartPointValue = if (![double]::IsNaN($ActorKitAnimationStartPoint)) { $ActorKitAnimationStartPoint } else { $null }

$Results = New-Object "System.Collections.Generic.List[object]"

foreach ($phase in $Phases) {
    foreach ($angle in $CameraAngles) {
        $neutralYawDeg = if (![double]::IsNaN($NeutralActorPreviewYawOffsetDeg)) { $NeutralActorPreviewYawOffsetDeg } else { [double]$angle.NeutralYawDeg }
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

        $phaseNeutralPreviewProfile = $NeutralActorPreviewProfile
        $phaseUsesWeaponCamera = $phase -ieq "weapon" -or (Test-ActorKitWeaponActionGroup $ActorKitAnimationGroup)
        if ($phaseUsesWeaponCamera -and ($NeutralActorPreviewProfile -ieq "audit" -or $NeutralActorPreviewProfile -ieq "bot-audit")) {
            $phaseNeutralPreviewProfile = "weapon-arms"
        }
        elseif (($phase -ieq "arm-baseline" -or $phase -ieq "t-pose") -and ($NeutralActorPreviewProfile -ieq "audit" -or $NeutralActorPreviewProfile -ieq "bot-audit")) {
            $phaseNeutralPreviewProfile = "arms"
        }

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
            NeutralActorPreviewProfile = $phaseNeutralPreviewProfile
            # Contract marker: NeutralActorPreviewProfile = $NeutralActorPreviewProfile
            NeutralActorPreviewYawOffsetDeg = $neutralYawDeg
            ActorStageX = $ActorStageX
            ActorStageY = $ActorStageY
            ActorStageZ = $ActorStageZ
            ActorStageRotX = $ActorStageRotX
            ActorStageRotY = $ActorStageRotY
            ActorStageRotZ = $ActorStageRotZ
            ActorViewOffsetX = [double]$angle.OffsetX
            ActorViewOffsetY = [double]$angle.OffsetY
            ActorViewOffsetZ = [double]$angle.OffsetZ
            ActorViewTargetZ = [double]$angle.TargetZ
            ActorViewLocalOffset = $true
            FnvPartMatrixAudit = $true
            FnvSkinningMatrixAudit = $FnvSkinningMatrixAudit
            FnvHairEmissionStrength = $FnvHairEmissionStrength
            FnvFaceGenTextureMode = $FnvFaceGenTextureMode
            FnvFaceGenCompositeScale = $FnvFaceGenCompositeScale
            FnvEgtDiffuseScale = $FnvEgtDiffuseScale
            FnvFaceGenBiasR = $FnvFaceGenBiasR
            FnvFaceGenBiasG = $FnvFaceGenBiasG
            FnvFaceGenBiasB = $FnvFaceGenBiasB
            FnvDisableEgtDiffuseSynthesis = $FnvDisableEgtDiffuseSynthesis
            FnvUseEgtMaterialTint = $FnvUseEgtMaterialTint
            FnvUseRawBodyTintSwatch = $FnvUseRawBodyTintSwatch
            FnvDisableEgtMaterialTint = $FnvDisableEgtMaterialTint
            FnvDisableRawBodyTintSwatch = $FnvDisableRawBodyTintSwatch
            FnvSkinningMode = $FnvSkinningMode
            FnvVrHandSkinningMode = $FnvVrHandSkinningMode
            FnvStaticizeRiggedHandParts = $FnvStaticizeRiggedHandParts
            FnvKeepRiggedHandParts = $FnvKeepRiggedHandParts
            FnvHandBindFrameAttach = $FnvHandBindFrameAttach
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
        if (![string]::IsNullOrWhiteSpace($ResolvedActorKitAnimationSource)) { $proofArgs.ActorKitAnimationSource = $ResolvedActorKitAnimationSource }
        if (![double]::IsNaN($ActorKitAnimationStartPoint)) { $proofArgs.ActorKitAnimationStartPoint = $ActorKitAnimationStartPoint }
        if (![string]::IsNullOrWhiteSpace($ActorKitAnimationGroup)) { $proofArgs.ActorKitAnimationGroup = $ActorKitAnimationGroup }
        if (![string]::IsNullOrWhiteSpace($ActorKitDialogueMode)) { $proofArgs.ActorKitDialogueMode = $ActorKitDialogueMode }
        if (![string]::IsNullOrWhiteSpace($FnvProofWeaponEdid)) { $proofArgs.FnvProofWeaponEdid = $FnvProofWeaponEdid }
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

        $ProofDir = Get-NewProofDirectory $before $startedAt $runtimeTag
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
        $reportData = $null
        if (Test-Path -LiteralPath $reportJson) {
            $reportData = Get-Content -LiteralPath $reportJson -Raw | ConvertFrom-Json
        }
        $reportStatus = if ($null -ne $reportData -and ![string]::IsNullOrWhiteSpace([string]$reportData.status)) {
            [string]$reportData.status
        } elseif ($reportExit -eq 0) {
            "PASS"
        } else {
            "FAIL"
        }
        if (($reportExit -eq 0 -and $reportStatus -ne "PASS") -or ($reportExit -ne 0 -and $reportStatus -eq "PASS")) {
            $runtimeGateStatus = "FAIL"
            $runtimeGateError = "report exit/status mismatch: exit=$reportExit json=$reportStatus"
        }
        $runtimeEvidence = Get-FnvRuntimeEvidence $ProofDir $FnvSkinningMatrixAudit

        $result = [pscustomobject][ordered]@{
            case = $caseName
            phase = $phase
            angle = $angle.Name
            actorTarget = $ActorTarget
            actorKind = $ActorKind
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
                neutralPreviewYawOffsetDeg = $neutralYawDeg
                localOffset = $true
            }
            actorKitSelection = [pscustomobject][ordered]@{
                parts = $ActorKitPartsCsv
                partModels = $ActorKitPartModelsCsv
                propSlots = $ActorKitPropSlotsCsv
                propModels = $ActorKitPropModelsCsv
                animationSource = $ResolvedActorKitAnimationSource
                animationSourceRequested = $ActorKitAnimationSource
                animationSourceDefaulted = [bool]$ActorKitAnimationSourceDefaulted
                animationStartPoint = $ActorKitAnimationStartPointValue
                animationGroup = $ActorKitAnimationGroup
                dialogueMode = $ActorKitDialogueMode
                fnvProofWeaponEdid = $FnvProofWeaponEdid
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
            reviewCrops = if ($null -ne $reportData) { @($reportData.reviewCrops) } else { @() }
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
$md.Add("| Case | Runtime | Report | Review Crops | Failures |")
$md.Add("|---|---|---|---:|---|")
foreach ($result in $Results) {
    $failureText = (@($result.failures) -join "<br>")
    $md.Add("| $($result.case) | $($result.runtimeGateStatus) | $($result.reportStatus) | $(@($result.reviewCrops).Count) | $failureText |")
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
