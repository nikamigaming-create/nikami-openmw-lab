param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Builder = Join-Path $PSScriptRoot "fnv_character_studio_catalog.py"
$Runner = Join-Path $PSScriptRoot "run-fnv-character-studio-catalog.ps1"
if (!(Test-Path -LiteralPath $Builder -PathType Leaf)) {
    throw "Missing FNV character studio catalog builder: $Builder"
}
if (!(Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "Missing FNV character studio catalog runner: $Runner"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-character-studio-catalog-contract/$Stamp"
$FixtureDir = Join-Path $ProofDir "fixture"
$OutDir = Join-Path $ProofDir "out"
New-Item -ItemType Directory -Force -Path $FixtureDir | Out-Null

$planPath = Join-Path $FixtureDir "viewer-batch-plan.json"
$contentDir = Join-Path $FixtureDir "content"
New-Item -ItemType Directory -Force -Path $contentDir | Out-Null

$plan = [pscustomobject][ordered]@{
    schema = "nikami-fnv-character-viewer-batch-plan-v1"
    status = "PASS"
    artifacts = [pscustomobject][ordered]@{
        plan = $planPath
    }
    entries = @(
        [pscustomobject][ordered]@{
            id = "actor-base-record:000001"
            source = "actor-base-record"
            plugin = "Contract.esm"
            actorKind = "npc"
            recordType = "NPC_"
            target = "ContractNpc"
            runtimeTarget = "ContractNpc"
            baseActorTarget = "ContractNpc"
            assemblyTarget = "ContractNpc"
            phases = @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk")
            classification = "loaded-pending-runtime"
            firstFailingGate = "base-actor-spawn-or-placement-runtime"
            actorFormId = "0x00000001"
            actorEditorId = "ContractNpc"
            placement = [pscustomobject][ordered]@{
                runtimeBootstrapReady = $false
            }
            componentCounts = [pscustomobject][ordered]@{
                "actor-base-record" = 1
                "npc-race" = 1
                "hair" = 1
            }
            componentPhases = [pscustomobject][ordered]@{
                "actor-base-record" = @("body")
                "npc-race" = @("body", "head", "face")
                "hair" = @("hair")
            }
            componentEvidence = @(
                [pscustomobject][ordered]@{
                    component = "hair"
                    phases = @("hair")
                    sourceRecordType = "HAIR"
                    sourceFormId = "0x00000003"
                    assetPath = "meshes\characters\hair\contract.nif"
                    assetStatus = "archive-entry-resolved"
                    assetArchive = "Fallout - Meshes.bsa"
                    proofAnchor = "npc-face-assembly"
                }
            )
        },
        [pscustomobject][ordered]@{
            id = "placed-reference:000003"
            source = "placed-reference"
            plugin = "Contract.esm"
            actorKind = "npc"
            recordType = "NPC_"
            target = "EasyPeteRef"
            runtimeTarget = "GSEasyPete"
            placedTarget = "EasyPeteRef"
            baseActorTarget = "GSEasyPete"
            assemblyTarget = "GSEasyPete"
            phases = @("body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk")
            classification = "loaded-pending-runtime"
            firstFailingGate = "placed-actor-runtime-viewer-proof"
            actorFormId = "0x00000013"
            actorEditorId = "GSEasyPete"
            placedRefFormId = "0x00000023"
            placedRefEditorId = "EasyPeteRef"
            placement = [pscustomobject][ordered]@{
                runtimeBootstrapReady = $true
                cell = "FormId:0x00000033"
                position = [pscustomobject][ordered]@{ x = 4; y = 5; z = 6 }
            }
            componentCounts = [pscustomobject][ordered]@{
                "actor-base-record" = 1
                "npc-race" = 1
                "hair" = 1
                "equipment-armor" = 1
            }
            componentPhases = [pscustomobject][ordered]@{
                "actor-base-record" = @("body")
                "npc-race" = @("body", "head", "face")
                "hair" = @("hair")
                "equipment-armor" = @("equipment", "headgear")
            }
            componentEvidence = @(
                [pscustomobject][ordered]@{
                    component = "equipment-armor"
                    phases = @("equipment", "headgear")
                    sourceRecordType = "ARMO"
                    sourceFormId = "0x00000024"
                    assetPath = "meshes\characters\headgear\contract_hat.nif"
                    assetStatus = "archive-entry-resolved"
                    assetArchive = "Fallout - Meshes.bsa"
                    proofAnchor = "npc-headgear-assembly"
                }
            )
        },
        [pscustomobject][ordered]@{
            id = "placed-reference:000002"
            source = "placed-reference"
            plugin = "Contract.esm"
            actorKind = "creature"
            recordType = "CREA"
            target = "ContractCreatureRef"
            runtimeTarget = "ContractCreature"
            placedTarget = "ContractCreatureRef"
            baseActorTarget = "ContractCreature"
            assemblyTarget = "ContractCreature"
            phases = @("creature-model", "creature-body", "creature-animation", "creature-full")
            classification = "loaded-pending-runtime"
            firstFailingGate = "placed-actor-runtime-viewer-proof"
            actorFormId = "0x00000002"
            actorEditorId = "ContractCreature"
            placedRefFormId = "0x00000012"
            placedRefEditorId = "ContractCreatureRef"
            placement = [pscustomobject][ordered]@{
                runtimeBootstrapReady = $true
                cell = "FormId:0x00000022"
                position = [pscustomobject][ordered]@{ x = 1; y = 2; z = 3 }
            }
            componentCounts = [pscustomobject][ordered]@{
                "actor-base-record" = 1
                "creature-model" = 1
            }
            componentPhases = [pscustomobject][ordered]@{
                "actor-base-record" = @("body")
                "creature-model" = @("creature-model")
            }
            componentEvidence = @(
                [pscustomobject][ordered]@{
                    component = "creature-model"
                    phases = @("creature-model")
                    sourceRecordType = "CREA"
                    sourceFormId = "0x00000002"
                    assetPath = "meshes\creatures\contract\contract.nif"
                    assetStatus = "archive-entry-resolved"
                    assetArchive = "Fallout - Meshes.bsa"
                    proofAnchor = "creature-body-assembly"
                }
            )
        }
    )
}
$plan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $planPath -Encoding UTF8

$gameplay = @(
    [pscustomobject][ordered]@{
        plugin = "Contract.esm"
        recordType = "WEAP"
        formId = "0x00001000"
        editorId = "ContractPistol"
        model = "Weapons\Contract\Pistol.nif"
        icon = "Interface\Icons\Contract\Pistol.dds"
        ammo = "0x00001001"
        projectile = ""
        worldModel = "0x00001002"
        equipType = "0x00000002"
        classification = "runtime-supported"
        readiness = "runtime-supported"
        runtimeProofGate = "contract-runtime"
        unprovenGameplayGates = @("contract-projectile")
    },
    [pscustomobject][ordered]@{
        plugin = "Contract.esm"
        recordType = "PROJ"
        formId = "0x00001003"
        editorId = "ContractProjectile"
        model = "Projectiles\ContractProjectile.nif"
        icon = ""
        classification = "loaded-pending-runtime"
        readiness = "loaded-pending-runtime"
    },
    [pscustomobject][ordered]@{
        plugin = "Contract.esm"
        recordType = "ARMO"
        formId = "0x00001004"
        editorId = "ContractArmor"
        model = "Armor\Contract\ContractArmor.nif"
        icon = "Interface\Icons\Contract\Armor.dds"
        classification = "loaded-pending-runtime"
        readiness = "loaded-pending-runtime"
        firstFailingGate = "item-studio-spawn-command"
    }
)
$gameplay | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $contentDir "gameplay-systems.json") -Encoding UTF8
([pscustomobject][ordered]@{ status = "PASS" }) | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $contentDir "result.json") -Encoding UTF8

