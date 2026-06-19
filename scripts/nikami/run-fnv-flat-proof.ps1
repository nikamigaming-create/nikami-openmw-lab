param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$StartCell = "Goodsprings",
    [string]$ScreenshotFrames = "180,300,420",
    [int]$RunSeconds = 20,
    [double]$BootstrapX = -67735,
    [double]$BootstrapY = 3204,
    [double]$BootstrapZ = 8425,
    [double]$BootstrapRotX = 0,
    [double]$BootstrapRotY = 0,
    [double]$BootstrapRotZ = -0.6981317,
    [string]$BootstrapCell = "",
    [string]$ActorTarget = "",
    [int]$ActorFrame = 420,
    [switch]$StageActor,
    [double]$ActorStageX = -67480,
    [double]$ActorStageY = 2200,
    [double]$ActorStageZ = 8425,
    [double]$ActorStageRotZ = 1.5708,
    [double]$ActorViewOffsetX = 34,
    [double]$ActorViewOffsetY = 0,
    [double]$ActorViewOffsetZ = 102,
    [double]$ActorViewTargetZ = 116,
    [string]$GuiMode = "",
    [int]$GuiFrame = 240,
    [switch]$OpenInventory,
    [int]$OpenInventoryFrame = 240,
    [switch]$DisableSky,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$BuildPath = Join-Path $RepoRoot $BuildDir
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Exe = Join-Path $BuildPath "$Configuration/openmw.exe"
$ConfigDir = Join-Path $ProofRoot "configs/fnv-flat-clean"
$RuntimeDir = Join-Path $ProofRoot "runtime/fnv-flat-clean"
$ScreenshotDir = Join-Path $RuntimeDir "screenshots"
$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-flat-proof/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
$StdoutLog = Join-Path $ConfigDir "openmw-proof.stdout.log"
$StderrLog = Join-Path $ConfigDir "openmw-proof.stderr.log"

New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null
New-Item -ItemType Directory -Force -Path $ScreenshotDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Stop-LabOpenMW {
    Get-CimInstance Win32_Process -Filter "name = 'openmw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*nikami-openmw-lab-publish*" -or $_.CommandLine -like "*--crash-monitor*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Get-ProofPngQuality([string]$Path) {
    Add-Type -AssemblyName System.Drawing
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
                $c = $bmp.GetPixel($x, $y)
                $luma = (0.2126 * $c.R) + (0.7152 * $c.G) + (0.0722 * $c.B)
                $sum += $luma
                $sumSq += ($luma * $luma)
                if ($luma -lt 16) { $dark++ }
                if ($c.R -gt 180 -and $c.B -gt 160 -and $c.G -lt 80) { $markerPink++ }
                $bucket = (($c.R -shr 4) -shl 8) -bor (($c.G -shr 4) -shl 4) -bor ($c.B -shr 4)
                $null = $buckets.Add($bucket)
                $count++
            }
        }
        $mean = if ($count -gt 0) { $sum / $count } else { 0.0 }
        $variance = if ($count -gt 0) { [Math]::Max(0.0, ($sumSq / $count) - ($mean * $mean)) } else { 0.0 }
        $stddev = [Math]::Sqrt($variance)
        [pscustomobject]@{
            width = $bmp.Width
            height = $bmp.Height
            stddev = [Math]::Round($stddev, 3)
            buckets = $buckets.Count
            darkPercent = if ($count -gt 0) { [Math]::Round(100.0 * $dark / $count, 3) } else { 0.0 }
            markerPinkPercent = if ($count -gt 0) { [Math]::Round(100.0 * $markerPink / $count, 3) } else { 0.0 }
            usable = ($stddev -ge 4.0 -and $buckets.Count -ge 16)
        }
    }
    finally {
        if ($bmp) { $bmp.Dispose() }
    }
}

function Get-NewScreenshots([datetime]$StartedAt) {
    $paths = @()
    $LogFile = Join-Path $ConfigDir "openmw.log"
    if (Test-Path -LiteralPath $LogFile) {
        $saved = Select-String -LiteralPath $LogFile -Pattern "has been saved" -ErrorAction SilentlyContinue
        foreach ($line in $saved) {
            if ($line.Line -match "([A-Z]:\\.*?\.(png|jpg|jpeg|tga|bmp)) has been saved") {
                $paths += $Matches[1]
            }
        }
    }
    $paths += @(Get-ChildItem -LiteralPath $ScreenshotDir -Filter "*.png" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $StartedAt.AddSeconds(-2) } |
        ForEach-Object { $_.FullName })
    @($paths | Select-Object -Unique | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
}

