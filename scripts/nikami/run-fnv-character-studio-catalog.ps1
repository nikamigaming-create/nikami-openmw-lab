param(
    [string]$ProofRoot = "",
    [string]$PlanJson = "",
    [string]$ContentDir = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [switch]$OpenStudio,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Builder = Join-Path $PSScriptRoot "fnv_character_studio_catalog.py"
if (!(Test-Path -LiteralPath $Builder -PathType Leaf)) {
    throw "Missing FNV character studio catalog builder: $Builder"
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

if ($OpenStudio) {
    Start-Process -FilePath $html | Out-Null
}
