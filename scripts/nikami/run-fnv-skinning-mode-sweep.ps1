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
    [string[]]$Modes = @("current", "auto"),
    [int]$RunSeconds = 60,
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
    [switch]$NoSound
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

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ProofRoot "fnv-skinning-mode-sweep\$Stamp"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$RequestedModes = @(
    foreach ($modeValue in $Modes) {
        foreach ($mode in ([string]$modeValue -split ",")) {
            $trimmed = $mode.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed)) {
                $trimmed
            }
        }
    }
)

function Add-Arg([System.Collections.Generic.List[string]]$ArgumentList, [string]$Name, $Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [double] -and [double]::IsNaN($Value)) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $ArgumentList.Add($Name)
    $ArgumentList.Add([string]$Value)
}

function Get-RegexValue([string]$Text, [string]$Pattern, [string]$Default = "") {
    $match = [regex]::Match($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $Default
}

function Get-RegexInt([string]$Text, [string]$Pattern, [int]$Default = -1) {
    $value = Get-RegexValue $Text $Pattern ""
    if ([string]::IsNullOrWhiteSpace($value)) { return $Default }
    return [int]$value
}

function Stop-RepoOpenMw {
    Get-Process openmw -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$RepoRoot\build-clean\Release\openmw.exe" } |
        Stop-Process -Force
}

$FlatProof = Join-Path $PSScriptRoot "run-fnv-flat-proof.ps1"
if (!(Test-Path -LiteralPath $FlatProof -PathType Leaf)) {
    throw "Missing flat proof runner: $FlatProof"
}

$previousMode = [Environment]::GetEnvironmentVariable("OPENMW_FNV_SKINNING_MODE", "Process")
$requiresVisibleHandGeometry = $ActorKind -ine "creature"
$results = @()
try {
    foreach ($mode in $RequestedModes) {
        if ([string]::IsNullOrWhiteSpace($mode)) { continue }
        Stop-RepoOpenMw
        [Environment]::SetEnvironmentVariable("OPENMW_FNV_SKINNING_MODE", $mode, "Process")

        $modeSafe = ($mode -replace '[^A-Za-z0-9_.-]', '_')
        $outputLog = Join-Path $RunDir "$modeSafe-output.log"

        $flatArgs = [System.Collections.Generic.List[string]]::new()
        $flatArgs.Add("-NoProfile")
        $flatArgs.Add("-ExecutionPolicy")
        $flatArgs.Add("Bypass")
        $flatArgs.Add("-File")
        $flatArgs.Add($FlatProof)
        Add-Arg $flatArgs "-BuildDir" $BuildDir
        Add-Arg $flatArgs "-Configuration" $Configuration
        Add-Arg $flatArgs "-FnvData" $FnvData
        Add-Arg $flatArgs "-FnvConfigData" $FnvConfigData
        Add-Arg $flatArgs "-VcpkgRoot" $VcpkgRoot
        Add-Arg $flatArgs "-ExtraOsgPluginDir" $ExtraOsgPluginDir
        Add-Arg $flatArgs "-Triplet" $Triplet
        Add-Arg $flatArgs "-ProofRoot" $ProofRoot
        Add-Arg $flatArgs "-RunSeconds" $RunSeconds
        Add-Arg $flatArgs "-BootstrapCell" $BootstrapCell
        Add-Arg $flatArgs "-BootstrapX" $BootstrapX
        Add-Arg $flatArgs "-BootstrapY" $BootstrapY
        Add-Arg $flatArgs "-BootstrapZ" $BootstrapZ
        Add-Arg $flatArgs "-BootstrapRotX" $BootstrapRotX
        Add-Arg $flatArgs "-BootstrapRotY" $BootstrapRotY
        Add-Arg $flatArgs "-BootstrapRotZ" $BootstrapRotZ
        Add-Arg $flatArgs "-BootstrapHour" $BootstrapHour
        Add-Arg $flatArgs "-ActorTarget" $ActorTarget
        Add-Arg $flatArgs "-ActorKind" $ActorKind
        $flatArgs.Add("-StageActor")
        $flatArgs.Add("-NeutralActorPreview")
        if ($ActorKind -ine "creature") {
            $flatArgs.Add("-NeutralActorPreviewStandingIdle")
        }
        Add-Arg $flatArgs "-NeutralActorPreviewProfile" $NeutralActorPreviewProfile
        Add-Arg $flatArgs "-ActorStageX" $ActorStageX
        Add-Arg $flatArgs "-ActorStageY" $ActorStageY
        Add-Arg $flatArgs "-ActorStageZ" $ActorStageZ
        Add-Arg $flatArgs "-ActorStageRotX" $ActorStageRotX
        Add-Arg $flatArgs "-ActorStageRotY" $ActorStageRotY
        Add-Arg $flatArgs "-ActorStageRotZ" $ActorStageRotZ
        Add-Arg $flatArgs "-ActorViewOffsetZ" $ActorViewOffsetZ
        Add-Arg $flatArgs "-ActorViewTargetZ" $ActorViewTargetZ
        $flatArgs.Add("-ActorViewLocalOffset")
        $flatArgs.Add("-FnvPartMatrixAudit")
        Add-Arg $flatArgs "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
        Add-Arg $flatArgs "-FnvRotationMode" $FnvRotationMode
        Add-Arg $flatArgs "-CharacterBuilderPhase" "full"
        if ($NoSound) { $flatArgs.Add("-NoSound") }

        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & powershell @flatArgs 2>&1
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        $text | Set-Content -LiteralPath $outputLog -Encoding UTF8

        $proofDir = Get-RegexValue $text '^ProofDir:\s+(.+)$'
        $openMwLog = ""
        if (![string]::IsNullOrWhiteSpace($proofDir)) {
            $candidate = Join-Path $proofDir "openmw.log"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $openMwLog = $candidate
            }
        }

        $openMwText = ""
        if (![string]::IsNullOrWhiteSpace($openMwLog)) {
            $openMwText = Get-Content -LiteralPath $openMwLog -Raw
        }
        $selectedModeLines = 0
        if (![string]::IsNullOrWhiteSpace($openMwText)) {
            $selectedModeLines = ([regex]::Matches($openMwText, "selected=$([regex]::Escape($mode))")).Count
        }

        $worldBad = Get-RegexInt $text 'Target world posture BAD lines:\s+([0-9]+)'
        $armBad = Get-RegexInt $text 'Target standing arm pose BAD lines:\s+([0-9]+)'
        $visibleHandStatus = Get-RegexValue $text 'Target visible hand geometry status:\s+([^\r\n]+)' "MISSING"
        $visibleHandSamples = Get-RegexValue $text 'Target visible hand geometry samples:\s+([^\r\n]+)' ""
        $firstFailingGate = ""
        if ($worldBad -gt 0) {
            $firstFailingGate = "target-world-posture"
        } elseif ($armBad -gt 0) {
            $firstFailingGate = "target-standing-arm-pose"
        } elseif ($requiresVisibleHandGeometry -and $visibleHandStatus -ne "PASS") {
            $firstFailingGate = "target-visible-hand-geometry"
        } elseif ($exitCode -ne 0) {
            if ($text -match "fatal/blocker log lines") {
                $firstFailingGate = "runtime-fatal-blocker-log"
            } elseif ($text -match "did not log target world posture") {
                $firstFailingGate = "target-world-posture-missing"
            } else {
                $firstFailingGate = "runtime-proof-exit"
            }
        }

        $classification = if ($exitCode -eq 0 -and [string]::IsNullOrWhiteSpace($firstFailingGate)) {
            "runtime-supported"
        } else {
            "loaded-pending-runtime"
        }

        $results += [pscustomobject][ordered]@{
            mode = $mode
            classification = $classification
            firstFailingGate = $firstFailingGate
            exitCode = $exitCode
            proofDir = $proofDir
            outputLog = $outputLog
            openmwLog = $openMwLog
            selectedModeLogLines = $selectedModeLines
            targetWorldPostureBad = $worldBad
            targetStandingArmPoseBad = $armBad
            targetVisibleHandGeometryRequired = $requiresVisibleHandGeometry
            targetVisibleHandGeometryStatus = $visibleHandStatus
            targetVisibleHandGeometrySamples = $visibleHandSamples
            payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
        }
    }
} finally {
    if ($null -eq $previousMode) {
        [Environment]::SetEnvironmentVariable("OPENMW_FNV_SKINNING_MODE", $null, "Process")
    } else {
        [Environment]::SetEnvironmentVariable("OPENMW_FNV_SKINNING_MODE", $previousMode, "Process")
    }
    Stop-RepoOpenMw
}

