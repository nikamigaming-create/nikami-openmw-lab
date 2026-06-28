param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ProofRoot = "",
    [string]$SandboxRoot = "",
    [string]$MergeTarget = "origin/main",
    [switch]$RunBuild,
    [switch]$RunLiveFingerSmoke,
    [switch]$RunTposeWeightBaseline,
    [switch]$NoSound
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($SandboxRoot)) {
    $SandboxRoot = Join-Path $ProofRoot "scratch"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$sandbox = Join-Path $SandboxRoot "fnv-live-animation-merge-sandbox-$stamp"
$runDir = Join-Path $ProofRoot "fnv-live-animation-merge-rehearsal\$stamp"
$manifestPath = Join-Path $runDir "merge-rehearsal.json"
$summaryPath = Join-Path $runDir "summary.md"
New-Item -ItemType Directory -Force -Path $SandboxRoot | Out-Null
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

function Invoke-Git([string]$WorkDir, [string[]]$GitArgs) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& git -C $WorkDir @GitArgs 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "git -C $WorkDir $($GitArgs -join ' ') failed: $($output -join [Environment]::NewLine)"
        }
        return @($output | ForEach-Object { [string]$_ })
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Add-SummaryLine([System.Collections.Generic.List[string]]$Lines, [string]$Text = "") {
    $Lines.Add($Text)
    Write-Host $Text
}

$statusShort = Invoke-Git $RepoRoot @("status", "--short", "--branch")
$trackedDiff = @(git -C $RepoRoot diff --binary)
if ($LASTEXITCODE -ne 0) {
    throw "git diff --binary failed."
}
$untracked = @(Invoke-Git $RepoRoot @("ls-files", "--others", "--exclude-standard"))

Invoke-Git $RepoRoot @("worktree", "add", "--detach", $sandbox, "HEAD") | Out-Null
$worktreeCreated = $true

try {
    if ($trackedDiff.Count -gt 0) {
        $trackedDiff | git -C $sandbox apply --whitespace=nowarn
        if ($LASTEXITCODE -ne 0) {
            throw "git apply failed while copying tracked checkpoint diff into sandbox."
        }
    }

    foreach ($rel in $untracked) {
        $src = Join-Path $RepoRoot $rel
        $dst = Join-Path $sandbox $rel
        $dstDir = Split-Path -Parent $dst
        if (![string]::IsNullOrWhiteSpace($dstDir)) {
            New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }

    Invoke-Git $sandbox @("add", "-A") | Out-Null
    $checkpointStatus = Invoke-Git $sandbox @("status", "--short")
    if ($checkpointStatus.Count -eq 0) {
        throw "No dirty checkpoint changes were copied into the sandbox; refusing an empty merge rehearsal."
    }

    Invoke-Git $sandbox @(
        "-c", "user.name=Codex Merge Rehearsal",
        "-c", "user.email=codex@example.invalid",
        "commit",
        "-m", "WIP FNV live animation authoring checkpoint merge rehearsal"
    ) | Out-Null
    $checkpointCommit = @(Invoke-Git $sandbox @("rev-parse", "HEAD"))[0]

    $mergeOutput = @(git -C $sandbox merge --no-edit $MergeTarget 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git merge $MergeTarget failed in sandbox: $($mergeOutput -join [Environment]::NewLine)"
    }
    $mergedCommit = @(Invoke-Git $sandbox @("rev-parse", "HEAD"))[0]

    $gateScript = Join-Path $sandbox "scripts\nikami\test-fnv-live-animation-merge-gate.ps1"
    if (!(Test-Path -LiteralPath $gateScript -PathType Leaf)) {
        throw "Sandbox merge gate script was not found: $gateScript"
    }

    $gateArgs = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $gateScript,
        "-BuildDir",
        $BuildDir,
        "-Configuration",
        $Configuration,
        "-ProofRoot",
        $ProofRoot
    )
    if (![string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        $gateArgs += @("-VcpkgRoot", $VcpkgRoot)
    }
    if ($RunBuild) { $gateArgs += "-RunBuild" }
    if ($RunLiveFingerSmoke) { $gateArgs += "-RunLiveFingerSmoke" }
    if ($RunTposeWeightBaseline) { $gateArgs += "-RunTposeWeightBaseline" }
    if ($NoSound) { $gateArgs += "-NoSound" }

    $gateOutput = @(powershell @gateArgs 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "merge rehearsal gate failed: $($gateOutput -join [Environment]::NewLine)"
    }
    $gateJsonLine = @($gateOutput | ForEach-Object { [string]$_ } | Where-Object { $_ -like "Gate JSON:*" } | Select-Object -Last 1)
    $gateJson = if ($gateJsonLine.Count -gt 0) { ([string]$gateJsonLine[0]).Substring("Gate JSON:".Length).Trim() } else { "" }
    if ([string]::IsNullOrWhiteSpace($gateJson) -or !(Test-Path -LiteralPath $gateJson -PathType Leaf)) {
        throw "merge rehearsal gate passed but did not report a readable Gate JSON path."
    }
    $gate = Get-Content -LiteralPath $gateJson -Raw | ConvertFrom-Json

    $manifest = [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-animation-merge-rehearsal-v1"
        createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        repoRoot = $RepoRoot
        sandbox = $sandbox
        mergeTarget = $MergeTarget
        sourceStatusShort = @($statusShort)
        trackedDiffCopied = [bool]($trackedDiff.Count -gt 0)
        untrackedCopied = @($untracked)
        checkpointCommit = $checkpointCommit
        mergedCommit = $mergedCommit
        gateJson = $gateJson
        gateVerdict = [string]$gate.verdict
        gateBranch = [string]$gate.branch
        aheadOriginMain = [int]$gate.aheadOriginMain
        behindOriginMain = [int]$gate.behindOriginMain
        runBuild = [bool]$RunBuild
        runLiveFingerSmoke = [bool]$RunLiveFingerSmoke
        runTposeWeightBaseline = [bool]$RunTposeWeightBaseline
        gateScript = $gateScript
        payloadPolicy = "throwaway generated proof metadata and local sandbox paths only; no retail payload bytes"
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    $summary = New-Object "System.Collections.Generic.List[string]"
    Add-SummaryLine $summary "# FNV Live Animation Merge Rehearsal"
    Add-SummaryLine $summary ""
    Add-SummaryLine $summary "Verdict: ``$($manifest.gateVerdict)``"
    Add-SummaryLine $summary "Sandbox: ``$sandbox``"
    Add-SummaryLine $summary "Checkpoint commit: ``$checkpointCommit``"
    Add-SummaryLine $summary "Merged commit: ``$mergedCommit``"
    Add-SummaryLine $summary "Gate JSON: ``$gateJson``"
    Add-SummaryLine $summary "Manifest: ``$manifestPath``"
    $summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

    Write-Host "FNV live animation merge rehearsal PASS"
    Write-Host "Rehearsal JSON: $manifestPath"
    Write-Host "Summary: $summaryPath"
}
catch {
    $failure = [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-animation-merge-rehearsal-v1"
        createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        repoRoot = $RepoRoot
        sandbox = $sandbox
        mergeTarget = $MergeTarget
        sourceStatusShort = @($statusShort)
        trackedDiffCopied = [bool]($trackedDiff.Count -gt 0)
        untrackedCopied = @($untracked)
        gateVerdict = "FAIL"
        error = $_.Exception.Message
        payloadPolicy = "throwaway generated proof metadata and local sandbox paths only; no retail payload bytes"
    }
    $failure | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    throw
}
