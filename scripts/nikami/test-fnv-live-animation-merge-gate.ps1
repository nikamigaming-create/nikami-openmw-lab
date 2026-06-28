param(
    [string]$BuildDir = "build-clean",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:NIKAMI_VCPKG_ROOT,
    [string]$ProofRoot = "",
    [string]$ActorTarget = "GSEasyPete",
    [string]$FingerBone = "Bip01 R Finger21",
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

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $ProofRoot "fnv-live-animation-merge-gate\$stamp"
$summaryPath = Join-Path $runDir "summary.md"
$jsonPath = Join-Path $runDir "merge-gate.json"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

function Add-Line([System.Collections.Generic.List[string]]$Lines, [string]$Text = "") {
    $Lines.Add($Text)
    Write-Host $Text
}

function Invoke-GateCommand([string]$Name, [scriptblock]$Body) {
    $started = Get-Date
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = @()
    try {
        $output = @(& $Body 2>&1)
        $exitCode = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
        if ($exitCode -ne 0) {
            throw "$Name exited with code $exitCode"
        }
        return [pscustomobject][ordered]@{
            name = $Name
            status = "PASS"
            startedAt = $started.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            finishedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            output = @($output | ForEach-Object { [string]$_ })
        }
    }
    catch {
        return [pscustomobject][ordered]@{
            name = $Name
            status = "FAIL"
            startedAt = $started.ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            finishedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            error = $_.Exception.Message
            output = @($output | ForEach-Object { [string]$_ })
        }
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Invoke-RequiredGate([System.Collections.Generic.List[object]]$Results, [string]$Name, [scriptblock]$Body) {
    $result = Invoke-GateCommand $Name $Body
    $Results.Add($result)
}

function Get-GitOutput([string[]]$GitArgs) {
    $output = @(& git @GitArgs 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArgs -join ' ') failed: $($output -join [Environment]::NewLine)"
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Get-RemoteHead([string]$RemoteName, [string]$RefName) {
    $output = @(git ls-remote $RemoteName $RefName 2>&1)
    if ($LASTEXITCODE -ne 0 -or $output.Count -eq 0) {
        return ""
    }
    $first = [string]$output[0]
    $parts = $first -split "\s+"
    if ($parts.Count -eq 0) {
        return ""
    }
    return [string]$parts[0]
}

function Get-MergeConflictForecast([string]$LeftRef, [string]$RightRef) {
    $base = @(Get-GitOutput -GitArgs @("merge-base", $LeftRef, $RightRef))[0]
    $output = @(git merge-tree $base $LeftRef $RightRef 2>&1)
    if ($LASTEXITCODE -ne 0) {
        return [pscustomobject][ordered]@{
            status = "FAIL"
            mergeBase = $base
            conflictMarkers = @()
            outputSample = @($output | Select-Object -First 40 | ForEach-Object { [string]$_ })
            error = "git merge-tree failed while forecasting $RightRef into $LeftRef."
        }
    }

    $markers = @(
        $output |
            ForEach-Object { [string]$_ } |
            Where-Object { $_ -match "^(CONFLICT|<<<<<<<|=======|>>>>>>>)" -or $_ -match "(changed in both|added in both|removed in both)" }
    )
    return [pscustomobject][ordered]@{
        status = if ($markers.Count -eq 0) { "PASS" } else { "FAIL" }
        mergeBase = $base
        conflictMarkers = @($markers)
        outputSample = @($output | Select-Object -First 40 | ForEach-Object { [string]$_ })
        error = if ($markers.Count -eq 0) { "" } else { "git merge-tree forecast found conflict markers integrating $RightRef into $LeftRef." }
    }
}

function Get-LogTextAfterBoneAuthoring([string]$LogText, [string]$BoneName) {
    $appliedNeedle = "bone=`"$BoneName`""
    $appliedIndex = $LogText.IndexOf($appliedNeedle, [System.StringComparison]::Ordinal)
    if ($appliedIndex -lt 0) {
        return ""
    }
    return $LogText.Substring($appliedIndex)
}

function Test-LiveFingerResultFabricNoTwist([object]$Result) {
    $logPath = [string]$Result.liveOpenMwLog
    $fingerBone = [string]$Result.fingerBone
    if ([string]::IsNullOrWhiteSpace($logPath) -or [string]::IsNullOrWhiteSpace($fingerBone)) {
        return $false
    }
    if (!(Test-Path -LiteralPath $logPath -PathType Leaf)) {
        return $false
    }
    $tail = Get-LogTextAfterBoneAuthoring (Get-Content -LiteralPath $logPath -Raw) $fingerBone
    if ($tail -notlike "*gate=runtime-fnv-fabric-no-twist*") {
        return $false
    }
    if ($tail -match "fabric no-twist edge audit[^\r\n]*verdict=BAD[^\r\n]*gate=runtime-fnv-fabric-no-twist") {
        return $false
    }
    return $true
}

function Get-LatestPassingFingerSweep([string]$Root, [string]$ExpectedActorTarget, [string]$ExpectedFingerBone) {
    $sweepRoot = Join-Path $Root "fnv-live-finger-closeup-sweep"
    if (!(Test-Path -LiteralPath $sweepRoot -PathType Container)) { return $null }
    $jsonFiles = @(Get-ChildItem -LiteralPath $sweepRoot -Recurse -Filter "live-finger-closeup-sweep.json" -File |
        Sort-Object LastWriteTime -Descending)
    foreach ($file in $jsonFiles) {
        try {
            $doc = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if (([int]$doc.passCount -gt 0) -and
                ([int]$doc.failCount -eq 0) -and
                ([bool]$doc.requireRiggedHandConsumption) -and
                ([bool]$doc.requireFabricNoTwist)) {
                if (![string]::IsNullOrWhiteSpace($ExpectedActorTarget) -and [string]$doc.actorTarget -ne $ExpectedActorTarget) { continue }
                if (![string]::IsNullOrWhiteSpace($ExpectedFingerBone) -and @($doc.requestedFingerBones) -notcontains $ExpectedFingerBone) { continue }
                $resultArray = @($doc.results)
                if ($resultArray.Count -eq 0) { continue }
                $unaudited = @($resultArray | Where-Object {
                    ($_.status -ne "PASS") -or
                    (-not [bool]$_.riggedHandConsumptionAudited) -or
                    (-not [bool]$_.liveRigWeightDebugAudited) -or
                    (-not [bool]$_.fabricNoTwistAudited) -or
                    (-not (Test-LiveFingerResultFabricNoTwist $_))
                })
                if ($unaudited.Count -gt 0) { continue }
                return [pscustomobject][ordered]@{
                    path = $file.FullName
                    runDir = Split-Path $file.FullName -Parent
                    passCount = [int]$doc.passCount
                    failCount = [int]$doc.failCount
                    requireFabricNoTwist = [bool]$doc.requireFabricNoTwist
                    results = $resultArray
                }
            }
        }
        catch {
        }
    }
    return $null
}

function Get-FirstJsonItem([object]$Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Array]) {
        if ($Value.Count -eq 0) { return $null }
        return $Value[0]
    }
    return $Value
}

function ConvertTo-IntSafe([object]$Value) {
    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) { return 0 }
    if ($text -match "^\s*(?<value>[-+]?\d+)") {
        return [int]$Matches["value"]
    }
    return 0
}

function Test-TposeBaselineActorTarget([object]$Case, [string]$LogText, [string]$ExpectedActorTarget) {
    if ([string]::IsNullOrWhiteSpace($ExpectedActorTarget)) {
        return $true
    }
    $caseActorTarget = if ($null -ne $Case.PSObject.Properties["actorTarget"]) { [string]$Case.actorTarget } else { "" }
    if ($caseActorTarget -eq $ExpectedActorTarget) {
        return $true
    }
    $escapedTarget = [regex]::Escape($ExpectedActorTarget)
    if ($LogText -match "CHARACTER BUILDER begin phase=t-pose actor=$escapedTarget\b") {
        return $true
    }
    if ($LogText -match "actor part assembly target match target=`"$escapedTarget`" actor=$escapedTarget\b") {
        return $true
    }
    return $false
}

function Get-LatestPassingTposeBonesWeightsBaseline([string]$Root, [string]$ExpectedActorTarget) {
    $builderRoot = Join-Path $Root "fnv-character-builder"
    if (!(Test-Path -LiteralPath $builderRoot -PathType Container)) { return $null }
    $jsonFiles = @(Get-ChildItem -LiteralPath $builderRoot -Recurse -Filter "character-builder-suite.json" -File |
        Sort-Object LastWriteTime -Descending)
    foreach ($file in $jsonFiles) {
        try {
            $case = Get-FirstJsonItem (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json)
            if ($null -eq $case) { continue }
            if ([string]$case.phase -ne "t-pose") { continue }
            if ([string]$case.runtimeGateStatus -ne "PASS" -or [string]$case.reportStatus -ne "PASS") { continue }

            $evidence = $case.runtimeEvidence
            if ($null -eq $evidence) { continue }
            if ([string]$evidence.fnvShowIkBones -ne "True") { continue }
            $overlayLines = ConvertTo-IntSafe $evidence.weaponIkBoneOverlayProofLines
            if ($overlayLines -le 0) { continue }
            $staticHand = [string]$evidence.staticHandGripProofLines
            if ($staticHand -notmatch "left=0 right=0 vertices=0") {
                # This field changed from pending articulation to static-grip-runtime; do not require a grip deformation.
            }

            $proofDir = [string]$case.proofDir
            $caseDir = [string]$case.caseDir
            $openMwLog = Join-Path $proofDir "openmw.log"
            $screenshot = Join-Path $caseDir "screenshot000.png"
            if (!(Test-Path -LiteralPath $openMwLog -PathType Leaf)) { continue }
            if (!(Test-Path -LiteralPath $screenshot -PathType Leaf)) { continue }
            $logText = Get-Content -LiteralPath $openMwLog -Raw
            if (!(Test-TposeBaselineActorTarget $case $logText $ExpectedActorTarget)) { continue }
            $caseActorTarget = if ($null -ne $case.PSObject.Properties["actorTarget"]) { [string]$case.actorTarget } else { "" }
            if ($logText -notlike "*gate=runtime-fnv-weapon-ik-bone-overlay*") { continue }
            if ($logText -notlike "*gate=runtime-fnv-all-bone-overlay*") { continue }
            if ($logText -notlike "*gate=runtime-fnv-fabric-no-twist*") { continue }
            if ($logText -match "gate=runtime-fnv-fabric-no-twist[^\r\n]*verdict=BAD") { continue }
            if ($logText -match "fabric no-twist edge audit[^\r\n]*verdict=BAD") { continue }
            if ($logText -notlike "*gate=runtime-fnv-static-hand-weight-debug*") { continue }
            if ($logText -notmatch "weightedVertices=(?<vertices>\d+)") { continue }
            if ([int]$Matches["vertices"] -le 0) { continue }
            if ($logText -notlike "*fingerWeights=LOADED*") { continue }

            return [pscustomobject][ordered]@{
                path = $file.FullName
                suiteDir = Split-Path $file.FullName -Parent
                case = [string]$case.case
                phase = [string]$case.phase
                actorTarget = if (![string]::IsNullOrWhiteSpace($caseActorTarget)) { $caseActorTarget } else { $ExpectedActorTarget }
                screenshot = $screenshot
                proofDir = $proofDir
                openMwLog = $openMwLog
                fnvShowIkBones = [string]$evidence.fnvShowIkBones
                weaponIkBoneOverlayProofLines = $overlayLines
                staticHandWeightDebug = "runtime-fnv-static-hand-weight-debug"
                fabricNoTwist = "runtime-fnv-fabric-no-twist"
                payloadPolicy = "generated proof metadata/log references only; no retail payload bytes"
            }
        }
        catch {
        }
    }
    return $null
}

$results = New-Object "System.Collections.Generic.List[object]"
$summary = New-Object "System.Collections.Generic.List[string]"

Push-Location $RepoRoot
try {
    $branchOutput = @(Get-GitOutput -GitArgs @("branch", "--show-current"))
    $branch = if ($branchOutput.Count -gt 0 -and ![string]::IsNullOrWhiteSpace([string]$branchOutput[0])) {
        [string]$branchOutput[0]
    }
    else {
        $shortHead = @(Get-GitOutput -GitArgs @("rev-parse", "--short", "HEAD"))[0]
        "DETACHED@$shortHead"
    }
    $aheadBehindRaw = @(Get-GitOutput -GitArgs @("rev-list", "--left-right", "--count", "origin/main...HEAD"))[0]
    $parts = $aheadBehindRaw -split "\s+"
    $behindOriginMain = [int]$parts[0]
    $aheadOriginMain = [int]$parts[1]
    $statusShort = Get-GitOutput -GitArgs @("status", "--short", "--branch")
    $localBranches = Get-GitOutput -GitArgs @("branch", "--format=%(refname:short)")
    $remoteBranches = Get-GitOutput -GitArgs @("branch", "-r", "--format=%(refname:short)")
    $localOriginMainHead = @(Get-GitOutput -GitArgs @("rev-parse", "origin/main"))[0]
    $remoteOriginMainHead = Get-RemoteHead "origin" "refs/heads/main"
    $localCanonicalBranchHead = @(Get-GitOutput -GitArgs @("rev-parse", "origin/nikami/fnv-vr-hands-hud"))[0]
    $remoteCanonicalBranchHead = Get-RemoteHead "origin" "refs/heads/nikami/fnv-vr-hands-hud"

    Invoke-RequiredGate $results "git-diff-check" { git diff --check }

    $dirtyStatus = @($statusShort | Where-Object { $_ -notmatch "^## " })
    if ($dirtyStatus.Count -gt 0) {
        $results.Add([pscustomobject][ordered]@{
            name = "worktree-clean-for-merge-promotion"
            status = "FAIL"
            dirtyEntryCount = $dirtyStatus.Count
            dirtyEntries = @($dirtyStatus)
            error = "Working tree has uncommitted or untracked files; commit or shelve the checkpoint before merge promotion."
        })
    }
    else {
        $results.Add([pscustomobject][ordered]@{
            name = "worktree-clean-for-merge-promotion"
            status = "PASS"
            dirtyEntryCount = 0
            dirtyEntries = @()
        })
    }

    if ([string]::IsNullOrWhiteSpace($remoteOriginMainHead) -or $localOriginMainHead -ne $remoteOriginMainHead) {
        $results.Add([pscustomobject][ordered]@{
            name = "origin-main-tracking-fresh"
            status = "FAIL"
            localOriginMainHead = $localOriginMainHead
            remoteOriginMainHead = $remoteOriginMainHead
            error = "Local origin/main is stale or remote main could not be verified. Run git fetch origin before merge promotion."
        })
    }
    else {
        $results.Add([pscustomobject][ordered]@{
            name = "origin-main-tracking-fresh"
            status = "PASS"
            localOriginMainHead = $localOriginMainHead
            remoteOriginMainHead = $remoteOriginMainHead
        })
    }

    if (![string]::IsNullOrWhiteSpace($remoteCanonicalBranchHead) -and $localCanonicalBranchHead -ne $remoteCanonicalBranchHead) {
        $results.Add([pscustomobject][ordered]@{
            name = "canonical-branch-tracking-fresh"
            status = "FAIL"
            localCanonicalBranchHead = $localCanonicalBranchHead
            remoteCanonicalBranchHead = $remoteCanonicalBranchHead
            error = "Local origin/nikami/fnv-vr-hands-hud does not match the remote branch."
        })
    }
    else {
        $results.Add([pscustomobject][ordered]@{
            name = "canonical-branch-tracking-fresh"
            status = "PASS"
            localCanonicalBranchHead = $localCanonicalBranchHead
            remoteCanonicalBranchHead = $remoteCanonicalBranchHead
        })
    }

    if ($behindOriginMain -gt 0) {
        $results.Add([pscustomobject][ordered]@{
            name = "origin-main-integrated"
            status = "FAIL"
            behindOriginMain = $behindOriginMain
            aheadOriginMain = $aheadOriginMain
            error = "Current branch is behind origin/main; integrate current main before final merge promotion."
        })
    }
    else {
        $results.Add([pscustomobject][ordered]@{
            name = "origin-main-integrated"
            status = "PASS"
            behindOriginMain = $behindOriginMain
            aheadOriginMain = $aheadOriginMain
        })
    }

    $mergeConflictForecast = Get-MergeConflictForecast "HEAD" "origin/main"
    $results.Add([pscustomobject][ordered]@{
        name = "origin-main-merge-conflict-forecast"
        status = $mergeConflictForecast.status
        mergeBase = $mergeConflictForecast.mergeBase
        conflictMarkers = @($mergeConflictForecast.conflictMarkers)
        outputSample = @($mergeConflictForecast.outputSample)
        error = $mergeConflictForecast.error
    })

    $syntaxFiles = @(
        "scripts/nikami/test-fnv-live-bone-authoring-runtime.ps1",
        "scripts/nikami/test-fnv-live-bone-authoring-closeup.ps1",
        "scripts/nikami/run-fnv-live-finger-closeup-sweep.ps1",
        "scripts/nikami/run-fnv-animation-rotation-sweep.ps1",
        "scripts/nikami/test-fnv-live-animation-merge-gate.ps1",
        "scripts/nikami/test-fnv-proof-harness-contract.ps1"
    )
    Invoke-RequiredGate $results "powershell-syntax" {
        foreach ($file in $syntaxFiles) {
            $text = Get-Content -LiteralPath $file -Raw
            [void][scriptblock]::Create($text)
            "syntax OK $file"
        }
    }

    Invoke-RequiredGate $results "python-compile-live-studio" {
        python -m py_compile `
            scripts/nikami/fnv_character_viewer_live_server.py `
            scripts/nikami/fnv_character_studio_catalog.py `
            scripts/nikami/test-fnv-character-studio-live-server.py
        if ($LASTEXITCODE -eq 0) { "python compile PASS" }
    }

    Invoke-RequiredGate $results "proof-harness-contract" {
        powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/test-fnv-proof-harness-contract.ps1
    }

    $tposeBonesWeightsBaseline = $null
    if ($RunTposeWeightBaseline) {
        Invoke-RequiredGate $results "t-pose-bones-weights-baseline" {
            $previousWireframe = $env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME
            $previousFingerWeights = $env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS
            $previousSkinWeights = $env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS
            $previousWeightBone = $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE
            $previousShowAllBones = $env:OPENMW_FNV_SHOW_ALL_BONES
            $previousFabricDetail = $env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL
            try {
                $env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME = "1"
                $env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS = "1"
                $env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS = "1"
                $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = "all"
                $env:OPENMW_FNV_SHOW_ALL_BONES = "1"
                $env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL = "1"
                $args = @(
                    "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", "scripts/nikami/run-fnv-character-builder-tester.ps1",
                    "-BuildDir", $BuildDir,
                    "-Configuration", $Configuration,
                    "-ProofRoot", $ProofRoot,
                    "-ActorTarget", $ActorTarget,
                    "-ActorKind", "npc",
                    "-Phases", "t-pose",
                    "-Angles", "front",
                    "-NeutralActorPreviewProfile", "right-hand-close",
                    "-RunSeconds", "42",
                    "-ScreenshotFrames", "240"
                )
                if (![string]::IsNullOrWhiteSpace($VcpkgRoot)) {
                    $args += @("-VcpkgRoot", $VcpkgRoot)
                }
                if ($NoSound) {
                    $args += "-NoSound"
                }
                powershell @args
            }
            finally {
                if ($null -eq $previousWireframe) { Remove-Item Env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME = $previousWireframe }
                if ($null -eq $previousFingerWeights) { Remove-Item Env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS = $previousFingerWeights }
                if ($null -eq $previousSkinWeights) { Remove-Item Env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS = $previousSkinWeights }
                if ($null -eq $previousWeightBone) { Remove-Item Env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE = $previousWeightBone }
                if ($null -eq $previousShowAllBones) { Remove-Item Env:OPENMW_FNV_SHOW_ALL_BONES -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_SHOW_ALL_BONES = $previousShowAllBones }
                if ($null -eq $previousFabricDetail) { Remove-Item Env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL -ErrorAction SilentlyContinue } else { $env:OPENMW_FNV_FABRIC_NO_TWIST_DETAIL = $previousFabricDetail }
            }
        }
    }
    $tposeBonesWeightsBaseline = Get-LatestPassingTposeBonesWeightsBaseline $ProofRoot $ActorTarget
    if ($null -eq $tposeBonesWeightsBaseline) {
        $results.Add([pscustomobject][ordered]@{
            name = "t-pose-bones-weights-baseline-artifact"
            status = "FAIL"
            error = "No passing T-pose bones/weights baseline artifact found. Re-run with -RunTposeWeightBaseline."
        })
    }
    if (!$RunTposeWeightBaseline -and $null -ne $tposeBonesWeightsBaseline) {
        $results.Add([pscustomobject][ordered]@{
            name = "t-pose-bones-weights-baseline"
            status = "PASS"
            artifact = $tposeBonesWeightsBaseline.path
            screenshot = $tposeBonesWeightsBaseline.screenshot
            openMwLog = $tposeBonesWeightsBaseline.openMwLog
        })
    }

    $liveFingerSweep = $null
    if ($RunLiveFingerSmoke) {
        Invoke-RequiredGate $results "live-finger-smoke" {
            $args = @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", "scripts/nikami/run-fnv-live-finger-closeup-sweep.ps1",
                "-BuildDir", $BuildDir,
                "-Configuration", $Configuration,
                "-ActorTarget", $ActorTarget,
                "-ActorKind", "npc",
                "-NeutralActorPreviewProfile", "right-hand-close",
                "-FingerBones", $FingerBone,
                "-HandRotationZ", "0",
                "-FingerRotationZ", "15"
            )
            if (![string]::IsNullOrWhiteSpace($VcpkgRoot)) {
                $args += @("-VcpkgRoot", $VcpkgRoot)
            }
            if ($NoSound) {
                $args += "-NoSound"
            }
            powershell @args
        }
        $liveFingerSweep = Get-LatestPassingFingerSweep $ProofRoot $ActorTarget $FingerBone
        if ($null -eq $liveFingerSweep) {
            $results.Add([pscustomobject][ordered]@{
                name = "latest-live-finger-sweep-artifact"
                status = "FAIL"
                error = "Live finger smoke completed but no passing fabric-audited sweep artifact was discoverable."
            })
        }
        else {
            $results.Add([pscustomobject][ordered]@{
                name = "latest-live-finger-sweep-artifact"
                status = "PASS"
                artifact = $liveFingerSweep.path
            })
        }
    }
    else {
        $liveFingerSweep = Get-LatestPassingFingerSweep $ProofRoot $ActorTarget $FingerBone
        if ($null -eq $liveFingerSweep) {
            $results.Add([pscustomobject][ordered]@{
                name = "latest-live-finger-sweep-artifact"
                status = "FAIL"
                error = "No passing live finger sweep artifact found. Re-run with -RunLiveFingerSmoke."
            })
        }
        else {
            $results.Add([pscustomobject][ordered]@{
                name = "latest-live-finger-sweep-artifact"
                status = "PASS"
                artifact = $liveFingerSweep.path
            })
        }
    }

    if ($RunBuild) {
        Invoke-RequiredGate $results "release-build-openmw" {
            $args = @(
                "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", "scripts/nikami/build-clean-openmw.ps1",
                "-BuildDir", $BuildDir,
                "-Configuration", $Configuration
            )
            if (![string]::IsNullOrWhiteSpace($VcpkgRoot)) {
                $args += @("-VcpkgRoot", $VcpkgRoot)
            }
            powershell @args
        }
    }
    else {
        $results.Add([pscustomobject][ordered]@{
            name = "release-build-openmw"
            status = "SKIP"
            reason = "Pass -RunBuild to compile OpenMW as part of this gate."
        })
    }

    $failed = @($results | Where-Object { $_.status -eq "FAIL" })
    $doc = [pscustomobject][ordered]@{
        schema = "nikami-fnv-live-animation-merge-gate-v1"
        createdAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        repoRoot = $RepoRoot
        branch = $branch
        mergeDestination = "origin/main"
        behindOriginMain = $behindOriginMain
        aheadOriginMain = $aheadOriginMain
        localOriginMainHead = $localOriginMainHead
        remoteOriginMainHead = $remoteOriginMainHead
        originMainMergeConflictForecast = $mergeConflictForecast
        localCanonicalBranchHead = $localCanonicalBranchHead
        remoteCanonicalBranchHead = $remoteCanonicalBranchHead
        localBranchCount = @($localBranches).Count
        remoteBranchCount = @($remoteBranches).Count
        statusShort = @($statusShort)
        canonicalBranch = "nikami/fnv-vr-hands-hud"
        quarantine = @(
            "D:\code\vulkanOpenMW\nikami-openmw-lab",
            "D:\code\vulkanOpenMW\vsgopenmw",
            "old-fnv/*",
            "backup/*"
        )
        liveFingerSweep = $liveFingerSweep
        tposeBonesWeightsBaseline = $tposeBonesWeightsBaseline
        runBuild = [bool]$RunBuild
        runLiveFingerSmoke = [bool]$RunLiveFingerSmoke
        runTposeWeightBaseline = [bool]$RunTposeWeightBaseline
        verdict = if ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
        results = @($results.ToArray())
    }
    $doc | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

    Add-Line $summary "# FNV Live Animation Merge Gate"
    Add-Line $summary ""
    Add-Line $summary "Verdict: ``$($doc.verdict)``"
    Add-Line $summary "Branch: ``$branch``"
    Add-Line $summary "Ahead/behind origin/main: ``$aheadOriginMain ahead / $behindOriginMain behind``"
    Add-Line $summary "Local origin/main: ``$localOriginMainHead``"
    Add-Line $summary "Remote origin/main: ``$remoteOriginMainHead``"
    Add-Line $summary "Local branches: ``$($doc.localBranchCount)``"
    Add-Line $summary "Remote refs: ``$($doc.remoteBranchCount)``"
    $tposePath = if ($null -ne $tposeBonesWeightsBaseline) { $tposeBonesWeightsBaseline.path } else { "<missing>" }
    $liveFingerPath = if ($null -ne $liveFingerSweep) { $liveFingerSweep.path } else { "<missing>" }
    Add-Line $summary "T-pose bones/weights baseline: ``$tposePath``"
    Add-Line $summary "Latest live finger sweep: ``$liveFingerPath``"
    Add-Line $summary ""
    Add-Line $summary "| Gate | Status |"
    Add-Line $summary "| --- | --- |"
    foreach ($result in $results) {
        Add-Line $summary "| $($result.name) | $($result.status) |"
    }
    Add-Line $summary ""
    Add-Line $summary "JSON: ``$jsonPath``"
    $summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

    if ($failed.Count -gt 0) {
        throw "FNV live animation merge gate failed. See $jsonPath"
    }

    Write-Host "FNV live animation merge gate PASS"
    Write-Host "Gate JSON: $jsonPath"
    Write-Host "Summary: $summaryPath"
}
finally {
    Pop-Location
}
