param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string[]]$Fixtures = @(
        "label=easy-pete-base;target=FormId:0x00104c7f;kind=npc",
        "label=sunny-smiles-base;target=FormId:0x00104e84;kind=npc",
        "label=doc-mitchell-base;target=FormId:0x00104c0c;kind=npc",
        "label=fire-gecko-base;target=NVCrV19FireGeckoRanged;kind=creature",
        "label=securitron-base;target=VStreetSecuritronMk2;kind=creature"
    ),
    [string[]]$Modes = @("current", "auto"),
    [int]$RunSeconds = 45,
    [int]$MaxFixtures = 0,
    [string]$NeutralActorPreviewProfile = "audit",
    [string]$FnvRotationMode = "bindCoreBindLowerSplitUpper",
    [string]$FnvSkinningMatrixAudit = "arms,rightHand,leftHand,HeadOld",
    [switch]$NoSound,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ProofRoot "fnv-skinning-fixture-sweep\$Stamp"
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$ModeSweep = Join-Path $PSScriptRoot "run-fnv-skinning-mode-sweep.ps1"
if (!(Test-Path -LiteralPath $ModeSweep -PathType Leaf)) {
    throw "Missing skinning mode sweep runner: $ModeSweep"
}

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

function ConvertTo-Fixture([string]$Spec) {
    $fixture = [ordered]@{
        label = ""
        target = ""
        kind = "npc"
        bootstrapCell = ""
        bootstrapX = $null
        bootstrapY = $null
        bootstrapZ = $null
        bootstrapRotX = $null
        bootstrapRotY = $null
        bootstrapRotZ = $null
        actorStageX = $null
        actorStageY = $null
        actorStageZ = $null
        actorStageRotX = $null
        actorStageRotY = $null
        actorStageRotZ = $null
        actorViewOffsetZ = $null
        actorViewTargetZ = $null
    }

    $trimmedSpec = $Spec.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmedSpec)) {
        throw "Empty fixture spec."
    }

    if ($trimmedSpec.Contains("=")) {
        foreach ($pair in ($trimmedSpec -split ";")) {
            if ([string]::IsNullOrWhiteSpace($pair)) { continue }
            $parts = $pair -split "=", 2
            if ($parts.Count -ne 2) {
                throw "Invalid fixture key/value segment '$pair' in '$Spec'."
            }
            $key = $parts[0].Trim()
            $value = $parts[1].Trim()
            switch -Regex ($key) {
                "^(label|name)$" { $fixture.label = $value; break }
                "^(target|actorTarget)$" { $fixture.target = $value; break }
                "^(kind|actorKind)$" { $fixture.kind = $value; break }
                "^bootstrapCell$" { $fixture.bootstrapCell = $value; break }
                "^bootstrapX$" { $fixture.bootstrapX = [double]$value; break }
                "^bootstrapY$" { $fixture.bootstrapY = [double]$value; break }
                "^bootstrapZ$" { $fixture.bootstrapZ = [double]$value; break }
                "^bootstrapRotX$" { $fixture.bootstrapRotX = [double]$value; break }
                "^bootstrapRotY$" { $fixture.bootstrapRotY = [double]$value; break }
                "^bootstrapRotZ$" { $fixture.bootstrapRotZ = [double]$value; break }
                "^actorStageX$" { $fixture.actorStageX = [double]$value; break }
                "^actorStageY$" { $fixture.actorStageY = [double]$value; break }
                "^actorStageZ$" { $fixture.actorStageZ = [double]$value; break }
                "^actorStageRotX$" { $fixture.actorStageRotX = [double]$value; break }
                "^actorStageRotY$" { $fixture.actorStageRotY = [double]$value; break }
                "^actorStageRotZ$" { $fixture.actorStageRotZ = [double]$value; break }
                "^actorViewOffsetZ$" { $fixture.actorViewOffsetZ = [double]$value; break }
                "^actorViewTargetZ$" { $fixture.actorViewTargetZ = [double]$value; break }
                default { throw "Unknown fixture key '$key' in '$Spec'." }
            }
        }
    } else {
        $parts = $trimmedSpec -split "\|"
        if ($parts.Count -lt 2 -or $parts.Count -gt 3) {
            throw "Fixture specs must be 'label|target|kind' or key/value pairs: $Spec"
        }
        $fixture.label = $parts[0].Trim()
        $fixture.target = $parts[1].Trim()
        if ($parts.Count -eq 3) {
            $fixture.kind = $parts[2].Trim()
        }
    }

    if ([string]::IsNullOrWhiteSpace($fixture.target)) {
        throw "Fixture missing target: $Spec"
    }
    if ([string]::IsNullOrWhiteSpace($fixture.label)) {
        $fixture.label = $fixture.target
    }
    if ($fixture.kind -notin @("npc", "creature", "auto")) {
        throw "Fixture kind must be npc, creature, or auto: $Spec"
    }

    [pscustomobject]$fixture
}

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

