param(
    [ValidateSet("start", "send", "status")]
    [string]$Action = "send",
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string]$AssetClass = "",
    [string]$Record = "",
    [string]$Session = "cli-live",
    [string]$Model = "",
    [string]$View = "",
    [string]$Profile = "",
    [string]$Command = "apply",
    [string]$Field = "",
    [string]$Value = "",
    [string]$Zoom = "",
    [string]$PanX = "",
    [string]$PanZ = "",
    [string]$Tilt = "",
    [string]$RotX = "",
    [string]$RotY = "",
    [string]$RotZ = "",
    [string]$Scale = "",
    [switch]$StartWorld,
    [switch]$Wait
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$RuntimeName = "fnv-flat-clean-native-asset-studio"
$RuntimeDir = Join-Path $ProofRoot "runtime/$RuntimeName"
$ConfigDir = Join-Path $ProofRoot "configs/$RuntimeName"
$CommandFile = Join-Path $RuntimeDir "asset-studio-command.json"
$OpenMwLog = Join-Path $ConfigDir "openmw.log"
$Runner = Join-Path $PSScriptRoot "run-fnv-native-asset-studio.ps1"

function Get-OpenMwStudioProcess {
    $exe = Join-Path $RepoRoot "$BuildDir/$Configuration/openmw.exe"
    @(Get-CimInstance Win32_Process -Filter "name = 'openmw.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and ($_.ExecutablePath -ieq $exe) -and ($_.CommandLine -notmatch "--crash-monitor")
    })
}

function Get-OpenMwCrashMonitorProcess {
    $exe = Join-Path $RepoRoot "$BuildDir/$Configuration/openmw.exe"
    @(Get-CimInstance Win32_Process -Filter "name = 'openmw.exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and ($_.ExecutablePath -ieq $exe) -and ($_.CommandLine -match "--crash-monitor")
    })
}

function Start-AssetStudioIfNeeded {
    if (@(Get-OpenMwStudioProcess).Count -gt 0 -and (Test-Path -LiteralPath $CommandFile -PathType Leaf)) {
        return
    }

    if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
        throw "Missing native Asset Studio runner: $Runner"
    }

    $startAssetClass = if (![string]::IsNullOrWhiteSpace($AssetClass)) { $AssetClass } else { "npc" }
    $startRecord = if (![string]::IsNullOrWhiteSpace($Record)) { $Record } else { "Sunny Smiles" }
    $startView = if (![string]::IsNullOrWhiteSpace($View)) { $View } else { "front" }
    $startProfile = if (![string]::IsNullOrWhiteSpace($Profile)) { $Profile } else { "face" }
    $startModel = if (![string]::IsNullOrWhiteSpace($Model)) { $Model } else { "meshes\armor\headgear\cowboyhat\cowboyhat.nif" }

    & $Runner `
        -BuildDir $BuildDir `
        -Configuration $Configuration `
        -FnvData $FnvData `
        -FnvConfigData $FnvConfigData `
        -VcpkgRoot $VcpkgRoot `
        -Triplet $Triplet `
        -ProofRoot $ProofRoot `
        -AssetClass $startAssetClass `
        -Record $startRecord `
        -Session $Session `
        -Model $startModel `
        -View $startView `
        -ActorProfile $startProfile `
        -CommandFile $CommandFile `
        -StartWorld:$StartWorld | Out-Host

    for ($i = 0; $i -lt 80; $i++) {
        if (@(Get-OpenMwStudioProcess).Count -gt 0 -and (Test-Path -LiteralPath $CommandFile -PathType Leaf)) {
            return
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Asset Studio did not come online with command file: $CommandFile"
}

function Add-IfPresent([System.Collections.Specialized.OrderedDictionary]$Payload, [string]$Name, [string]$Text) {
    if (![string]::IsNullOrWhiteSpace($Text)) {
        $Payload[$Name] = $Text
    }
}

function Write-StudioCommand {
    New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
    $sequence = "{0}-{1}" -f (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmss.fffZ"), ([System.Guid]::NewGuid().ToString("N").Substring(0, 8))
    $payload = [ordered]@{
        schema = "nikami-fnv-asset-studio-command-v1"
        sequence = $sequence
        updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
        command = $Command
    }
    Add-IfPresent $payload "field" $Field
    Add-IfPresent $payload "value" $Value
    Add-IfPresent $payload "assetClass" $AssetClass
    Add-IfPresent $payload "record" $Record
    Add-IfPresent $payload "session" $Session
    Add-IfPresent $payload "model" $Model
    Add-IfPresent $payload "view" $View
    Add-IfPresent $payload "profile" $Profile
    Add-IfPresent $payload "zoom" $Zoom
    Add-IfPresent $payload "panX" $PanX
    Add-IfPresent $payload "panZ" $PanZ
    Add-IfPresent $payload "tilt" $Tilt
    Add-IfPresent $payload "rotX" $RotX
    Add-IfPresent $payload "rotY" $RotY
    Add-IfPresent $payload "rotZ" $RotZ
    Add-IfPresent $payload "scale" $Scale
    $payload["policy"] = [ordered]@{
        generatedProofOutputsOnly = $true
        noRetailPayloadBytes = $true
        nativeCliControlSurface = $true
        nativeAssetStudio = $true
    }

    $payload | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $CommandFile -Encoding UTF8
    return $sequence
}

function Wait-StudioCommand([string]$Sequence) {
    if (!$Wait) { return $false }
    for ($i = 0; $i -lt 80; $i++) {
        if (Test-Path -LiteralPath $OpenMwLog -PathType Leaf) {
            $needle = "sequence=`"$Sequence`""
            $match = Select-String -LiteralPath $OpenMwLog -Pattern $needle -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($null -ne $match) {
                Write-Host "applied: $($match.Line)"
                return $true
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for Asset Studio command sequence $Sequence"
}

if ($Action -eq "status") {
    $processes = @(Get-OpenMwStudioProcess)
    $monitors = @(Get-OpenMwCrashMonitorProcess)
    [pscustomobject][ordered]@{
        running = $processes.Count -gt 0
        processIds = @($processes | ForEach-Object { $_.ProcessId })
        crashMonitorProcessIds = @($monitors | ForEach-Object { $_.ProcessId })
        commandFile = $CommandFile
        commandFileExists = Test-Path -LiteralPath $CommandFile -PathType Leaf
        log = $OpenMwLog
    } | ConvertTo-Json -Depth 4
    exit 0
}

if ($Action -eq "start") {
    Start-AssetStudioIfNeeded
    Write-Host "Asset Studio online"
    Write-Host "CommandFile: $CommandFile"
    Write-Host "Log: $OpenMwLog"
    exit 0
}

Start-AssetStudioIfNeeded
$sequence = Write-StudioCommand
Write-Host "sent sequence=$sequence command=$Command field=$Field value=$Value"
Write-Host "CommandFile: $CommandFile"
Wait-StudioCommand $sequence | Out-Null
