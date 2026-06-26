param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$AssetClass = "mesh",
    [string]$Record = "direct-model",
    [string]$Session = "native-proof",
    [string]$Model = "meshes\armor\headgear\cowboyhat\cowboyhat.nif",
    [string]$View = "front",
    [string]$ActorProfile = "",
    [switch]$AllowPackageProcedureIdles,
    [double]$RotX = 0,
    [double]$RotY = 0,
    [double]$RotZ = 0,
    [double]$Scale = 1,
    [double]$Zoom = 1,
    [switch]$StartWorld,
    [int]$RunSeconds = 14,
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

function Get-FirstLogLine([string]$Path, [string]$Pattern) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $match = Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $match) { return $null }
    return $match.Line
}

function Get-LogMatches([string]$Path, [string]$Pattern) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    return @(Select-String -LiteralPath $Path -Pattern $Pattern -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
}

function Get-NativeStudioRuntimeDir([string]$ProofRoot) {
    return Join-Path $ProofRoot "configs/fnv-flat-clean-native-asset-studio"
}

function ConvertTo-ProofLineText([object]$Value) {
    if ($null -eq $Value) { return "" }
    return [string]$Value
}

$Stamp = New-ProofRunStamp
$ProofDir = Join-Path $ProofRoot "fnv-native-asset-studio-proof/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
$ProofJson = Join-Path $ProofDir "native-asset-studio-proof.json"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

$Runner = Join-Path $PSScriptRoot "run-fnv-native-asset-studio.ps1"
if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "Missing native asset studio runner: $Runner"
}

Write-ProofLine "FNV native asset studio proof $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine "AssetClass: $AssetClass"
Write-ProofLine "Record: $Record"
Write-ProofLine "Session: $Session"
Write-ProofLine "Model: $Model"
Write-ProofLine "View: $View"
Write-ProofLine "ActorProfile: $ActorProfile"
Write-ProofLine "ActorStandingIdle: $(!$AllowPackageProcedureIdles)"
Write-ProofLine "Rotation: $RotX,$RotY,$RotZ"
Write-ProofLine "Scale: $Scale"
Write-ProofLine "Zoom: $Zoom"
Write-ProofLine "World start: $StartWorld"
Write-ProofLine "CleanBoot: $(!$StartWorld)"
Write-ProofLine "RunSeconds: $RunSeconds"
Write-ProofLine "Policy: generated proof output only; no retail assets are committed"
Write-ProofLine ""

