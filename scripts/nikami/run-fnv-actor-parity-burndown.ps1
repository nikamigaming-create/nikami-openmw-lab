param(
    [string]$ProofRoot = "",
    [string]$PlanJson = "",
    [string]$OutDir = "",
    [int]$Limit = 0,
    [string]$BurnDownJson = "",
    [switch]$RunRows,
    [switch]$DryRun,
    [string]$RowId = "",
    [ValidateSet("all", "npc", "creature")]
    [string]$ActorKind = "all",
    [string]$Target = "",
    [string]$Priority = "",
    [string]$Phase = "",
    [string]$Gate = "",
    [string]$Classification = "",
    [int]$MaxRows = 1,
    [int]$RowOffset = 0,
    [int]$ShardIndex = 0,
    [int]$ShardCount = 0,
    [int]$RunSeconds = 28,
    [int]$ActorFrame = 520,
    [string]$ScreenshotFrames = "120,180,240,300",
    [string[]]$Angles = @("front", "front-left", "front-right"),
    [string]$ViewerRunner = "",
    [switch]$NoSound,
    [switch]$Serve,
    [int]$ServePort = 0,
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
if ([string]::IsNullOrWhiteSpace($ViewerRunner)) {
    $ViewerRunner = Join-Path $PSScriptRoot "run-fnv-character-viewer.ps1"
}
if (!(Test-Path -LiteralPath $Generator -PathType Leaf)) {
    throw "Missing FNV actor parity burn-down generator: $Generator"
}
if (!(Test-Path -LiteralPath $BatchPlanner -PathType Leaf)) {
    throw "Missing FNV character viewer batch planner runner: $BatchPlanner"
}
if (!(Test-Path -LiteralPath $ViewerRunner -PathType Leaf)) {
    throw "Missing FNV character viewer runner: $ViewerRunner"
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

function Get-LatestBurnDownJson {
    $root = Join-Path $ProofRoot "fnv-actor-parity-burndown"
    if (!(Test-Path -LiteralPath $root -PathType Container)) {
        return ""
    }
    $latest = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "actor-parity-burndown.json") -PathType Leaf } |
        Select-Object -First 1
    if ($null -eq $latest) {
        return ""
    }
    return Join-Path $latest.FullName "actor-parity-burndown.json"
}

function New-RunDirectory {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $runRoot = Join-Path $ProofRoot "fnv-actor-parity-burndown-run"
    $runDir = Join-Path $runRoot $stamp
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    return $runDir
}

function ConvertTo-PlainArray($Value) {
    if ($null -eq $Value) { return @() }
    if ($Value -is [array]) { return @($Value) }
    return @($Value)
}

function Get-ObjectProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Resolve-ValidatedAngles([string[]]$Values) {
    $allowed = @("front", "front-left", "front-right")
    $items = [System.Collections.Generic.List[string]]::new()
    foreach ($value in @($Values)) {
        foreach ($part in ([string]$value -split ",")) {
            $trimmed = $part.Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
            if ($allowed -notcontains $trimmed) {
                throw "Unknown actor burn-down camera angle '$trimmed'. Valid angles: $($allowed -join ',')"
            }
            if (!$items.Contains($trimmed)) {
                $items.Add($trimmed)
            }
        }
    }
    if ($items.Count -eq 0) {
        throw "No actor burn-down camera angles selected."
    }
    return @($items.ToArray())
}

function Add-DoubleArgIfPresent([hashtable]$ArgMap, [string]$Name, [object]$Value) {
    if ($null -eq $Value) { return }
    $text = [string]$Value
    if ([string]::IsNullOrWhiteSpace($text)) { return }
    try {
        $ArgMap[$Name] = [double]::Parse($text, [Globalization.CultureInfo]::InvariantCulture)
    }
    catch {
        throw "Invalid numeric placement value for ${Name}: $text"
    }
}

function Add-PlacementArgsIfPresent([hashtable]$ArgMap, [object]$Placement) {
    if ($null -eq $Placement) { return }
    $ready = Get-ObjectProperty $Placement "runtimeBootstrapReady"
    if ($ready -ne $true) { return }
    $cell = [string](Get-ObjectProperty $Placement "cell")
    if (![string]::IsNullOrWhiteSpace($cell)) {
        $ArgMap.BootstrapCell = $cell
    }
    $position = Get-ObjectProperty $Placement "position"
    $rotation = Get-ObjectProperty $Placement "rotation"
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapX" -Value (Get-ObjectProperty $position "x")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapY" -Value (Get-ObjectProperty $position "y")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapZ" -Value (Get-ObjectProperty $position "z")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageX" -Value (Get-ObjectProperty $position "x")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageY" -Value (Get-ObjectProperty $position "y")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageZ" -Value (Get-ObjectProperty $position "z")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapRotX" -Value (Get-ObjectProperty $rotation "x")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapRotY" -Value (Get-ObjectProperty $rotation "y")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "BootstrapRotZ" -Value (Get-ObjectProperty $rotation "z")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageRotX" -Value (Get-ObjectProperty $rotation "x")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageRotY" -Value (Get-ObjectProperty $rotation "y")
    Add-DoubleArgIfPresent -ArgMap $ArgMap -Name "ActorStageRotZ" -Value (Get-ObjectProperty $rotation "z")
}

function Resolve-ActorKitAnimationGroup([string[]]$RuntimeStates) {
    $states = @($RuntimeStates | ForEach-Object { ([string]$_).ToLowerInvariant() })
    if ($states -contains "projectile-fire") { return "attackright" }
    if ($states -contains "attack") { return "attackright" }
    if ($states -contains "run") { return "runforward" }
    if ($states -contains "walk") { return "walkforward" }
    if ($states -contains "idle" -or $states -contains "neutral") { return "idle" }
    return ""
}

function Resolve-ActorKitDialogueMode([string[]]$RuntimeStates) {
    $states = @($RuntimeStates | ForEach-Object { ([string]$_).ToLowerInvariant() })
    if ($states -contains "mouth-open") { return "mouth-open-pose" }
    if ($states -contains "talk") { return "mouth-open" }
    return ""
}

