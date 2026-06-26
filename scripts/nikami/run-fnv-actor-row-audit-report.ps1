param(
    [string]$ProofRoot = "",
    [string]$BurnDownJson = "",
    [string]$RunRoot = "",
    [string]$OutDir = "",
    [switch]$IncludeAllRuns,
    [switch]$RequireRuntimePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Reporter = Join-Path $PSScriptRoot "fnv_actor_row_audit_report.py"
if (!(Test-Path -LiteralPath $Reporter -PathType Leaf)) {
    throw "Missing FNV actor row audit reporter: $Reporter"
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
    throw "Python 3 is required to build the FNV actor row audit report."
}

$python = Find-Python

Write-Host "FNV actor row audit report"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "BurnDownJson: $BurnDownJson"
Write-Host "RunRoot: $RunRoot"
Write-Host "OutDir: $OutDir"
Write-Host "IncludeAllRuns: $IncludeAllRuns"
Write-Host "RequireRuntimePass: $RequireRuntimePass"
Write-Host "Policy: generated row/audit metadata and proof links only; no retail assets are committed"
Write-Host ""

$argsList = @()
$argsList += $python.Args
$argsList += $Reporter
$argsList += @("--proof-root", $ProofRoot)
if (![string]::IsNullOrWhiteSpace($BurnDownJson)) { $argsList += @("--burn-down-json", $BurnDownJson) }
if (![string]::IsNullOrWhiteSpace($RunRoot)) { $argsList += @("--run-root", $RunRoot) }
if (![string]::IsNullOrWhiteSpace($OutDir)) { $argsList += @("--out-dir", $OutDir) }
if ($IncludeAllRuns) { $argsList += "--include-all-runs" }
if ($RequireRuntimePass) { $argsList += "--require-runtime-pass" }

& $python.Command @argsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor row audit report failed with exit code $LASTEXITCODE."
}
