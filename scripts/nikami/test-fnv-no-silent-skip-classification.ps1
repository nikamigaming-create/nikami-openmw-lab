param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$BsaTool = $env:NIKAMI_BSATOOL,
    [string]$ProofRoot = "",
    [string]$HarvestDir = "",
    [string]$ContentLedgerDir = "",
    [string]$PcvrReferenceConfigDir = "D:\Modlists\fnv\openmw-config",
    [switch]$RefreshHarvest,
    [switch]$RefreshContentLedger,
    [switch]$FailKnownBlocked
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

function Get-PythonCommand {
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

    $pythonCandidates = @()
    if (![string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $pythonCandidates += Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA "Programs/Python/Python*/python.exe") `
            -File -ErrorAction SilentlyContinue
    }
    foreach ($path in @("C:/Python312/python.exe", "C:/Python311/python.exe")) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $pythonCandidates += Get-Item -LiteralPath $path
        }
    }
    $candidate = $pythonCandidates | Sort-Object FullName | Select-Object -First 1
    if ($null -ne $candidate) {
        return [pscustomobject]@{ Command = $candidate.FullName; Args = @() }
    }
    if (![string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        foreach ($path in @(
                (Join-Path $env:USERPROFILE "AppData/Local/Programs/Python/Python311/python.exe"),
                (Join-Path $env:USERPROFILE "AppData/Local/Programs/Python/Python312/python.exe")
            )) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                return [pscustomobject]@{ Command = $path; Args = @() }
            }
        }
    }
    throw "Python 3 is required to run the FNV no-silent-skip classification gate."
}

function Get-LatestProofDir([string]$Root, [string]$Description) {
    if (!(Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Missing $Description proof root: $Root"
    }
    $latest = Get-ChildItem -LiteralPath $Root -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($null -eq $latest) {
        throw "Missing $Description proof directories under $Root"
    }
    return $latest.FullName
}

if ($RefreshHarvest -or [string]::IsNullOrWhiteSpace($HarvestDir)) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if ($RefreshHarvest -or !(Test-Path -LiteralPath $harvestRoot -PathType Container)) {
        $harvestScript = Join-Path $PSScriptRoot "harvest-fnv-retail-ledger.ps1"
        $harvestArgs = @{ ProofRoot = $ProofRoot }
        if (![string]::IsNullOrWhiteSpace($FnvRoot)) { $harvestArgs.FnvRoot = $FnvRoot }
        if (![string]::IsNullOrWhiteSpace($FnvData)) { $harvestArgs.FnvData = $FnvData }
        if (![string]::IsNullOrWhiteSpace($BsaTool)) { $harvestArgs.BsaTool = $BsaTool }
        & $harvestScript @harvestArgs
        if ($LASTEXITCODE -ne 0) { throw "FNV retail harvest failed with exit code $LASTEXITCODE." }
    }
    $HarvestDir = Get-LatestProofDir $harvestRoot "FNV retail harvest"
}
else {
    $HarvestDir = (Resolve-Path $HarvestDir).Path
}

if ($RefreshContentLedger -or [string]::IsNullOrWhiteSpace($ContentLedgerDir)) {
    $contentRoot = Join-Path $ProofRoot "fnv-content-ledger"
    if ($RefreshContentLedger -or !(Test-Path -LiteralPath $contentRoot -PathType Container)) {
        $contentScript = Join-Path $PSScriptRoot "test-fnv-content-ledger.ps1"
        $contentArgs = @{ ProofRoot = $ProofRoot }
        if (![string]::IsNullOrWhiteSpace($FnvRoot)) { $contentArgs.FnvRoot = $FnvRoot }
        if (![string]::IsNullOrWhiteSpace($FnvData)) { $contentArgs.FnvData = $FnvData }
        & $contentScript @contentArgs
        if ($LASTEXITCODE -ne 0) { throw "FNV content ledger failed with exit code $LASTEXITCODE." }
    }
    $ContentLedgerDir = Get-LatestProofDir $contentRoot "FNV content ledger"
}
else {
    $ContentLedgerDir = (Resolve-Path $ContentLedgerDir).Path
}

$Python = Get-PythonCommand
$Classifier = Join-Path $PSScriptRoot "fnv_no_silent_skip_classification.py"
$ArgsList = @()
$ArgsList += $Python.Args
$ArgsList += $Classifier
$ArgsList += @("--repo-root", $RepoRoot)
$ArgsList += @("--proof-root", $ProofRoot)
$ArgsList += @("--harvest-dir", $HarvestDir)
$ArgsList += @("--content-ledger-dir", $ContentLedgerDir)
if (![string]::IsNullOrWhiteSpace($PcvrReferenceConfigDir)) {
    $ArgsList += @("--pcvr-reference-config-dir", $PcvrReferenceConfigDir)
}
if ($FailKnownBlocked) {
    $ArgsList += "--fail-known-blocked"
}

Write-Host "Using Python: $($Python.Command)"
Write-Host "HarvestDir: $HarvestDir"
Write-Host "ContentLedgerDir: $ContentLedgerDir"
& $Python.Command @ArgsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV no-silent-skip classification gate failed with exit code $LASTEXITCODE."
}
