param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$FnvData = $env:NIKAMI_FNV_DATA,
    [string]$FnvConfigData = $env:NIKAMI_FNV_CONFIG_DATA,
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ExtraOsgPluginDir = $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR,
    [string]$Triplet = "x64-windows",
    [string]$ProofRoot = "",
    [string[]]$Targets = @("GSEasyPete"),
    [ValidateSet("npc", "creature", "auto")]
    [string]$ActorKind = "npc",
    [string[]]$Phases = @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"),
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
    [string]$SuiteDir = "",
    [switch]$NoRun,
    [switch]$CreatureDiagnostics,
    [switch]$NoSound,
    [switch]$OpenViewer,
    [switch]$Serve,
    [int]$ServePort = 0
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$BuilderRunner = Join-Path $PSScriptRoot "run-fnv-character-builder-tester.ps1"
$BundleScript = Join-Path $PSScriptRoot "fnv_character_viewer_bundle.py"
if (!(Test-Path -LiteralPath $BuilderRunner)) { throw "Missing character builder tester: $BuilderRunner" }
if (!(Test-Path -LiteralPath $BundleScript)) { throw "Missing character viewer bundle generator: $BundleScript" }

function Normalize-List([string[]]$Values, [string]$Name) {
    $items = New-Object "System.Collections.Generic.List[string]"
    foreach ($value in $Values) {
        foreach ($part in ($value -split ",")) {
            $trimmed = $part.Trim()
            if (![string]::IsNullOrWhiteSpace($trimmed)) {
                $items.Add($trimmed)
            }
        }
    }
    if ($items.Count -eq 0) { throw "No $Name selected." }
    return @($items)
}

function Get-SuiteDirectories {
    $base = Join-Path $ProofRoot "fnv-character-builder"
    if (!(Test-Path -LiteralPath $base)) { return @() }
    return @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
}

