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
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "760",
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

$diagonal = $ActorViewDistance * 0.7071067811865476
$Angles = @(
    [pscustomobject]@{ Name = "front"; OffsetX = $ActorViewDistance; OffsetY = 0.0 },
    [pscustomobject]@{ Name = "front-left"; OffsetX = $diagonal; OffsetY = -$diagonal },
    [pscustomobject]@{ Name = "front-right"; OffsetX = $diagonal; OffsetY = $diagonal }
)

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

Write-SuiteLine "FNV character builder tester $Stamp"
Write-SuiteLine "RepoRoot: $RepoRoot"
Write-SuiteLine "SuiteDir: $SuiteDir"
Write-SuiteLine "ActorTarget: $ActorTarget"
Write-SuiteLine "ActorKind: $ActorKind"
Write-SuiteLine "CreatureDiagnostics: $($CreatureDiagnostics -or $ActorKind -ieq 'creature')"
Write-SuiteLine "Phases: $($Phases -join ',')"
Write-SuiteLine "Angles: $(@($Angles | ForEach-Object { $_.Name }) -join ',')"
Write-SuiteLine "Policy: no retail assets copied into repo; generated proof output only"
Write-SuiteLine ""

$Results = New-Object "System.Collections.Generic.List[object]"

foreach ($phase in $Phases) {
    foreach ($angle in $Angles) {
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
            BootstrapCell = "FormId:0x10daeb9"
            BootstrapX = -67480
            BootstrapY = 1500
            BootstrapZ = 8425
            BootstrapRotX = 0
            BootstrapRotZ = 1.5708
            BootstrapHour = 10
            ActorTarget = $ActorTarget
            ActorKind = $ActorKind
            StageActor = $true
            ActorFrame = $ActorFrame
            ActorStageX = -67480
            ActorStageY = 1500
            ActorStageZ = 8425
            ActorStageRotZ = 1.5708
            ActorViewOffsetX = [double]$angle.OffsetX
            ActorViewOffsetY = [double]$angle.OffsetY
            ActorViewOffsetZ = $ActorViewOffsetZ
            ActorViewTargetZ = $ActorViewTargetZ
            ActorViewLocalOffset = $true
            FnvPartMatrixAudit = $true
            CharacterBuilderPhase = $phase
        }
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
