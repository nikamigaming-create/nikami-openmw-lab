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
        "TribalPack.esm",
        "FNVR.esp"
    )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$Parser = Join-Path $PSScriptRoot "fnv_actor_presentation_ledger.py"

if ([string]::IsNullOrWhiteSpace($FnvRoot) -and [string]::IsNullOrWhiteSpace($FnvData)) {
    throw "Set -FnvRoot, -FnvData, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof."
}
if ([string]::IsNullOrWhiteSpace($FnvData)) {
    $FnvData = Join-Path $FnvRoot "Data"
}
if ([string]::IsNullOrWhiteSpace($FnvRoot)) {
    $FnvRoot = Split-Path $FnvData -Parent
}
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}
if ([string]::IsNullOrWhiteSpace($HarvestDir)) {
    $harvestRoot = Join-Path $ProofRoot "fnv-retail-harvest"
    if (Test-Path -LiteralPath $harvestRoot -PathType Container) {
        $latest = Get-ChildItem -LiteralPath $harvestRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($null -ne $latest) {
            $HarvestDir = $latest.FullName
        }
    }
}

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
if ([string]::IsNullOrWhiteSpace($Python) -and ![string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
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
    throw "Python 3 is required to run the FNV actor presentation ledger proof."
}
Write-Host "Using Python: $Python"

$BeforeDirs = @()
$ledgerRoot = Join-Path $ProofRoot "fnv-actor-presentation-ledger"
if (Test-Path -LiteralPath $ledgerRoot -PathType Container) {
    $BeforeDirs = @(Get-ChildItem -LiteralPath $ledgerRoot -Directory | Select-Object -ExpandProperty FullName)
}

$ArgsList = @()
$ArgsList += $PythonArgs
$ArgsList += $Parser
$ArgsList += @("--fnv-data", $FnvData)
$ArgsList += @("--fnv-root", $FnvRoot)
$ArgsList += @("--repo-root", $RepoRoot)
$ArgsList += @("--proof-root", $ProofRoot)
if (![string]::IsNullOrWhiteSpace($HarvestDir)) {
    $ArgsList += @("--harvest-dir", $HarvestDir)
}
$ArgsList += "--content"
$ArgsList += $Content

& $Python @ArgsList
if ($LASTEXITCODE -ne 0) {
    throw "FNV actor presentation ledger proof failed with exit code $LASTEXITCODE."
}

$AfterDirs = @(Get-ChildItem -LiteralPath $ledgerRoot -Directory | Sort-Object Name -Descending)
$ProofDir = $null
foreach ($dir in $AfterDirs) {
    if ($BeforeDirs -notcontains $dir.FullName) {
        $ProofDir = $dir.FullName
        break
    }
}
if ($null -eq $ProofDir) {
    $ProofDir = ($AfterDirs | Select-Object -First 1).FullName
}
if ([string]::IsNullOrWhiteSpace($ProofDir)) {
    throw "FNV actor presentation ledger did not create a proof directory."
}

$ResultPath = Join-Path $ProofDir "result.json"
$LedgerPath = Join-Path $ProofDir "actor-presentation-ledger.json"
if (!(Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
    throw "Missing actor presentation result: $ResultPath"
}
if (!(Test-Path -LiteralPath $LedgerPath -PathType Leaf)) {
    throw "Missing actor presentation ledger: $LedgerPath"
}

$Result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
$LedgerJson = Get-Content -LiteralPath $LedgerPath -Raw | ConvertFrom-Json
$Ledger = [System.Collections.Generic.List[object]]::new()
foreach ($row in $LedgerJson) {
    $Ledger.Add($row)
}

$allowed = @(
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof"
)
$invalidRows = @($Ledger | Where-Object { $allowed -notcontains $_.classification })
if ($invalidRows.Count -gt 0) {
    throw "Actor presentation ledger has invalid/unclassified rows: $($invalidRows.Count). See $LedgerPath"
}
if ([int]$Result.unclassifiedCount -ne 0) {
    throw "Actor presentation result reports unclassified rows: $($Result.unclassifiedCount). See $ResultPath"
}
if ([int]$Result.npcBaseRecords -le 0) {
    throw "Actor presentation ledger did not account for any NPC_ base records."
}
if ([int]$Result.creatureBaseRecords -le 0) {
    throw "Actor presentation ledger did not account for any CREA base records."
}
if ([int]$Result.placedNpcCreatureRefs -le 0) {
    throw "Actor presentation ledger did not account for any ACHR/ACRE placed actor references."
}

$npcBaseRows = @($Ledger | Where-Object { $_.component -eq "actor-base-record" -and $_.actorKind -eq "NPC_" })
$creatureBaseRows = @($Ledger | Where-Object { $_.component -eq "actor-base-record" -and $_.actorKind -eq "CREA" })
if ($npcBaseRows.Count -ne [int]$Result.npcBaseRecords) {
    throw "Actor presentation ledger NPC_ base rows ($($npcBaseRows.Count)) do not match summary ($($Result.npcBaseRecords)). See $LedgerPath"
}
if ($creatureBaseRows.Count -ne [int]$Result.creatureBaseRecords) {
    throw "Actor presentation ledger CREA base rows ($($creatureBaseRows.Count)) do not match summary ($($Result.creatureBaseRecords)). See $LedgerPath"
}

$npcOnlyComponents = @(
    "npc-race",
    "hair",
    "eyes",
    "npc-headpart",
    "facegen-symmetric-shape",
    "facegen-asymmetric-shape",
    "facegen-symmetric-texture",
    "npc-model"
)
$creatureOnlyComponents = @(
    "creature-model",
    "creature-body-nif",
    "bodypart-data"
)
$creatureRowsWithNpcComponents = @($Ledger | Where-Object { $_.actorKind -eq "CREA" -and $npcOnlyComponents -contains $_.component })
if ($creatureRowsWithNpcComponents.Count -gt 0) {
    throw "Actor presentation ledger put NPC-only components on CREA rows: $($creatureRowsWithNpcComponents.Count). See $LedgerPath"
}
$npcRowsWithCreatureComponents = @($Ledger | Where-Object { $_.actorKind -eq "NPC_" -and $creatureOnlyComponents -contains $_.component })
if ($npcRowsWithCreatureComponents.Count -gt 0) {
    throw "Actor presentation ledger put creature-only components on NPC_ rows: $($npcRowsWithCreatureComponents.Count). See $LedgerPath"
}

$placedRows = @($Ledger | Where-Object { $_.component -eq "placed-reference" })
$badPlacedRows = @($placedRows | Where-Object {
        @("NPC_", "CREA") -notcontains $_.resolvedRecordType -or $_.actorKind -ne $_.resolvedRecordType
    })
if ($badPlacedRows.Count -gt 0) {
    throw "Actor presentation ledger placed-reference rows have mismatched actorKind/resolvedRecordType: $($badPlacedRows.Count). See $LedgerPath"
}
$placedRowsMissingRuntimeContext = @($placedRows | Where-Object {
        [string]::IsNullOrWhiteSpace([string]$_.placedCellFormId) -or
        [string]::IsNullOrWhiteSpace([string]$_.placedRuntimeCellFormId) -or
        $null -eq $_.placedPosX -or $null -eq $_.placedPosY -or $null -eq $_.placedPosZ -or
        $null -eq $_.placedRotX -or $null -eq $_.placedRotY -or $null -eq $_.placedRotZ
    })
if ($placedRowsMissingRuntimeContext.Count -gt 0) {
    throw "Actor presentation ledger placed-reference rows are missing decoded parent/runtime cell or DATA position: $($placedRowsMissingRuntimeContext.Count). See $LedgerPath"
}
if ([int]$Result.placedActorRefsWithDataPosition -ne $placedRows.Count -or [int]$Result.placedActorRefsWithParentCell -ne $placedRows.Count -or [int]$Result.placedActorRefsWithRuntimeParentCell -ne $placedRows.Count) {
    throw "Actor presentation ledger summary placement context counts do not match placed-reference rows. See $ResultPath"
}
$placedRowsWithChildSubgroupCell = @($placedRows | Where-Object { @("8", "9", "10") -contains [string]$_.placedCellGroupType })
if ($placedRowsWithChildSubgroupCell.Count -gt 0) {
    throw "Actor presentation ledger used persistent/temp/visible child subgroup as placed actor bootstrap cell instead of parent cell-child GRUP: $($placedRowsWithChildSubgroupCell.Count). See $LedgerPath"
}
$easyPeteRows = @($placedRows | Where-Object { [string]$_.placedRefEditorId -eq "EasyPeteRef" })
if ($easyPeteRows.Count -ne 1) {
    throw "Actor presentation ledger did not produce exactly one EasyPeteRef placed-reference row. Count=$($easyPeteRows.Count). See $LedgerPath"
}
$easyPete = $easyPeteRows[0]
if ([string]$easyPete.placedCellFormId -ne "0x000daeb9" -or [string]$easyPete.placedRuntimeCellFormId -ne "0x010daeb9" -or [string]$easyPete.placedCellSource -ne "worldspace-xclc-from-position") {
    throw "EasyPeteRef bootstrap cell was not resolved from WRLD/XCLC grid to canonical 0x000daeb9 / runtime 0x010daeb9. Actual canonical=$($easyPete.placedCellFormId) runtime=$($easyPete.placedRuntimeCellFormId) source=$($easyPete.placedCellSource). See $LedgerPath"
}
if ([int]$easyPete.placedCellGridX -ne -17 -or [int]$easyPete.placedCellGridY -ne 0 -or [string]$easyPete.placedCellFallbackFormId -ne "0x000846ea") {
    throw "EasyPeteRef grid/fallback proof is wrong. Expected grid=(-17,0) fallback=0x000846ea; actual grid=($($easyPete.placedCellGridX),$($easyPete.placedCellGridY)) fallback=$($easyPete.placedCellFallbackFormId). See $LedgerPath"
}
$easyPeteActorRows = @($Ledger | Where-Object { [string]$_.actorFormId -eq [string]$easyPete.actorFormId })
if (@($easyPeteActorRows | Where-Object { [string]$_.actorEditorId -eq "GSEasyPete" }).Count -le 0) {
    throw "Actor presentation ledger did not decompress Easy Pete's base NPC record into GSEasyPete actor rows. See $LedgerPath"
}
if ([int]$Result.compressionCounts.'compressed-zlib' -le 0) {
    throw "Actor presentation ledger did not report any compressed-zlib records. See $ResultPath"
}
$easyPeteRequiredComponents = @(
    "hair",
    "npc-headpart",
    "facegen-symmetric-texture",
    "equipment-armor",
    "equipment-armor-model",
    "equipment-weapon"
)
$easyPeteComponentSet = @{}
foreach ($row in $easyPeteActorRows) {
    $easyPeteComponentSet[[string]$row.component] = $true
}
$missingEasyPeteComponents = @($easyPeteRequiredComponents | Where-Object { !$easyPeteComponentSet.ContainsKey($_) })
if ($missingEasyPeteComponents.Count -gt 0) {
    throw "Easy Pete actor rows are missing decompressed face/hair/equipment components: $($missingEasyPeteComponents -join ', '). See $LedgerPath"
}
$expectedEasyPeteRows = @{
    "hair:HairAfricanAmericanBaseOld" = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "hair" -and [string]$_.resolvedEditorId -eq "HairAfricanAmericanBaseOld" })
    "npc-headpart:BeardFullOld" = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "npc-headpart" -and [string]$_.resolvedEditorId -eq "BeardFullOld" })
    "equipment-armor:CowboyHat02" = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-armor" -and [string]$_.resolvedEditorId -eq "CowboyHat02" })
    "equipment-armor:OutfitRepublican02" = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-armor" -and [string]$_.resolvedEditorId -eq "OutfitRepublican02" })
    "equipment-weapon:WeapNVDynamite" = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-weapon" -and [string]$_.resolvedEditorId -eq "WeapNVDynamite" })
}
foreach ($expectation in $expectedEasyPeteRows.GetEnumerator()) {
    if ($expectation.Value.Count -le 0) {
        throw "Easy Pete actor rows are missing required static evidence $($expectation.Key). See $LedgerPath"
    }
}
$easyPeteHatRows = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-armor" -and [string]$_.resolvedEditorId -eq "CowboyHat02" })
if ($easyPeteHatRows.Count -le 0) {
    throw "Easy Pete actor rows did not resolve CowboyHat02 from actor inventory. See $LedgerPath"
}
$easyPeteHatModelRows = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-armor-model" -and [string]$_.sourceEditorId -eq "CowboyHat02" -and [string]$_.assetPath -match "cowboyhat" })
if ($easyPeteHatModelRows.Count -le 0) {
    throw "Easy Pete actor rows did not inherit CowboyHat02 model paths for headgear proof. See $LedgerPath"
}
$easyPeteOutfitModelRows = @($easyPeteActorRows | Where-Object { [string]$_.component -eq "equipment-armor-model" -and [string]$_.sourceEditorId -eq "OutfitRepublican02" -and [string]$_.assetPath -match "republican" })
if ($easyPeteOutfitModelRows.Count -le 0) {
    throw "Easy Pete actor rows did not inherit OutfitRepublican02 worn model paths. See $LedgerPath"
}

