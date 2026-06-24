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

function ConvertTo-HtmlText([object]$Value) {
    return [System.Net.WebUtility]::HtmlEncode([string]$Value)
}

function ConvertTo-RelativeHref([string]$BaseFile, [string]$TargetPath) {
    if ([string]::IsNullOrWhiteSpace($TargetPath) -or !(Test-Path -LiteralPath $TargetPath)) {
        return ""
    }
    $baseDirectory = Split-Path $BaseFile -Parent
    if ([string]::IsNullOrWhiteSpace($baseDirectory)) {
        $baseDirectory = (Get-Location).Path
    }
    if (Test-Path -LiteralPath $baseDirectory -PathType Container) {
        $baseDirectory = (Resolve-Path -LiteralPath $baseDirectory).Path
    }
    if (!$baseDirectory.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseDirectory += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetResolved = (Resolve-Path -LiteralPath $TargetPath).Path
    $relative = ([uri]$baseDirectory).MakeRelativeUri([uri]$targetResolved).ToString()
    return [uri]::UnescapeDataString($relative)
}

function New-LinkHtml([string]$BaseFile, [string]$TargetPath, [string]$Label) {
    $labelText = ConvertTo-HtmlText $Label
    $href = ConvertTo-RelativeHref $BaseFile $TargetPath
    if ([string]::IsNullOrWhiteSpace($href)) {
        return $labelText
    }
    return "<a href=`"$(ConvertTo-HtmlText $href)`">$labelText</a>"
}

function Write-BatchRunMarkdown([string]$Path, [object]$Summary) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# FNV Character Viewer Batch Run")
    $lines.Add("")
    $lines.Add("Status: **$($Summary.status)**")
    $lines.Add("Dry run: ``$($Summary.dryRun)``")
    $lines.Add("Selected entries: $($Summary.selectedEntries)")
    $lines.Add("Actor kind filter: ``$($Summary.actorKind)``")
    $lines.Add("Target filter: ``$($Summary.target)``")
    $lines.Add("Source filter: ``$($Summary.source)``")
    $lines.Add("Plan: ``$($Summary.planJson)``")
    $lines.Add("Policy: $($Summary.payloadPolicy)")
    $lines.Add("")
    $lines.Add("| Status | Kind | Source | Target | Command | Viewer | Entry JSON |")
    $lines.Add("|---|---|---|---|---|---|---|")
    foreach ($result in @($Summary.results)) {
        $viewer = if (![string]::IsNullOrWhiteSpace($result.viewerIndex)) { $result.viewerIndex } elseif (![string]::IsNullOrWhiteSpace($result.viewerServer)) { $result.viewerServer } else { "" }
        $command = ([string]$result.command).Replace("|", "\|")
        $lines.Add("| $($result.status) | $($result.actorKind) | $($result.source) | ``$($result.target)`` | ``$command`` | ``$viewer`` | ``$($result.entryJson)`` |")
    }
    $lines.Add("")
    $Path | Split-Path -Parent | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-BatchRunHtml([string]$Path, [object]$Summary) {
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($result in @($Summary.results)) {
        $viewerTarget = if (![string]::IsNullOrWhiteSpace($result.viewerIndex)) { $result.viewerIndex } elseif (![string]::IsNullOrWhiteSpace($result.viewerServer)) { $result.viewerServer } else { "" }
        $viewerCell = New-LinkHtml $Path $viewerTarget "viewer"
        $entryCell = New-LinkHtml $Path $result.entryJson "entry"
        $rows.Add("<tr><td class=`"status $($result.status)`">$(ConvertTo-HtmlText $result.status)</td><td>$(ConvertTo-HtmlText $result.actorKind)</td><td>$(ConvertTo-HtmlText $result.source)</td><td><code>$(ConvertTo-HtmlText $result.target)</code></td><td><code>$(ConvertTo-HtmlText $result.command)</code></td><td>$viewerCell</td><td>$entryCell</td><td>$(ConvertTo-HtmlText $result.error)</td></tr>")
    }

    $planLink = New-LinkHtml $Path $Summary.planJson "plan"
    $selectedLink = New-LinkHtml $Path $Summary.selectedEntriesJson "selected entries"
    $body = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FNV Character Viewer Batch Run</title>
<style>
body{margin:0;background:#111316;color:#eceff3;font:13px/1.4 Segoe UI,Arial,sans-serif}
main{padding:16px;display:grid;gap:14px}
h1{font-size:18px;margin:0}
.panel{border:1px solid #363c45;border-radius:6px;background:#1a1d22;padding:12px}
.meta{display:flex;gap:10px;flex-wrap:wrap}
.pill{border:1px solid #363c45;border-radius:999px;padding:4px 8px;background:#20242a}
table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid #363c45;padding:7px;text-align:left;vertical-align:top}
th{color:#aeb6c2}
code{color:#d8e6ff;overflow-wrap:anywhere}
a{color:#9fc2ff}
.status{font-weight:600}
.PASS,.DRY-RUN{color:#64d488}
.FAIL{color:#ff6f61}
</style>
</head>
<body>
<main>
<h1>FNV Character Viewer Batch Run</h1>
<section class="panel meta">
  <span class="pill">Status: $(ConvertTo-HtmlText $Summary.status)</span>
  <span class="pill">Dry run: $(ConvertTo-HtmlText $Summary.dryRun)</span>
  <span class="pill">Selected: $(ConvertTo-HtmlText $Summary.selectedEntries)</span>
  <span class="pill">Actor kind: $(ConvertTo-HtmlText $Summary.actorKind)</span>
  <span class="pill">Source: $(ConvertTo-HtmlText $Summary.source)</span>
  <span class="pill">Target: $(ConvertTo-HtmlText $Summary.target)</span>
</section>
<section class="panel">
  <div>Plan: $planLink</div>
  <div>Selected entries: $selectedLink</div>
  <div>Policy: $(ConvertTo-HtmlText $Summary.payloadPolicy)</div>
</section>
<section class="panel">
<table>
<thead><tr><th>Status</th><th>Kind</th><th>Source</th><th>Target</th><th>Command</th><th>Viewer</th><th>Entry</th><th>Error</th></tr></thead>
<tbody>
$($rows -join "`n")
</tbody>
</table>
</section>
</main>
</body>
</html>
"@
    $Path | Split-Path -Parent | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
    $body | Set-Content -LiteralPath $Path -Encoding UTF8
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
    html = (Join-Path $runDir "viewer-batch-run.html")
    markdown = (Join-Path $runDir "viewer-batch-run.md")
    results = @($runResults)
}
$summaryPath = Join-Path $runDir "viewer-batch-run.json"
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-BatchRunMarkdown $summary.markdown $summary
Write-BatchRunHtml $summary.html $summary

Write-Host ""
Write-Host "Batch run status: $($summary.status)"
Write-Host "Batch run JSON: $summaryPath"
Write-Host "Batch run HTML: $($summary.html)"
Write-Host "Batch run Markdown: $($summary.markdown)"
Write-Host "Selected entries JSON: $selectedPath"
if ($RequirePass -and $summary.status -ne "PASS") {
    throw "FNV character viewer batch run failed. See $summaryPath"
}