function Get-ResultScore([object]$Result) {
    if ($null -eq $Result) { return [int]::MaxValue }
    $handPenalty = if ($Result.targetVisibleHandGeometryStatus -eq "PASS") { 0 } else { 10000 }
    $armPenalty = [Math]::Max(0, [int]$Result.targetStandingArmPoseBad) * 1000
    $worldPenalty = [Math]::Max(0, [int]$Result.targetWorldPostureBad) * 100
    $exitPenalty = if ([int]$Result.exitCode -eq 0) { 0 } else { 10 }
    return $handPenalty + $armPenalty + $worldPenalty + $exitPenalty
}

function New-DryRunResult([object]$Fixture) {
    [pscustomobject][ordered]@{
        label = $Fixture.label
        target = $Fixture.target
        actorKind = $Fixture.kind
        classification = "loaded-pending-runtime"
        firstFailingGate = "dry-run-no-runtime-proof"
        sweepExitCode = 0
        sweepJson = ""
        sweepSummary = ""
        selectedBestMode = ""
        currentScore = [int]::MaxValue
        bestCandidateMode = ""
        bestCandidateScore = [int]::MaxValue
        improvesCurrent = $false
        results = @()
        payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
    }
}

$fixtureObjects = @($Fixtures | ForEach-Object { ConvertTo-Fixture $_ })
if ($MaxFixtures -gt 0) {
    $fixtureObjects = @($fixtureObjects | Select-Object -First $MaxFixtures)
}

$fixtureResults = @()
foreach ($fixture in $fixtureObjects) {
    if ($DryRun) {
        $fixtureResults += New-DryRunResult $fixture
        continue
    }

    $labelSafe = ($fixture.label -replace '[^A-Za-z0-9_.-]', '_')
    $outputLog = Join-Path $RunDir "$labelSafe-output.log"

    $childArgs = [System.Collections.Generic.List[string]]::new()
    $childArgs.Add("-NoProfile")
    $childArgs.Add("-ExecutionPolicy")
    $childArgs.Add("Bypass")
    $childArgs.Add("-File")
    $childArgs.Add($ModeSweep)
    Add-Arg $childArgs "-BuildDir" $BuildDir
    Add-Arg $childArgs "-Configuration" $Configuration
    Add-Arg $childArgs "-FnvData" $FnvData
    Add-Arg $childArgs "-FnvConfigData" $FnvConfigData
    Add-Arg $childArgs "-VcpkgRoot" $VcpkgRoot
    Add-Arg $childArgs "-ExtraOsgPluginDir" $ExtraOsgPluginDir
    Add-Arg $childArgs "-Triplet" $Triplet
    Add-Arg $childArgs "-ProofRoot" $ProofRoot
    Add-Arg $childArgs "-ActorTarget" $fixture.target
    Add-Arg $childArgs "-ActorKind" $fixture.kind
    Add-Arg $childArgs "-Modes" ($RequestedModes -join ",")
    Add-Arg $childArgs "-RunSeconds" $RunSeconds
    Add-Arg $childArgs "-NeutralActorPreviewProfile" $NeutralActorPreviewProfile
    Add-Arg $childArgs "-FnvRotationMode" $FnvRotationMode
    Add-Arg $childArgs "-FnvSkinningMatrixAudit" $FnvSkinningMatrixAudit
    Add-Arg $childArgs "-BootstrapCell" $fixture.bootstrapCell
    Add-Arg $childArgs "-BootstrapX" $fixture.bootstrapX
    Add-Arg $childArgs "-BootstrapY" $fixture.bootstrapY
    Add-Arg $childArgs "-BootstrapZ" $fixture.bootstrapZ
    Add-Arg $childArgs "-BootstrapRotX" $fixture.bootstrapRotX
    Add-Arg $childArgs "-BootstrapRotY" $fixture.bootstrapRotY
    Add-Arg $childArgs "-BootstrapRotZ" $fixture.bootstrapRotZ
    Add-Arg $childArgs "-ActorStageX" $fixture.actorStageX
    Add-Arg $childArgs "-ActorStageY" $fixture.actorStageY
    Add-Arg $childArgs "-ActorStageZ" $fixture.actorStageZ
    Add-Arg $childArgs "-ActorStageRotX" $fixture.actorStageRotX
    Add-Arg $childArgs "-ActorStageRotY" $fixture.actorStageRotY
    Add-Arg $childArgs "-ActorStageRotZ" $fixture.actorStageRotZ
    Add-Arg $childArgs "-ActorViewOffsetZ" $fixture.actorViewOffsetZ
    Add-Arg $childArgs "-ActorViewTargetZ" $fixture.actorViewTargetZ
    if ($NoSound) { $childArgs.Add("-NoSound") }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & powershell @childArgs 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    $text | Set-Content -LiteralPath $outputLog -Encoding UTF8
    $sweepJson = Get-RegexValue $text '^Sweep JSON:\s+(.+)$'
    $sweepSummary = Get-RegexValue $text '^Summary:\s+(.+)$'

    if ([string]::IsNullOrWhiteSpace($sweepJson) -or !(Test-Path -LiteralPath $sweepJson -PathType Leaf)) {
        $fixtureResults += [pscustomobject][ordered]@{
            label = $fixture.label
            target = $fixture.target
            actorKind = $fixture.kind
            classification = "known-blocked"
            firstFailingGate = "skinning-mode-sweep-runner"
            sweepExitCode = $exitCode
            sweepJson = $sweepJson
            sweepSummary = $sweepSummary
            outputLog = $outputLog
            selectedBestMode = ""
            currentScore = [int]::MaxValue
            bestCandidateMode = ""
            bestCandidateScore = [int]::MaxValue
            improvesCurrent = $false
            results = @()
            payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
        }
        continue
    }

    $sweep = Get-Content -LiteralPath $sweepJson -Raw | ConvertFrom-Json
    $results = @($sweep.results)
    $current = $results | Where-Object { $_.mode -eq "current" } | Select-Object -First 1
    $candidate = $results |
        Where-Object { $_.mode -ne "current" } |
        Sort-Object @{ Expression = { Get-ResultScore $_ } }, mode |
        Select-Object -First 1
    $currentScore = Get-ResultScore $current
    $candidateScore = Get-ResultScore $candidate
    $improvesCurrent = $null -ne $candidate -and $candidateScore -lt $currentScore
    $firstFailingGate = if ($null -ne $current -and ![string]::IsNullOrWhiteSpace($current.firstFailingGate)) {
        $current.firstFailingGate
    } elseif ($null -ne $candidate -and ![string]::IsNullOrWhiteSpace($candidate.firstFailingGate)) {
        $candidate.firstFailingGate
    } else {
        ""
    }

    $fixtureResults += [pscustomobject][ordered]@{
        label = $fixture.label
        target = $fixture.target
        actorKind = $fixture.kind
        classification = if ($results | Where-Object { $_.classification -eq "runtime-supported" }) { "runtime-supported" } else { "loaded-pending-runtime" }
        firstFailingGate = $firstFailingGate
        sweepExitCode = $exitCode
        sweepJson = $sweepJson
        sweepSummary = $sweepSummary
        outputLog = $outputLog
        selectedBestMode = $sweep.selectedBestMode
        currentScore = $currentScore
        bestCandidateMode = if ($null -ne $candidate) { $candidate.mode } else { "" }
        bestCandidateScore = $candidateScore
        improvesCurrent = $improvesCurrent
        results = $results
        payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
    }
}

