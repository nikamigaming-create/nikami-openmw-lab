param(
    [string]$FnvRoot = $env:NIKAMI_FNV_ROOT,
    [string]$FnvData = "",
    [string]$HarvestDir = "",
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
        "TribalPack.esm"
    )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Parser = Join-Path $PSScriptRoot "fnv_speedtree_spt_ledger.py"

$Python = $null
$PythonArgs = @()
$CandidateList = @()
$LocalAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
if (![string]::IsNullOrWhiteSpace($LocalAppData)) {
    $CandidateList += @(
        (Join-Path $LocalAppData "Programs\Python\Launcher\py.exe"),
        (Join-Path $LocalAppData "Programs\Python\Python312\python.exe"),
        (Join-Path $LocalAppData "Programs\Python\Python311\python.exe")
    )
}
$CandidateList += @(
    "py",
    "py.exe",
    "python",
    "python.exe"
)
foreach ($Candidate in $CandidateList) {
    if ($null -ne $Python) {
        break
    }

    if ([IO.Path]::IsPathRooted($Candidate) -and (Test-Path -LiteralPath $Candidate -PathType Leaf)) {
        $Python = [pscustomobject]@{ Source = $Candidate; Name = [IO.Path]::GetFileName($Candidate) }
    }
    else {
        $Command = Get-Command $Candidate -ErrorAction SilentlyContinue
        if ($null -ne $Command) {
            $Python = @($Command)[0]
        }
    }

    if ($null -ne $Python -and $Python.Name -like "py*") {
        $PythonArgs += "-3"
    }
}
if ($null -eq $Python) {
    throw "Python 3 is required to run the FNV SpeedTree SPT ledger proof."
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
if (![string]::IsNullOrWhiteSpace($HarvestDir)) {
    $ArgsList += @("--harvest-dir", $HarvestDir)
}
if (![string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ArgsList += @("--proof-root", $ProofRoot)
}
$ArgsList += @("--repo-root", $RepoRoot)
$ArgsList += "--content"
$ArgsList += $Content

& $Python.Source @ArgsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV SpeedTree SPT ledger proof failed with exit code $LASTEXITCODE."
}
