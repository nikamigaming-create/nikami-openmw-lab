param(
    [string]$ProofRoot = "",
    [string]$LedgerJson = "",
    [string]$ResultJson = "",
    [string]$PlanJson = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [switch]$RunPlanned,
    [switch]$DryRun,
    [ValidateSet("all", "npc", "creature")]
    [string]$ActorKind = "all",
    [string]$Target = "",
    [ValidateSet("all", "actor-base-record", "placed-reference")]
    [string]$Source = "all",
    [int]$MaxEntries = 1,
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "760",
    [switch]$NoSound,
    [switch]$Serve,
    [int]$ServePort = 0,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Planner = Join-Path $PSScriptRoot "fnv_character_viewer_batch_plan.py"
$ViewerRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer.ps1"
if (!(Test-Path -LiteralPath $Planner -PathType Leaf)) {
    throw "Missing FNV character viewer batch planner: $Planner"
}
if (!(Test-Path -LiteralPath $ViewerRunner -PathType Leaf)) {
    throw "Missing FNV character viewer runner: $ViewerRunner"
}

function Find-Python {
    foreach ($candidate in @(
            @{ Command = "python"; Args = @() },
            @{ Command = "python.exe"; Args = @() },
            @{ Command = "py"; Args = @("-3") },
            @{ Command = "py.exe"; Args = @("-3") }
        )) {
        try {
            & ($candidate["Command"]) @($candidate["Args"]) --version *> $null
            if ($LASTEXITCODE -eq 0) {
                return [pscustomobject]@{ Command = $candidate["Command"]; Args = @($candidate["Args"]) }
            }
        }
        catch {
        }
    }
    throw "Python 3 is required to build the FNV character viewer batch plan."
}

$python = Find-Python
$generatedPlan = ""

function New-RunDirectory {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $runRoot = Join-Path $ProofRoot "fnv-character-viewer-batch-run"
    $runDir = Join-Path $runRoot $stamp
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    return $runDir
}

function ConvertTo-PlainArray($Value) {
    if ($null -eq $Value) { return @() }
    if ($Value -is [array]) { return @($Value) }
    return @($Value)
}

function Select-PlanEntries([object]$Plan) {
    $entries = @(ConvertTo-PlainArray $Plan.entries)
    if ($ActorKind -ne "all") {
        $entries = @($entries | Where-Object { $_.actorKind -eq $ActorKind })
    }
    if ($Source -ne "all") {
        $entries = @($entries | Where-Object { $_.source -eq $Source })
    }
    if (![string]::IsNullOrWhiteSpace($Target)) {
        $entries = @($entries | Where-Object { $_.target -eq $Target -or $_.actorEditorId -eq $Target -or $_.placedRefEditorId -eq $Target -or $_.actorFormId -eq $Target -or $_.placedRefFormId -eq $Target })
    }
    if ($MaxEntries -gt 0) {
        $entries = @($entries | Select-Object -First $MaxEntries)
    }
    return $entries
}

function Invoke-ViewerEntry([object]$Entry, [string]$RunDir) {
    $entryDir = Join-Path $RunDir ($Entry.id -replace '[^A-Za-z0-9_.-]', '_')
    New-Item -ItemType Directory -Force -Path $entryDir | Out-Null
    $entryJson = Join-Path $entryDir "entry.json"
    $Entry | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $entryJson -Encoding UTF8

    $phases = @(ConvertTo-PlainArray $Entry.phases)
    if ($phases.Count -eq 0) {
        throw "Planned entry has no phases: $($Entry.id)"
    }

    $result = [ordered]@{
        id = $Entry.id
        target = $Entry.target
        actorKind = $Entry.actorKind
        source = $Entry.source
        status = "PENDING"
        dryRun = [bool]$DryRun
        command = $Entry.command
        entryJson = $entryJson
        viewerIndex = ""
        viewerManifest = ""
        viewerServer = ""
        error = ""
    }

    if ($DryRun) {
        $result.status = "DRY-RUN"
        return [pscustomobject]$result
    }

    $before = @()
    $viewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
    if (Test-Path -LiteralPath $viewerRoot -PathType Container) {
        $before = @(Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    }
    $viewerArgs = @{
        ProofRoot = $ProofRoot
        Targets = @([string]$Entry.target)
        ActorKind = [string]$Entry.actorKind
        Phases = @($phases)
        RunSeconds = $RunSeconds
        ActorFrame = $ActorFrame
        ScreenshotFrames = $ScreenshotFrames
    }
    if ($Entry.actorKind -eq "creature") { $viewerArgs.CreatureDiagnostics = $true }
    if ($NoSound) { $viewerArgs.NoSound = $true }
    if ($Serve) { $viewerArgs.Serve = $true }
    if ($ServePort -gt 0) { $viewerArgs.ServePort = $ServePort }

    try {
        & $ViewerRunner @viewerArgs | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Viewer runner exited with code $LASTEXITCODE."
        }
        $after = @(Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
        $newDir = $null
        foreach ($dir in $after) {
            if ($before -notcontains $dir.FullName) {
                $newDir = $dir.FullName
                break
            }
        }
        if ([string]::IsNullOrWhiteSpace($newDir) -and $after.Count -gt 0) {
            $newDir = $after[0].FullName
        }
        $result.status = "PASS"
        $result.viewerIndex = if (![string]::IsNullOrWhiteSpace($newDir)) { Join-Path $newDir "index.html" } else { "" }
        $serverJson = if (![string]::IsNullOrWhiteSpace($newDir)) { Join-Path $newDir "viewer-server.json" } else { "" }
        if (![string]::IsNullOrWhiteSpace($serverJson) -and (Test-Path -LiteralPath $serverJson -PathType Leaf)) {
            $result.viewerServer = $serverJson
        }
    }
    catch {
        $result.status = "FAIL"
        $result.error = $_.Exception.Message
        if ($RequirePass) {
            return [pscustomobject]$result
        }
    }
    return [pscustomobject]$result
}

Write-Host "FNV character viewer batch plan"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "LedgerJson: $LedgerJson"
Write-Host "ResultJson: $ResultJson"
Write-Host "PlanJson: $PlanJson"
Write-Host "OutDir: $OutDir"
Write-Host "Limit: $Limit"
Write-Host "RunPlanned: $RunPlanned"
Write-Host "DryRun: $DryRun"
Write-Host "ActorKind: $ActorKind"
Write-Host "Target: $Target"
Write-Host "Source: $Source"
Write-Host "MaxEntries: $MaxEntries"
Write-Host "Policy: generated command/identifier plan only; no retail assets are committed"
Write-Host ""

if ([string]::IsNullOrWhiteSpace($PlanJson)) {
    $argsList = @()
    $argsList += $python.Args
    $argsList += $Planner
    $argsList += @("--proof-root", $ProofRoot)
    if (![string]::IsNullOrWhiteSpace($LedgerJson)) { $argsList += @("--ledger-json", $LedgerJson) }
    if (![string]::IsNullOrWhiteSpace($ResultJson)) { $argsList += @("--result-json", $ResultJson) }
    if (![string]::IsNullOrWhiteSpace($OutDir)) { $argsList += @("--out-dir", $OutDir) }
    if ($Limit -gt 0) { $argsList += @("--limit", [string]$Limit) }
    if ($RequirePass) { $argsList += "--require-pass" }

    & $python.Command @argsList
    if ($LASTEXITCODE -ne 0) {
        throw "FNV character viewer batch plan failed with exit code $LASTEXITCODE."
    }
    if (![string]::IsNullOrWhiteSpace($OutDir)) {
        $generatedPlan = Join-Path $OutDir "viewer-batch-plan.json"
    }
    else {
        $planRoot = Join-Path $ProofRoot "fnv-character-viewer-batch-plan"
        $latest = Get-ChildItem -LiteralPath $planRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $latest) {
            $generatedPlan = Join-Path $latest.FullName "viewer-batch-plan.json"
        }
    }
}
else {
    $generatedPlan = (Resolve-Path -LiteralPath $PlanJson).Path
}

if (!$RunPlanned -and !$DryRun) {
    return
}

if ([string]::IsNullOrWhiteSpace($generatedPlan) -or !(Test-Path -LiteralPath $generatedPlan -PathType Leaf)) {
    throw "Unable to resolve viewer batch plan JSON for execution."
}

$plan = Get-Content -LiteralPath $generatedPlan -Raw | ConvertFrom-Json
$selected = @(Select-PlanEntries $plan)
$runDir = New-RunDirectory
$selectedPath = Join-Path $runDir "selected-entries.json"
$selected | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $selectedPath -Encoding UTF8

$runResults = [System.Collections.Generic.List[object]]::new()
foreach ($entry in $selected) {
    $runResults.Add((Invoke-ViewerEntry $entry $runDir))
}

$failed = @($runResults | Where-Object { $_.status -eq "FAIL" })
$summary = [pscustomobject][ordered]@{
    schema = "nikami-fnv-character-viewer-batch-run-v1"
    status = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
    dryRun = [bool]$DryRun
    planJson = $generatedPlan
    selectedEntries = $selected.Count
    actorKind = $ActorKind
    target = $Target
    source = $Source
    maxEntries = $MaxEntries
    payloadPolicy = "generated run summary only; no retail assets are committed"
    selectedEntriesJson = $selectedPath
    results = @($runResults)
}
$summaryPath = Join-Path $runDir "viewer-batch-run.json"
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ""
Write-Host "Batch run status: $($summary.status)"
Write-Host "Batch run JSON: $summaryPath"
Write-Host "Selected entries JSON: $selectedPath"
if ($RequirePass -and $summary.status -ne "PASS") {
    throw "FNV character viewer batch run failed. See $summaryPath"
}
