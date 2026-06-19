param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [int]$RunSeconds = 20,
    [int]$ActorFrame = 620,
    [string]$ScreenshotFrames = "640",
    [double]$ActorViewOffsetX = 18,
    [double]$ActorViewOffsetY = 0,
    [double]$ActorViewOffsetZ = 70,
    [double]$ActorViewTargetZ = 78,
    [int]$CropX = 550,
    [int]$CropY = 315,
    [int]$CropWidth = 360,
    [int]$CropHeight = 340,
    [int]$ZoomWidth = 900,
    [int]$ZoomHeight = 850,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
$SweepStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$SweepDir = Join-Path $ProofRoot "fnv-flat-proof/easy-pete-angle-sweep-$SweepStamp"
New-Item -ItemType Directory -Force -Path $SweepDir | Out-Null

Add-Type -AssemblyName System.Drawing

function Copy-ZoomedCrop {
    param(
        [string]$SourcePath,
        [string]$OutputPath
    )

    $bmp = [System.Drawing.Bitmap]::new($SourcePath)
    try {
        $crop = [System.Drawing.Rectangle]::new($CropX, $CropY, $CropWidth, $CropHeight)
        $cropped = $bmp.Clone($crop, $bmp.PixelFormat)
        try {
            $scaled = [System.Drawing.Bitmap]::new($ZoomWidth, $ZoomHeight)
            try {
                $graphics = [System.Drawing.Graphics]::FromImage($scaled)
                try {
                    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
                    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
                    $graphics.DrawImage($cropped, 0, 0, $ZoomWidth, $ZoomHeight)
                }
                finally {
                    $graphics.Dispose()
                }

                $scaled.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
            }
            finally {
                $scaled.Dispose()
            }
        }
        finally {
            $cropped.Dispose()
        }
    }
    finally {
        $bmp.Dispose()
    }
}

$angles = @(
    @{ Name = "back"; RotZ = 0.0 },
    @{ Name = "front"; RotZ = 1.5708 },
    @{ Name = "side"; RotZ = 3.1416 }
)

foreach ($angle in $angles) {
    $before = @(Get-ChildItem -LiteralPath (Join-Path $ProofRoot "fnv-flat-proof") -Directory -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName)

    $proofArgs = @{
        BuildDir = $BuildDir
        Configuration = $Configuration
        FnvData = $FnvData
        VcpkgRoot = $VcpkgRoot
        Triplet = $Triplet
        ProofRoot = $ProofRoot
        RunSeconds = $RunSeconds
        ScreenshotFrames = $ScreenshotFrames
        BootstrapCell = "FormId:0x10daeb9"
        BootstrapX = -67480
        BootstrapY = 1500
        BootstrapZ = 8425
        BootstrapRotX = 0
        BootstrapRotZ = 1.5708
        ActorTarget = "GSEasyPete"
        StageActor = $true
        ActorFrame = $ActorFrame
        ActorStageX = -67480
        ActorStageY = 1500
        ActorStageZ = 8425
        ActorStageRotZ = [double]$angle.RotZ
        ActorViewOffsetX = $ActorViewOffsetX
        ActorViewOffsetY = $ActorViewOffsetY
        ActorViewOffsetZ = $ActorViewOffsetZ
        ActorViewTargetZ = $ActorViewTargetZ
    }
    if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
    if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
    if ($NoSound) { $proofArgs.NoSound = $true }

    & $FlatProof @proofArgs | Out-Host

    $latest = Get-ChildItem -LiteralPath (Join-Path $ProofRoot "fnv-flat-proof") -Directory |
        Where-Object { $before -notcontains $_.FullName } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "Unable to find proof directory for angle $($angle.Name)"
    }

    $shot = Get-ChildItem -LiteralPath $latest.FullName -Filter "*.png" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $shot) {
        throw "No screenshot found in $($latest.FullName)"
    }

    Copy-Item -LiteralPath $shot.FullName -Destination (Join-Path $SweepDir "$($angle.Name)_source.png") -Force
    Copy-Item -LiteralPath (Join-Path $latest.FullName "summary.txt") -Destination (Join-Path $SweepDir "$($angle.Name)_summary.txt") -Force
    Copy-Item -LiteralPath (Join-Path $latest.FullName "openmw.log") -Destination (Join-Path $SweepDir "$($angle.Name)_openmw.log") -Force
    Copy-ZoomedCrop -SourcePath $shot.FullName -OutputPath (Join-Path $SweepDir "$($angle.Name)_zoom.png")
}

Write-Host "Easy Pete angle sweep:"
Write-Host "  $SweepDir"
Get-ChildItem -LiteralPath $SweepDir -Filter "*_zoom.png" | Select-Object FullName, Length
