param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$ProofRoot = "",
    [string]$ActorTarget = "GSEasyPete",
    [string]$RuntimeTag = "",
    [string]$BootstrapCell = "FormId:0x10daeb9",
    [double]$BootstrapX = -67480,
    [double]$BootstrapY = 1500,
    [double]$BootstrapZ = 8425,
    [double]$BootstrapRotZ = 1.5708,
    [double]$BootstrapHour = 6,
    [int]$RunSeconds = 44,
    [int]$FirstWriteDelaySeconds = 28,
    [int]$SecondWriteDelaySeconds = 5,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set NIKAMI_FNV_DATA or pass -FnvData."
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "Set VCPKG_ROOT or pass -VcpkgRoot."
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($RuntimeTag)) {
    $RuntimeTag = "fnv-live-post-construction-$Stamp"
}
$RunDir = Join-Path $ProofRoot "fnv-live-post-construction-selector-proof/$Stamp"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
$LiveRuntimeCommandFile = Join-Path $RunDir "live-runtime-command.json"

function Write-LiveCommand {
    param(
        [string]$Phase,
        [string]$Parts,
        [string]$AnimationStartPoint
    )

    [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-runtime-command-v1"
        schemaMarkers = @(
            "runtime-live-target-switch-v1",
            "runtime-live-actor-kit-controls-v1",
            "generated-command-file-only-v1"
        )
        command = "update-actor-kit"
        actorTarget = $ActorTarget
        runtimeTarget = $ActorTarget
        actorKind = "npc"
        characterBuilderPhase = $Phase
        actorKitParts = $Parts
        actorKitPropSlots = $Parts
        actorKitAnimationGroup = "idle"
        actorKitAnimationStartPoint = $AnimationStartPoint
        selectors = [pscustomobject][ordered]@{
            phase = $Phase
            parts = @($Parts)
            propSlots = @($Parts)
            animationGroup = "idle"
            animationStartPoint = $AnimationStartPoint
        }
        policy = [pscustomobject][ordered]@{
            generatedProofOutputsOnly = $true
            noRetailPayloadBytes = $true
            actorKitSelectorControls = $true
            partRebuild = "runtime-supported"
        }
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LiveRuntimeCommandFile -Encoding UTF8
}

Write-LiveCommand -Phase "full" -Parts "all" -AnimationStartPoint "0.10"
$StartedAt = Get-Date

$RuntimeJob = Start-Job -ScriptBlock {
    param(
        [string]$RepoRoot,
        [string]$BuildDir,
        [string]$Configuration,
        [string]$VcpkgRoot,
        [string]$FnvData,
        [string]$ProofRoot,
        [string]$ActorTarget,
        [string]$BootstrapCell,
        [double]$BootstrapX,
        [double]$BootstrapY,
        [double]$BootstrapZ,
        [double]$BootstrapRotZ,
        [double]$BootstrapHour,
        [string]$LiveRuntimeCommandFile,
        [string]$RuntimeTag,
        [int]$RunSeconds
    )
    Set-Location $RepoRoot
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\nikami\run-fnv-flat-proof.ps1 `
        -BuildDir $BuildDir `
        -Configuration $Configuration `
        -RuntimeTag $RuntimeTag `
        -VcpkgRoot $VcpkgRoot `
        -FnvData $FnvData `
        -ProofRoot $ProofRoot `
        -NeutralActorPreview `
        -NeutralActorPreviewStandingIdle `
        -NeutralActorPreviewProfile audit `
        -ActorTarget $ActorTarget `
        -ActorKind npc `
        -StageActor `
        -BootstrapCell $BootstrapCell `
        -BootstrapX $BootstrapX `
        -BootstrapY $BootstrapY `
        -BootstrapZ $BootstrapZ `
        -BootstrapRotZ $BootstrapRotZ `
        -BootstrapHour $BootstrapHour `
        -ActorStageX $BootstrapX `
        -ActorStageY $BootstrapY `
        -ActorStageZ $BootstrapZ `
        -ActorStageRotZ $BootstrapRotZ `
        -ActorViewLocalOffset `
        -ActorViewOffsetZ 108 `
        -ActorViewTargetZ 108 `
        -LiveRuntimeCommandFile $LiveRuntimeCommandFile `
        -RunSeconds $RunSeconds `
        -ScreenshotFrames "540,720,900" `
        -RequireLogPattern "runtime-live-actor-kit-post-construction-selector" `
        -NoSound
    if ($LASTEXITCODE -ne 0) {
        throw "FNV flat proof failed with exit code $LASTEXITCODE."
    }
} -ArgumentList $RepoRoot, $BuildDir, $Configuration, $VcpkgRoot, $FnvData, $ProofRoot, $ActorTarget, $BootstrapCell, $BootstrapX, $BootstrapY, $BootstrapZ, $BootstrapRotZ, $BootstrapHour, $LiveRuntimeCommandFile, $RuntimeTag, $RunSeconds

Start-Sleep -Seconds $FirstWriteDelaySeconds
Write-LiveCommand -Phase "full" -Parts "all" -AnimationStartPoint "0.35"
Start-Sleep -Seconds $SecondWriteDelaySeconds
Write-LiveCommand -Phase "full" -Parts "all" -AnimationStartPoint "0.65"

$Output = Receive-Job -Job $RuntimeJob -Wait -AutoRemoveJob
$Output | Out-Host

$ProofDir = Get-ChildItem -Path (Join-Path $ProofRoot "fnv-flat-proof") -Directory |
    Where-Object { $_.LastWriteTime -ge $StartedAt.AddSeconds(-2) } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($null -eq $ProofDir) {
    throw "No generated FNV flat proof directory was found after $StartedAt."
}

$OpenMwLog = Join-Path $ProofDir.FullName "openmw.log"
if (!(Test-Path -LiteralPath $OpenMwLog -PathType Leaf)) {
    throw "Missing OpenMW log from generated proof: $OpenMwLog"
}

$LogText = Get-Content -LiteralPath $OpenMwLog -Raw
if ($LogText -notmatch "runtime-live-actor-kit-post-construction-selector") {
    throw "Missing post-construction actor-kit selector marker."
}
if ($LogText -notmatch "actor=$([regex]::Escape($ActorTarget)).*targetMatches=1") {
    throw "Missing target actor post-construction selector evidence for $ActorTarget."
}
if ($LogText -notmatch "generation=2 actor=$([regex]::Escape($ActorTarget))") {
    throw "Missing second post-construction selector generation for $ActorTarget."
}
$ActorPattern = [regex]::Escape($ActorTarget)
$RebuildLines = @($LogText -split "\r?\n" | Where-Object {
    $_ -match "runtime-live-actor-kit-part-rebuild" -and
    $_ -match "\bactor=$ActorPattern\b" -and
    $_ -match "\btargetMatches=1\b"
})

$SecondRebuildLine = $RebuildLines | Where-Object {
    $_ -match "\bgeneration=2\b" -and
    $_ -match "\brequestedParts=([1-9]\d*)\b" -and
    $_ -match "\brebuiltParts=([1-9]\d*)\b" -and
    $_ -match "\battachedParts=([1-9]\d*)\b" -and
    $_ -match "\bstaleAfterRemoval=0\b" -and
    $_ -match "\bfailedParts=0\b" -and
    $_ -match "\bruntime=runtime-supported\b"
} | Select-Object -First 1

if ($null -eq $SecondRebuildLine) {
    throw "Missing successful target actor runtime part rebuild evidence for $ActorTarget."
}
$PendingPartRebuild = "partRebuild=" + "loaded-pending-runtime"
if ($LogText -match "actor=$ActorPattern.*$([regex]::Escape($PendingPartRebuild))") {
    throw "Target actor post-construction proof still contains pending part-rebuild classification."
}
if ($LogText -notmatch "actor-kit animation request actor=$([regex]::Escape($ActorTarget)).*startPoint=0.65") {
    throw "Missing post-construction actor-kit animation request for final live selector."
}

$Result = [pscustomobject][ordered]@{
    schema = "nikami-fnv-live-actor-kit-post-construction-proof-v1"
    status = "PASS"
    proofDir = $ProofDir.FullName
    liveRuntimeCommandFile = $LiveRuntimeCommandFile
    actorTarget = $ActorTarget
    runtimeTag = $RuntimeTag
    bootstrapCell = $BootstrapCell
    bootstrapHour = $BootstrapHour
    firstWriteDelaySeconds = $FirstWriteDelaySeconds
    secondWriteDelaySeconds = $SecondWriteDelaySeconds
    generatedProofOutputsOnly = $true
    noRetailAssetsCommitted = $true
    partRebuild = "runtime-supported"
    rebuildEvidence = $SecondRebuildLine
}
$ResultPath = Join-Path $RunDir "result.json"
$Result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ResultPath -Encoding UTF8

Write-Host "FNV live actor-kit post-construction proof PASS"
Write-Host "ProofDir: $($ProofDir.FullName)"
Write-Host "Result: $ResultPath"

if ($RequirePass -and $Result.status -ne "PASS") {
    throw "FNV live actor-kit post-construction proof did not pass."
}
