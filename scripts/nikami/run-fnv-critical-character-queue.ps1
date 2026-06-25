param(
    [string]$ProofRoot = "",
    [string]$CatalogJson = "",
    [string[]]$QueueIds = @(),
    [int]$MaxEntries = 1,
    [ValidateSet("focus", "critical")]
    [string]$PhaseMode = "focus",
    [string[]]$Phases = @(),
    [string[]]$Angles = @("front"),
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "600,660,720,780,840",
    [switch]$DryRun,
    [switch]$NoSound,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$ViewerRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer.ps1"
if (!(Test-Path -LiteralPath $ViewerRunner -PathType Leaf)) {
    throw "Missing FNV character viewer runner: $ViewerRunner"
}

function ConvertTo-PlainArray($Value) {
    if ($null -eq $Value) { return @() }
    if ($Value -is [array]) { return @($Value) }
    return @($Value)
}

function Normalize-StringList([object[]]$Values) {
    $items = New-Object "System.Collections.Generic.List[string]"
    foreach ($value in @($Values)) {
        foreach ($part in ([string]$value -split ",")) {
            $trimmed = $part.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed)) {
                $items.Add($trimmed)
            }
        }
    }
    return @($items)
}

function Get-ObjectProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Add-DoubleArgIfPresent([hashtable]$TargetArgs, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    if ($Value -is [string] -and [string]::IsNullOrWhiteSpace($Value)) { return }
    $TargetArgs[$Name] = [double]$Value
}

function Resolve-LatestCatalog([string]$Root) {
    $catalogRoot = Join-Path $Root "fnv-character-studio-catalog"
    if (!(Test-Path -LiteralPath $catalogRoot -PathType Container)) {
        throw "No generated FNV character studio catalog root found: $catalogRoot"
    }
    $latest = Get-ChildItem -LiteralPath $catalogRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "character-studio-catalog.json") -PathType Leaf } |
        Select-Object -First 1
    if ($null -eq $latest) {
        throw "No generated character-studio-catalog.json found under $catalogRoot"
    }
    return Join-Path $latest.FullName "character-studio-catalog.json"
}

function New-RunDirectory([string]$Root) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $runRoot = Join-Path $Root "fnv-critical-character-queue-run"
    $runDir = Join-Path $runRoot $stamp
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    return $runDir
}

