param(
    [string]$ProofRoot = "",
    [string]$LedgerJson = "",
    [string]$ResultJson = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [switch]$RequirePass
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Planner = Join-Path $PSScriptRoot "fnv_character_viewer_batch_plan.py"
if (!(Test-Path -LiteralPath $Planner -PathType Leaf)) {
    throw "Missing FNV character viewer batch planner: $Planner"
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
$argsList = @()
$argsList += $python.Args
$argsList += $Planner
$argsList += @("--proof-root", $ProofRoot)
if (![string]::IsNullOrWhiteSpace($LedgerJson)) { $argsList += @("--ledger-json", $LedgerJson) }
if (![string]::IsNullOrWhiteSpace($ResultJson)) { $argsList += @("--result-json", $ResultJson) }
if (![string]::IsNullOrWhiteSpace($OutDir)) { $argsList += @("--out-dir", $OutDir) }
if ($Limit -gt 0) { $argsList += @("--limit", [string]$Limit) }
if ($RequirePass) { $argsList += "--require-pass" }

Write-Host "FNV character viewer batch plan"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "LedgerJson: $LedgerJson"
Write-Host "ResultJson: $ResultJson"
Write-Host "OutDir: $OutDir"
Write-Host "Limit: $Limit"
Write-Host "Policy: generated command/identifier plan only; no retail assets are committed"
Write-Host ""

& $python.Command @argsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV character viewer batch plan failed with exit code $LASTEXITCODE."
}
