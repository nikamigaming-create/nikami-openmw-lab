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

$Python = Get-Command python -ErrorAction SilentlyContinue
$PythonArgs = @()
if ($null -eq $Python) {
    $Python = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $Python) {
        $PythonArgs += "-3"
    }
}
if ($null -eq $Python) {
    throw "Python 3 is required to run the FNV content ledger proof."
}

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

& $Python.Source @ArgsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV content ledger proof failed with exit code $LASTEXITCODE."
}