$requiredComponents = @(
    "actor-base-record",
    "placed-reference",
    "npc-race",
    "hair",
    "eyes",
    "race-head",
    "race-body",
    "equipment-armor",
    "equipment-armor-addon",
    "armor-addon-fnv-modl",
    "creature-model",
    "bodypart-data",
    "dialogue-info",
    "voice-lip",
    "animation-locomotion-fallback"
)
$componentSet = @{}
foreach ($row in $Ledger) {
    $componentSet[[string]$row.component] = $true
}
$missingComponents = @($requiredComponents | Where-Object { !$componentSet.ContainsKey($_) })
if ($missingComponents.Count -gt 0) {
    throw "Actor presentation ledger is missing required components: $($missingComponents -join ', '). See $LedgerPath"
}
if ([int]$Result.clothingRecords -gt 0 -and !$componentSet.ContainsKey("equipment-clothing")) {
    throw "Actor presentation ledger saw CLOT records but did not emit equipment-clothing rows. See $LedgerPath"
}

$knownBlocked = @($Ledger | Where-Object { $_.classification -eq "known-blocked" })
if ($knownBlocked.Count -le 0) {
    throw "Actor presentation ledger should explicitly report known-blocked rows for current FNV parity gaps."
}
$armaPending = @($Ledger | Where-Object {
        $_.component -eq "armor-addon-fnv-modl" -and
        $_.classification -eq "loaded-pending-runtime" -and
        $_.firstFailingGate -eq "armor-addon-runtime-sweep"
    })
if ($armaPending.Count -le 0) {
    throw "Actor presentation ledger did not account FO3/FNV ARMA MODL as parsed pending runtime armor-addon sweep proof."
}
$staleArmaBlocked = @($Ledger | Where-Object {
        $_.component -eq "armor-addon-fnv-modl" -and
        $_.firstFailingGate -eq "loadarma-fnv-modl-skipped"
    })
if ($staleArmaBlocked.Count -gt 0) {
    throw "Actor presentation ledger still reports stale loadarma-fnv-modl-skipped rows after ARMA MODL parsing was enabled."
}

Write-Host "FNV actor presentation ledger gate PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "Rows: $($Result.rowCount)"
Write-Host "NPC: $($Result.npcBaseRecords) CREA: $($Result.creatureBaseRecords) Placed: $($Result.placedNpcCreatureRefs)"
Write-Host "Known blocked: $($Result.knownBlockedCount) Unclassified: $($Result.unclassifiedCount)"
