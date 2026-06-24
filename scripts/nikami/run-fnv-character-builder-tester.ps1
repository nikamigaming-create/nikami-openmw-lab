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
    [string]$ActorKitAnimationGroup = "",
    [string]$ActorKitDialogueMode = "",
    [string[]]$Angles = @("front", "front-left", "front-right"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "760",
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
Write-SuiteLine "ActorKind: $ActorKind"
Write-SuiteLine "CreatureDiagnostics: $($CreatureDiagnostics -or $ActorKind -ieq 'creature')"
Write-SuiteLine "Phases: $($Phases -join ',')"
Write-SuiteLine "ActorKitParts: $ActorKitPartsCsv"
Write-SuiteLine "ActorKitPartModels: $ActorKitPartModelsCsv"
Write-SuiteLine "ActorKitPropSlots: $ActorKitPropSlotsCsv"
Write-SuiteLine "ActorKitPropModels: $ActorKitPropModelsCsv"
Write-SuiteLine "ActorKitAnimationGroup: $ActorKitAnimationGroup"
Write-SuiteLine "ActorKitDialogueMode: $ActorKitDialogueMode"
Write-SuiteLine "Angles: $(@($CameraAngles | ForEach-Object { $_.Name }) -join ',')"
Write-SuiteLine "BootstrapCell: $BootstrapCell"
Write-SuiteLine "BootstrapPosition: $BootstrapX,$BootstrapY,$BootstrapZ"
Write-SuiteLine "BootstrapRotation: $BootstrapRotX,$BootstrapRotY,$BootstrapRotZ"
Write-SuiteLine "ActorStagePosition: $ActorStageX,$ActorStageY,$ActorStageZ"
Write-SuiteLine "ActorStageRotation: $ActorStageRotX,$ActorStageRotY,$ActorStageRotZ"
Write-SuiteLine "Policy: no retail assets copied into repo; generated proof output only"
Write-SuiteLine ""

$Results = New-Object "System.Collections.Generic.List[object]"

foreach ($phase in $Phases) {
    foreach ($angle in $CameraAngles) {
        $safePhase = ConvertTo-SafeName $phase
        $safeAngle = ConvertTo-SafeName $angle.Name
        $caseName = "${safePhase}_${safeAngle}"
        $CaseDir = Join-Path $SuiteDir $caseName
        New-Item -ItemType Directory -Force -Path $CaseDir | Out-Null

        Write-SuiteLine "CASE $caseName"
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
            CharacterBuilderPhase = $phase
        }
        if (![string]::IsNullOrWhiteSpace($ActorKitPartsCsv)) { $proofArgs.ActorKitParts = $ActorKitPartsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPartModelsCsv)) { $proofArgs.ActorKitPartModels = $ActorKitPartModelsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPropSlotsCsv)) { $proofArgs.ActorKitPropSlots = $ActorKitPropSlotsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitPropModelsCsv)) { $proofArgs.ActorKitPropModels = $ActorKitPropModelsCsv }
        if (![string]::IsNullOrWhiteSpace($ActorKitAnimationGroup)) { $proofArgs.ActorKitAnimationGroup = $ActorKitAnimationGroup }
        if (![string]::IsNullOrWhiteSpace($ActorKitDialogueMode)) { $proofArgs.ActorKitDialogueMode = $ActorKitDialogueMode }
        if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $proofArgs.FnvConfigData = $FnvConfigData }
        if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $proofArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
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
                animationGroup = $ActorKitAnimationGroup
                dialogueMode = $ActorKitDialogueMode
            }
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