function ConvertTo-RelativeHref([string]$BaseFile, [string]$TargetPath) {
    if ([string]::IsNullOrWhiteSpace($TargetPath) -or !(Test-Path -LiteralPath $TargetPath)) {
        return ""
    }
    $baseDirectory = Split-Path $BaseFile -Parent
    if ([string]::IsNullOrWhiteSpace($baseDirectory)) {
        $baseDirectory = (Get-Location).Path
    }
    $basePath = (Resolve-Path -LiteralPath $baseDirectory).Path
    if (!$basePath.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $basePath += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetResolved = (Resolve-Path -LiteralPath $TargetPath).Path
    $relative = ([uri]$basePath).MakeRelativeUri([uri]$targetResolved).ToString()
    return [uri]::UnescapeDataString($relative)
}

function ConvertTo-HtmlText([object]$Value) {
    return [System.Net.WebUtility]::HtmlEncode([string]$Value)
}

function New-LinkHtml([string]$BaseFile, [string]$TargetPath, [string]$Label) {
    $href = ConvertTo-RelativeHref $BaseFile $TargetPath
    if ([string]::IsNullOrWhiteSpace($href)) {
        return ConvertTo-HtmlText $Label
    }
    return "<a href=`"$(ConvertTo-HtmlText $href)`">$(ConvertTo-HtmlText $Label)</a>"
}

function Get-NewestViewerRun([string[]]$Before, [datetime]$StartedAt) {
    $viewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
    if (!(Test-Path -LiteralPath $viewerRoot -PathType Container)) { return "" }
    $beforeSet = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($path in @($Before)) {
        if (![string]::IsNullOrWhiteSpace($path)) { [void]$beforeSet.Add($path) }
    }
    $newDir = Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { !$beforeSet.Contains($_.FullName) -and $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $newDir) { return $newDir.FullName }
    $fallback = Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $fallback) { return $fallback.FullName }
    return ""
}

function Get-SelectedPhases([object]$QueueRow, [object]$Entry) {
    $explicit = @(Normalize-StringList $Phases)
    if ($explicit.Count -gt 0) { return @($explicit) }
    if ($PhaseMode -eq "critical") {
        $critical = @(Normalize-StringList (ConvertTo-PlainArray (Get-ObjectProperty $QueueRow "criticalPhases")))
        if ($critical.Count -gt 0) { return @($critical) }
    }
    $focus = @(Normalize-StringList @((Get-ObjectProperty $QueueRow "defaultPhase")))
    if ($focus.Count -gt 0) { return @($focus) }
    $entryPhases = @(Normalize-StringList (ConvertTo-PlainArray (Get-ObjectProperty $Entry "phases")))
    if ($entryPhases.Count -gt 0) { return @($entryPhases) }
    return @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk")
}

function Invoke-QueueEntry([object]$QueueRow, [object]$Entry, [string]$RunDir) {
    $queueId = [string](Get-ObjectProperty $QueueRow "id")
    $entryId = [string](Get-ObjectProperty $QueueRow "entryId")
    $safeId = (($queueId, $entryId) -join "_") -replace '[^A-Za-z0-9_.-]', '_'
    $entryDir = Join-Path $RunDir $safeId
    New-Item -ItemType Directory -Force -Path $entryDir | Out-Null
    $queuePath = Join-Path $entryDir "queue-entry.json"
    $QueueRow | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $queuePath -Encoding UTF8
    $entryPath = Join-Path $entryDir "catalog-entry.json"
    $Entry | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $entryPath -Encoding UTF8

    $target = [string](Get-ObjectProperty $QueueRow "runtimeTarget")
    if ([string]::IsNullOrWhiteSpace($target)) { $target = [string](Get-ObjectProperty $Entry "runtimeTarget") }
    if ([string]::IsNullOrWhiteSpace($target)) { $target = [string](Get-ObjectProperty $Entry "target") }
    $actorKind = [string](Get-ObjectProperty $QueueRow "actorKind")
    if ([string]::IsNullOrWhiteSpace($actorKind) -or $actorKind -eq "unknown") { $actorKind = [string](Get-ObjectProperty $Entry "kind") }
    $phaseList = @(Get-SelectedPhases $QueueRow $Entry)
    $angleList = @(Normalize-StringList $Angles)
    if ($angleList.Count -eq 0) { $angleList = @("front") }

    $result = [ordered]@{
        queueId = $queueId
        label = [string](Get-ObjectProperty $QueueRow "label")
        entryId = $entryId
        entryLabel = [string](Get-ObjectProperty $QueueRow "entryLabel")
        actorKind = $actorKind
        runtimeTarget = $target
        placedTarget = [string](Get-ObjectProperty $QueueRow "placedTarget")
        phaseMode = $PhaseMode
        phases = @($phaseList)
        angles = @($angleList)
        dryRun = [bool]$DryRun
        status = "PENDING"
        queueEntryJson = $queuePath
        catalogEntryJson = $entryPath
        viewerIndex = ""
        viewerManifest = ""
        actorKit = ""
        error = ""
    }

    if ([string]::IsNullOrWhiteSpace($target) -or [string]::IsNullOrWhiteSpace($actorKind) -or $actorKind -eq "unknown") {
        $result.status = "FAIL"
        $result.error = "critical queue row is missing runtime target or actor kind"
        return [pscustomobject]$result
    }
    if ($DryRun) {
        $result.status = "DRY-RUN"
        return [pscustomobject]$result
    }

    $viewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
    $before = @()
    if (Test-Path -LiteralPath $viewerRoot -PathType Container) {
        $before = @(Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    }

    $viewerArgs = @{
        ProofRoot = $ProofRoot
        Targets = @($target)
        ActorKind = $actorKind
        Phases = @($phaseList)
        Angles = @($angleList)
        RunSeconds = $RunSeconds
        ActorFrame = $ActorFrame
        ScreenshotFrames = $ScreenshotFrames
    }
    $placement = Get-ObjectProperty $Entry "placement"
    if ($null -ne $placement -and [bool](Get-ObjectProperty $placement "runtimeBootstrapReady")) {
        $cell = [string](Get-ObjectProperty $placement "cell")
        if (![string]::IsNullOrWhiteSpace($cell)) { $viewerArgs.BootstrapCell = $cell }
        $position = Get-ObjectProperty $placement "position"
        $rotation = Get-ObjectProperty $placement "rotation"
        Add-DoubleArgIfPresent $viewerArgs "BootstrapX" (Get-ObjectProperty $position "x")
        Add-DoubleArgIfPresent $viewerArgs "BootstrapY" (Get-ObjectProperty $position "y")
        Add-DoubleArgIfPresent $viewerArgs "BootstrapZ" (Get-ObjectProperty $position "z")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageX" (Get-ObjectProperty $position "x")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageY" (Get-ObjectProperty $position "y")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageZ" (Get-ObjectProperty $position "z")
        Add-DoubleArgIfPresent $viewerArgs "BootstrapRotX" (Get-ObjectProperty $rotation "x")
        Add-DoubleArgIfPresent $viewerArgs "BootstrapRotY" (Get-ObjectProperty $rotation "y")
        Add-DoubleArgIfPresent $viewerArgs "BootstrapRotZ" (Get-ObjectProperty $rotation "z")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageRotX" (Get-ObjectProperty $rotation "x")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageRotY" (Get-ObjectProperty $rotation "y")
        Add-DoubleArgIfPresent $viewerArgs "ActorStageRotZ" (Get-ObjectProperty $rotation "z")
    }
    if ($NoSound) { $viewerArgs.NoSound = $true }
    if ($actorKind -eq "creature") { $viewerArgs.CreatureDiagnostics = $true }

    $started = Get-Date
    try {
        & $ViewerRunner @viewerArgs | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "viewer runner exited with code $LASTEXITCODE" }
        $newDir = Get-NewestViewerRun $before $started
        if (![string]::IsNullOrWhiteSpace($newDir)) {
            $result.viewerIndex = Join-Path $newDir "index.html"
            $manifestPath = Join-Path $newDir "viewer-runs.json"
            if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
                $result.viewerManifest = $manifestPath
                $runs = @(Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)
                $failedRuns = @($runs | Where-Object { [string]$_.Status -ne "PASS" })
                $firstRun = $runs | Select-Object -First 1
                if ($null -ne $firstRun -and $null -ne $firstRun.ActorKitJson) {
                    $result.actorKit = [string]$firstRun.ActorKitJson
                }
                if ($runs.Count -eq 0) {
                    $result.status = "FAIL"
                    $result.error = "viewer-runs.json contained no child runs"
                }
                elseif ($failedRuns.Count -gt 0) {
                    $result.status = "FAIL"
                    $result.error = "viewer child failed: $(@($failedRuns | ForEach-Object { "$($_.Target)=$($_.Status)" }) -join ', ')"
                }
                else {
                    $result.status = "PASS"
                }
            }
            else {
                $result.status = "FAIL"
                $result.error = "viewer-runs.json missing"
            }
        }
        else {
            $result.status = "FAIL"
            $result.error = "viewer output directory missing"
        }
    }
    catch {
        $result.status = "FAIL"
        $result.error = $_.Exception.Message
    }
    return [pscustomobject]$result
}

function Write-QueueMarkdown([string]$Path, [object]$Summary) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# FNV Critical Character Queue Run")
    $lines.Add("")
    $lines.Add("Status: **$($Summary.status)**")
    $lines.Add("Phase mode: ``$($Summary.phaseMode)``")
    $lines.Add("Selected entries: $($Summary.selectedEntries)")
    $lines.Add("Catalog: ``$($Summary.catalogJson)``")
    $lines.Add("Policy: $($Summary.payloadPolicy)")
    $lines.Add("")
    $lines.Add("| Status | Queue | Target | Phases | Angles | Viewer | Error |")
    $lines.Add("|---|---|---|---|---|---|---|")
    foreach ($result in @($Summary.results)) {
        $viewer = if (![string]::IsNullOrWhiteSpace($result.viewerIndex)) { $result.viewerIndex } else { "" }
        $lines.Add("| $($result.status) | $($result.queueId) | ``$($result.runtimeTarget)`` | ``$(@($result.phases) -join ',')`` | ``$(@($result.angles) -join ',')`` | ``$viewer`` | $($result.error) |")
    }
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-QueueHtml([string]$Path, [object]$Summary) {
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($result in @($Summary.results)) {
        $viewer = New-LinkHtml $Path $result.viewerIndex "viewer"
        $manifest = New-LinkHtml $Path $result.viewerManifest "manifest"
        $rows.Add("<tr><td class=`"status $($result.status)`">$(ConvertTo-HtmlText $result.status)</td><td>$(ConvertTo-HtmlText $result.queueId)</td><td><code>$(ConvertTo-HtmlText $result.runtimeTarget)</code></td><td><code>$(ConvertTo-HtmlText (@($result.phases) -join ','))</code></td><td><code>$(ConvertTo-HtmlText (@($result.angles) -join ','))</code></td><td>$viewer $manifest</td><td>$(ConvertTo-HtmlText $result.error)</td></tr>")
    }
    $catalogLink = New-LinkHtml $Path $Summary.catalogJson "catalog"
    $body = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FNV Critical Character Queue Run</title>
<style>
body{margin:0;background:#111316;color:#eceff3;font:13px/1.4 Segoe UI,Arial,sans-serif}
main{padding:16px;display:grid;gap:14px}
.panel{border:1px solid #363c45;border-radius:6px;background:#1a1d22;padding:12px}
.meta{display:flex;gap:10px;flex-wrap:wrap}
.pill{border:1px solid #363c45;border-radius:999px;padding:4px 8px;background:#20242a}
table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid #363c45;padding:7px;text-align:left;vertical-align:top}
th{color:#aeb6c2}
code{color:#d8e6ff;overflow-wrap:anywhere}
a{color:#9fc2ff}
.PASS,.DRY-RUN{color:#64d488}.FAIL{color:#ff6f61}
</style>
</head>
<body>
<main>
<h1>FNV Critical Character Queue Run</h1>
<section class="panel meta">
<span class="pill">Status: $(ConvertTo-HtmlText $Summary.status)</span>
<span class="pill">Phase mode: $(ConvertTo-HtmlText $Summary.phaseMode)</span>
<span class="pill">Selected: $(ConvertTo-HtmlText $Summary.selectedEntries)</span>
<span class="pill">Dry run: $(ConvertTo-HtmlText $Summary.dryRun)</span>
</section>
<section class="panel">Catalog: $catalogLink<br>Policy: $(ConvertTo-HtmlText $Summary.payloadPolicy)</section>
<section class="panel"><table><thead><tr><th>Status</th><th>Queue</th><th>Target</th><th>Phases</th><th>Angles</th><th>Artifacts</th><th>Error</th></tr></thead><tbody>
$($rows -join "`n")
</tbody></table></section>
</main>
</body>
</html>
"@
    $body | Set-Content -LiteralPath $Path -Encoding UTF8
}

if ([string]::IsNullOrWhiteSpace($CatalogJson)) {
    $CatalogJson = Resolve-LatestCatalog $ProofRoot
}
$CatalogJson = (Resolve-Path -LiteralPath $CatalogJson).Path
$catalog = Get-Content -LiteralPath $CatalogJson -Raw | ConvertFrom-Json
$entries = @(ConvertTo-PlainArray $catalog.entries)
$queue = @(ConvertTo-PlainArray $catalog.criticalQueue | Where-Object { [string]$_.queueStatus -eq "queued" })
$selectedIds = @(Normalize-StringList $QueueIds)
if ($selectedIds.Count -gt 0) {
    $queue = @($queue | Where-Object { $selectedIds -contains [string]$_.id })
}
if ($MaxEntries -gt 0) {
    $queue = @($queue | Select-Object -First $MaxEntries)
}

$runDir = New-RunDirectory $ProofRoot
$selectedPath = Join-Path $runDir "selected-critical-queue.json"
$queue | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $selectedPath -Encoding UTF8

Write-Host "FNV critical character queue run"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "CatalogJson: $CatalogJson"
Write-Host "RunDir: $runDir"
Write-Host "QueueIds: $($selectedIds -join ',')"
Write-Host "PhaseMode: $PhaseMode"
Write-Host "Angles: $((Normalize-StringList $Angles) -join ',')"
Write-Host "Selected: $($queue.Count)"
Write-Host "Policy: generated proof/viewer output only; no retail assets are committed"
Write-Host ""

$results = [System.Collections.Generic.List[object]]::new()
foreach ($row in $queue) {
    $entryId = [string]$row.entryId
    $entry = $entries | Where-Object { [string]$_.id -eq $entryId } | Select-Object -First 1
    if ($null -eq $entry) {
        $results.Add([pscustomobject][ordered]@{
            queueId = [string]$row.id
            label = [string]$row.label
            entryId = $entryId
            actorKind = [string]$row.actorKind
            runtimeTarget = [string]$row.runtimeTarget
            phaseMode = $PhaseMode
            phases = @()
            angles = @(Normalize-StringList $Angles)
            dryRun = [bool]$DryRun
            status = "FAIL"
            queueEntryJson = ""
            catalogEntryJson = ""
            viewerIndex = ""
            viewerManifest = ""
            actorKit = ""
            error = "catalog entry not found for critical queue row"
        })
        continue
    }
    $results.Add((Invoke-QueueEntry $row $entry $runDir))
}

$failed = @($results | Where-Object { [string]$_.status -eq "FAIL" })
$summary = [pscustomobject][ordered]@{
    schema = "nikami-fnv-critical-character-queue-run-v1"
    status = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
    dryRun = [bool]$DryRun
    catalogJson = $CatalogJson
    selectedEntriesJson = $selectedPath
    selectedEntries = $queue.Count
    phaseMode = $PhaseMode
    explicitPhases = @(Normalize-StringList $Phases)
    angles = @(Normalize-StringList $Angles)
    runSeconds = $RunSeconds
    payloadPolicy = "generated queue/run metadata and proof links only; no retail asset payload bytes"
    artifacts = [ordered]@{
        json = Join-Path $runDir "critical-character-queue-run.json"
        markdown = Join-Path $runDir "critical-character-queue-run.md"
        html = Join-Path $runDir "critical-character-queue-run.html"
    }
    results = @($results)
}

$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summary.artifacts.json -Encoding UTF8
Write-QueueMarkdown $summary.artifacts.markdown $summary
Write-QueueHtml $summary.artifacts.html $summary

Write-Host "Critical queue status: $($summary.status)"
Write-Host "Critical queue JSON: $($summary.artifacts.json)"
Write-Host "Critical queue HTML: $($summary.artifacts.html)"
Write-Host "Critical queue Markdown: $($summary.artifacts.markdown)"
if ($RequirePass -and $summary.status -ne "PASS") {
    exit 2
}