function Assert-BurnDownMatrix([object]$BurnDown, [string]$Path) {
    if ($null -eq $BurnDown) {
        throw "Burn-down JSON could not be parsed: $Path"
    }
    if ([string]$BurnDown.schema -ne "nikami-fnv-actor-parity-burndown-v1") {
        throw "Unexpected actor parity burn-down schema in $Path`: $($BurnDown.schema)"
    }
    if ([string]$BurnDown.status -ne "PASS") {
        throw "Actor parity burn-down matrix is not PASS: $Path status=$($BurnDown.status)"
    }
    $allowed = @(
        "runtime-supported",
        "loaded-pending-runtime",
        "known-blocked",
        "non-runtime-support-file",
        "intentionally-excluded-with-proof"
    )
    $rows = @(ConvertTo-PlainArray $BurnDown.rows)
    if ($rows.Count -eq 0) {
        throw "Actor parity burn-down matrix has no rows: $Path"
    }
    $invalid = @($rows | Where-Object { $allowed -notcontains [string]$_.classification })
    $unclassified = @($rows | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.classification) })
    $missingCommands = @($rows | Where-Object {
            ([string]$_.classification -eq "runtime-supported" -or [string]$_.classification -eq "loaded-pending-runtime") -and
            [string]::IsNullOrWhiteSpace([string]$_.runGateCommand)
        })
    if ($invalid.Count -gt 0 -or $unclassified.Count -gt 0 -or $missingCommands.Count -gt 0) {
        throw "Actor parity burn-down matrix has invalid runtime accounting: invalid=$($invalid.Count) unclassified=$($unclassified.Count) missingCommands=$($missingCommands.Count) path=$Path"
    }
    $counts = Get-ObjectProperty $BurnDown "counts"
    if ($null -ne $counts) {
        foreach ($field in @("invalidClassification", "unclassified", "missingRuntimeCommand")) {
            $value = Get-ObjectProperty $counts $field
            if ($null -ne $value -and [int]$value -ne 0) {
                throw "Actor parity burn-down count $field is non-zero in $Path`: $value"
            }
        }
    }
}

function Add-UniqueString([System.Collections.Generic.List[string]]$List, [string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return }
    if (!$List.Contains($Value)) {
        $List.Add($Value)
    }
}

function Test-ObjectArrayHasItems([object]$Object, [string]$Name) {
    return @(ConvertTo-PlainArray (Get-ObjectProperty $Object $Name)).Count -gt 0
}

function Test-CaseLineNeedle([object[]]$Cases, [string[]]$Names, [string[]]$Needles) {
    foreach ($case in @($Cases)) {
        foreach ($name in @($Names)) {
            foreach ($item in @(ConvertTo-PlainArray (Get-ObjectProperty $case $name))) {
                $text = [string]$item
                if ($item -isnot [string]) {
                    $text = ($item | ConvertTo-Json -Depth 6 -Compress)
                }
                foreach ($needle in @($Needles)) {
                    if ($text -match [regex]::Escape($needle)) {
                        return $true
                    }
                }
            }
        }
    }
    return $false
}

function Get-RowRequiredEvidenceKinds([object]$Row) {
    $required = [System.Collections.Generic.List[string]]::new()
    foreach ($kind in @("viewer-manifest", "phase-angle", "case-pass")) {
        Add-UniqueString $required $kind
    }

    $gate = ([string](Get-ObjectProperty $Row "gate")).ToLowerInvariant()
    $phase = ([string](Get-ObjectProperty $Row "phase")).ToLowerInvariant()
    $states = @(ConvertTo-PlainArray (Get-ObjectProperty $Row "runtimeStates") | ForEach-Object { ([string]$_).ToLowerInvariant() })

    if ($phase -notmatch "^creature-") {
        Add-UniqueString $required "screenshots"
    }

    switch ($gate) {
        "actor-base-record" { Add-UniqueString $required "actor-match" }
        "race-body-skeleton" { Add-UniqueString $required "actor-match"; Add-UniqueString $required "runtime-part-audits" }
        "skin-material-tone" { Add-UniqueString $required "material-evidence" }
        "full-body-screenshot" { Add-UniqueString $required "screenshots"; Add-UniqueString $required "assembly-inventory" }
        "standing-pose-family" { Add-UniqueString $required "animation-playback" }
        "crouch-pose-family" { Add-UniqueString $required "animation-playback" }
        "kneel-pose-family" { Add-UniqueString $required "animation-playback" }
        "prone-pose-family" { Add-UniqueString $required "animation-playback" }
        "headpart-stack" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "face-drawables" }
        "facegen-morph-targets" { Add-UniqueString $required "morph-lines" }
        "head-transform-pivot" { Add-UniqueString $required "runtime-part-audits"; Add-UniqueString $required "attachment-bounds" }
        "face-skin-tone-wrinkles" { Add-UniqueString $required "material-evidence"; Add-UniqueString $required "face-drawables" }
        "eyes-mouth-teeth-tongue" { Add-UniqueString $required "face-drawables" }
        "face-closeup-screenshot" { Add-UniqueString $required "screenshots"; Add-UniqueString $required "face-drawables" }
        "hair-beard-brow" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits" }
        "hair-under-headgear-policy" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits" }
        "outfit-inventory-resolution" { Add-UniqueString $required "assembly-inventory" }
        "armor-addon-geometry" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits" }
        "biped-slot-visibility" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits" }
        "weapon-prop-attachment" { Add-UniqueString $required "weapon-lines"; Add-UniqueString $required "attachment-bounds"; Add-UniqueString $required "runtime-part-audits" }
        "weapon-animation-family" { Add-UniqueString $required "weapon-lines"; Add-UniqueString $required "animation-playback" }
        "projectile-muzzle-sound" { Add-UniqueString $required "weapon-lines"; Add-UniqueString $required "projectile-runtime-evidence" }
        "hand-weapon-transform" { Add-UniqueString $required "weapon-lines"; Add-UniqueString $required "runtime-part-audits" }
        "headgear-slot-composition" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits" }
        "hat-hair-occlusion" { Add-UniqueString $required "assembly-inventory"; Add-UniqueString $required "runtime-part-audits"; Add-UniqueString $required "face-drawables" }
        "headgear-transform-pivot" { Add-UniqueString $required "attachment-bounds"; Add-UniqueString $required "runtime-part-audits" }
        "dialogue-info-selection" { Add-UniqueString $required "dialogue-mode" }
        "voice-lip-sidecar" { Add-UniqueString $required "dialogue-mode"; Add-UniqueString $required "mouth-runtime-evidence" }
        "mouth-teeth-lip-sync" { Add-UniqueString $required "dialogue-mode"; Add-UniqueString $required "mouth-runtime-evidence"; Add-UniqueString $required "face-drawables" }
        "creature-model-root" { Add-UniqueString $required "creature-evidence" }
        "creature-bounds-camera" { Add-UniqueString $required "creature-evidence" }
        "creature-bodypart-data" { Add-UniqueString $required "creature-evidence"; Add-UniqueString $required "assembly-inventory" }
        "creature-material-texture" { Add-UniqueString $required "creature-evidence" }
        "creature-idle-walk-run" { Add-UniqueString $required "creature-evidence"; Add-UniqueString $required "animation-playback" }
        "creature-attack-hit-death" { Add-UniqueString $required "creature-evidence"; Add-UniqueString $required "animation-playback" }
        "creature-full-runtime-view" { Add-UniqueString $required "creature-evidence"; Add-UniqueString $required "assembly-inventory" }
    }

    foreach ($state in @($states)) {
        switch ($state) {
            "talk" { Add-UniqueString $required "dialogue-mode" }
            "mouth-open" { Add-UniqueString $required "mouth-runtime-evidence" }
            "attack" { Add-UniqueString $required "animation-playback" }
            "reload" { Add-UniqueString $required "animation-playback" }
            "walk" { Add-UniqueString $required "animation-playback" }
            "run" { Add-UniqueString $required "animation-playback" }
            "idle" { Add-UniqueString $required "animation-playback" }
            "crouch" { Add-UniqueString $required "animation-playback" }
            "kneel" { Add-UniqueString $required "animation-playback" }
            "prone" { Add-UniqueString $required "animation-playback" }
            "projectile-fire" { Add-UniqueString $required "projectile-runtime-evidence" }
        }
    }

    return @($required.ToArray())
}