try {
    Stop-LabOpenMW

    $prepArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        StartCell = $StartCell
        MaxRunSeconds = 1
    }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $prepArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $prepArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if ($NoSound) { $prepArgs.NoSound = $true }
    & (Join-Path $PSScriptRoot "run-fnv-flat.ps1") @prepArgs | Out-Host

    if (!(Test-Path -LiteralPath $Exe)) { throw "Missing executable: $Exe" }

    $RuntimeDllDirs = @(
        (Join-Path $VcpkgRoot "installed/$Triplet/bin"),
        (Join-Path $VcpkgRoot "installed/$Triplet/bin/$Configuration"),
        (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin"),
        (Join-Path $VcpkgRoot "installed/$Triplet/debug/bin/$Configuration")
    ) | Where-Object { Test-Path -LiteralPath $_ }
    $env:Path = ($RuntimeDllDirs + $env:Path) -join ";"
    $PathValue = $env:Path
    Remove-Item Env:PATH -ErrorAction SilentlyContinue
    $env:Path = $PathValue

    $OutputOsgPluginDir = Join-Path $BuildPath "$Configuration/osgPlugins-3.6.5"
    $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/plugins/osgPlugins-3.6.5"
    if ($Configuration -eq "Debug") {
        $OsgPluginDir = Join-Path $VcpkgRoot "installed/$Triplet/debug/plugins/osgPlugins-3.6.5"
    }
    $OsgPluginDirs = @($OutputOsgPluginDir, $OsgPluginDir, $ExtraOsgPluginDir) |
        Where-Object { ![string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_) }
    $env:OSG_LIBRARY_PATH = $OsgPluginDirs -join ";"

    Remove-Item -LiteralPath $StdoutLog,$StderrLog -Force -ErrorAction SilentlyContinue
    $env:OPENMW_PROOF_SCREENSHOT_FRAME = $ScreenshotFrames
    $env:OPENMW_FNV_BOOTSTRAP_HOUR = "12"
    $env:OPENMW_FNV_BOOTSTRAP_POS_X = [string]$BootstrapX
    $env:OPENMW_FNV_BOOTSTRAP_POS_Y = [string]$BootstrapY
    $env:OPENMW_FNV_BOOTSTRAP_POS_Z = [string]$BootstrapZ
    $env:OPENMW_FNV_BOOTSTRAP_ROT_X = [string]$BootstrapRotX
    $env:OPENMW_FNV_BOOTSTRAP_ROT_Y = [string]$BootstrapRotY
    $env:OPENMW_FNV_BOOTSTRAP_ROT_Z = [string]$BootstrapRotZ
    $env:OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE = "0"
    if (![string]::IsNullOrWhiteSpace($BootstrapCell)) {
        $env:OPENMW_FNV_BOOTSTRAP_CELL = $BootstrapCell
    }
    if ($DisableSky) { $env:OPENMW_PROOF_DISABLE_SKY = "1" }
    if ($OpenInventory -and [string]::IsNullOrWhiteSpace($GuiMode)) {
        $GuiMode = "items"
        $GuiFrame = $OpenInventoryFrame
    }
    if (![string]::IsNullOrWhiteSpace($GuiMode)) {
        $env:OPENMW_PROOF_GUI_MODE = $GuiMode
        $env:OPENMW_PROOF_GUI_FRAME = [string]$GuiFrame
        $env:OPENMW_FNV_INVENTORY_PLAYER_PROXY = "1"
    }
    if (![string]::IsNullOrWhiteSpace($ActorTarget)) {
        $env:OPENMW_PROOF_ACTOR_TARGET = $ActorTarget
        $env:OPENMW_PROOF_ACTOR_FRAME = [string]$ActorFrame
        $env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_X = [string]$ActorViewOffsetX
        $env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y = [string]$ActorViewOffsetY
        $env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z = [string]$ActorViewOffsetZ
        $env:OPENMW_PROOF_ACTOR_VIEW_TARGET_Z = [string]$ActorViewTargetZ
        $env:OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE = "0"
        if ($StageActor) {
            $env:OPENMW_PROOF_STAGE_ACTOR = "1"
            $env:OPENMW_PROOF_ACTOR_STAGE_X = [string]$ActorStageX
            $env:OPENMW_PROOF_ACTOR_STAGE_Y = [string]$ActorStageY
            $env:OPENMW_PROOF_ACTOR_STAGE_Z = [string]$ActorStageZ
            $env:OPENMW_PROOF_ACTOR_STAGE_ROT_X = "0"
            $env:OPENMW_PROOF_ACTOR_STAGE_ROT_Y = "0"
            $env:OPENMW_PROOF_ACTOR_STAGE_ROT_Z = [string]$ActorStageRotZ
        }
    }

    $OpenMwArgs = @("--replace", "config", "--config", $ConfigDir, "--user-data", $RuntimeDir, "--skip-menu", "--start", $StartCell, "--no-grab")
    if ($NoSound) { $OpenMwArgs += "--no-sound" }

    Write-ProofLine "FNV flat proof $Stamp"
    Write-ProofLine "Exe: $Exe"
    Write-ProofLine "Arguments: $($OpenMwArgs -join ' ')"
    Write-ProofLine "Screenshot frames: $ScreenshotFrames"
    if (![string]::IsNullOrWhiteSpace($BootstrapCell)) {
        Write-ProofLine "Bootstrap cell: $BootstrapCell"
    }
    if (![string]::IsNullOrWhiteSpace($ActorTarget)) {
        Write-ProofLine "Actor target: $ActorTarget frame=$ActorFrame stage=$([bool]$StageActor)"
    }
    if (![string]::IsNullOrWhiteSpace($GuiMode)) {
        Write-ProofLine "Proof GUI mode: $GuiMode frame=$GuiFrame"
    }
    Write-ProofLine "Proof dir: $ProofDir"

    $startedAt = Get-Date
    $process = Start-Process -FilePath $Exe -ArgumentList $OpenMwArgs -WorkingDirectory (Split-Path $Exe -Parent) -RedirectStandardOutput $StdoutLog -RedirectStandardError $StderrLog -PassThru
    Start-Sleep -Seconds $RunSeconds
    $process.Refresh()
    if (!$process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Write-ProofLine "Stopped OpenMW after $RunSeconds seconds."
    } else {
        Write-ProofLine "OpenMW exited with code $($process.ExitCode)."
    }

    $screenshots = @(Get-NewScreenshots $startedAt)
    Write-ProofLine ""
    Write-ProofLine "Screenshots captured: $($screenshots.Count)"
    $index = 0
    foreach ($shot in $screenshots) {
        $index++
        $copyName = "shot{0:00}_{1}" -f $index, (Split-Path -Leaf $shot)
        $copyPath = Join-Path $ProofDir $copyName
        Copy-Item -LiteralPath $shot -Destination $copyPath -Force
        $quality = Get-ProofPngQuality $copyPath
        $status = if ($quality.usable) { "PASS" } else { "WARN" }
        Write-ProofLine ("{0}: {1} {2}x{3} stddev={4} buckets={5} dark={6}% markerPink={7}%" -f `
            $status, $copyPath, $quality.width, $quality.height, $quality.stddev, $quality.buckets, $quality.darkPercent, $quality.markerPinkPercent)
    }

    foreach ($name in @("openmw.log", "MyGUI.log", "openmw-proof.stdout.log", "openmw-proof.stderr.log")) {
        $path = Join-Path $ConfigDir $name
        if (Test-Path -LiteralPath $path) {
            Copy-Item -LiteralPath $path -Destination (Join-Path $ProofDir $name) -Force
        }
    }

    $knownMissingMeshes = @(
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
    $realBlockerLines = @()
    $knownNoiseLines = @()
    $logPath = Join-Path $ConfigDir "openmw.log"
    if (Test-Path -LiteralPath $logPath) {
        $candidateLines = @(Select-String -LiteralPath $logPath -Pattern "Fatal error|Failed to start new game|unknown global|List of NPC classes|Resource 'meshes/base_anim|marker_error" -ErrorAction SilentlyContinue)
        foreach ($line in $candidateLines) {
            $isKnownNoise = $false
            foreach ($mesh in $knownMissingMeshes) {
                if ($line.Line -like "*$mesh*") {
                    $isKnownNoise = $true
                    break
                }
            }

            if ($isKnownNoise) {
                $knownNoiseLines += $line
            } else {
                $realBlockerLines += $line
            }
        }
    }
    Write-ProofLine ""
    Write-ProofLine "Real fatal/blocker lines: $($realBlockerLines.Count)"
    foreach ($line in ($realBlockerLines | Select-Object -Last 40)) {
        Write-ProofLine $line.Line
    }
    Write-ProofLine "Known tolerated missing default mesh lines: $($knownNoiseLines.Count)"
    foreach ($line in ($knownNoiseLines | Select-Object -Last 20)) {
        Write-ProofLine $line.Line
    }

    if ($screenshots.Count -eq 0) {
        throw "No proof screenshots were captured. See $SummaryFile and $logPath."
    }
}
finally {
    Remove-Item Env:OPENMW_PROOF_SCREENSHOT_FRAME -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_HOUR -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_POS_X -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_POS_Y -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_POS_Z -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_ROT_X -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_ROT_Y -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_ROT_Z -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_CAMERA_DISTANCE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_BOOTSTRAP_CELL -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_DISABLE_SKY -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_GUI_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_GUI_FRAME -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_FNV_INVENTORY_PLAYER_PROXY -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_TARGET -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_FRAME -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_X -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_Y -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_VIEW_OFFSET_Z -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_VIEW_TARGET_Z -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_VIEW_CAMERA_DISTANCE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_STAGE_ACTOR -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_X -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_Y -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_Z -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_ROT_X -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_ROT_Y -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_PROOF_ACTOR_STAGE_ROT_Z -ErrorAction SilentlyContinue
    Stop-LabOpenMW
}
