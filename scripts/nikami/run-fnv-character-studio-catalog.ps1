param(
    [string]$ProofRoot = "",
    [string]$PlanJson = "",
    [string]$ContentDir = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [string]$LiveAuthoringFile = "",
    [string]$LiveRuntimeCommandFile = "",
    [switch]$OpenStudio,
    [switch]$LiveServe,
    [int]$ServePort = 0,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Builder = Join-Path $PSScriptRoot "fnv_character_studio_catalog.py"
$LiveServer = Join-Path $PSScriptRoot "fnv_character_viewer_live_server.py"
$ViewerRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer.ps1"
if (!(Test-Path -LiteralPath $Builder -PathType Leaf)) {
    throw "Missing FNV character studio catalog builder: $Builder"
}
if ($LiveServe -and !(Test-Path -LiteralPath $LiveServer -PathType Leaf)) {
    throw "Missing FNV live studio server: $LiveServer"
}
if ($LiveServe -and !(Test-Path -LiteralPath $ViewerRunner -PathType Leaf)) {
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
    throw "Python 3 is required to build the FNV character studio catalog."
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

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '"', '\"') + '"'
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

function ConvertTo-HttpPath([string]$BaseDirectory, [string]$TargetPath) {
    $relative = ConvertTo-RelativeHref $BaseDirectory $TargetPath
    return ($relative -replace '\\', '/')
}

function Start-LiveStudioServer([string]$RootDirectory, [string]$HtmlPath, [string]$CatalogPath, [string]$RunDirectory, [int]$RequestedPort, [string]$RequestedLiveAuthoringFile, [string]$RequestedLiveRuntimeCommandFile) {
    $root = (Resolve-Path -LiteralPath $RootDirectory).Path
    $htmlResolved = (Resolve-Path -LiteralPath $HtmlPath).Path
    $catalogResolved = (Resolve-Path -LiteralPath $CatalogPath).Path
    $repo = (Resolve-Path -LiteralPath $RepoRoot).Path
    $runner = (Resolve-Path -LiteralPath $ViewerRunner).Path
    $server = (Resolve-Path -LiteralPath $LiveServer).Path
    $port = if ($RequestedPort -gt 0) { $RequestedPort } else { Get-FreeLoopbackPort }
    $liveAuthoringFile = if (![string]::IsNullOrWhiteSpace($RequestedLiveAuthoringFile)) { $RequestedLiveAuthoringFile } else { Join-Path $RunDirectory "live-authoring.json" }
    $liveRuntimeCommandFile = if (![string]::IsNullOrWhiteSpace($RequestedLiveRuntimeCommandFile)) { $RequestedLiveRuntimeCommandFile } else { Join-Path $RunDirectory "live-runtime-command.json" }
    $serverLog = Join-Path $RunDirectory "studio-live-server.stdout.log"
    $serverErr = Join-Path $RunDirectory "studio-live-server.stderr.log"
    $arguments = @(
        $server,
        "--root", $root,
        "--repo-root", $repo,
        "--run-dir", (Resolve-Path -LiteralPath $RunDirectory).Path,
        "--runner", $runner,
        "--catalog-path", $catalogResolved,
        "--live-authoring-path", $liveAuthoringFile,
        "--live-runtime-path", $liveRuntimeCommandFile,
        "--host", "127.0.0.1",
        "--port", [string]$port
    ) | ForEach-Object { Quote-ProcessArgument $_ }
    $process = Start-Process -FilePath "python" -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $serverLog -RedirectStandardError $serverErr

    $relativeIndex = ConvertTo-HttpPath $root $htmlResolved
    $url = "http://127.0.0.1:$port/$relativeIndex"
    $health = "http://127.0.0.1:$port/nikami/health"
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $health -TimeoutSec 1 | Out-Null
            Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 1 | Out-Null
            $ready = $true
            break
        }
        catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (!$ready) {
        throw "Live studio server did not become ready at $url. ProcessId=$($process.Id) stdout=$serverLog stderr=$serverErr"
    }

    $serverInfo = [pscustomobject][ordered]@{
        url = $url
        health = $health
        root = $root
        html = $htmlResolved
        catalog = $catalogResolved
        host = "127.0.0.1"
        port = $port
        processId = $process.Id
        stdout = $serverLog
        stderr = $serverErr
        endpoints = [pscustomobject][ordered]@{
            catalogSearch = "http://127.0.0.1:$port/nikami/catalog/search"
            sessions = "http://127.0.0.1:$port/nikami/studio/sessions"
            jobs = "http://127.0.0.1:$port/nikami/actor-kit/jobs"
            liveAuthoring = "http://127.0.0.1:$port/nikami/live-authoring"
            liveRuntime = "http://127.0.0.1:$port/nikami/live-runtime"
        }
        liveAuthoringFile = $liveAuthoringFile
        liveRuntimeCommandFile = $liveRuntimeCommandFile
        policy = [pscustomobject][ordered]@{
            loopbackOnly = $true
            generatedProofOutputsOnly = $true
            noRetailAssetsCommitted = $true
            activeCellActorSwitchOnly = $true
        }
    }
    $serverInfoPath = Join-Path $RunDirectory "studio-live-server.json"
    $serverInfo | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $serverInfoPath -Encoding UTF8
    $serverUrlPath = Join-Path $RunDirectory "studio-url.txt"
    $url | Set-Content -LiteralPath $serverUrlPath -Encoding UTF8
    return $serverInfo
}

