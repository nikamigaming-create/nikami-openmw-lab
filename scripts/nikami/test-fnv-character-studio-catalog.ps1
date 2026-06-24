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
        },
        [pscustomobject][ordered]@{
            id = "placed-reference:000002"
            source = "placed-reference"
            plugin = "Contract.esm"
            actorKind = "creature"
            recordType = "CREA"
            target = "ContractCreatureRef"
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
if ([int]$catalog.counts.total -ne 4) {
    throw "Studio catalog expected 4 entries but got $($catalog.counts.total)."
}
if ([int]$catalog.counts.domains.actor -ne 2 -or [int]$catalog.counts.domains.gameplay -ne 2) {
    throw "Studio catalog did not split actor/gameplay domains correctly."
}
if ([int]$catalog.counts.missingSearchText -ne 0 -or [int]$catalog.counts.invalidClassifications -ne 0) {
    throw "Studio catalog emitted invalid entries."
}

$entries = @($catalog.entries)
$npc = $entries | Where-Object { $_.label -eq "ContractNpc" } | Select-Object -First 1
$creature = $entries | Where-Object { $_.label -eq "ContractCreatureRef" } | Select-Object -First 1
$weapon = $entries | Where-Object { $_.label -eq "ContractPistol" } | Select-Object -First 1
if ($null -eq $npc -or $npc.commands.runtimeThreeCamera -notmatch "-Angles 'front,front-left,front-right'") {
    throw "Studio catalog NPC entry missing three-camera runtime command."
}
if ($null -eq $creature -or $creature.commands.runtimeThreeCamera -notmatch "-CreatureDiagnostics") {
    throw "Studio catalog creature entry missing creature diagnostics command."
}
if ($null -eq $weapon -or $weapon.commands.runtimeThreeCamera -ne "") {
    throw "Studio catalog weapon entry should be cataloged pending generic item runtime command."
}
if ($weapon.studioGates[0].gate -ne "item-studio-spawn-command") {
    throw "Studio catalog weapon entry missing item-studio pending gate."
}
if ($weapon.searchText -notmatch "contract pistol") {
    throw "Studio catalog search text did not split editor/model names into human-searchable tokens."
}
$html = Get-Content -LiteralPath $htmlPath -Raw
if (!$html.Contains("FNV Character Studio Catalog") -or !$html.Contains("neutral stage pending") -or !$html.Contains("ContractPistol")) {
    throw "Studio catalog HTML does not expose expected search/stage/item text."
}

Write-Host "FNV character studio catalog contract PASS"
Write-Host "ProofDir: $ProofDir"
Write-Host "Catalog: $catalogPath"
Write-Host "Html: $htmlPath"
