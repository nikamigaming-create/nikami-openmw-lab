param(
    [string]$ProofRoot = "",
    [string]$PlanJson = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [switch]$RegeneratePlan,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Generator = Join-Path $PSScriptRoot "fnv_actor_parity_burndown.py"
$BatchPlanner = Join-Path $PSScriptRoot "run-fnv-character-viewer-batch-plan.ps1"
if (!(Test-Path -LiteralPath $Generator -PathType Leaf)) {
    throw "Missing FNV actor parity burn-down generator: $Generator"
}
if (!(Test-Path -LiteralPath $BatchPlanner -PathType Leaf)) {
    throw "Missing FNV character viewer batch planner runner: $BatchPlanner"
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
    throw "Python 3 is required to build the FNV actor parity burn-down."
}

function Get-LatestPlanJson {
    $root = Join-Path $ProofRoot "fnv-character-viewer-batch-plan"
    if (!(Test-Path -LiteralPath $root -PathType Container)) {
        return ""
    }
    $latest = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "viewer-batch-plan.json") -PathType Leaf } |
        Select-Object -First 1
    if ($null -eq $latest) {
        return ""
    }
    return Join-Path $latest.FullName "viewer-batch-plan.json"
}

$python = Find-Python

Write-Host "FNV actor parity burn-down"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "PlanJson: $PlanJson"
Write-Host "OutDir: $OutDir"
Write-Host "Limit: $Limit"
Write-Host "RegeneratePlan: $RegeneratePlan"
Write-Host "Policy: generated command/classification/proof metadata only; no retail assets are committed"
Write-Host ""

if ($RegeneratePlan -or [string]::IsNullOrWhiteSpace($PlanJson)) {
    if ($RegeneratePlan -or [string]::IsNullOrWhiteSpace((Get-LatestPlanJson))) {
        $plannerArgs = @{
            ProofRoot = $ProofRoot
            RequirePass = $true
        }
        & $BatchPlanner @plannerArgs | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "FNV character viewer batch plan generation failed with exit code $LASTEXITCODE."
        }
    }
}

if ([string]::IsNullOrWhiteSpace($PlanJson)) {
    $PlanJson = Get-LatestPlanJson
}
if ([string]::IsNullOrWhiteSpace($PlanJson) -or !(Test-Path -LiteralPath $PlanJson -PathType Leaf)) {
    throw "Unable to resolve viewer batch plan JSON for actor parity burn-down."
}

$argsList = @()
$argsList += $python.Args
$argsList += $Generator
$argsList += @("--proof-root", $ProofRoot)
$argsList += @("--plan-json", $PlanJson)
if (![string]::IsNullOrWhiteSpace($OutDir)) { $argsList += @("--out-dir", $OutDir) }
if ($Limit -gt 0) { $argsList += @("--limit", [string]$Limit) }
if ($RequirePass) { $argsList += "--require-pass" }

& $python.Command @argsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor parity burn-down failed with exit code $LASTEXITCODE."
}