function Get-ManifestEvidenceKinds([object]$Manifest, [object[]]$Cases) {
    $evidence = [System.Collections.Generic.List[string]]::new()
    Add-UniqueString $evidence "viewer-manifest"
    if (@($Cases).Count -gt 0) {
        Add-UniqueString $evidence "phase-angle"
        $badCases = @($Cases | Where-Object { [string]$_.runtimeGateStatus -ne "PASS" -or [string]$_.reportStatus -ne "PASS" })
        if ($badCases.Count -eq 0) {
            Add-UniqueString $evidence "case-pass"
        }
    }
    if (Test-ObjectArrayHasItems $Manifest "assemblyInventory") { Add-UniqueString $evidence "assembly-inventory" }

    foreach ($case in @($Cases)) {
        if (Test-ObjectArrayHasItems $case "screenshots" -or ![string]::IsNullOrWhiteSpace([string](Get-ObjectProperty $case "mainImage"))) { Add-UniqueString $evidence "screenshots" }
        if (Test-ObjectArrayHasItems $case "actorMatches") { Add-UniqueString $evidence "actor-match" }
        if (Test-ObjectArrayHasItems $case "attachmentBounds") { Add-UniqueString $evidence "attachment-bounds" }
        if (Test-ObjectArrayHasItems $case "runtimePartAudits") { Add-UniqueString $evidence "runtime-part-audits" }
        if (Test-ObjectArrayHasItems $case "runtimeAuditSummary") { Add-UniqueString $evidence "runtime-audit-summary" }
        if (Test-ObjectArrayHasItems $case "runtimePartTimelines") { Add-UniqueString $evidence "runtime-part-timelines" }
        if (Test-ObjectArrayHasItems $case "faceDrawables") { Add-UniqueString $evidence "face-drawables" }
        if (Test-ObjectArrayHasItems $case "materialEvidence") { Add-UniqueString $evidence "material-evidence" }
        if (Test-ObjectArrayHasItems $case "morphLines") { Add-UniqueString $evidence "morph-lines" }
        if (Test-ObjectArrayHasItems $case "weaponLines") { Add-UniqueString $evidence "weapon-lines" }
        if (Test-ObjectArrayHasItems $case "animationPlayback") { Add-UniqueString $evidence "animation-playback" }
        if (Test-ObjectArrayHasItems $case "animationRequests") { Add-UniqueString $evidence "animation-request" }
        if (Test-ObjectArrayHasItems $case "creatureEvidence") { Add-UniqueString $evidence "creature-evidence" }
        $selection = Get-ObjectProperty $case "actorKitSelection"
        if (![string]::IsNullOrWhiteSpace([string](Get-ObjectProperty $selection "dialogueMode"))) { Add-UniqueString $evidence "dialogue-mode" }
        if (![string]::IsNullOrWhiteSpace([string](Get-ObjectProperty $selection "animationGroup"))) { Add-UniqueString $evidence "animation-group" }
    }

    $controls = Get-ObjectProperty $Manifest "controls"
    if (Test-ObjectArrayHasItems $controls "dialogueControls") { Add-UniqueString $evidence "dialogue-controls" }
    if (Test-CaseLineNeedle $Cases @("animationLines", "morphLines", "skinLines", "hairLines") @("mouth driver", "dialogue morph", "dialogue pose", "mouth open")) { Add-UniqueString $evidence "mouth-runtime-evidence" }
    if (Test-CaseLineNeedle $Cases @("weaponLines", "animationLines") @("projectile", "muzzle", "ammo", "firing trace")) { Add-UniqueString $evidence "projectile-runtime-evidence" }
    return @($evidence.ToArray())
}