& powershell -NoProfile -ExecutionPolicy Bypass -File $Runner -ProofRoot $ProofRoot -PlanJson $planPath -ContentDir $contentDir -OutDir $OutDir -RequirePass | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "FNV character studio catalog fixture failed with exit code $LASTEXITCODE."
}

$catalogPath = Join-Path $OutDir "character-studio-catalog.json"
$htmlPath = Join-Path $OutDir "character-studio.html"
if (!(Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
    throw "Missing generated studio catalog: $catalogPath"
}
if (!(Test-Path -LiteralPath $htmlPath -PathType Leaf)) {
    throw "Missing generated studio HTML: $htmlPath"
}

$catalog = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
if ($catalog.schema -ne "nikami-fnv-character-studio-catalog-v1") {
    throw "Unexpected studio catalog schema: $($catalog.schema)"
}
if ($catalog.status -ne "PASS") {
    throw "Studio catalog fixture did not pass: $($catalog.status)"
}
if (!@($catalog.schemaMarkers).Contains("searchable-studio-catalog-v1")) {
    throw "Studio catalog missing searchable schema marker."
}
if (!@($catalog.schemaMarkers).Contains("neutral-stage-gate-pending-v1")) {
    throw "Studio catalog missing neutral-stage pending gate marker."
}
if (!@($catalog.schemaMarkers).Contains("live-studio-workbench-v1")) {
    throw "Studio catalog missing live workbench marker."
}
if (!@($catalog.schemaMarkers).Contains("three-camera-session-strip-v1")) {
    throw "Studio catalog missing three-camera session marker."
}
if (!@($catalog.schemaMarkers).Contains("component-selector-job-payload-v1")) {
    throw "Studio catalog missing component selector payload marker."
}
if (!@($catalog.schemaMarkers).Contains("component-review-rows-v1")) {
    throw "Studio catalog missing component review rows marker."
}
if (!@($catalog.schemaMarkers).Contains("critical-character-queue-v1")) {
    throw "Studio catalog missing critical character queue marker."
}
if (!@($catalog.schemaMarkers).Contains("compact-html-index-v1")) {
    throw "Studio catalog missing compact HTML index marker."
}
if (!@($catalog.schemaMarkers).Contains("live-api-catalog-search-v1")) {
    throw "Studio catalog missing live API catalog search marker."
}
if (!@($catalog.schemaMarkers).Contains("placed-runtime-target-map-v1")) {
    throw "Studio catalog missing placed/runtime target map marker."
}
if (!@($catalog.schemaMarkers).Contains("placement-bootstrap-job-args-v1")) {
    throw "Studio catalog missing placement bootstrap args marker."
}
if (!@($catalog.schemaMarkers).Contains("authoring-snapshot-saveback-v1")) {
    throw "Studio catalog missing authoring snapshot saveback marker."
}
if (!@($catalog.schemaMarkers).Contains("snapshot-replay-job-v1")) {
    throw "Studio catalog missing snapshot replay job marker."
}
if ([int]$catalog.counts.total -ne 6) {
    throw "Studio catalog expected 6 entries but got $($catalog.counts.total)."
}
if ([int]$catalog.counts.domains.actor -ne 3 -or [int]$catalog.counts.domains.gameplay -ne 3) {
    throw "Studio catalog did not split actor/gameplay domains correctly."
}
if ([int]$catalog.counts.missingSearchText -ne 0 -or [int]$catalog.counts.invalidClassifications -ne 0) {
    throw "Studio catalog emitted invalid entries."
}
if ([int]$catalog.counts.criticalQueue -lt 6) {
    throw "Studio catalog did not build the full critical character queue."
}

$entries = @($catalog.entries)
$npc = $entries | Where-Object { $_.label -eq "ContractNpc" } | Select-Object -First 1
$creature = $entries | Where-Object { $_.label -eq "ContractCreatureRef" } | Select-Object -First 1
$easyPete = $entries | Where-Object { $_.label -eq "EasyPeteRef" } | Select-Object -First 1
$weapon = $entries | Where-Object { $_.label -eq "ContractPistol" } | Select-Object -First 1
$armor = $entries | Where-Object { $_.label -eq "ContractArmor" } | Select-Object -First 1
if ($null -eq $npc -or $npc.commands.runtimeThreeCamera -notmatch "-Angles 'left,right,top'") {
    throw "Studio catalog NPC entry missing three-camera runtime command."
}
if ($null -eq $creature -or $creature.commands.runtimeThreeCamera -notmatch "-CreatureDiagnostics") {
    throw "Studio catalog creature entry missing creature diagnostics command."
}
if ($null -eq $easyPete -or $easyPete.commands.runtimeThreeCamera -notmatch "-Targets 'GSEasyPete'") {
    throw "Studio catalog EasyPeteRef fixture missing base-runtime three-camera command."
}
if ($easyPete.commands.runtimeThreeCamera -notmatch "-Angles 'left,right,top'") {
    throw "Studio catalog EasyPeteRef fixture did not default to orthogonal rig diagnostic cameras."
}
$criticalQueue = @($catalog.criticalQueue)
$easyPeteQueue = $criticalQueue | Where-Object { $_.id -eq "easy-pete" } | Select-Object -First 1
$firstCreatureQueue = $criticalQueue | Where-Object { $_.id -eq "first-creature" } | Select-Object -First 1
if ($null -eq $easyPeteQueue -or $easyPeteQueue.entryId -ne $easyPete.id -or $easyPeteQueue.runtimeTarget -ne "GSEasyPete") {
    throw "Studio catalog critical queue did not match Easy Pete to the generated placed actor row."
}
if ($easyPeteQueue.defaultJobType -ne "critical-face-skin-headgear" -or !@($easyPeteQueue.reviewFocus).Contains("headgear")) {
    throw "Studio catalog critical Easy Pete queue row lost face/skin/headgear review focus."
}
if ($easyPeteQueue.defaultPartFocus -ne "") {
    throw "Studio catalog critical Easy Pete queue must default to full-character context, not naked face organs."
}
if (!@($easyPeteQueue.criticalPhases).Contains("face") -or !@($easyPeteQueue.criticalPhases).Contains("talk") -or !@($easyPeteQueue.criticalPhases).Contains("headgear")) {
    throw "Studio catalog critical Easy Pete queue row lost full critical phase coverage."
}
if ($null -eq $firstCreatureQueue -or $firstCreatureQueue.actorKind -ne "creature" -or $firstCreatureQueue.queueStatus -ne "queued") {
    throw "Studio catalog critical queue did not match a generated creature row."
}
if ($creature.runtimeTarget -ne "ContractCreature" -or $creature.placedTarget -ne "ContractCreatureRef" -or $creature.assemblyTarget -ne "ContractCreature") {
    throw "Studio catalog creature entry lost placed/runtime/base actor target roles."
}
if ($creature.commands.runtimeThreeCamera -notmatch "-Targets 'ContractCreature'") {
    throw "Studio catalog creature command did not use base runtime target."
}
if ($creature.commands.runtimeThreeCamera -notmatch "-BootstrapCell" -or $creature.commands.runtimeThreeCamera -notmatch "-ActorStageX 1") {
    throw "Studio catalog creature command lost placed-reference bootstrap/stage args."
}
if ($creature.placementCommandArgs -notmatch "-BootstrapCell" -or $creature.searchText -notmatch "contractcreatureref" -or $creature.searchText -notmatch "contractcreature") {
    throw "Studio catalog creature entry does not expose placement/target data in searchable fields."
}
if ($null -eq $creature.componentPhases -or $null -eq $creature.componentEvidence -or @($creature.componentEvidence).Count -eq 0) {
    throw "Studio catalog creature entry missing component phase/provenance evidence."
}
if ($null -eq $weapon -or $weapon.commands.runtimeThreeCamera -notmatch "run-fnv-item-viewer.ps1") {
    throw "Studio catalog weapon entry should expose generated item visual viewer command."
}
if ($weapon.studioGates[0].gate -ne "runtime-visual-model-spawn" -or $weapon.studioGates[1].gate -ne "item-studio-spawn-command") {
    throw "Studio catalog weapon entry missing visual-spawn gate plus pending item behavior gate."
}
if ($weapon.searchText -notmatch "contract pistol") {
    throw "Studio catalog search text did not split editor/model names into human-searchable tokens."
}
if ($null -eq $armor -or $armor.kind -ne "armor" -or $armor.studioGates[0].gate -ne "runtime-visual-model-spawn") {
    throw "Studio catalog armor entry did not expose armor kind with explicit runtime visual spawn gate."
}
if ($armor.commands.runtimeThreeCamera -notmatch "run-fnv-item-viewer.ps1" -or $armor.commands.runtimeThreeCamera -notmatch "ContractArmor.nif") {
    throw "Studio catalog armor entry did not expose generated item viewer command."
}
if ($armor.studioGates[1].gate -ne "item-studio-spawn-command") {
    throw "Studio catalog armor entry did not preserve pending item behavior gate."
}
$html = Get-Content -LiteralPath $htmlPath -Raw
if (!$html.Contains("FNV Character Studio Catalog") -or !$html.Contains("neutral stage pending") -or !$html.Contains("ContractPistol") -or !$html.Contains("ContractArmor")) {
    throw "Studio catalog HTML does not expose expected search/stage/item text."
}
if (!$html.Contains("Studio Session") -or !$html.Contains("Run 3 Camera") -or !$html.Contains("/nikami/studio/sessions")) {
    throw "Studio catalog HTML does not expose live workbench controls."
}
if (!$html.Contains("Auto Live") -or !$html.Contains("queueLiveRuntimeUpdate") -or !$html.Contains("live-runtime.auto-update")) {
    throw "Studio catalog HTML does not expose auto live runtime selector updates."
}
if (!$html.Contains("runtimeStatus") -or !$html.Contains("/nikami/runtime-status") -or !$html.Contains("engine running")) {
    throw "Studio catalog HTML does not expose live OpenMW runtime status."
}
if (!$html.Contains("runtimeAudit") -or !$html.Contains("/nikami/runtime-audit") -or !$html.Contains("runtime consumption audit")) {
    throw "Studio catalog HTML does not expose live OpenMW runtime consumption audit."
}
if (!$html.Contains("liveActorKitControls") -or !$html.Contains('selectors ${esc(counts.liveActorKitControls || 0)}')) {
    throw "Studio catalog HTML does not expose live actor-kit selector consumption evidence."
}
if (!$html.Contains("liveActorKitPostConstruction") -or !$html.Contains('post ${esc(counts.liveActorKitPostConstruction || 0)}')) {
    throw "Studio catalog HTML does not expose post-construction actor-kit selector evidence."
}
if (!$html.Contains("liveActorKitPartRebuilds") -or !$html.Contains('rebuilds ${esc(counts.liveActorKitPartRebuilds || 0)}')) {
    throw "Studio catalog HTML does not expose actor-kit part rebuild evidence."
}
if (!$html.Contains("cameraStrip") -or !$html.Contains("selectedParts") -or !$html.Contains("studioPayload")) {
    throw "Studio catalog HTML does not expose three-camera/component session payload controls."
}
if (!$html.Contains("PART_DEPENDENCIES") -or !$html.Contains("expandPartSelection") -or !$html.Contains('"face-organs": ["body-skin", "head-skin", "face-organs", "hair-beard"]')) {
    throw "Studio catalog HTML does not expand face/headgear selectors into required character context."
}
if (!$html.Contains("componentReviews") -or !$html.Contains("componentReviewRows") -or !$html.Contains("Save Component Review Rows") -or !$html.Contains("/reviews")) {
    throw "Studio catalog HTML does not expose per-component review row controls."
}
if (!$html.Contains("Save Snapshot") -or !$html.Contains("Replay Snapshot") -or !$html.Contains("snapshotPayload") -or !$html.Contains("saveSnapshotEvent") -or !$html.Contains("replaySnapshot")) {
    throw "Studio catalog HTML does not expose snapshot saveback/replay controls."
}
if (!$html.Contains("Apply Snapshot Live") -or !$html.Contains("applySnapshotLive") -or !$html.Contains("apply-live") -or !$html.Contains("snapshot.apply-live")) {
    throw "Studio catalog HTML does not expose no-restart snapshot live apply controls."
}
if (!$html.Contains("/snapshots") -or !$html.Contains("coordinateRows") -or !$html.Contains("authoring-snapshot-saveback-v1")) {
    throw "Studio catalog HTML does not expose generated snapshot metadata round trip."
}
if (!$html.Contains("snapshot-live-apply-v1") -or !$html.Contains("row.liveApplyArtifact?.path")) {
    throw "Studio catalog HTML does not expose generated snapshot live apply metadata round trip."
}
if (!$html.Contains("Critical Queue") -or !$html.Contains("criticalQueue") -or !$html.Contains("data-critical-run") -or !$html.Contains("critical-face-skin-headgear")) {
    throw "Studio catalog HTML does not expose critical character queue controls."
}
if (!$html.Contains("compact-index-full-row-on-select") -or !$html.Contains("/nikami/catalog/search") -or !$html.Contains("/nikami/catalog/entries/")) {
    throw "Studio catalog HTML does not use compact live catalog search/detail loading."
}
if ($html.Contains("componentEvidence")) {
    throw "Studio catalog HTML embedded full component evidence instead of compact search rows."
}
if ($html.Contains('"searchText"')) {
    throw "Studio catalog HTML embedded giant searchText payloads instead of compact rows."
}

Write-Host "FNV character studio catalog contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "Catalog: $catalogPath"
Write-Host "Html: $htmlPath"