$best = $results |
    Sort-Object @{ Expression = { if ($_.targetVisibleHandGeometryStatus -eq "PASS") { 0 } else { 1 } } },
        targetStandingArmPoseBad, targetWorldPostureBad,
        @{ Expression = { if ($_.mode -eq "current") { 0 } else { 1 } } }, mode |
    Select-Object -First 1

$doc = [pscustomobject][ordered]@{
    schema = "nikami-fnv-skinning-mode-sweep-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $RunDir
    actorTarget = $ActorTarget
    actorKind = $ActorKind
    modes = $RequestedModes
    selectedBestMode = if ($null -ne $best) { $best.mode } else { "" }
    promoteToCpp = $false
    promotionPolicy = "Only promote when a mode improves proof gates without new runtime regressions."
    payloadPolicy = "generated proof metadata/log references only; no retail assets are committed"
    results = $results
}
$jsonPath = Join-Path $RunDir "skinning-mode-sweep.json"
$doc | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

$summaryPath = Join-Path $RunDir "summary.md"
$summary = @()
$summary += "# FNV Skinning Mode Sweep"
$summary += ""
$summary += "Actor: $ActorTarget"
$summary += "Visible hand geometry required: $requiresVisibleHandGeometry"
$summary += "Best observed mode: $($doc.selectedBestMode)"
$summary += "Promotion: false"
$summary += ""
$summary += "| Mode | Class | First failing gate | Exit | World BAD | Arm BAD | Hand required | Hand | Selected log lines |"
$summary += "| --- | --- | --- | ---: | ---: | ---: | --- | --- | ---: |"
foreach ($result in $results) {
    $summary += "| $($result.mode) | $($result.classification) | $($result.firstFailingGate) | $($result.exitCode) | $($result.targetWorldPostureBad) | $($result.targetStandingArmPoseBad) | $($result.targetVisibleHandGeometryRequired) | $($result.targetVisibleHandGeometryStatus) | $($result.selectedModeLogLines) |"
}
$summary += ""
$summary += "No retail payload bytes are written; proof rows reference generated logs only."
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host "FNV skinning mode sweep: $RunDir"
Write-Host "Sweep JSON: $jsonPath"
Write-Host "Summary: $summaryPath"
