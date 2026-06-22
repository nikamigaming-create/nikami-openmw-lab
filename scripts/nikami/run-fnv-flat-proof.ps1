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
    [string]$ScreenshotFrames = "",
    [string[]]$RequireLogPattern = @(),
    [string]$TerrainProbePoints = "",
    [string]$TerrainProbeGrid = "",
    [switch]$RequireTerrainProbeFullSupport,
    [string]$BootstrapCell = "",
    [double]$BootstrapX = [double]::NaN,
    [double]$BootstrapY = [double]::NaN,
    [double]$BootstrapZ = [double]::NaN,
    [double]$BootstrapRotX = [double]::NaN,
    [double]$BootstrapRotY = [double]::NaN,
    [double]$BootstrapRotZ = [double]::NaN,
    [double]$BootstrapCameraDistance = [double]::NaN,
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
    [switch]$FnvPartMatrixAudit,
    [switch]$FnvDisableNativeAnimationCallbacks,
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

function Assert-LogPattern([string]$Path, [string]$Pattern) {
    if ((Count-LogMatches $Path $Pattern) -eq 0) {
        throw "Missing required OpenMW log pattern: $Pattern"
    }
    Write-ProofLine "OK required log pattern: $Pattern"
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
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $flatArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $flatArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if ($WithMenu) { $flatArgs.WithMenu = $true }
    if ($NoSound) { $flatArgs.NoSound = $true }

    Write-ProofLine "FNV flat runtime proof $Stamp"
    Write-ProofLine "RepoRoot: $RepoRoot"
    Write-ProofLine "ProofDir: $ProofDir"
    Write-ProofLine "RunSeconds: $RunSeconds"
    Write-ProofLine "WithMenu: $WithMenu"
    Write-ProofLine "BootstrapCell: $BootstrapCell"
    Write-ProofLine "TerrainProbePoints: $probePoints"
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
$screenshots = @(Get-ChildItem -LiteralPath $ProofDir -Filter "*.png" -File -ErrorAction SilentlyContinue)

Write-ProofLine ""
Write-ProofLine "Artifacts:"
Write-ProofLine "OpenMW log: $OpenMwLog"
Write-ProofLine "MyGUI log: $MyGuiLog"
Write-ProofLine "Stdout log: $StdoutLog"
Write-ProofLine "Stderr log: $StderrLog"
Write-ProofLine "Screenshots: $($screenshots.Count)"
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

if ($fatalCount -gt 0) { throw "FNV flat proof saw fatal/blocker log lines. See $OpenMwLog" }
if ($RequireFlatCameraSettled -and $flatCameraSettledLines -eq 0) { throw "FNV flat proof did not prove flat camera settlement. See $OpenMwLog" }
if ($flatCameraFailureLines -gt 0) { throw "FNV flat proof saw flat camera failure lines. See $OpenMwLog" }
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