& $Runner `
    -BuildDir $BuildDir `
    -Configuration $Configuration `
    -FnvData $FnvData `
    -FnvConfigData $FnvConfigData `
    -VcpkgRoot $VcpkgRoot `
    -Triplet $Triplet `
    -ProofRoot $ProofRoot `
    -AssetClass $AssetClass `
    -Record $Record `
    -Session $Session `
    -Model $Model `
    -View $View `
    -ActorProfile $ActorProfile `
    -AllowPackageProcedureIdles:$AllowPackageProcedureIdles `
    -RotX $RotX `
    -RotY $RotY `
    -RotZ $RotZ `
    -Scale $Scale `
    -Zoom $Zoom `
    -StartWorld:$StartWorld `
    -RunSeconds $RunSeconds | Out-Host

$ConfigDir = Get-NativeStudioRuntimeDir $ProofRoot
$OpenMwLog = Join-Path $ConfigDir "openmw.log"
$CopiedLog = Join-Path $ProofDir "openmw.log"
if (Test-Path -LiteralPath $OpenMwLog -PathType Leaf) {
    Copy-Item -LiteralPath $OpenMwLog -Destination $CopiedLog -Force
}

$registeredLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio registered gate=native-asset-studio-window"
$cleanBootLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio clean boot active .*gate=native-asset-studio-clean-boot runtime=runtime-supported"
$openedLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio native window opened"
$selectorLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio selector .*gate=native-asset-studio-selector runtime=loaded-pending-runtime"
$threeCameraLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio three camera preview .*gate=native-asset-studio-three-camera-preview runtime=runtime-supported"
$threeCameraActorLine = Get-FirstLogLine $CopiedLog "FNV/ESM4 asset studio three camera actor preview .*gate=native-asset-studio-three-camera-actor-preview runtime=runtime-supported"
$loadedLines = @(Get-LogMatches $CopiedLog "FNV/ESM4 asset studio model loaded .*gate=native-asset-studio-model-preview runtime=runtime-supported")
$failedLines = @(Get-LogMatches $CopiedLog "FNV/ESM4 asset studio model failed .*gate=native-asset-studio-model-preview runtime=known-blocked")
$actorLoadedLines = @(Get-LogMatches $CopiedLog "FNV/ESM4 asset studio actor loaded .*gate=native-asset-studio-actor-preview runtime=runtime-supported")
$actorFailedLines = @(Get-LogMatches $CopiedLog "FNV/ESM4 asset studio actor failed .*gate=native-asset-studio-actor-preview runtime=known-blocked")
$fatalLines = @(Get-LogMatches $CopiedLog "Fatal error|Failed to start new game|Lua error|marker_error")

$terminalCount = $loadedLines.Count + $failedLines.Count + $actorLoadedLines.Count + $actorFailedLines.Count
$status = "PASS"
$failures = New-Object "System.Collections.Generic.List[string]"
if ([string]::IsNullOrWhiteSpace($registeredLine)) { $status = "FAIL"; $failures.Add("missing asset studio registered log line") }
if (!$StartWorld -and [string]::IsNullOrWhiteSpace($cleanBootLine)) {
    $status = "FAIL"
    $failures.Add("missing clean asset studio boot log line")
}
if ([string]::IsNullOrWhiteSpace($openedLine)) { $status = "FAIL"; $failures.Add("missing native window opened log line") }
if ([string]::IsNullOrWhiteSpace($selectorLine)) { $status = "FAIL"; $failures.Add("missing asset selector log line") }
if ([string]::IsNullOrWhiteSpace($threeCameraLine) -and [string]::IsNullOrWhiteSpace($threeCameraActorLine)) {
    $status = "FAIL"
    $failures.Add("missing three-camera preview log line")
}
if ($terminalCount -ne 1) { $status = "FAIL"; $failures.Add("expected exactly one preview terminal state, saw $terminalCount") }
if ($fatalLines.Count -gt 0) { $status = "FAIL"; $failures.Add("fatal/blocker log lines present") }

$terminalLine = if ($loadedLines.Count -gt 0) {
    $loadedLines[0]
} elseif ($actorLoadedLines.Count -gt 0) {
    $actorLoadedLines[0]
} elseif ($failedLines.Count -gt 0) {
    $failedLines[0]
} elseif ($actorFailedLines.Count -gt 0) {
    $actorFailedLines[0]
} else {
    ""
}
$runtimeClassification = if ($loadedLines.Count -gt 0 -or $actorLoadedLines.Count -gt 0) {
    "runtime-supported"
} elseif ($failedLines.Count -gt 0 -or $actorFailedLines.Count -gt 0) {
    "known-blocked"
} else {
    "loaded-pending-runtime"
}

$proof = [pscustomobject][ordered]@{
    stamp = $Stamp
    status = $status
    assetClass = $AssetClass
    record = $Record
    session = $Session
    model = $Model
    view = $View
    actorProfile = $ActorProfile
    actorStandingIdle = (!$AllowPackageProcedureIdles)
    rotation = [pscustomobject][ordered]@{ x = $RotX; y = $RotY; z = $RotZ }
    scale = $Scale
    zoom = $Zoom
    startWorld = [bool]$StartWorld
    cleanBoot = (!$StartWorld)
    runSeconds = $RunSeconds
    runtimeClassification = $runtimeClassification
    gates = [pscustomobject][ordered]@{
        registered = (![string]::IsNullOrWhiteSpace($registeredLine))
        cleanBoot = (![string]::IsNullOrWhiteSpace($cleanBootLine))
        opened = (![string]::IsNullOrWhiteSpace($openedLine))
        selector = (![string]::IsNullOrWhiteSpace($selectorLine))
        threeCamera = (![string]::IsNullOrWhiteSpace($threeCameraLine))
        threeCameraActor = (![string]::IsNullOrWhiteSpace($threeCameraActorLine))
        terminalStateCount = $terminalCount
        loadedCount = $loadedLines.Count
        failedCount = $failedLines.Count
        actorLoadedCount = $actorLoadedLines.Count
        actorFailedCount = $actorFailedLines.Count
        fatalCount = $fatalLines.Count
    }
    evidence = [pscustomobject][ordered]@{
        registeredLine = (ConvertTo-ProofLineText $registeredLine)
        cleanBootLine = (ConvertTo-ProofLineText $cleanBootLine)
        openedLine = (ConvertTo-ProofLineText $openedLine)
        selectorLine = (ConvertTo-ProofLineText $selectorLine)
        threeCameraLine = (ConvertTo-ProofLineText $threeCameraLine)
        threeCameraActorLine = (ConvertTo-ProofLineText $threeCameraActorLine)
        terminalLine = $terminalLine
        openmwLog = $CopiedLog
        sourceConfigDir = $ConfigDir
    }
    failures = @($failures.ToArray())
    policy = [pscustomobject][ordered]@{
        noRetailAssetsCommitted = $true
        generatedProofOutputOnly = $true
    }
}

$proof | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ProofJson -Encoding UTF8

Write-ProofLine "OpenMW log: $CopiedLog"
Write-ProofLine "Proof JSON: $ProofJson"
Write-ProofLine "Status: $status"
Write-ProofLine "RuntimeClassification: $runtimeClassification"
Write-ProofLine "RegisteredLine: $(ConvertTo-ProofLineText $registeredLine)"
Write-ProofLine "CleanBootLine: $(ConvertTo-ProofLineText $cleanBootLine)"
Write-ProofLine "OpenedLine: $(ConvertTo-ProofLineText $openedLine)"
Write-ProofLine "SelectorLine: $(ConvertTo-ProofLineText $selectorLine)"
Write-ProofLine "ThreeCameraLine: $(ConvertTo-ProofLineText $threeCameraLine)"
Write-ProofLine "ThreeCameraActorLine: $(ConvertTo-ProofLineText $threeCameraActorLine)"
Write-ProofLine "TerminalLine: $terminalLine"
foreach ($failure in $failures) {
    Write-ProofLine "Failure: $failure"
}

if ($RequirePass -and $status -ne "PASS") {
    throw "FNV native asset studio proof failed. See $ProofJson"
}