function Get-NewSuiteDirectory([string[]]$Before, [datetime]$StartedAt) {
    $base = Join-Path $ProofRoot "fnv-character-builder"
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

function ConvertTo-HtmlText([string]$Value) {
    return [System.Net.WebUtility]::HtmlEncode($Value)
}

function ConvertTo-RelativeHref([string]$BaseDirectory, [string]$TargetPath) {
    $basePath = (Resolve-Path -LiteralPath $BaseDirectory).Path
    if (!$basePath.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $basePath += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetResolved = (Resolve-Path -LiteralPath $TargetPath).Path
    $relative = ([uri]$basePath).MakeRelativeUri([uri]$targetResolved).ToString()
    return [uri]::UnescapeDataString($relative)
}

function Write-ViewerIndex([string]$IndexPath, [object[]]$Runs) {
    $lines = New-Object "System.Collections.Generic.List[string]"
    $lines.Add("<!doctype html>")
    $lines.Add("<html lang=`"en`"><head><meta charset=`"utf-8`"><meta name=`"viewport`" content=`"width=device-width, initial-scale=1`">")
    $lines.Add("<title>FNV Character Viewer Runs</title>")
    $lines.Add("<style>body{margin:0;background:#111316;color:#eceff3;font:14px Segoe UI,Arial,sans-serif}main{padding:16px}a{color:#9fc2ff}table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #363c45;padding:8px;text-align:left}.PASS{color:#64d488}.FAIL{color:#ff6f61}.MISSING{color:#e8c86a}</style>")
    $lines.Add("</head><body><main><h1>FNV Character Viewer Runs</h1><table><thead><tr><th>Target</th><th>Status</th><th>Viewer</th><th>Manifest</th><th>Actor Kit</th><th>Suite</th></tr></thead><tbody>")
    $baseDirectory = Split-Path $IndexPath -Parent
    foreach ($run in $Runs) {
        $viewer = ConvertTo-HtmlText $run.ViewerHtml
        $manifest = ConvertTo-HtmlText $run.ViewerJson
        $actorKit = ConvertTo-HtmlText $run.ActorKitJson
        $suite = ConvertTo-HtmlText $run.SuiteDir
        $target = ConvertTo-HtmlText $run.Target
        $status = ConvertTo-HtmlText $run.Status
        $viewerRel = ConvertTo-HtmlText (ConvertTo-RelativeHref $baseDirectory $run.ViewerHtml)
        $manifestRel = ConvertTo-HtmlText (ConvertTo-RelativeHref $baseDirectory $run.ViewerJson)
        $actorKitRel = ConvertTo-HtmlText (ConvertTo-RelativeHref $baseDirectory $run.ActorKitJson)
        $lines.Add("<tr><td>$target</td><td class=`"$status`">$status</td><td><a href=`"$viewerRel`">$viewer</a></td><td><a href=`"$manifestRel`">$manifest</a></td><td><a href=`"$actorKitRel`">$actorKit</a></td><td>$suite</td></tr>")
    }
    $lines.Add("</tbody></table></main></body></html>")
    $lines | Set-Content -LiteralPath $IndexPath -Encoding UTF8
}

function Get-FreeLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return [int]$listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

function ConvertTo-HttpPath([string]$BaseDirectory, [string]$TargetPath) {
    $relative = ConvertTo-RelativeHref $BaseDirectory $TargetPath
    return ($relative -replace '\\', '/')
}

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Start-ViewerServer([string]$RootDirectory, [string]$IndexPath, [string]$RunDirectory, [int]$RequestedPort) {
    $root = (Resolve-Path -LiteralPath $RootDirectory).Path
    $index = (Resolve-Path -LiteralPath $IndexPath).Path
    $port = if ($RequestedPort -gt 0) { $RequestedPort } else { Get-FreeLoopbackPort }
    $serverLog = Join-Path $RunDirectory "viewer-server.stdout.log"
    $serverErr = Join-Path $RunDirectory "viewer-server.stderr.log"
    $arguments = @("-m", "http.server", [string]$port, "--bind", "127.0.0.1", "--directory", $root) |
        ForEach-Object { Quote-ProcessArgument $_ }
    $process = Start-Process -FilePath "python" -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $serverLog -RedirectStandardError $serverErr

    $relativeIndex = ConvertTo-HttpPath $root $index
    $url = "http://127.0.0.1:$port/$relativeIndex"
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 1 | Out-Null
            $ready = $true
            break
        }
        catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (!$ready) {
        throw "Viewer HTTP server did not become ready at $url. ProcessId=$($process.Id) stdout=$serverLog stderr=$serverErr"
    }

    $serverInfo = [pscustomobject][ordered]@{
        url = $url
        root = $root
        index = $index
        host = "127.0.0.1"
        port = $port
        processId = $process.Id
        stdout = $serverLog
        stderr = $serverErr
        policy = [pscustomobject][ordered]@{
            loopbackOnly = $true
            generatedProofOutputsOnly = $true
            noRetailAssetsCommitted = $true
        }
    }
    $serverInfoPath = Join-Path $RunDirectory "viewer-server.json"
    $serverInfo | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $serverInfoPath -Encoding UTF8
    $serverUrlPath = Join-Path $RunDirectory "viewer-url.txt"
    $url | Set-Content -LiteralPath $serverUrlPath -Encoding UTF8
    return [pscustomobject][ordered]@{
        Url = $url
        InfoPath = $serverInfoPath
        UrlPath = $serverUrlPath
        ProcessId = $process.Id
        Stdout = $serverLog
        Stderr = $serverErr
    }
}

$Targets = Normalize-List $Targets "targets"
$Phases = Normalize-List $Phases "phases"
$ViewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
New-Item -ItemType Directory -Force -Path $ViewerRoot | Out-Null
$RunStamp = Get-Date -Format "yyyyMMdd_HHmmss"
$RunDir = Join-Path $ViewerRoot $RunStamp
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

Write-Host "FNV standalone character viewer run $RunStamp"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "RunDir: $RunDir"
Write-Host "Targets: $($Targets -join ',')"
Write-Host "ActorKind: $ActorKind"
Write-Host "CreatureDiagnostics: $($CreatureDiagnostics -or $ActorKind -ieq 'creature')"
Write-Host "Phases: $($Phases -join ',')"
Write-Host "BootstrapCell: $BootstrapCell"
Write-Host "BootstrapPosition: $BootstrapX,$BootstrapY,$BootstrapZ"
Write-Host "ActorStagePosition: $ActorStageX,$ActorStageY,$ActorStageZ"
Write-Host "Policy: generated proof/viewer output only; no retail assets are committed"
if ($Serve) {
    Write-Host "Serve: loopback HTTP enabled"
}

$Runs = New-Object "System.Collections.Generic.List[object]"

if ($NoRun -or ![string]::IsNullOrWhiteSpace($SuiteDir)) {
    if ([string]::IsNullOrWhiteSpace($SuiteDir)) {
        throw "-NoRun requires -SuiteDir."
    }
    $resolvedSuite = (Resolve-Path -LiteralPath $SuiteDir).Path
    $targetName = $Targets[0]
    $viewerHtml = Join-Path $resolvedSuite "character-viewer.html"
    $viewerJson = Join-Path $resolvedSuite "character-viewer-manifest.json"
    $actorKitJson = Join-Path $resolvedSuite "character-actor-kit.json"
    & python $BundleScript --suite-dir $resolvedSuite --out-html $viewerHtml --out-json $viewerJson --out-kit-json $actorKitJson | Out-Host
    $manifest = Get-Content -LiteralPath $viewerJson -Raw | ConvertFrom-Json
    $Runs.Add([pscustomobject][ordered]@{
        Target = $targetName
        Status = $manifest.overallStatus
        SuiteDir = $resolvedSuite
        ViewerHtml = $viewerHtml
        ViewerJson = $viewerJson
        ActorKitJson = $actorKitJson
    })
}
else {
    foreach ($target in $Targets) {
        Write-Host ""
        Write-Host "TARGET $target"
        $before = @(Get-SuiteDirectories)
        $startedAt = Get-Date

        $builderArgs = @{
            BuildDir = $BuildDir
            Configuration = $Configuration
            FnvData = $FnvData
            VcpkgRoot = $VcpkgRoot
            Triplet = $Triplet
            ProofRoot = $ProofRoot
            ActorTarget = $target
            ActorKind = $ActorKind
            Phases = $Phases
            RunSeconds = $RunSeconds
            ActorFrame = $ActorFrame
            ScreenshotFrames = $ScreenshotFrames
            BootstrapCell = $BootstrapCell
            BootstrapX = $BootstrapX
            BootstrapY = $BootstrapY
            BootstrapZ = $BootstrapZ
            BootstrapRotX = $BootstrapRotX
            BootstrapRotY = $BootstrapRotY
            BootstrapRotZ = $BootstrapRotZ
            BootstrapHour = $BootstrapHour
            ActorStageX = $ActorStageX
            ActorStageY = $ActorStageY
            ActorStageZ = $ActorStageZ
            ActorStageRotX = $ActorStageRotX
            ActorStageRotY = $ActorStageRotY
            ActorStageRotZ = $ActorStageRotZ
            ActorViewDistance = $ActorViewDistance
            ActorViewOffsetZ = $ActorViewOffsetZ
            ActorViewTargetZ = $ActorViewTargetZ
        }
        if (![string]::IsNullOrWhiteSpace($FnvConfigData)) { $builderArgs.FnvConfigData = $FnvConfigData }
        if (![string]::IsNullOrWhiteSpace($ExtraOsgPluginDir)) { $builderArgs.ExtraOsgPluginDir = $ExtraOsgPluginDir }
        if ($CreatureDiagnostics -or $ActorKind -ieq "creature") { $builderArgs.CreatureDiagnostics = $true }
        if ($NoSound) { $builderArgs.NoSound = $true }

        & $BuilderRunner @builderArgs | Out-Host
        $newSuite = Get-NewSuiteDirectory $before $startedAt
        if ([string]::IsNullOrWhiteSpace($newSuite)) {
            throw "Unable to find generated character builder suite for $target."
        }

        $viewerHtml = Join-Path $newSuite "character-viewer.html"
        $viewerJson = Join-Path $newSuite "character-viewer-manifest.json"
        $actorKitJson = Join-Path $newSuite "character-actor-kit.json"
        & python $BundleScript --suite-dir $newSuite --out-html $viewerHtml --out-json $viewerJson --out-kit-json $actorKitJson | Out-Host
        $manifest = Get-Content -LiteralPath $viewerJson -Raw | ConvertFrom-Json
        $Runs.Add([pscustomobject][ordered]@{
            Target = $target
            Status = $manifest.overallStatus
            SuiteDir = $newSuite
            ViewerHtml = $viewerHtml
            ViewerJson = $viewerJson
            ActorKitJson = $actorKitJson
        })
    }
}

$RunsJson = Join-Path $RunDir "viewer-runs.json"
$Runs | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $RunsJson -Encoding UTF8
$IndexHtml = Join-Path $RunDir "index.html"
Write-ViewerIndex $IndexHtml @($Runs.ToArray())

Write-Host ""
Write-Host "Viewer index: $IndexHtml"
foreach ($run in $Runs) {
    Write-Host "Viewer for $($run.Target): $($run.ViewerHtml)"
    Write-Host "Manifest for $($run.Target): $($run.ViewerJson)"
}

$ServedUrl = ""
if ($Serve) {
    $server = Start-ViewerServer -RootDirectory $ProofRoot -IndexPath $IndexHtml -RunDirectory $RunDir -RequestedPort $ServePort
    $ServedUrl = $server.Url
    Write-Host "Viewer URL: $($server.Url)"
    Write-Host "Viewer server JSON: $($server.InfoPath)"
    Write-Host "Viewer URL file: $($server.UrlPath)"
    Write-Host "Viewer server PID: $($server.ProcessId)"
}

if ($OpenViewer) {
    if (![string]::IsNullOrWhiteSpace($ServedUrl)) {
        Start-Process $ServedUrl | Out-Null
    }
    else {
        Invoke-Item -LiteralPath $IndexHtml
    }
}