$candidateRows = @($fixtureResults | Where-Object { $_.improvesCurrent -and ![string]::IsNullOrWhiteSpace($_.bestCandidateMode) })
$promotionCandidateMode = ""
$promotionEligible = $false
if ($candidateRows.Count -eq $fixtureResults.Count -and $candidateRows.Count -gt 0) {
    $candidateModes = @($candidateRows | Select-Object -ExpandProperty bestCandidateMode -Unique)
    if ($candidateModes.Count -eq 1) {
        $promotionCandidateMode = $candidateModes[0]
        $promotionEligible = $true
    }
}

$promotionBlockers = @()
if (!$promotionEligible) {
    $promotionBlockers += "No non-current skinning mode improved every fixture in the sweep."
}
$promotionBlockers += "C++ promotion still requires an explicit source change, rebuild, and rerun of the fixture sweep."

$doc = [pscustomobject][ordered]@{
    schema = "nikami-fnv-skinning-fixture-sweep-v1"
    createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    runDir = $RunDir
    dryRun = [bool]$DryRun
    modes = $RequestedModes
    fixtureCount = $fixtureResults.Count
    promotionCandidateMode = $promotionCandidateMode
    promotionEligible = $promotionEligible
    promoteToCpp = $false
    promotionPolicy = "Only promote a generic C++ skinning default after a non-current mode improves every fixture and a rebuilt runtime passes the same fixture sweep."
    promotionBlockers = $promotionBlockers
    payloadPolicy = "generated proof metadata/log references only; no retail assets are committed"
    fixtures = $fixtureResults
}

$jsonPath = Join-Path $RunDir "skinning-fixture-sweep.json"
$doc | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

$summaryPath = Join-Path $RunDir "summary.md"
$summary = @()
$summary += "# FNV Skinning Fixture Sweep"
$summary += ""
$summary += "Fixtures: $($fixtureResults.Count)"
$summary += "Modes: $($RequestedModes -join ', ')"
$summary += "Promotion eligible: $promotionEligible"
$summary += "Promotion candidate mode: $promotionCandidateMode"
$summary += "Promotion: false"
$summary += ""
$summary += "| Fixture | Target | Kind | Class | First failing gate | Best mode | Candidate | Improves current | Current score | Candidate score |"
$summary += "| --- | --- | --- | --- | --- | --- | --- | --- | ---: | ---: |"
foreach ($result in $fixtureResults) {
    $summary += "| $($result.label) | $($result.target) | $($result.actorKind) | $($result.classification) | $($result.firstFailingGate) | $($result.selectedBestMode) | $($result.bestCandidateMode) | $($result.improvesCurrent) | $($result.currentScore) | $($result.bestCandidateScore) |"
}
$summary += ""
$summary += "No retail payload bytes are written; fixture rows reference generated proof logs and JSON only."
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host "FNV skinning fixture sweep: $RunDir"
Write-Host "Sweep JSON: $jsonPath"
Write-Host "Summary: $summaryPath"
