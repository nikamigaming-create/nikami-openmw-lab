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
    [int]$RunSeconds = 3600,
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
    [string]$NeutralActorPreviewProfile = "audit",
    [string]$FnvRotationMode = "bindCoreBindLowerSplitUpper",
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,HeadOld",
    [switch]$NoSound,
    [switch]$OpenStudio,
    [int]$ServePort = 0
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ProofRoot "fnv-live-character-authoring/$Stamp"
$StudioDir = Join-Path $RunDir "studio"
$LiveAuthoringFile = Join-Path $RunDir "live-authoring.json"
$LiveRuntimeCommandFile = Join-Path $RunDir "live-runtime-command.json"
$ManifestPath = Join-Path $RunDir "live-authoring-run.json"
New-Item -ItemType Directory -Force -Path $RunDir, $StudioDir | Out-Null

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Add-Arg([System.Collections.Generic.List[string]]$List, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $List.Add($Name)
    $List.Add([string]$Value)
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

function Resolve-VcpkgRootFromKnownPaths([string]$RepoRootPath) {
    $candidates = @(
        $env:NIKAMI_VCPKG_ROOT,
        "D:\code\c\FMODS\vcpkg",
        (Join-Path $RepoRootPath "vcpkg"),
        (Join-Path (Split-Path $RepoRootPath -Parent) "vcpkg")
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

if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Resolve-FnvDataFromLatestHarvest $ProofRoot
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $VcpkgRoot = Resolve-VcpkgRootFromKnownPaths $RepoRoot
}

$initialControls = [ordered]@{}
foreach ($prefix in @("OPENMW_FNV_HEADGEAR", "OPENMW_FNV_HAIR", "OPENMW_FNV_BROW", "OPENMW_FNV_EYE", "OPENMW_FNV_BEARD", "OPENMW_FNV_MOUTH")) {
    $defaultZ = if ($prefix -in @("OPENMW_FNV_HAIR", "OPENMW_FNV_BROW", "OPENMW_FNV_EYE", "OPENMW_FNV_BEARD", "OPENMW_FNV_MOUTH")) { -90.0 } else { 0.0 }
    $initialControls["${prefix}_OFFSET_X"] = 0.0
    $initialControls["${prefix}_OFFSET_Y"] = 0.0
    $initialControls["${prefix}_OFFSET_Z"] = 0.0
    $initialControls["${prefix}_ROTATION_X"] = 0.0
    $initialControls["${prefix}_ROTATION_Y"] = 0.0
    $initialControls["${prefix}_ROTATION_Z"] = $defaultZ
    $initialControls["${prefix}_PIVOT_MODE"] = $false
}

[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-authoring-v1"
    schemaMarkers = @("runtime-live-authoring-v1", "head-surface-transform-controls-v1", "generated-control-file-only-v1")
    path = $LiveAuthoringFile
    updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    controls = $initialControls
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        numericRuntimeControlsOnly = $true
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LiveAuthoringFile -Encoding UTF8

[pscustomobject][ordered]@{
    schema = "nikami-fnv-live-runtime-command-v1"
    schemaMarkers = @("runtime-live-target-switch-v1", "generated-command-file-only-v1")
    path = $LiveRuntimeCommandFile
    updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    actorTarget = $ActorTarget
    runtimeTarget = $ActorTarget
    actorKind = $ActorKind
    entryId = ""
    command = "set-actor-target"
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        activeCellActorSwitchOnly = $true
        baseNpcPreviewWhenInactive = $true
        baseCreaturePreviewWhenInactive = $true
        baseActorPreviewWhenInactive = $true
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $LiveRuntimeCommandFile -Encoding UTF8

$StudioRunner = Join-Path $PSScriptRoot "run-fnv-character-studio-catalog.ps1"
$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
if (!(Test-Path -LiteralPath $StudioRunner -PathType Leaf)) { throw "Missing studio catalog runner: $StudioRunner" }
if (!(Test-Path -LiteralPath $FlatProof -PathType Leaf)) { throw "Missing flat proof runner: $FlatProof" }

& $StudioRunner -ProofRoot $ProofRoot -OutDir $StudioDir -LiveServe -LiveAuthoringFile $LiveAuthoringFile -LiveRuntimeCommandFile $LiveRuntimeCommandFile -ServePort $ServePort
if ($LASTEXITCODE -ne 0) {
    throw "Live studio startup failed with exit code $LASTEXITCODE"
}

$StudioServerJson = Join-Path $StudioDir "studio-live-server.json"
$StudioUrlFile = Join-Path $StudioDir "studio-url.txt"
$StudioUrl = if (Test-Path -LiteralPath $StudioUrlFile -PathType Leaf) { (Get-Content -LiteralPath $StudioUrlFile -Raw).Trim() } else { "" }

$runtimeArgs = [System.Collections.Generic.List[string]]::new()
$runtimeArgs.Add("-NoProfile")
$runtimeArgs.Add("-ExecutionPolicy")
$runtimeArgs.Add("Bypass")
$runtimeArgs.Add("-File")
$runtimeArgs.Add($FlatProof)
Add-Arg $runtimeArgs "-BuildDir" $BuildDir
Add-Arg $runtimeArgs "-Configuration" $Configuration
Add-Arg $runtimeArgs "-FnvData" $FnvData
Add-Arg $runtimeArgs "-FnvConfigData" $FnvConfigData
Add-Arg $runtimeArgs "-VcpkgRoot" $VcpkgRoot
Add-Arg $runtimeArgs "-ExtraOsgPluginDir" $ExtraOsgPluginDir
Add-Arg $runtimeArgs "-Triplet" $Triplet
Add-Arg $runtimeArgs "-ProofRoot" $ProofRoot
Add-Arg $runtimeArgs "-RunSeconds" $RunSeconds
Add-Arg $runtimeArgs "-BootstrapCell" $BootstrapCell
Add-Arg $runtimeArgs "-BootstrapX" $BootstrapX
Add-Arg $runtimeArgs "-BootstrapY" $BootstrapY
Add-Arg $runtimeArgs "-BootstrapZ" $BootstrapZ
Add-Arg $runtimeArgs "-BootstrapRotX" $BootstrapRotX
Add-Arg $runtimeArgs "-BootstrapRotY" $BootstrapRotY
Add-Arg $runtimeArgs "-BootstrapRotZ" $BootstrapRotZ
Add-Arg $runtimeArgs "-BootstrapHour" $BootstrapHour
Add-Arg $runtimeArgs "-ActorTarget" $ActorTarget
Add-Arg $runtimeArgs "-ActorKind" $ActorKind
$runtimeArgs.Add("-StageActor")
$runtimeArgs.Add("-NeutralActorPreview")
if ($ActorKind -ine "creature") {
    $runtimeArgs.Add("-NeutralActorPreviewStandingIdle")
}
Add-Arg $runtimeArgs "-NeutralActorPreviewProfile" $NeutralActorPreviewProfile
Add-Arg $runtimeArgs "-ActorStageX" $ActorStageX
Add-Arg $runtimeArgs "-ActorStageY" $ActorStageY
Add-Arg $runtimeArgs "-ActorStageZ" $ActorStageZ
Add-Arg $runtimeArgs "-ActorStageRotX" $ActorStageRotX
Add-Arg $runtimeArgs "-ActorStageRotY" $ActorStageRotY
Add-Arg $runtimeArgs "-ActorStageRotZ" $ActorStageRotZ
Add-Arg $runtimeArgs "-ActorViewOffsetZ" $ActorViewOffsetZ
Add-Arg $runtimeArgs "-ActorViewTargetZ" $ActorViewTargetZ
$runtimeArgs.Add("-ActorViewLocalOffset")
$runtimeArgs.Add("-FnvPartMatrixAudit")
Add-Arg $runtimeArgs "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
Add-Arg $runtimeArgs "-FnvRotationMode" $FnvRotationMode
Add-Arg $runtimeArgs "-CharacterBuilderPhase" "full"
Add-Arg $runtimeArgs "-LiveAuthoringFile" $LiveAuthoringFile
Add-Arg $runtimeArgs "-LiveRuntimeCommandFile" $LiveRuntimeCommandFile
if ($NoSound) { $runtimeArgs.Add("-NoSound") }

$RuntimeStdout = Join-Path $RunDir "runtime-proof.stdout.log"
$RuntimeStderr = Join-Path $RunDir "runtime-proof.stderr.log"
$RuntimeCommand = "powershell " + (($runtimeArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) -join " ")
$runtimeProcess = Start-Process -FilePath "powershell" `
    -ArgumentList ($runtimeArgs.ToArray() | ForEach-Object { Quote-ProcessArgument $_ }) `
    -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $RuntimeStdout `
    -RedirectStandardError $RuntimeStderr

$manifest = [pscustomobject][ordered]@{
    schema = "nikami-fnv-live-character-authoring-run-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $RunDir
    studioUrl = $StudioUrl
    studioServerJson = $StudioServerJson
    liveAuthoringFile = $LiveAuthoringFile
    liveRuntimeCommandFile = $LiveRuntimeCommandFile
    runtimeProcessId = $runtimeProcess.Id
    runtimeStdout = $RuntimeStdout
    runtimeStderr = $RuntimeStderr
    runtimeCommand = $RuntimeCommand
    target = [pscustomobject][ordered]@{
        actorTarget = $ActorTarget
        actorKind = $ActorKind
    }
    policy = [pscustomobject][ordered]@{
        generatedProofOutputsOnly = $true
        noRetailAssetsCommitted = $true
        liveNumericControlsOnly = $true
        activeCellActorSwitchOnly = $true
        baseNpcPreviewWhenInactive = $true
        baseCreaturePreviewWhenInactive = $true
        baseActorPreviewWhenInactive = $true
        pcFlatFirst = $true
    }
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "Live character authoring run: $RunDir"
Write-Host "Studio URL: $StudioUrl"
Write-Host "Live authoring file: $LiveAuthoringFile"
Write-Host "Live runtime command file: $LiveRuntimeCommandFile"
Write-Host "Runtime proof PID: $($runtimeProcess.Id)"
Write-Host "Run manifest: $ManifestPath"
Write-Host "Policy: generated proof/control output only; no retail assets are committed"

if ($OpenStudio -and ![string]::IsNullOrWhiteSpace($StudioUrl)) {
    Start-Process -FilePath $StudioUrl | Out-Null
}
