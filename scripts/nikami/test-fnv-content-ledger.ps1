param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$ProofRoot = "",
    [string[]]$Content = @(
        "FalloutNV.esm",
        "DeadMoney.esm",
        "HonestHearts.esm",
        "OldWorldBlues.esm",
        "LonesomeRoad.esm",
        "GunRunnersArsenal.esm",
        "CaravanPack.esm",
        "ClassicPack.esm",
        "MercenaryPack.esm",
        "TribalPack.esm",
        "FNVR.esp"
    )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Parser = Join-Path $PSScriptRoot "fnv_content_ledger.py"

$Python = ""
$PythonArgs = @()

foreach ($candidate in @(
        @{ Command = "python"; Args = @() },
        @{ Command = "python.exe"; Args = @() },
        @{ Command = "py"; Args = @("-3") },
        @{ Command = "py.exe"; Args = @("-3") }
    )) {
    try {
        & ($candidate["Command"]) @($candidate["Args"]) --version *> $null
        if ($LASTEXITCODE -eq 0) {
            $Python = $candidate["Command"]
            $PythonArgs = @($candidate["Args"])
            break
        }
    }
    catch {
    }
}

if ([string]::IsNullOrWhiteSpace($Python)) {
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
        $Python = $candidate.FullName
    }
}
if ([string]::IsNullOrWhiteSpace($Python) -and !( [string]::IsNullOrWhiteSpace($env:USERPROFILE))) {
    foreach ($path in @(
            (Join-Path $env:USERPROFILE "AppData/Local/Programs/Python/Python311/python.exe"),
            (Join-Path $env:USERPROFILE "AppData/Local/Programs/Python/Python312/python.exe")
        )) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $Python = $path
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Python)) {
    throw "Python 3 is required to run the FNV content ledger proof."
}
Write-Host "Using Python: $Python"

$ArgsList = @()
$ArgsList += $PythonArgs
$ArgsList += $Parser
if (![string]::IsNullOrWhiteSpace($FnvRoot)) {
    $ArgsList += @("--fnv-root", $FnvRoot)
}
if (![string]::IsNullOrWhiteSpace($FnvData)) {
    $ArgsList += @("--fnv-data", $FnvData)
}
if (![string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ArgsList += @("--proof-root", $ProofRoot)
}
$ArgsList += @("--repo-root", $RepoRoot)
$ArgsList += "--content"
$ArgsList += $Content

& $Python @ArgsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}