function New-RowGateAudit([object]$Row, [object]$Manifest, [string[]]$RequestedAngles) {
    $phaseValue = [string](Get-ObjectProperty $Row "phase")
    $gateValue = [string](Get-ObjectProperty $Row "gate")
    $runtimeStates = @(ConvertTo-PlainArray (Get-ObjectProperty $Row "runtimeStates") | ForEach-Object { [string]$_ })
    $manifestPhases = @(ConvertTo-PlainArray (Get-ObjectProperty $Manifest "phases") | ForEach-Object { [string]$_ })
    $manifestAngles = @(ConvertTo-PlainArray (Get-ObjectProperty $Manifest "angles") | ForEach-Object { [string]$_ })
    $cases = @(ConvertTo-PlainArray (Get-ObjectProperty $Manifest "cases") | Where-Object { [string]$_.phase -eq $phaseValue })
    $caseAngles = @($cases | ForEach-Object { [string]$_.angle } | Where-Object { ![string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
    $missingAngles = @($RequestedAngles | Where-Object {
            ($manifestAngles.Count -gt 0 -and $manifestAngles -notcontains $_) -or
            ($caseAngles.Count -gt 0 -and $caseAngles -notcontains $_)
        })
    $required = @(Get-RowRequiredEvidenceKinds $Row)
    $evidence = @(Get-ManifestEvidenceKinds $Manifest $cases)
    $missingEvidence = @($required | Where-Object { $evidence -notcontains $_ })
    $badCases = @($cases | Where-Object { [string]$_.runtimeGateStatus -ne "PASS" -or [string]$_.reportStatus -ne "PASS" })

    $errors = [System.Collections.Generic.List[string]]::new()
    if ([string](Get-ObjectProperty $Manifest "overallStatus") -ne "PASS" -and [string](Get-ObjectProperty $Manifest "status") -ne "PASS") {
        $errors.Add("child viewer manifest status is not PASS")
    }
    if ($manifestPhases.Count -gt 0 -and $manifestPhases -notcontains $phaseValue) {
        $errors.Add("child viewer manifest does not list selected phase")
    }
    if ($cases.Count -eq 0) {
        $errors.Add("child viewer manifest has no selected phase cases")
    }
    if ($missingAngles.Count -gt 0) {
        $errors.Add("child viewer manifest is missing selected angle(s): $($missingAngles -join ',')")
    }
    if ($badCases.Count -gt 0) {
        $errors.Add("child viewer case status is not PASS for selected phase")
    }

    $status = if ($errors.Count -gt 0) { "FAIL" } elseif ($missingEvidence.Count -gt 0) { "PENDING" } else { "PASS" }
    $classification = if ($status -eq "PASS") { "runtime-supported" } elseif ($status -eq "PENDING") { "loaded-pending-runtime" } else { "known-blocked" }

    return [pscustomobject][ordered]@{
        schema = "nikami-fnv-actor-row-gate-audit-v1"
        status = $status
        classification = $classification
        selectedPhase = $phaseValue
        selectedGate = $gateValue
        selectedRuntimeStates = @($runtimeStates)
        requestedAngles = @($RequestedAngles)
        manifestPhases = @($manifestPhases)
        manifestAngles = @($manifestAngles)
        caseCount = $cases.Count
        passingCaseCount = @($cases | Where-Object { [string]$_.runtimeGateStatus -eq "PASS" -and [string]$_.reportStatus -eq "PASS" }).Count
        missingAngles = @($missingAngles)
        requiredEvidenceKinds = @($required)
        observedEvidenceKinds = @($evidence)
        missingEvidenceKinds = @($missingEvidence)
        errors = @($errors.ToArray())
        boundary = if ($status -eq "PASS") {
            "Selected row has child manifest phase/angle/case success and the required gate/state evidence buckets."
        } elseif ($status -eq "PENDING") {
            "Selected row reached the viewer phase, but exact gate/state runtime evidence is incomplete."
        } else {
            "Selected row child runtime proof failed or did not cover the selected phase/angle."
        }
    }
}

function Select-BurnDownRows([object]$BurnDown) {
    $rows = @(ConvertTo-PlainArray $BurnDown.rows)
    if (![string]::IsNullOrWhiteSpace($RowId)) {
        $rows = @($rows | Where-Object { $_.id -eq $RowId })
    }
    if ($ActorKind -ne "all") {
        $rows = @($rows | Where-Object { $_.actorKind -eq $ActorKind })
    }
    if (![string]::IsNullOrWhiteSpace($Target)) {
        $rows = @($rows | Where-Object {
            $_.target -eq $Target -or $_.runtimeTarget -eq $Target -or $_.placedTarget -eq $Target -or
            $_.baseActorTarget -eq $Target -or $_.actorEditorId -eq $Target -or $_.actorFormId -eq $Target
        })
    }
    if (![string]::IsNullOrWhiteSpace($Priority)) {
        $rows = @($rows | Where-Object { $_.priority -eq $Priority })
    }
    if (![string]::IsNullOrWhiteSpace($Phase)) {
        $rows = @($rows | Where-Object { $_.phase -eq $Phase })
    }
    if (![string]::IsNullOrWhiteSpace($Gate)) {
        $rows = @($rows | Where-Object { $_.gate -eq $Gate })
    }
    if (![string]::IsNullOrWhiteSpace($Classification)) {
        $rows = @($rows | Where-Object { $_.classification -eq $Classification })
    }
    $filteredCount = $rows.Count

    if ($ShardCount -gt 0) {
        if ($ShardIndex -lt 1 -or $ShardIndex -gt $ShardCount) {
            throw "ShardIndex must be between 1 and ShardCount when sharding is enabled."
        }
        $slot = $ShardIndex - 1
        $sharded = [System.Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt $rows.Count; $index++) {
            if (($index % $ShardCount) -eq $slot) {
                $sharded.Add($rows[$index])
            }
        }
        $rows = @($sharded.ToArray())
    }
    elseif ($ShardIndex -gt 0) {
        throw "ShardIndex requires ShardCount."
    }
    $shardedCount = $rows.Count

    if ($RowOffset -lt 0) {
        throw "RowOffset must be zero or greater."
    }
    if ($RowOffset -gt 0) {
        $rows = @($rows | Select-Object -Skip $RowOffset)
    }
    $afterOffsetCount = $rows.Count

    if ($MaxRows -gt 0) {
        $rows = @($rows | Select-Object -First $MaxRows)
    }

    return [pscustomobject][ordered]@{
        Rows = @($rows)
        FilteredCount = $filteredCount
        ShardedCount = $shardedCount
        RowOffset = $RowOffset
        AfterOffsetCount = $afterOffsetCount
        SelectedCount = $rows.Count
    }
}

function ConvertTo-HtmlText([object]$Value) {
    return [System.Net.WebUtility]::HtmlEncode([string]$Value)
}

function ConvertTo-RelativeHref([string]$BaseFile, [string]$TargetPath) {
    if ([string]::IsNullOrWhiteSpace($TargetPath) -or !(Test-Path -LiteralPath $TargetPath)) {
        return ""
    }
    $baseDirectory = Split-Path $BaseFile -Parent
    if ([string]::IsNullOrWhiteSpace($baseDirectory)) {
        $baseDirectory = (Get-Location).Path
    }
    if (Test-Path -LiteralPath $baseDirectory -PathType Container) {
        $baseDirectory = (Resolve-Path -LiteralPath $baseDirectory).Path
    }
    if (!$baseDirectory.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseDirectory += [System.IO.Path]::DirectorySeparatorChar
    }
    $targetResolved = (Resolve-Path -LiteralPath $TargetPath).Path
    $relative = ([uri]$baseDirectory).MakeRelativeUri([uri]$targetResolved).ToString()
    return [uri]::UnescapeDataString($relative)
}

function New-LinkHtml([string]$BaseFile, [string]$TargetPath, [string]$Label) {
    $labelText = ConvertTo-HtmlText $Label
    $href = ConvertTo-RelativeHref $BaseFile $TargetPath
    if ([string]::IsNullOrWhiteSpace($href)) {
        return $labelText
    }
    return "<a href=`"$(ConvertTo-HtmlText $href)`">$labelText</a>"
}

function New-ViewerCommandText([string]$RuntimeTarget, [string]$ActorKindValue, [string]$PhaseValue, [string[]]$RuntimeStates, [string]$PlacementCommandArgs) {
    $angleText = @($Angles) -join ","
    $command = "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 -ProofRoot `"$ProofRoot`" -Targets `"$RuntimeTarget`" -ActorKind $ActorKindValue -Phases `"$PhaseValue`" -Angles $angleText -RunSeconds $RunSeconds -ActorFrame $ActorFrame -ScreenshotFrames `"$ScreenshotFrames`""
    $animationGroup = Resolve-ActorKitAnimationGroup $RuntimeStates
    if (![string]::IsNullOrWhiteSpace($animationGroup)) {
        $command += " -ActorKitAnimationGroup $animationGroup"
    }
    $dialogueMode = Resolve-ActorKitDialogueMode $RuntimeStates
    if (![string]::IsNullOrWhiteSpace($dialogueMode)) {
        $command += " -ActorKitDialogueMode $dialogueMode"
    }
    if (![string]::IsNullOrWhiteSpace($PlacementCommandArgs)) {
        $command += " $PlacementCommandArgs"
    }
    return $command
}

function Invoke-BurnDownRow([object]$Row, [string]$RunDir) {
    $rowDir = Join-Path $RunDir ([string]$Row.id -replace '[^A-Za-z0-9_.-]', '_')
    New-Item -ItemType Directory -Force -Path $rowDir | Out-Null
    $rowJson = Join-Path $rowDir "row.json"
    $Row | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $rowJson -Encoding UTF8

    $runtimeTarget = [string](Get-ObjectProperty $Row "runtimeTarget")
    if ([string]::IsNullOrWhiteSpace($runtimeTarget)) {
        $runtimeTarget = [string](Get-ObjectProperty $Row "target")
    }
    $actorKindValue = [string](Get-ObjectProperty $Row "actorKind")
    $phaseValue = [string](Get-ObjectProperty $Row "phase")
    $runtimeStates = @(ConvertTo-PlainArray (Get-ObjectProperty $Row "runtimeStates") | ForEach-Object { [string]$_ })
    $gateValue = [string](Get-ObjectProperty $Row "gate")
    $placement = Get-ObjectProperty $Row "placement"
    $placementCommandArgs = [string](Get-ObjectProperty $Row "placementCommandArgs")
    $animationGroup = Resolve-ActorKitAnimationGroup $runtimeStates
    $dialogueMode = Resolve-ActorKitDialogueMode $runtimeStates
    $commandText = if (![string]::IsNullOrWhiteSpace($runtimeTarget) -and ![string]::IsNullOrWhiteSpace($phaseValue)) {
        New-ViewerCommandText $runtimeTarget $actorKindValue $phaseValue $runtimeStates $placementCommandArgs
    }
    else {
        [string](Get-ObjectProperty $Row "runGateCommand")
    }

    $result = [ordered]@{
        id = $Row.id
        entryId = $Row.entryId
        priority = $Row.priority
        actorKind = $actorKindValue
        target = $Row.target
        runtimeTarget = $runtimeTarget
        phase = $phaseValue
        gate = $gateValue
        classification = $Row.classification
        firstFailingGate = $Row.firstFailingGate
        runtimeStates = @($runtimeStates)
        actorKitAnimationGroup = $animationGroup
        actorKitDialogueMode = $dialogueMode
        placementCommandArgs = $placementCommandArgs
        status = "PENDING"
        dryRun = [bool]$DryRun
        rowJson = $rowJson
        command = $commandText
        viewerIndex = ""
        viewerManifest = ""
        viewerJson = ""
        viewerHtml = ""
        actorKit = ""
        viewerServer = ""
        rowRuntimeClassification = $Row.classification
        rowGateProofMode = "not-run"
        rowGateAudit = [pscustomobject][ordered]@{
            schema = "nikami-fnv-actor-row-gate-audit-v1"
            status = "NOT-RUN"
            classification = $Row.classification
            selectedPhase = $phaseValue
            selectedGate = $gateValue
            selectedRuntimeStates = @($runtimeStates)
            requestedAngles = @($Angles)
            requiredEvidenceKinds = @()
            observedEvidenceKinds = @()
            missingEvidenceKinds = @()
            errors = @()
            boundary = "Runtime was not launched for this selected row."
        }
        runtimeEvidenceBoundary = "Selected row gate/state is preserved in the run summary; full row-level gameplay proof requires child runtime evidence for the selected gate and runtime states."
        evidence = @()
        error = ""
    }

    if ([string]::IsNullOrWhiteSpace($runtimeTarget) -or [string]::IsNullOrWhiteSpace($phaseValue) -or [string]::IsNullOrWhiteSpace($actorKindValue)) {
        $result.status = "FAIL"
        $result.error = "Row lacks runtime target, actor kind, or phase."
        return [pscustomobject]$result
    }

    if ($DryRun) {
        $result.status = "DRY-RUN"
        $result.rowGateProofMode = "dry-run-command-only"
        $result.rowRuntimeClassification = $Row.classification
        $result.rowGateAudit = [pscustomobject][ordered]@{
            schema = "nikami-fnv-actor-row-gate-audit-v1"
            status = "DRY-RUN"
            classification = $Row.classification
            selectedPhase = $phaseValue
            selectedGate = $gateValue
            selectedRuntimeStates = @($runtimeStates)
            requestedAngles = @($Angles)
            requiredEvidenceKinds = @(Get-RowRequiredEvidenceKinds $Row)
            observedEvidenceKinds = @("selected-row", "selected-gate", "selected-runtime-states", "validated-target-phase-kind")
            missingEvidenceKinds = @(Get-RowRequiredEvidenceKinds $Row)
            errors = @()
            boundary = "Dry-run validates row selection and command shape only; no runtime evidence is claimed."
        }
        $result.evidence = @("selected-row", "selected-gate", "selected-runtime-states", "validated-target-phase-kind", "no-runtime-launch")
        return [pscustomobject]$result
    }

    $before = @()
    $viewerRoot = Join-Path $ProofRoot "fnv-character-viewer"
    if (Test-Path -LiteralPath $viewerRoot -PathType Container) {
        $before = @(Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    }

    $viewerArgs = @{
        ProofRoot = $ProofRoot
        Targets = @($runtimeTarget)
        ActorKind = $actorKindValue
        Phases = @($phaseValue)
        Angles = @($Angles)
        RunSeconds = $RunSeconds
        ActorFrame = $ActorFrame
        ScreenshotFrames = $ScreenshotFrames
    }
    if ($actorKindValue -eq "creature") { $viewerArgs.CreatureDiagnostics = $true }
    if (![string]::IsNullOrWhiteSpace($animationGroup)) { $viewerArgs.ActorKitAnimationGroup = $animationGroup }
    if (![string]::IsNullOrWhiteSpace($dialogueMode)) { $viewerArgs.ActorKitDialogueMode = $dialogueMode }
    if ($runtimeStates -contains "projectile-fire" -or $runtimeStates -contains "attack") {
        $viewerArgs.FnvUseNativeAnimationCallbacks = $true
    }
    Add-PlacementArgsIfPresent -ArgMap $viewerArgs -Placement $placement
    if ($NoSound) { $viewerArgs.NoSound = $true }
    if ($Serve) { $viewerArgs.Serve = $true }
    if ($ServePort -gt 0) { $viewerArgs.ServePort = $ServePort }

    try {
        & $ViewerRunner @viewerArgs | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Viewer runner exited with code $LASTEXITCODE."
        }
        $after = @(Get-ChildItem -LiteralPath $viewerRoot -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
        $newDir = $null
        foreach ($dir in $after) {
            if ($before -notcontains $dir.FullName) {
                $newDir = $dir.FullName
                break
            }
        }
        if ([string]::IsNullOrWhiteSpace($newDir) -and $after.Count -gt 0) {
            $newDir = $after[0].FullName
        }
        $result.status = "PASS"
        $result.viewerIndex = if (![string]::IsNullOrWhiteSpace($newDir)) { Join-Path $newDir "index.html" } else { "" }
        $manifestJson = if (![string]::IsNullOrWhiteSpace($newDir)) { Join-Path $newDir "viewer-runs.json" } else { "" }
        if (![string]::IsNullOrWhiteSpace($manifestJson) -and (Test-Path -LiteralPath $manifestJson -PathType Leaf)) {
            $result.viewerManifest = $manifestJson
            try {
                $viewerRuns = @(Get-Content -LiteralPath $manifestJson -Raw | ConvertFrom-Json)
                $firstRun = $viewerRuns | Select-Object -First 1
                if ($null -ne $firstRun -and $null -ne $firstRun.ViewerJson) {
                    $result.viewerJson = [string]$firstRun.ViewerJson
                }
                if ($null -ne $firstRun -and $null -ne $firstRun.ViewerHtml) {
                    $result.viewerHtml = [string]$firstRun.ViewerHtml
                }
                if ($null -ne $firstRun -and $null -ne $firstRun.ActorKitJson) {
                    $result.actorKit = [string]$firstRun.ActorKitJson
                }
                $failedRuns = @($viewerRuns | Where-Object { [string]$_.Status -ne "PASS" })
                if ($viewerRuns.Count -eq 0) {
                    $result.status = "FAIL"
                    $result.error = "Viewer runner wrote no child run status."
                }
                elseif ($failedRuns.Count -gt 0) {
                    $result.status = "FAIL"
                    $result.error = "Viewer child failed: $(@($failedRuns | ForEach-Object { "$($_.Target)=$($_.Status)" }) -join ', ')"
                }
                else {
                    $result.rowGateProofMode = "viewer-phase-proof-pending-gate-state-audit"
                    $result.evidence = @("viewer-runs.json", "selected-gate", "selected-runtime-states", "screenshots", "actor-kit", "viewer-phase-proof-pending-gate-state-audit")
                    if (![string]::IsNullOrWhiteSpace($result.viewerJson) -and (Test-Path -LiteralPath $result.viewerJson -PathType Leaf)) {
                        $childManifest = Get-Content -LiteralPath $result.viewerJson -Raw | ConvertFrom-Json
                        $audit = New-RowGateAudit $Row $childManifest $Angles
                        $result.rowGateAudit = $audit
                        $result.rowRuntimeClassification = $audit.classification
                        if ($audit.status -eq "PASS") {
                            $result.rowGateProofMode = "viewer-gate-state-runtime-supported"
                            $result.runtimeEvidenceBoundary = $audit.boundary
                            $result.evidence = @($result.evidence + @("viewer-gate-state-runtime-supported"))
                        }
                        elseif ($audit.status -eq "PENDING") {
                            $result.rowGateProofMode = "viewer-phase-proof-pending-gate-state-audit"
                            $result.runtimeEvidenceBoundary = $audit.boundary
                        }
                        else {
                            $result.status = "FAIL"
                            $result.error = "Viewer child row gate audit failed: $(@($audit.errors) -join '; ')"
                        }
                        if ($result.status -eq "PASS") {
                            $result.evidence = @($result.evidence + @("viewer-manifest-phase-angle-match"))
                        }
                    }
                    else {
                        $result.status = "FAIL"
                        $result.error = "Viewer runner did not expose a parseable child viewer manifest."
                    }
                }
            }
            catch {
                $result.status = "FAIL"
                $result.error = "Unable to parse viewer child status: $($_.Exception.Message)"
            }
        }
        else {
            $result.status = "FAIL"
            $result.error = "Viewer runner did not write viewer-runs.json."
        }
        $serverJson = if (![string]::IsNullOrWhiteSpace($newDir)) { Join-Path $newDir "viewer-server.json" } else { "" }
        if (![string]::IsNullOrWhiteSpace($serverJson) -and (Test-Path -LiteralPath $serverJson -PathType Leaf)) {
            $result.viewerServer = $serverJson
        }
    }
    catch {
        $result.status = "FAIL"
        $result.error = $_.Exception.Message
    }
    return [pscustomobject]$result
}

function Write-BurnDownRunMarkdown([string]$Path, [object]$Summary) {
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# FNV Actor Parity Burn-Down Run")
    $lines.Add("")
    $lines.Add("Status: **$($Summary.status)**")
    $lines.Add("Dry run: ``$($Summary.dryRun)``")
    $lines.Add("Selected rows: $($Summary.selectedRows)")
    $lines.Add("Actor kind filter: ``$($Summary.actorKind)``")
    $lines.Add("Target filter: ``$($Summary.target)``")
    $lines.Add("Priority filter: ``$($Summary.priority)``")
    $lines.Add("Phase filter: ``$($Summary.phase)``")
    $lines.Add("Gate filter: ``$($Summary.gate)``")
    $lines.Add("Classification filter: ``$($Summary.classification)``")
    $lines.Add("Shard: ``$($Summary.shardIndex) / $($Summary.shardCount)``")
    $lines.Add("Row offset: ``$($Summary.rowOffset)``")
    $lines.Add("Filtered rows: $($Summary.filteredRows)")
    $lines.Add("Rows after shard: $($Summary.shardedRows)")
    $lines.Add("Rows after offset: $($Summary.afterOffsetRows)")
    $lines.Add("Angles: ``$(@($Summary.angles) -join ',')``")
    $lines.Add("Burn-down: ``$($Summary.burnDownJson)``")
    $lines.Add("Policy: $($Summary.payloadPolicy)")
    $lines.Add("")
    $lines.Add("| Status | Row Audit | Runtime Class | Priority | Kind | Target | Phase | Gate | Missing Evidence | Viewer | Row JSON |")
    $lines.Add("|---|---|---|---|---|---|---|---|---|---|---|")
    foreach ($result in @($Summary.results)) {
        $viewer = if (![string]::IsNullOrWhiteSpace($result.viewerIndex)) { $result.viewerIndex } elseif (![string]::IsNullOrWhiteSpace($result.viewerServer)) { $result.viewerServer } else { "" }
        $auditStatus = if ($null -ne $result.rowGateAudit) { $result.rowGateAudit.status } else { "" }
        $missingEvidence = if ($null -ne $result.rowGateAudit) { @($result.rowGateAudit.missingEvidenceKinds) -join "," } else { "" }
        $lines.Add("| $($result.status) | $auditStatus | $($result.rowRuntimeClassification) | $($result.priority) | $($result.actorKind) | ``$($result.runtimeTarget)`` | $($result.phase) | $($result.gate) | ``$missingEvidence`` | ``$viewer`` | ``$($result.rowJson)`` |")
    }
    $lines.Add("")
    $Path | Split-Path -Parent | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
    $lines | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Write-BurnDownRunHtml([string]$Path, [object]$Summary) {
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($result in @($Summary.results)) {
        $viewerTarget = if (![string]::IsNullOrWhiteSpace($result.viewerIndex)) { $result.viewerIndex } elseif (![string]::IsNullOrWhiteSpace($result.viewerServer)) { $result.viewerServer } else { "" }
        $viewerCell = New-LinkHtml $Path $viewerTarget "viewer"
        $rowCell = New-LinkHtml $Path $result.rowJson "row"
        $auditStatus = if ($null -ne $result.rowGateAudit) { [string]$result.rowGateAudit.status } else { "" }
        $missingEvidence = if ($null -ne $result.rowGateAudit) { @($result.rowGateAudit.missingEvidenceKinds) -join "," } else { "" }
        $rows.Add("<tr><td class=`"status $($result.status)`">$(ConvertTo-HtmlText $result.status)</td><td>$(ConvertTo-HtmlText $auditStatus)</td><td>$(ConvertTo-HtmlText $result.rowRuntimeClassification)</td><td>$(ConvertTo-HtmlText $result.priority)</td><td>$(ConvertTo-HtmlText $result.actorKind)</td><td><code>$(ConvertTo-HtmlText $result.runtimeTarget)</code></td><td>$(ConvertTo-HtmlText $result.phase)</td><td>$(ConvertTo-HtmlText $result.gate)</td><td><code>$(ConvertTo-HtmlText $missingEvidence)</code></td><td>$viewerCell</td><td>$rowCell</td><td>$(ConvertTo-HtmlText $result.error)</td></tr>")
    }
    $burnLink = New-LinkHtml $Path $Summary.burnDownJson "burn-down"
    $selectedLink = New-LinkHtml $Path $Summary.selectedRowsJson "selected rows"
    $body = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FNV Actor Parity Burn-Down Run</title>
<style>
body{margin:0;background:#111316;color:#eceff3;font:13px/1.4 Segoe UI,Arial,sans-serif}
main{padding:16px;display:grid;gap:14px}
h1{font-size:18px;margin:0}
.panel{border:1px solid #363c45;border-radius:6px;background:#1a1d22;padding:12px}
.meta{display:flex;gap:10px;flex-wrap:wrap}
.pill{border:1px solid #363c45;border-radius:999px;padding:4px 8px;background:#20242a}
table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid #363c45;padding:7px;text-align:left;vertical-align:top}
th{color:#aeb6c2}
code{color:#d8e6ff;overflow-wrap:anywhere}
a{color:#9fc2ff}
.status{font-weight:600}
.PASS,.DRY-RUN{color:#64d488}
.FAIL{color:#ff6f61}
</style>
</head>
<body>
<main>
<h1>FNV Actor Parity Burn-Down Run</h1>
<section class="panel meta">
  <span class="pill">Status: $(ConvertTo-HtmlText $Summary.status)</span>
  <span class="pill">Dry run: $(ConvertTo-HtmlText $Summary.dryRun)</span>
  <span class="pill">Selected: $(ConvertTo-HtmlText $Summary.selectedRows)</span>
  <span class="pill">Actor kind: $(ConvertTo-HtmlText $Summary.actorKind)</span>
  <span class="pill">Target: $(ConvertTo-HtmlText $Summary.target)</span>
  <span class="pill">Phase: $(ConvertTo-HtmlText $Summary.phase)</span>
  <span class="pill">Gate: $(ConvertTo-HtmlText $Summary.gate)</span>
  <span class="pill">Classification: $(ConvertTo-HtmlText $Summary.classification)</span>
  <span class="pill">Shard: $(ConvertTo-HtmlText "$($Summary.shardIndex)/$($Summary.shardCount)")</span>
  <span class="pill">Offset: $(ConvertTo-HtmlText $Summary.rowOffset)</span>
  <span class="pill">Filtered: $(ConvertTo-HtmlText $Summary.filteredRows)</span>
  <span class="pill">After shard: $(ConvertTo-HtmlText $Summary.shardedRows)</span>
  <span class="pill">After offset: $(ConvertTo-HtmlText $Summary.afterOffsetRows)</span>
</section>
<section class="panel">
  <div>Burn-down: $burnLink</div>
  <div>Selected rows: $selectedLink</div>
  <div>Policy: $(ConvertTo-HtmlText $Summary.payloadPolicy)</div>
</section>
<section class="panel">
<table>
<thead><tr><th>Status</th><th>Row Audit</th><th>Runtime Class</th><th>Priority</th><th>Kind</th><th>Target</th><th>Phase</th><th>Gate</th><th>Missing Evidence</th><th>Viewer</th><th>Row</th><th>Error</th></tr></thead>
<tbody>
$($rows -join "`n")
</tbody>
</table>
</section>
</main>
</body>
</html>
"@
    $Path | Split-Path -Parent | ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }
    $body | Set-Content -LiteralPath $Path -Encoding UTF8
}

$Angles = Resolve-ValidatedAngles $Angles

Write-Host "FNV actor parity burn-down"
Write-Host "RepoRoot: $RepoRoot"
Write-Host "ProofRoot: $ProofRoot"
Write-Host "PlanJson: $PlanJson"
Write-Host "OutDir: $OutDir"
Write-Host "Limit: $Limit"
Write-Host "BurnDownJson: $BurnDownJson"
Write-Host "RunRows: $RunRows"
Write-Host "DryRun: $DryRun"
Write-Host "ActorKind: $ActorKind"
Write-Host "Target: $Target"
Write-Host "Priority: $Priority"
Write-Host "Phase: $Phase"
Write-Host "Gate: $Gate"
Write-Host "Classification: $Classification"
Write-Host "MaxRows: $MaxRows"
Write-Host "RowOffset: $RowOffset"
Write-Host "ShardIndex: $ShardIndex"
Write-Host "ShardCount: $ShardCount"
Write-Host "Angles: $($Angles -join ',')"
Write-Host "ViewerRunner: $ViewerRunner"
Write-Host "RegeneratePlan: $RegeneratePlan"
Write-Host "Policy: generated command/classification/proof metadata only; no retail assets are committed"
Write-Host ""

$useExistingBurnDown = !$RegeneratePlan -and ![string]::IsNullOrWhiteSpace($BurnDownJson)
if ($useExistingBurnDown) {
    if (!(Test-Path -LiteralPath $BurnDownJson -PathType Leaf)) {
        throw "Explicit BurnDownJson does not exist: $BurnDownJson"
    }
    Write-Host "Using existing burn-down JSON; skipping matrix regeneration."
}
else {
    $python = Find-Python
    $BurnDownJson = ""

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
    if ($ActorKind -ne "all") { $argsList += @("--actor-kind", $ActorKind) }
    if (![string]::IsNullOrWhiteSpace($Target)) { $argsList += @("--target", $Target) }
    if (![string]::IsNullOrWhiteSpace($Priority)) { $argsList += @("--priority", $Priority) }
    if ($RequirePass) { $argsList += "--require-pass" }

    & $python.Command @argsList
    if ($LASTEXITCODE -ne 0) {
        throw "FNV actor parity burn-down failed with exit code $LASTEXITCODE."
    }

    if ([string]::IsNullOrWhiteSpace($BurnDownJson)) {
        if (![string]::IsNullOrWhiteSpace($OutDir)) {
            $candidateBurnDown = Join-Path $OutDir "actor-parity-burndown.json"
            if (Test-Path -LiteralPath $candidateBurnDown -PathType Leaf) {
                $BurnDownJson = $candidateBurnDown
            }
        }
        if ([string]::IsNullOrWhiteSpace($BurnDownJson)) {
            $BurnDownJson = Get-LatestBurnDownJson
        }
    }
}

if (!$RunRows -and !$DryRun) {
    return
}

if ([string]::IsNullOrWhiteSpace($BurnDownJson) -or !(Test-Path -LiteralPath $BurnDownJson -PathType Leaf)) {
    throw "Unable to resolve actor parity burn-down JSON for row execution."
}

$burnDown = Get-Content -LiteralPath $BurnDownJson -Raw | ConvertFrom-Json
Assert-BurnDownMatrix $burnDown $BurnDownJson
$selection = Select-BurnDownRows $burnDown
$selectedRows = @($selection.Rows)
$runDir = New-RunDirectory
$selectedRowsPath = Join-Path $runDir "selected-rows.json"
ConvertTo-Json -InputObject @($selectedRows) -Depth 14 | Set-Content -LiteralPath $selectedRowsPath -Encoding UTF8

$runResults = [System.Collections.Generic.List[object]]::new()
foreach ($row in $selectedRows) {
    $runResults.Add((Invoke-BurnDownRow $row $runDir))
}

$failed = @($runResults | Where-Object { $_.status -eq "FAIL" })
$auditSupported = @($runResults | Where-Object { $null -ne $_.rowGateAudit -and [string]$_.rowGateAudit.status -eq "PASS" })
$auditPending = @($runResults | Where-Object { $null -ne $_.rowGateAudit -and ([string]$_.rowGateAudit.status -eq "PENDING" -or [string]$_.rowGateAudit.status -eq "DRY-RUN") })
$auditFailed = @($runResults | Where-Object { $null -ne $_.rowGateAudit -and [string]$_.rowGateAudit.status -eq "FAIL" })
$selectionStatus = if ($selectedRows.Count -eq 0) { "FAIL" } elseif ($failed.Count -eq 0) { "PASS" } else { "FAIL" }
$summary = [pscustomobject][ordered]@{
    schema = "nikami-fnv-actor-parity-burndown-run-v1"
    status = $selectionStatus
    dryRun = [bool]$DryRun
    burnDownJson = $BurnDownJson
    selectedRowsJson = $selectedRowsPath
    selectedRows = $selectedRows.Count
    filteredRows = $selection.FilteredCount
    shardedRows = $selection.ShardedCount
    rowOffset = $selection.RowOffset
    afterOffsetRows = $selection.AfterOffsetCount
    shardIndex = $ShardIndex
    shardCount = $ShardCount
    actorKind = $ActorKind
    target = $Target
    priority = $Priority
    phase = $Phase
    gate = $Gate
    classification = $Classification
    maxRows = $MaxRows
    angles = @($Angles)
    runSeconds = $RunSeconds
    actorFrame = $ActorFrame
    screenshotFrames = $ScreenshotFrames
    rowGateAuditCounts = [pscustomobject][ordered]@{
        runtimeSupported = $auditSupported.Count
        loadedPendingRuntime = $auditPending.Count
        failed = $auditFailed.Count
    }
    payloadPolicy = "generated row/run metadata and proof links only; no retail asset payload bytes"
    selectionError = if ($selectedRows.Count -eq 0) { "No burn-down rows matched the requested filters." } else { "" }
    results = @($runResults.ToArray())
    artifacts = [pscustomobject][ordered]@{
        json = (Join-Path $runDir "actor-parity-burndown-run.json")
        html = (Join-Path $runDir "actor-parity-burndown-run.html")
        markdown = (Join-Path $runDir "actor-parity-burndown-run.md")
        selectedRows = $selectedRowsPath
    }
}

$summaryPath = Join-Path $runDir "actor-parity-burndown-run.json"
$summary | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-BurnDownRunHtml (Join-Path $runDir "actor-parity-burndown-run.html") $summary
Write-BurnDownRunMarkdown (Join-Path $runDir "actor-parity-burndown-run.md") $summary

Write-Host ""
Write-Host "Burn-down row run status: $($summary.status)"
Write-Host "Burn-down row run JSON: $summaryPath"
Write-Host "Burn-down row run HTML: $(Join-Path $runDir "actor-parity-burndown-run.html")"
Write-Host "Burn-down row run Markdown: $(Join-Path $runDir "actor-parity-burndown-run.md")"
Write-Host "Selected rows JSON: $selectedRowsPath"

if ($summary.status -ne "PASS") {
    throw "FNV actor parity burn-down row run failed. See $summaryPath"
}