$python = Find-Python
$argsList = @()
$argsList += @($python.Args)
$argsList += $Builder
$argsList += @("--proof-root", $ProofRoot)
if (![string]::IsNullOrWhiteSpace($PlanJson)) {
    $argsList += @("--plan-json", $PlanJson)
}
if (![string]::IsNullOrWhiteSpace($ContentDir)) {
    $argsList += @("--content-dir", $ContentDir)
}
if (![string]::IsNullOrWhiteSpace($OutDir)) {
    $argsList += @("--out-dir", $OutDir)
}
if ($Limit -gt 0) {
    $argsList += @("--limit", $Limit)
}
if ($RequirePass) {
    $argsList += "--require-pass"
}

& $python.Command @argsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV character studio catalog failed with exit code $LASTEXITCODE."
}

$catalogRoot = Join-Path $ProofRoot "fnv-character-studio-catalog"
$latest = $null
if (![string]::IsNullOrWhiteSpace($OutDir)) {
    $latest = Get-Item -LiteralPath $OutDir
}
elseif (Test-Path -LiteralPath $catalogRoot -PathType Container) {
    $latest = Get-ChildItem -LiteralPath $catalogRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
}
if ($null -eq $latest) {
    throw "FNV character studio catalog did not produce an output directory."
}

$html = Join-Path $latest.FullName "character-studio.html"
$json = Join-Path $latest.FullName "character-studio-catalog.json"
if (!(Test-Path -LiteralPath $html -PathType Leaf)) {
    throw "Missing generated studio HTML: $html"
}
if (!(Test-Path -LiteralPath $json -PathType Leaf)) {
    throw "Missing generated studio catalog: $json"
}

Write-Host "Studio Catalog: $json"
Write-Host "Studio HTML: $html"
Write-Host "generated proof/viewer output only; no retail assets are committed"

if ($LiveServe) {
    $serverInfo = Start-LiveStudioServer -RootDirectory $ProofRoot -HtmlPath $html -CatalogPath $json -RunDirectory $latest.FullName -RequestedPort $ServePort -RequestedLiveAuthoringFile $LiveAuthoringFile -RequestedLiveRuntimeCommandFile $LiveRuntimeCommandFile
    Write-Host "Studio URL: $($serverInfo.url)"
    Write-Host "Studio server JSON: $(Join-Path $latest.FullName "studio-live-server.json")"
    Write-Host "Studio URL file: $(Join-Path $latest.FullName "studio-url.txt")"
    Write-Host "Studio server PID: $($serverInfo.processId)"
}

if ($OpenStudio) {
    if ($LiveServe) {
        Start-Process -FilePath $serverInfo.url | Out-Null
    }
    else {
        Start-Process -FilePath $html | Out-Null
    }
}
