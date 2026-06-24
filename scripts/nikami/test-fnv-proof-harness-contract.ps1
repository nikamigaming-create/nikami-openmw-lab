param(
    [string]$ProofRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if ([string]::IsNullOrWhiteSpace($ProofRoot)) {
    $ProofRoot = Join-Path (Split-Path $RepoRoot -Parent) "proof"
}

$Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ProofDir = Join-Path $ProofRoot "fnv-proof-harness-contract/$Stamp"
$SummaryFile = Join-Path $ProofDir "summary.txt"
New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

function Write-ProofLine([string]$Text = "") {
    Write-Host $Text
    Add-Content -LiteralPath $SummaryFile -Value $Text
}

function Assert-File([string]$RelativePath) {
    $path = Join-Path $RepoRoot $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing proof harness file: $RelativePath"
    }
    Write-ProofLine "OK file: $RelativePath"
    return $path
}

function Assert-Text([string]$Path, [string]$Needle, [string]$Description) {
    $text = Get-Content -LiteralPath $Path -Raw
    if (!$text.Contains($Needle)) {
        throw "Missing ${Description}: $Needle in $Path"
    }
    Write-ProofLine "OK contract: $Description"
}

function Assert-NoText([string]$Path, [string]$Needle, [string]$Description) {
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text.Contains($Needle)) {
        throw "Unexpected ${Description}: $Needle in $Path"
    }
    Write-ProofLine "OK absent contract: $Description"
}

Write-ProofLine "FNV proof harness contract $Stamp"
Write-ProofLine "RepoRoot: $RepoRoot"
Write-ProofLine "ProofDir: $ProofDir"
Write-ProofLine ""

$flat = Assert-File "scripts/nikami/run-fnv-flat-proof.ps1"
$engine = Assert-File "apps/openmw/engine.cpp"
$npcAnimation = Assert-File "apps/openmw/mwrender/esm4npcanimation.cpp"
$creatureAnimation = Assert-File "apps/openmw/mwrender/creatureanimation.cpp"
$esm4Creature = Assert-File "apps/openmw/mwclass/esm4creature.cpp"
$classes = Assert-File "apps/openmw/mwclass/classes.cpp"
$objects = Assert-File "apps/openmw/mwrender/objects.cpp"
$doc = Assert-File "scripts/nikami/run-fnv-opening-doc-proof.ps1"
$walk = Assert-File "scripts/nikami/run-fnv-goodsprings-walk-replay-proof.ps1"
$ui = Assert-File "scripts/nikami/run-fnv-ui-baseline-proof.ps1"
$collision = Assert-File "scripts/nikami/run-fnv-goodsprings-collision-path-proof.ps1"
Assert-File "scripts/nikami/run-fnv-opening-vertical-slice.ps1" | Out-Null
$mugshot = Assert-File "scripts/nikami/run-fnv-actor-mugshot-sweep.ps1"
$easyPete = Assert-File "scripts/nikami/run-fnv-easy-pete-angle-sweep.ps1"
$characterBuilder = Assert-File "scripts/nikami/run-fnv-character-builder-tester.ps1"
$characterBuilderReport = Assert-File "scripts/nikami/fnv_character_builder_report.py"
$characterViewer = Assert-File "scripts/nikami/run-fnv-character-viewer.ps1"
$characterViewerBundle = Assert-File "scripts/nikami/fnv_character_viewer_bundle.py"
$characterViewerLiveServer = Assert-File "scripts/nikami/fnv_character_viewer_live_server.py"
$characterStudioLiveServerTest = Assert-File "scripts/nikami/test-fnv-character-studio-live-server.py"
$characterViewerBatchPlanner = Assert-File "scripts/nikami/fnv_character_viewer_batch_plan.py"
$characterViewerBatchRunner = Assert-File "scripts/nikami/run-fnv-character-viewer-batch-plan.ps1"
$characterViewerBatchTest = Assert-File "scripts/nikami/test-fnv-character-viewer-batch-plan.ps1"
$characterStudioCatalog = Assert-File "scripts/nikami/fnv_character_studio_catalog.py"
$characterStudioCatalogRunner = Assert-File "scripts/nikami/run-fnv-character-studio-catalog.ps1"
$characterStudioCatalogTest = Assert-File "scripts/nikami/test-fnv-character-studio-catalog.ps1"
$actorPresentationLedgerTest = Assert-File "scripts/nikami/test-fnv-actor-presentation-ledger.ps1"
$noRetailArtifacts = Assert-File "scripts/nikami/test-no-retail-fnv-artifacts.ps1"
$proofNoRetailArtifacts = Assert-File "scripts/nikami/test-proof-artifacts-no-retail-payloads.ps1"

foreach ($needle in @(
    "[string]`$BuildDir",
    "[string]`$Configuration",
    "[string]`$FnvData",
    "[string]`$FnvConfigData",
    "[string]`$VcpkgRoot",
    "[string]`$ExtraOsgPluginDir",
    "[string]`$Triplet",
    "[string]`$ProofRoot",
    "[int]`$RunSeconds",
    "[string]`$ScreenshotFrames",
    "[string[]]`$RequireLogPattern",
    "[string]`$TerrainProbePoints",
    "[string]`$TerrainProbeGrid",
    "[switch]`$RequireTerrainProbeFullSupport",
    "[string]`$BootstrapCell",
    "[string]`$ActorTarget",
    "[switch]`$StageActor",
    "[switch]`$RequirePlayerTerrainSupport",
    "[switch]`$RequireFlatCameraSettled",
    "[switch]`$RequireScreenshotStability",
    "[int]`$ScreenshotTimingMinPostCameraFrames",
    "[int]`$ScreenshotTimingMaxActorCameraAgeFrames",
    "[switch]`$RequireActorVisibleHandGeometry",
    "[double]`$ActorVisibleHandMaxDistance",
    "[string]`$ActorKind",
    "[string]`$CharacterBuilderPhase",
    "[string[]]`$ActorKitParts",
    "[string[]]`$ActorKitPartModels",
    "[string[]]`$ActorKitPropSlots",
    "[string[]]`$ActorKitPropModels",
    "[string]`$ActorKitAnimationGroup",
    "[string]`$ActorKitDialogueMode",
    "[switch]`$CharacterBuilderTalk",
    "[switch]`$CreatureDiagnostics",
    "[switch]`$ActorViewLocalOffset",
    "[switch]`$RequireSkyColorSanity"
)) {
    Assert-Text $flat $needle "flat proof parameter $needle"
}

Assert-Text $flat "OPENMW_PROOF_POSTURE_TARGET" "flat proof asks runtime to audit targeted actor posture"
Assert-Text $flat "World posture BAD lines:" "flat proof reports bad world posture lines"
Assert-Text $flat "Standing arm pose BAD lines:" "flat proof reports standing arm bind/T-pose lines"
Assert-Text $flat "Target world posture BAD lines:" "flat proof reports target bad world posture lines"
Assert-Text $flat "Target standing arm pose BAD lines:" "flat proof reports target standing arm bind/T-pose lines"
Assert-Text $flat "Target visible hand geometry status:" "flat proof reports target visible skinned hand geometry"
Assert-Text $flat "did not prove visible skinned hand geometry follows animated hand anchors" "flat proof fails when visible hand skin does not follow skeleton"
Assert-Text $flat "OPENMW_PROOF_ACTOR_VIEW_LOCAL_OFFSET" "flat proof can frame actor closeups in actor-local space"
Assert-Text $flat "OPENMW_FNV_CHARACTER_BUILDER_PHASE" "flat proof can select standalone character-builder assembly phase"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_PARTS" "flat proof can select actor-kit part categories at runtime"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_PART_MODELS" "flat proof can select exact actor-kit part models at runtime"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_PROP_SLOTS" "flat proof can select actor-kit prop slots at runtime"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_PROP_MODELS" "flat proof can select exact actor-kit prop models at runtime"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP" "flat proof can select exact actor-kit animation groups at runtime"
Assert-Text $flat "OPENMW_FNV_ACTOR_KIT_DIALOGUE_MODE" "flat proof can select actor-kit dialogue proof mode at runtime"
Assert-Text $flat "OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN" "flat proof can force talk/mouth proof phase"
Assert-Text $flat "OPENMW_FNV_PROOF_TARGET_NPC" "flat proof bridges generic actor target into FNV NPC assembly diagnostics"
Assert-Text $flat "OPENMW_PROOF_ACTOR_KIND" "flat proof records actor-kind intent for proof artifacts"
Assert-Text $flat "OPENMW_FNV_CREATURE_ANIM_GROUP_DIAG" "flat proof can enable creature animation group diagnostics"
Assert-Text $flat "OPENMW_FNV_CREATURE_BODY_DIAG" "flat proof can enable creature body diagnostics"
Assert-Text $flat "OPENMW_FNV_CREATURE_KF_DIAG" "flat proof can enable creature KF diagnostics"
Assert-Text $flat "CharacterBuilderPhase:" "flat proof summary records character-builder phase"
Assert-Text $flat "ActorKitParts:" "flat proof summary records actor-kit part selector"
Assert-Text $flat "ActorKitPartModels:" "flat proof summary records actor-kit model selector"
Assert-Text $flat "ActorKitPropSlots:" "flat proof summary records actor-kit prop slot selector"
Assert-Text $flat "ActorKitPropModels:" "flat proof summary records actor-kit prop model selector"
Assert-Text $flat "ActorKitAnimationGroup:" "flat proof summary records selected actor-kit animation group"
Assert-Text $flat "ActorKitDialogueMode:" "flat proof summary records selected actor-kit dialogue mode"
Assert-Text $flat "ActorViewLocalOffset:" "flat proof summary records actor-local closeup mode"
Assert-Text $flat "FnvProofTargetNpc:" "flat proof summary records FNV-specific NPC assembly target"
Assert-Text $flat "ActorKind:" "flat proof summary records actor kind"
Assert-Text $flat "CreatureDiagnostics:" "flat proof summary records creature diagnostic mode"
Assert-Text $flat "fnv-data-provenance.json" "flat proof writes data provenance manifest"
Assert-Text $engine "falloutProofFormTargetMatches" "runtime proof camera target matching normalizes Fallout form IDs"
Assert-Text $npcAnimation "falloutProofFormTargetMatches" "runtime NPC part assembly target matching normalizes Fallout form IDs"
Assert-Text $engine "#include <components/esm4/loadcrea.hpp>" "runtime proof target matching can read ESM4 creature metadata"
Assert-Text $engine "ptr.getType() == ESM::REC_CREA4" "runtime actor proof target matching handles ESM4 creatures"
Assert-Text $engine "MWWorld::LiveCellRef<ESM4::Creature>" "runtime actor proof target matching reads creature base records"
Assert-Text $classes "ESM4Creature::registerSelf()" "ESM4 creature runtime class is registered separately from NPC"
Assert-Text $classes "ESM4Npc::registerSelf()" "ESM4 NPC runtime class is registered separately from creature"
Assert-Text $objects "insertCreature" "renderer has creature insertion path"
Assert-Text $objects "insertNPC" "renderer has NPC insertion path"
Assert-Text $npcAnimation "replaceVertexColorRgb" "NPC tint visitor has explicit vertex RGB replacement mode"
Assert-Text $npcAnimation "TintMaterialVisitor visitor(*tint, emissionStrength, hairTintModel, hairTintModel)" "hair/beard/brow maps FNV dye-control vertex colors to record tint with neutral material"
Assert-Text $npcAnimation "mNeutralMaterialWhenReplacing" "hair tint avoids double-darkening vertex-colored dye-control hair"
Assert-Text $npcAnimation "rgbMin=(" "face drawable audit reports vertex color ranges"
Assert-Text $npcAnimation "renderHairTint" "FONV hair/beard tint logs raw/render record values"
Assert-Text $npcAnimation 'OPENMW_FNV_FACE_OFFSET_X", 0.f' "FONV face surface frame has data-zero default offset"
Assert-Text $npcAnimation 'OPENMW_FNV_EYE_OFFSET_X", 0.f' "FONV eye attachment has data-zero default offset"
Assert-Text $npcAnimation 'OPENMW_FNV_MOUTH_OFFSET_X", 0.f' "FONV mouth and teeth attachment has data-zero default offset"
Assert-Text $npcAnimation 'return "headframe"' "FONV static face children keep the stable visible head-frame default"
Assert-Text $npcAnimation 'Misc::StringUtils::ciEqual(mode, "animatedheadframe")' "FONV animated face child frame is explicit opt-in until its basis is proven"
Assert-Text $npcAnimation 'Misc::StringUtils::ciEqual(headgearMode, "animatedheadframe")' "FONV animated headgear frame is explicit opt-in until its basis is proven"
Assert-Text $npcAnimation 'return "z90"' "FNV headgear coordinate basis defaults to the proven z90 static-part basis"
Assert-Text $npcAnimation "applying FNV headgear coordinate basis" "runtime logs the headgear basis used for proof shots"
Assert-Text $npcAnimation 'mode, "none"' "headgear rotation has identity escape hatch"
Assert-Text $npcAnimation "fonvCoveredSlotsHideScalpHair" "FONV headgear slots suppress covered scalp hair"
Assert-Text $npcAnimation "no-hat hair drawable" "FONV proof logs hat-compatible hair variant selection"
Assert-Text $npcAnimation "hair-under-headgear" "FONV covered headgear keeps hat-compatible hair instead of deleting it"
Assert-Text $npcAnimation "opaque-unsafe hat hair drawable" "FONV proof suppresses covered hair submeshes that mask the face"
Assert-Text $npcAnimation "hiddenGeometry=" "FONV covered hair hide masks the drawable and its render geometry"
Assert-Text $npcAnimation "enabled opaque cutout alpha" "FONV hair cutout does not alpha-blend across headgear"
Assert-Text $npcAnimation "not applying FaceGen EGM morph to non-face model" "FaceGen EGM morphs do not deform armor or headgear meshes"
Assert-Text $npcAnimation "runtime=loaded-pending-equipment-facegen-fit" "non-face FaceGen EGM assets are accounted instead of silently applied"
Assert-Text $npcAnimation 'hairAttached=" << hairAttachStatus' "FONV face check proves hair is attached instead of silently skipped"
Assert-Text $npcAnimation "OPENMW_FNV_STATICIZE_RIGGED_HAND_PARTS" "FONV rigged bare hand staticization remains explicit opt-in until its transform is proven"
Assert-Text $npcAnimation "rigged bare-hand drawable" "FONV rigged bare hand staticization is logged"
Assert-Text $npcAnimation 'armor " << armor->mEditorId' "Easy Pete proof logs armor/headgear equipment"
Assert-Text $npcAnimation "flags=0x" "Easy Pete proof logs equipment slot flags"
Assert-Text $npcAnimation "headgear-final" "FONV headgear is inserted as the final visual equipment layer"
Assert-Text $npcAnimation "forced opaque no-blend headgear" "FONV headgear is forced opaque over hair"
Assert-NoText $npcAnimation "binding baked NPC FaceGen diffuse" "FONV exported NPC FaceGen texture is not used as raw skin replacement"
Assert-Text $npcAnimation "loaded exported NPC FaceGen texture source" "FONV exported NPC FaceGen texture is explicitly loaded as source data"
Assert-Text $npcAnimation "preserving race head diffuse" "FONV head keeps the opaque race skin diffuse while FaceGen synthesis is pending"
Assert-Text $npcAnimation "accounting baked NPC FaceGen texture" "FONV exported NPC FaceGen texture is accounted as synthesis source data"
Assert-Text $npcAnimation "faceGenTexture=" "FONV face check separates loaded FaceGen source from bound race diffuse"
Assert-Text $npcAnimation "proof-only detail overlay" "FONV face detail overlay is proof-only instead of default"
Assert-Text $npcAnimation "binding data-derived face normal map" "FONV race/NPC normal companion is bound for facial ridges"
Assert-Text $npcAnimation "face skin/subsurface companion" "FONV _sk skin companion is explicitly accounted"
Assert-Text $npcAnimation "loaded head TRI dialogue morph source" "FONV head TRI dialogue morphs are accounted without mutating head skin geometry"
Assert-Text $npcAnimation "runtime=loaded-pending-runtime-head-dialogue-morph" "FONV head dialogue morph runtime support is not falsely claimed"
Assert-Text $npcAnimation "OPENMW_FNV_CHARACTER_BUILDER_PHASE" "runtime exposes standalone character-builder phase gate"
Assert-Text $npcAnimation "OPENMW_FNV_ACTOR_KIT_PARTS" "runtime exposes actor-kit part category selector"
Assert-Text $npcAnimation "OPENMW_FNV_ACTOR_KIT_PART_MODELS" "runtime exposes actor-kit exact part model selector"
Assert-Text $npcAnimation "OPENMW_FNV_ACTOR_KIT_PROP_SLOTS" "runtime exposes actor-kit prop slot selector"
Assert-Text $npcAnimation "OPENMW_FNV_ACTOR_KIT_PROP_MODELS" "runtime exposes actor-kit exact prop model selector"
Assert-Text $npcAnimation "OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP" "runtime exposes actor-kit exact animation group selector"
Assert-Text $npcAnimation "actor-kit animation request actor=" "runtime logs selected NPC animation group requests"
Assert-Text $npcAnimation "falloutCharacterBuilderAllows(std::string_view category, std::string_view model)" "runtime gates NPC assembly by category and exact model"
Assert-Text $npcAnimation "FNV/ESM4 CHARACTER BUILDER" "runtime logs character-builder include/skip decisions"
Assert-Text $npcAnimation "intentionally-excluded-with-proof" "runtime classifies phase-gated character builder omissions"
Assert-Text $npcAnimation "installed mouth open driver" "talk phase can force mouth runtime movement proof"
Assert-Text $npcAnimation "applied raw BSA body tint swatch" "FONV tiny body tint uses raw BSA bytes without a brightness multiplier"
Assert-Text $npcAnimation "loaded-pending-exact-body-tint-synthesis" "FONV tiny body tint is accounted without default whole-actor color filtering"
Assert-Text $npcAnimation "OPENMW_FNV_USE_RAW_BODY_TINT_SWATCH" "coarse raw body tint swatch application is explicit opt-in only"
Assert-Text $npcAnimation "multiplier=none" "FONV body tint has no hidden brightness multiplier"
Assert-Text $npcAnimation "loaded-pending-exact-facegen-texture-synthesis" "FaceGen EGT loading is not claimed as exact pixel synthesis"
Assert-Text $npcAnimation "OPENMW_FNV_USE_EGT_MATERIAL_TINT" "coarse EGT material tint is explicit opt-in only"
Assert-Text $npcAnimation "forced opaque no-blend skin surface" "FONV visible skin surfaces are solid instead of alpha-blended"
Assert-Text $creatureAnimation "OPENMW_FNV_CREATURE_BODY_DIAG" "creature body diagnostics are runtime-gated"
Assert-Text $creatureAnimation "OPENMW_FNV_ACTOR_KIT_PARTS" "creature runtime honors actor-kit part selector"
Assert-Text $creatureAnimation "OPENMW_FNV_ACTOR_KIT_PART_MODELS" "creature runtime honors exact body model selector"
Assert-Text $creatureAnimation "OPENMW_FNV_ACTOR_KIT_ANIMATION_GROUP" "creature runtime honors selected animation group selector"
Assert-Text $creatureAnimation "creatureActorKitAllows" "creature body NIF attachment is gated by actor-kit selector"
Assert-Text $creatureAnimation "actor-kit animation request actor=" "creature runtime logs selected animation group requests"
Assert-Text $creatureAnimation "category=creature-body" "creature selector logs creature body category gates"
Assert-Text $creatureAnimation "OPENMW_FNV_CREATURE_KF_DIAG" "creature KF diagnostics are runtime-gated"
Assert-Text $creatureAnimation "OPENMW_FNV_CREATURE_ANIM_GROUP_DIAG" "creature animation group diagnostics are runtime-gated"
Assert-Text $creatureAnimation "FNV/ESM4 diag: attached creature body nif" "creature proof logs attached body NIFs"
Assert-Text $creatureAnimation "FNV/ESM4 diag: forced creature body render mask" "creature proof logs visible body render masks"
Assert-Text $creatureAnimation "FNV/ESM4 diag: creature animation groups" "creature proof logs animation group availability"
Assert-Text $creatureAnimation "FNV/ESM4 diag: inserted creature animation for" "creature proof logs runtime animation insertion"
Assert-Text $creatureAnimation "attachedBodyNifs=" "creature proof logs attached body count"
Assert-Text $creatureAnimation "fallbackKfs=" "creature proof logs fallback KF count"
Assert-Text $creatureAnimation "discoveredKfs=" "creature proof logs discovered KF count"
Assert-Text $esm4Creature "FNV/ESM4 diag: initialized creature actor shell" "creature class logs initialized actor shell"
Assert-Text $esm4Creature "canWalk=" "creature shell proof logs movement capabilities"
Assert-Text $esm4Creature "canSwim=" "creature shell proof logs swim capability"
Assert-Text $esm4Creature "canFly=" "creature shell proof logs fly capability"
Assert-Text $esm4Creature "effective=" "creature shell proof logs effective template record"
Assert-Text $esm4Creature "template=" "creature shell proof logs template record"
Assert-Text $flat "Screenshot stability status:" "flat proof reports screenshot stability"
Assert-Text $flat "screenshot-stability.json" "flat proof writes screenshot stability JSON"
Assert-Text $flat "Screenshot timing status:" "flat proof reports post-settle screenshot timing"
Assert-Text $flat "screenshot-timing.json" "flat proof writes screenshot timing JSON"
Assert-Text $flat "bad world posture" "flat proof fails targeted actor bad world posture"
Assert-Text $flat "standing arm bind/T-pose" "flat proof fails targeted actor bind/T-pose posture"
Assert-Text $flat "did not prove screenshot stability" "flat proof fails unstable screenshot capture"
Assert-Text $flat "did not prove post-settle screenshot timing" "flat proof fails early or stale screenshot capture"
Assert-Text $engine "flatCameraSettled=" "runtime screenshot log includes flat camera settled state"
Assert-Text $engine "flatCameraSettledFrame=" "runtime screenshot log includes flat camera settled frame"
Assert-Text $engine "actorCameraApplied=" "runtime screenshot log includes actor camera state"
Assert-Text $engine "actorCameraFirstFrame=" "runtime screenshot log includes first actor camera frame"
Assert-Text $mugshot "[switch]`$DisableNativeAnimationCallbacks" "mugshot native callback disable is explicit opt-in"
Assert-Text $mugshot "[double]`$BootstrapHour = 3" "mugshot defaults to neutral standing package hour"
Assert-Text $mugshot "BootstrapHour = `$BootstrapHour" "mugshot passes explicit package hour to flat proof"
Assert-Text $mugshot "RequireScreenshotStability = `$true" "mugshot requires screenshot stability"
Assert-Text $mugshot "RequireActorVisibleHandGeometry = `$true" "mugshot requires visible skinned hand geometry"
Assert-Text $mugshot "`$proofArgs.ActorViewLocalOffset = `$true" "mugshot frames actor closeups in actor-local space by default"
Assert-Text $easyPete "FnvPartMatrixAudit = `$true" "Easy Pete angle sweep includes runtime part audit"
Assert-Text $easyPete "[switch]`$AllowRuntimeGateFailure" "Easy Pete visual sweep can keep collecting screenshots after expected runtime gate failures"
Assert-Text $easyPete "_fnv-data-provenance.json" "Easy Pete visual sweep copies per-angle data provenance"
Assert-Text $characterBuilder '"body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"' "character builder has explicit assembly ladder"
Assert-Text $characterBuilder "[string]`$ActorKind" "character builder accepts actor-kind mode"
Assert-Text $characterBuilder "[string[]]`$ActorKitParts" "character builder accepts actor-kit part selector"
Assert-Text $characterBuilder "[string[]]`$ActorKitPartModels" "character builder accepts actor-kit exact part model selector"
Assert-Text $characterBuilder "[string[]]`$ActorKitPropSlots" "character builder accepts actor-kit prop slot selector"
Assert-Text $characterBuilder "[string[]]`$ActorKitPropModels" "character builder accepts actor-kit exact prop model selector"
Assert-Text $characterBuilder "[string]`$ActorKitAnimationGroup" "character builder accepts selected actor-kit animation group"
Assert-Text $characterBuilder "[string]`$ActorKitDialogueMode" "character builder accepts selected actor-kit dialogue proof mode"
Assert-Text $characterBuilder "[string[]]`$Angles" "character builder accepts explicit camera angle selector"
Assert-Text $characterBuilder "[string]`$BootstrapCell" "character builder accepts explicit bootstrap cell"
Assert-Text $characterBuilder "[double]`$ActorStageX" "character builder accepts explicit actor stage coordinates"
Assert-Text $characterBuilder "Resolve-FnvDataFromLatestHarvest" "character builder can bootstrap FNV data from generated harvest proof"
Assert-Text $characterBuilder "FnvDataProvenance" "character builder records generated harvest provenance"
Assert-Text $characterBuilder "Resolve-VcpkgRootFromKnownPaths" "character builder can bootstrap local vcpkg toolchain"
Assert-Text $characterBuilder "VcpkgRootProvenance" "character builder records vcpkg provenance"
Assert-Text $characterBuilder "bootstrap = [pscustomobject][ordered]@" "character builder writes per-case bootstrap coordinate dump"
Assert-Text $characterBuilder "actorStage = [pscustomobject][ordered]@" "character builder writes per-case actor stage coordinate dump"
Assert-Text $characterBuilder "actorCamera = [pscustomobject][ordered]@" "character builder writes per-case camera coordinate dump"
Assert-Text $characterBuilder "[switch]`$CreatureDiagnostics" "character builder accepts creature diagnostic mode"
Assert-Text $characterBuilder '"creature-model", "creature-body", "creature-animation", "creature-full"' "character builder has explicit creature isolation ladder"
Assert-Text $characterBuilder '-split ","' "character builder splits comma-delimited phases instead of creating fake merged phases"
Assert-Text $characterBuilder "FnvPartMatrixAudit = `$true" "character builder always requests runtime part matrix audit"
Assert-Text $characterBuilder "CharacterBuilderPhase = `$phase" "character builder passes selected phase to flat proof"
Assert-Text $characterBuilder "ActorKitParts = `$ActorKitPartsCsv" "character builder passes actor-kit part selector to flat proof"
Assert-Text $characterBuilder "ActorKitPartModels = `$ActorKitPartModelsCsv" "character builder passes actor-kit exact model selector to flat proof"
Assert-Text $characterBuilder "ActorKitPropSlots = `$ActorKitPropSlotsCsv" "character builder passes actor-kit prop selector to flat proof"
Assert-Text $characterBuilder "ActorKitPropModels = `$ActorKitPropModelsCsv" "character builder passes actor-kit exact prop model selector to flat proof"
Assert-Text $characterBuilder "ActorKitAnimationGroup = `$ActorKitAnimationGroup" "character builder passes selected animation group to flat proof"
Assert-Text $characterBuilder "ActorKitDialogueMode = `$ActorKitDialogueMode" "character builder passes selected dialogue mode to flat proof"
Assert-Text $characterBuilder "actorKitSelection = [pscustomobject][ordered]@" "character builder writes selector state per case"
Assert-Text $characterBuilder "ActorKind = `$ActorKind" "character builder passes actor kind to flat proof"
Assert-Text $characterBuilder "CreatureDiagnostics = `$true" "character builder passes creature diagnostics to flat proof"
Assert-Text $characterBuilder "Unknown character builder camera angle" "character builder rejects unknown camera angles"
Assert-Text $characterBuilder "front" "character builder captures a front camera"
Assert-Text $characterBuilder "front-left" "character builder captures a left/front camera"
Assert-Text $characterBuilder "front-right" "character builder captures a right/front camera"
Assert-Text $characterBuilder "character-builder-suite.json" "character builder writes aggregate JSON"
Assert-Text $characterBuilder "character-builder-suite.md" "character builder writes aggregate Markdown"
Assert-Text $characterViewer "[string]`$BootstrapCell" "character viewer forwards explicit bootstrap cell"
Assert-Text $characterViewer "[string[]]`$ActorKitParts" "character viewer accepts actor-kit part selector"
Assert-Text $characterViewer "[string[]]`$ActorKitPartModels" "character viewer accepts actor-kit exact part model selector"
Assert-Text $characterViewer "[string[]]`$ActorKitPropSlots" "character viewer accepts actor-kit prop slot selector"
Assert-Text $characterViewer "[string[]]`$ActorKitPropModels" "character viewer accepts actor-kit exact prop model selector"
Assert-Text $characterViewer "[string]`$ActorKitAnimationGroup" "character viewer accepts selected actor-kit animation group"
Assert-Text $characterViewer "[string]`$ActorKitDialogueMode" "character viewer accepts selected actor-kit dialogue mode"
Assert-Text $characterViewer "[string[]]`$Angles" "character viewer accepts explicit camera angle selector"
Assert-Text $characterViewer "Resolve-FnvDataFromLatestHarvest" "character viewer can bootstrap FNV data from generated harvest proof"
Assert-Text $characterViewer "FnvDataProvenance" "character viewer records generated harvest provenance"
Assert-Text $characterViewer "Resolve-VcpkgRootFromKnownPaths" "character viewer can bootstrap local vcpkg toolchain"
Assert-Text $characterViewer "VcpkgRootProvenance" "character viewer records vcpkg provenance"
Assert-Text $characterViewer "BootstrapCell = `$BootstrapCell" "character viewer passes bootstrap cell to builder"
Assert-Text $characterViewer "ActorKitParts = `$ActorKitPartsCsv" "character viewer passes actor-kit part selector to builder"
Assert-Text $characterViewer "ActorKitPartModels = `$ActorKitPartModelsCsv" "character viewer passes actor-kit exact part model selector to builder"
Assert-Text $characterViewer "ActorKitPropSlots = `$ActorKitPropSlotsCsv" "character viewer passes actor-kit prop selector to builder"
Assert-Text $characterViewer "ActorKitPropModels = `$ActorKitPropModelsCsv" "character viewer passes actor-kit exact prop model selector to builder"
Assert-Text $characterViewer "ActorKitAnimationGroup = `$ActorKitAnimationGroup" "character viewer passes selected animation group to builder"
Assert-Text $characterViewer "ActorKitDialogueMode = `$ActorKitDialogueMode" "character viewer passes selected dialogue mode to builder"
Assert-Text $characterViewer "Angles = `$Angles" "character viewer passes selected camera angles to builder"
Assert-Text $characterViewer "ActorStageX = `$ActorStageX" "character viewer passes actor stage coordinates to builder"
Assert-Text $characterViewerBundle '"bootstrap": raw.get("bootstrap")' "character viewer manifest preserves per-case bootstrap coordinates"
Assert-Text $characterViewerBundle "actor_kit_selection = raw.get(`"actorKitSelection`")" "character viewer reads per-case actor-kit selector state"
Assert-Text $characterViewerBundle '"actorKitSelection": actor_kit_selection' "character viewer manifest preserves normalized per-case actor-kit selector state"
Assert-Text $characterViewerBundle "actorCamera" "character viewer manifest/UI preserves per-case camera coordinates"
Assert-Text $characterViewerBundle "screenshot_names" "character viewer normalizes screenshot entries from suite JSON"
Assert-Text $characterViewerBundle 'name == "{}"' "character viewer drops fake empty screenshot objects"
Assert-Text $characterViewerBatchPlanner "missingPlacementContext" "batch planner fails placed actors missing placement context"
Assert-Text $characterViewerBatchPlanner "runtimeBootstrapReady" "batch planner marks placed actors ready for runtime bootstrap"
Assert-Text $characterViewerBatchRunner "BootstrapCell = `$cell" "batch runner forwards planned bootstrap cell"
Assert-Text $characterViewerBatchRunner "ActorStageX" "batch runner forwards planned actor stage coordinates"
Assert-Text $characterViewerBatchTest "Placed-reference batch commands do not carry decoded bootstrap/stage placement." "batch plan contract proves placed actor placement commands"
Assert-Text $characterBuilderReport "parse_attachment_bounds" "character builder report parses attachment coordinate bounds"
Assert-Text $characterBuilderReport "parse_runtime_audits" "character builder report parses runtime part audit coordinates"
Assert-Text $characterBuilderReport "parse_actor_matches" "character builder report parses active actor target matches"
Assert-Text $characterBuilderReport "parse_animation_sources" "character builder report parses bound animation controller evidence"
Assert-Text $characterBuilderReport "parse_animation_playback" "character builder report parses target animation playback evidence"
Assert-Text $characterBuilderReport "parse_animation_requests" "character builder report parses selected animation request evidence"
Assert-Text $characterBuilderReport "missing selected animation playback evidence" "character builder report fails when selected animation group does not play"
Assert-Text $characterBuilderReport '"animationRequests"' "character builder report writes selected animation request JSON"
Assert-Text $characterBuilderReport "parse_creature_evidence" "character builder report parses creature runtime evidence"
Assert-Text $characterBuilderReport "summarize_runtime_audits" "character builder report summarizes runtime part drift over time"
Assert-Text $characterBuilderReport "PART MATRIX AUDIT" "character builder report parses runtime part matrix audit coordinates"
Assert-Text $characterBuilderReport "build_runtime_part_timelines" "character builder report builds frame/timestamp part timelines"
Assert-Text $characterBuilderReport '"runtimePartTimelines"' "character builder report writes runtime part timeline JSON"
Assert-Text $characterBuilderReport "firstBadSampleIndex" "character builder report records first bad runtime part sample"
Assert-Text $characterBuilderReport "deltaPartInAnchorTrans" "character builder report records part-in-anchor transform deltas"
Assert-Text $characterBuilderReport "runtime audit regressions after initial OK" "character builder report fails parts that attach then regress during runtime"
Assert-Text $characterBuilderReport '"runtimeAuditSummary"' "character builder report writes runtime audit summary JSON"
Assert-Text $characterBuilderReport "collapsed or empty head source geometry" "character builder report fails collapsed head source geometry"
Assert-Text $characterBuilderReport "missing talk/mouth runtime evidence" "character builder report fails missing talk runtime proof"
Assert-Text $characterBuilderReport "missing equipped weapon evidence" "character builder report fails missing weapon runtime proof"
Assert-Text $characterBuilderReport "missing target animation playback evidence" "character builder report fails missing target animation playback proof"
Assert-Text $characterBuilderReport "missing creature body/model runtime evidence" "character builder report fails missing creature body/model proof"
Assert-Text $characterBuilderReport '"actorKind"' "character builder report writes actor-kind JSON"
Assert-Text $characterBuilderReport '"creatureEvidence"' "character builder report writes creature evidence JSON"
Assert-Text $characterBuilderReport '"animationPlayback"' "character builder report writes animation playback JSON"
Assert-Text $characterViewer "fnv_character_viewer_bundle.py" "standalone character viewer launcher builds human/bot viewer bundle"
Assert-Text $characterViewer "[string]`$ActorKind" "standalone character viewer accepts actor-kind mode"
Assert-Text $characterViewer "[switch]`$CreatureDiagnostics" "standalone character viewer accepts creature diagnostic mode"
Assert-Text $characterViewer "ActorKind = `$ActorKind" "standalone character viewer passes actor kind to builder"
Assert-Text $characterViewer "CreatureDiagnostics = `$true" "standalone character viewer passes creature diagnostics to builder"
Assert-Text $characterViewer "character-viewer.html" "standalone character viewer writes human-openable HTML"
Assert-Text $characterViewer "character-viewer-manifest.json" "standalone character viewer writes bot-readable manifest"
Assert-Text $characterViewer "character-actor-kit.json" "standalone character viewer writes bot-readable actor kit"
Assert-Text $characterViewer "ActorKitJson" "standalone character viewer records actor kit path"
Assert-Text $characterViewer "generated proof/viewer output only; no retail assets are committed" "standalone character viewer keeps generated outputs outside repo"
Assert-Text $characterViewer "[switch]`$Serve" "standalone character viewer can serve generated proof output over HTTP"
Assert-Text $characterViewer "[switch]`$LiveServe" "standalone character viewer can serve live actor-kit rerun endpoint"
Assert-Text $characterViewer "127.0.0.1" "standalone character viewer serves on loopback only"
Assert-Text $characterViewer "http.server" "standalone character viewer uses a simple local generated-proof server"
Assert-Text $characterViewer "fnv_character_viewer_live_server.py" "standalone character viewer can launch live rerun server"
Assert-Text $characterViewer "liveActorKitEndpoint" "standalone character viewer records live actor-kit endpoint"
Assert-Text $characterViewer "allowedRunner" "standalone character viewer live server records allowed runner policy"
Assert-Text $characterViewer "viewer-server.json" "standalone character viewer writes bot-readable server info"
Assert-Text $characterViewer "viewer-url.txt" "standalone character viewer writes human-readable URL file"
Assert-Text $characterViewer "Viewer URL:" "standalone character viewer prints a human-openable URL"
Assert-Text $characterViewer "loopbackOnly" "standalone character viewer server manifest records loopback-only policy"
Assert-Text $characterViewer "generatedProofOutputsOnly" "standalone character viewer server manifest records generated-output-only policy"
Assert-Text $characterViewer "noRetailAssetsCommitted" "standalone character viewer server manifest records no-retail-asset policy"
Assert-Text $characterViewerLiveServer "ThreadingHTTPServer" "live actor-kit server uses loopback HTTP server"
Assert-Text $characterViewerLiveServer "/nikami/actor-kit/run" "live actor-kit server exposes rerun endpoint"
Assert-Text $characterViewerLiveServer "ALLOWED_PREFIX" "live actor-kit server allowlists generated runner prefix"
Assert-Text $characterViewerLiveServer "FORBIDDEN_CHARS" "live actor-kit server rejects shell metacharacters"
Assert-Text $characterViewerLiveServer "subprocess.run" "live actor-kit server launches runner without a shell"
Assert-Text $characterViewerLiveServer "noRetailAssetsCommitted" "live actor-kit server records no-retail policy"
Assert-Text $characterViewerLiveServer "/nikami/health" "live studio server exposes health endpoint"
Assert-Text $characterViewerLiveServer "/nikami/catalog/search" "live studio server exposes generated catalog search endpoint"
Assert-Text $characterViewerLiveServer "/nikami/studio/sessions" "live studio server exposes studio session endpoints"
Assert-Text $characterViewerLiveServer "CatalogStore" "live studio server loads generated studio catalog"
Assert-Text $characterViewerLiveServer "catalog_path" "live studio server can pin the generated catalog served by the workbench"
Assert-Text $characterViewerLiveServer "StudioSessionStore" "live studio server records studio sessions"
Assert-Text $characterViewerLiveServer "structured_actor_command" "live studio server builds structured actor jobs instead of requiring raw commands"
Assert-Text $characterViewerLiveServer "structured_actor_job" "live studio server records structured job request metadata"
Assert-Text $characterViewerLiveServer "target_mapping" "live studio server maps selected placed refs to runtime actor targets"
Assert-Text $characterViewerLiveServer '"runtimeTarget"' "live studio server request records runtime actor target"
Assert-Text $characterViewerLiveServer '"placedTarget"' "live studio server request records placed actor target"
Assert-Text $characterViewerLiveServer '"selectedTarget"' "live studio server request records selected catalog target"
Assert-Text $characterViewerLiveServer "placement_command_args" "live studio server forwards placement bootstrap and stage args"
Assert-Text $characterViewerLiveServer "ActorKitParts" "live studio server preserves structured component selector overrides"
Assert-Text $characterViewerLiveServer "nikami-fnv-character-studio-session-v1" "live studio server writes stable studio session schema"
Assert-Text $characterStudioLiveServerTest "FNV character studio live server contract PASS" "live studio server contract has focused helper test"
Assert-Text $characterStudioLiveServerTest "structured command incorrectly accepted item entry" "live studio server contract blocks generic item jobs until item summon support exists"
Assert-Text $characterStudioLiveServerTest "structured command ignored part selector overrides" "live studio server contract proves part selector overrides"
Assert-Text $characterViewerBundle "nikami-fnv-character-viewer-v2" "standalone character viewer has control-capable manifest schema"
Assert-Text $characterViewerBundle "schemaMarkers" "standalone character viewer writes schema markers"
Assert-Text $characterViewerBundle "actorProfile" "standalone character viewer writes actor profile"
Assert-Text $characterViewerBundle "CREATURE_LAYER_ORDER" "standalone character viewer has creature-specific layers"
Assert-Text $characterViewerBundle "grid3" "standalone character viewer shows three camera panes together"
Assert-Text $characterViewerBundle "Actor Matches" "standalone character viewer exposes actor target match evidence"
Assert-Text $characterViewerBundle "Skin Evidence" "standalone character viewer exposes skin data-flow evidence"
Assert-Text $characterViewerBundle "Hair Headgear Evidence" "standalone character viewer exposes hair/headgear evidence"
Assert-Text $characterViewerBundle "Animation Talk Weapon Evidence" "standalone character viewer exposes animation/talk/weapon evidence"
Assert-Text $characterViewerBundle "Creature Evidence" "standalone character viewer exposes creature evidence"
Assert-Text $characterViewerBundle "Animation Playback" "standalone character viewer exposes target animation playback evidence"
Assert-Text $characterViewerBundle "Runtime Controls" "standalone character viewer exposes human runtime controls"
Assert-Text $characterViewerBundle "Part Toggles" "standalone character viewer exposes part toggles"
Assert-Text $characterViewerBundle "Prop Slots" "standalone character viewer exposes prop and weapon slots"
Assert-Text $characterViewerBundle "Animation Dialogue" "standalone character viewer exposes animation and dialogue controls"
Assert-Text $characterViewerBundle "Bot Commands" "standalone character viewer exposes bot-readable rerun commands"
Assert-Text $characterViewerBundle '"partToggles"' "standalone character viewer manifest contains part toggle schema"
Assert-Text $characterViewerBundle '"propSlots"' "standalone character viewer manifest contains prop slot schema"
Assert-Text $characterViewerBundle '"animationControls"' "standalone character viewer manifest contains animation control schema"
Assert-Text $characterViewerBundle '"dialogueControls"' "standalone character viewer manifest contains dialogue control schema"
Assert-Text $characterViewerBundle '"botCommands"' "standalone character viewer manifest contains bot command schema"
Assert-Text $characterViewerBundle "viewer_command" "standalone character viewer builds deterministic runtime selector commands"
Assert-Text $characterViewerBundle "ActorKitParts" "standalone character viewer commands include actor-kit part selectors"
Assert-Text $characterViewerBundle "ActorKitPartModels" "standalone character viewer commands include exact model selectors"
Assert-Text $characterViewerBundle "ActorKitPropSlots" "standalone character viewer commands include prop slot selectors"
Assert-Text $characterViewerBundle "ActorKitPropModels" "standalone character viewer commands include exact prop model selectors"
Assert-Text $characterViewerBundle "ActorKitAnimationGroup" "standalone character viewer commands include selected animation group"
Assert-Text $characterViewerBundle "ActorKitDialogueMode" "standalone character viewer commands include selected dialogue mode"
Assert-Text $characterViewerBundle 'selector_arg("Angles"' "standalone character viewer commands include camera angle selectors"
Assert-Text $characterViewerBundle '"animationGroup"' "standalone character viewer manifests exact animation group controls"
Assert-Text $characterViewerBundle '"dialogueMode"' "standalone character viewer manifests exact dialogue mode controls"
Assert-Text $characterViewerBundle "animationRequests" "standalone character viewer exposes selected animation request evidence"
Assert-Text $characterViewerBundle '"runtimeSelector"' "standalone character viewer manifest exposes per-control runtime selector metadata"
Assert-Text $characterViewerBundle "renderControls" "standalone character viewer renders control panels"
Assert-Text $characterViewerBundle "startLiveRun" "standalone character viewer can request live actor-kit reruns"
Assert-Text $characterViewerBundle "/nikami/actor-kit/run" "standalone character viewer posts live actor-kit rerun requests"
Assert-Text $characterViewerBundle "Live Actor Kit Runs" "standalone character viewer exposes live rerun job panel"
Assert-Text $characterViewerBundle "commandBlock" "standalone character viewer renders runnable command controls"
Assert-Text $characterViewerBundle "slotAllows" "standalone character viewer filters prop and weapon slots"
Assert-Text $characterViewerBundle "attachmentBounds" "standalone character viewer exposes coordinate dumps"
Assert-Text $characterViewerBundle "runtimePartAudits" "standalone character viewer exposes runtime part math audits"
Assert-Text $characterViewerBundle "Runtime Drift" "standalone character viewer exposes temporal part drift"
Assert-Text $characterViewerBundle "runtimeAuditSummary" "standalone character viewer consumes runtime audit summary"
Assert-Text $characterViewerBundle "renderDrift" "standalone character viewer renders runtime drift table"
Assert-Text $characterViewerBundle "Part Timeline" "standalone character viewer exposes per-sample part timelines"
Assert-Text $characterViewerBundle "runtimePartTimelines" "standalone character viewer consumes runtime part timelines"
Assert-Text $characterViewerBundle "renderTimeline" "standalone character viewer renders runtime part timeline table"
Assert-Text $characterViewerBundle "part-timeline-v1" "standalone character viewer advertises part timeline schema marker"
Assert-Text $characterViewerBundle "build_failure_focus" "standalone character viewer builds failure-focused isolation rows"
Assert-Text $characterViewerBundle '"failureFocus"' "standalone character viewer writes failure-focused isolation JSON"
Assert-Text $characterViewerBundle "Failure Focus" "standalone character viewer exposes failure focus table"
Assert-Text $characterViewerBundle "failure_bot_commands" "standalone character viewer emits failure-focused bot rerun commands"
Assert-Text $characterViewerBundle "deltaPartInAnchorTrans" "standalone character viewer exposes failure transform deltas"
Assert-Text $characterViewerBundle "build_failure_summary" "standalone character viewer builds cross-angle failure summary"
Assert-Text $characterViewerBundle '"failureSummary"' "standalone character viewer writes cross-angle failure summary JSON"
Assert-Text $characterViewerBundle "Failure Summary" "standalone character viewer exposes cross-angle failure summary table"
Assert-Text $characterViewerBundle "failure-summary-v1" "standalone character viewer advertises failure summary schema marker"
Assert-Text $characterViewerBundle "maxAbsDeltaPartInAnchorTrans" "standalone character viewer summarizes maximum transform drift"
Assert-Text $characterViewerBundle '"status": overall' "standalone character viewer writes top-level runtime status"
Assert-Text $characterViewerBundle "build_assembly_inventory" "standalone character viewer builds part-by-part assembly inventory"
Assert-Text $characterViewerBundle '"assemblyInventory"' "standalone character viewer writes assembly inventory JSON"
Assert-Text $characterViewerBundle "Assembly Inventory" "standalone character viewer exposes assembly inventory table"
Assert-Text $characterViewerBundle "assembly-inventory-v1" "standalone character viewer advertises assembly inventory schema marker"
Assert-Text $characterViewerBundle "renderAssemblyInventory" "standalone character viewer renders assembly inventory rows"
Assert-Text $characterViewerBundle "build_capture_failures" "standalone character viewer builds missing capture rerun rows"
Assert-Text $characterViewerBundle '"captureFailures"' "standalone character viewer writes missing capture rerun JSON"
Assert-Text $characterViewerBundle "Capture Failures" "standalone character viewer exposes missing capture failures"
Assert-Text $characterViewerBundle "capture-failures-v1" "standalone character viewer advertises missing capture schema marker"
Assert-Text $characterViewerBundle "nikami-fnv-actor-kit-v1" "standalone character viewer writes actor kit schema"
Assert-Text $characterViewerBundle "actor_kit_manifest" "standalone character viewer builds actor kit metadata"
Assert-Text $characterViewerBundle "--out-kit-json" "standalone character viewer bundle accepts actor kit output path"
Assert-Text $characterViewerBatchPlanner "nikami-fnv-character-viewer-batch-plan-v1" "character viewer batch planner has stable manifest schema"
Assert-Text $characterViewerBatchPlanner "actor-base-record" "character viewer batch planner accounts for base actors"
Assert-Text $characterViewerBatchPlanner "placed-reference" "character viewer batch planner accounts for placed actor refs"
Assert-Text $characterViewerBatchPlanner '"runtimeTarget"' "character viewer batch planner separates runtime/base actor target"
Assert-Text $characterViewerBatchPlanner '"placedTarget"' "character viewer batch planner preserves placed ref target"
Assert-Text $characterViewerBatchPlanner '"selectedTarget"' "character viewer batch planner preserves human selected target"
Assert-Text $characterViewerBatchPlanner "placementCommandArgs" "character viewer batch planner records placement bootstrap args"
Assert-Text $characterViewerBatchPlanner "NPC_PHASES" "character viewer batch planner has NPC assembly ladder"
Assert-Text $characterViewerBatchPlanner "CREATURE_PHASES" "character viewer batch planner has creature assembly ladder"
Assert-Text $characterViewerBatchPlanner "ALLOWED_CLASSIFICATIONS" "character viewer batch planner enforces no-silent-skip classifications"
Assert-Text $characterViewerBatchPlanner "base-actor-spawn-or-placement-runtime" "character viewer batch planner marks base actors pending spawn/placement proof"
Assert-Text $characterViewerBatchPlanner "placed-actor-runtime-viewer-proof" "character viewer batch planner marks placed actors pending runtime viewer proof"
Assert-Text $characterViewerBatchPlanner "asset path provenance only; no retail payload bytes" "character viewer batch planner separates asset path provenance from retail payloads"
Assert-Text $characterViewerBatchPlanner "run-fnv-character-viewer.ps1" "character viewer batch planner emits runnable viewer commands"
Assert-Text $characterViewerBatchRunner "fnv_character_viewer_batch_plan.py" "character viewer batch runner invokes planner"
Assert-Text $characterViewerBatchRunner "generated command/identifier plan only; no retail assets are committed" "character viewer batch runner records no-retail policy"
Assert-Text $characterViewerBatchRunner "[switch]`$RunPlanned" "character viewer batch runner can execute selected plan entries"
Assert-Text $characterViewerBatchRunner "[switch]`$DryRun" "character viewer batch runner supports no-launch dry-run execution"
Assert-Text $characterViewerBatchRunner "nikami-fnv-character-viewer-batch-run-v1" "character viewer batch runner writes stable run summary schema"
Assert-Text $characterViewerBatchRunner "Select-PlanEntries" "character viewer batch runner filters plan entries"
Assert-Text $characterViewerBatchRunner "runtimeTarget" "character viewer batch runner executes runtime/base actor target"
Assert-Text $characterViewerBatchRunner "placedTarget" "character viewer batch runner records placed ref target separately"
Assert-Text $characterViewerBatchRunner "Invoke-ViewerEntry" "character viewer batch runner invokes selected viewer entries"
Assert-Text $characterViewerBatchRunner "run-fnv-character-viewer.ps1" "character viewer batch runner executes standalone viewer runner"
Assert-Text $characterViewerBatchRunner "Write-BatchRunHtml" "character viewer batch runner writes human HTML run index"
Assert-Text $characterViewerBatchRunner "Write-BatchRunMarkdown" "character viewer batch runner writes Markdown run index"
Assert-Text $characterViewerBatchRunner "viewer-batch-run.html" "character viewer batch runner records HTML run artifact"
Assert-Text $characterViewerBatchRunner "viewer-batch-run.md" "character viewer batch runner records Markdown run artifact"
Assert-Text $characterViewerBatchRunner "actorKit" "character viewer batch runner records actor kit artifact"
Assert-Text $characterViewerBatchTest "ContractCreature" "character viewer batch plan contract covers creature fixtures"
Assert-Text $characterViewerBatchTest "-CreatureDiagnostics" "character viewer batch plan contract requires creature diagnostics in commands"
Assert-Text $characterViewerBatchTest "nikami-fnv-character-viewer-batch-run-v1" "character viewer batch plan contract proves dry-run summary schema"
Assert-Text $characterViewerBatchTest '$runResult.actorKind -ne "creature"' "character viewer batch plan contract proves creature dry-run selection"
Assert-Text $characterViewerBatchTest '$runHtmlText.Contains("FNV Character Viewer Batch Run")' "character viewer batch plan contract proves generated HTML index"
Assert-Text $characterStudioCatalog "nikami-fnv-character-studio-catalog-v1" "character studio catalog writes stable schema"
Assert-Text $characterStudioCatalog "searchable-studio-catalog-v1" "character studio catalog advertises searchable studio schema"
Assert-Text $characterStudioCatalog "neutral-stage-gate-pending-v1" "character studio catalog explicitly gates neutral stage as pending runtime work"
Assert-Text $characterStudioCatalog "live-studio-workbench-v1" "character studio catalog exposes live workbench marker"
Assert-Text $characterStudioCatalog "three-camera-session-strip-v1" "character studio catalog exposes three-camera session marker"
Assert-Text $characterStudioCatalog "component-selector-job-payload-v1" "character studio catalog exposes component selector payload marker"
Assert-Text $characterStudioCatalog "component-review-rows-v1" "character studio catalog exposes component review rows marker"
Assert-Text $characterStudioCatalog "componentReviewRows" "character studio catalog builds per-component review payloads"
Assert-Text $characterStudioCatalog "Save Component Review Rows" "character studio catalog exposes human component review control"
Assert-Text $characterStudioCatalog "placed-runtime-target-map-v1" "character studio catalog exposes placed/runtime target mapping marker"
Assert-Text $characterStudioCatalog "placement-bootstrap-job-args-v1" "character studio catalog exposes placement bootstrap command marker"
Assert-Text $characterStudioCatalog "Studio Session" "character studio catalog HTML includes session workbench"
Assert-Text $characterStudioCatalog "cameraStrip" "character studio catalog HTML includes top camera strip"
Assert-Text $characterStudioCatalog "runtimeGateError" "character studio catalog camera strip shows runtime gate failures"
Assert-Text $characterStudioCatalog "studioPayload" "character studio catalog HTML builds structured component job payloads"
Assert-Text $characterStudioCatalog "targetMapping" "character studio catalog emits target mapping data"
Assert-Text $characterStudioCatalog "actor_runtime_target" "character studio catalog derives runtime/base actor target"
Assert-Text $characterStudioCatalog "actor_placed_target" "character studio catalog preserves placed ref target"
Assert-Text $characterStudioCatalog "actor_studio_command" "character studio catalog builds actor runtime studio commands"
Assert-Text $characterStudioCatalog "GAMEPLAY_RECORD_DOMAINS" "character studio catalog indexes gameplay records"
Assert-Text $characterStudioCatalog "every(token => haystack.includes(token))" "character studio catalog uses token-based human search"
Assert-Text $characterStudioCatalog "item-studio-spawn-command" "character studio catalog does not falsely claim generic item summon runtime support"
Assert-Text $characterStudioCatalog "no retail assets or payload bytes" "character studio catalog records no-retail payload policy"
Assert-Text $characterStudioCatalogRunner "fnv_character_studio_catalog.py" "character studio catalog runner invokes catalog builder"
Assert-Text $characterStudioCatalogRunner "[switch]`$OpenStudio" "character studio catalog runner can open generated studio HTML"
Assert-Text $characterStudioCatalogRunner "[switch]`$LiveServe" "character studio catalog runner can serve the live workbench"
Assert-Text $characterStudioCatalogRunner "studio-url.txt" "character studio catalog runner records live studio URL"
Assert-Text $characterStudioCatalogRunner "studio-live-server.json" "character studio catalog runner records live studio server metadata"
Assert-Text $characterStudioCatalogRunner "generated proof/viewer output only; no retail assets are committed" "character studio catalog runner records generated-output-only policy"
Assert-Text $characterStudioCatalogTest "ContractPistol" "character studio catalog contract covers gameplay item rows"
Assert-Text $characterStudioCatalogTest "ContractCreatureRef" "character studio catalog contract covers creature actor rows"
Assert-Text $characterStudioCatalogTest "neutral-stage pending" "character studio catalog contract proves human studio exposes neutral stage pending state"
Assert-Text $characterStudioCatalogTest "live workbench controls" "character studio catalog contract proves live workbench controls"
Assert-Text $characterViewerLiveServer "STUDIO_REVIEW_SCHEMA" "live studio server writes stable component review schema"
Assert-Text $characterViewerLiveServer "append_reviews" "live studio server records component review rows"
Assert-Text $characterViewerLiveServer 'parts[-1] == "reviews"' "live studio server exposes component review endpoints"
Assert-Text $characterStudioLiveServerTest "STUDIO_REVIEW_SCHEMA" "live studio server contract proves component review schema"
Assert-Text $characterStudioLiveServerTest "invalid review state" "live studio server contract rejects silent review states"
Assert-Text $actorPresentationLedgerTest '$npcBaseRows.Count -ne [int]$Result.npcBaseRecords' "actor presentation ledger gate validates NPC base row count"
Assert-Text $actorPresentationLedgerTest '$creatureBaseRows.Count -ne [int]$Result.creatureBaseRecords' "actor presentation ledger gate validates CREA base row count"
Assert-Text $actorPresentationLedgerTest '$npcOnlyComponents' "actor presentation ledger gate defines NPC-only components"
Assert-Text $actorPresentationLedgerTest '$creatureOnlyComponents' "actor presentation ledger gate defines creature-only components"
Assert-Text $actorPresentationLedgerTest '$badPlacedRows' "actor presentation ledger gate validates placed actor kind resolution"
Assert-Text $actorPresentationLedgerTest 'placedCellGroupType' "actor presentation ledger gate validates placed actor parent cell group"
Assert-Text $actorPresentationLedgerTest 'persistent/temp/visible child subgroup' "actor presentation ledger rejects child subgroup bootstrap cells"

$fixtureSuite = Join-Path $ProofDir "viewer-creature-fixture"
$fixtureCase = Join-Path $fixtureSuite "creature-animation_front"
New-Item -ItemType Directory -Force -Path $fixtureCase | Out-Null
$fixtureLog = Join-Path $fixtureCase "openmw.log"
@(
    'FNV/ESM4 proof: active-cell actor match target="ContractCreature" frame=10 ref=FormId:0x1 base=ContractCreature name="Contract Creature" baseEditor="ContractCreature" baseFull="Contract Creature" pos=(1,2,3) rot=(0,0,1)',
    'FNV/ESM4 diag: inserted creature animation for ContractCreature model=generated/contract-creature-root.mesh animated=1 effective=ContractCreature kfCount=1 bodyPartCount=1 attachedBodyNifs=1 fallbackKfs=1 discoveredKfs=1',
    'FNV/ESM4 diag: creature animation groups ContractCreature present=[idle,walkforward] missing=[]',
    'FNV/ESM4 diag: play matched ContractCreature group ''idle'' source=generated/contract-idle.anim checkedSources=1 controllers=4 startTime=0 loopStart=0 loopStop=1 stopTime=1 playing=1'
) | Set-Content -LiteralPath $fixtureLog -Encoding UTF8
$fixtureReport = [pscustomobject][ordered]@{
    status = "PASS"
    failures = @()
    proofDir = "generated-proof-only"
    actor = "ContractCreature"
    actorKind = "creature"
    phase = "creature-animation"
    actorPatterns = @("ContractCreature")
    actorMatches = @([pscustomobject][ordered]@{ frame = 10; ref = "FormId:0x1"; base = "ContractCreature"; name = "Contract Creature"; baseEditor = "ContractCreature"; baseFull = "Contract Creature"; line = "active-cell actor match target" })
    screenshots = @()
    categorySummary = [pscustomobject]@{}
    gates = @()
    attachmentBounds = @()
    runtimePartAudits = @()
    runtimeAuditSummary = @()
    faceDrawables = @()
    morphLines = @()
    weaponLines = @()
    animationSources = @([pscustomobject][ordered]@{ source = "generated/contract-idle.anim"; model = "generated/contract-creature-root.mesh"; matchedControllers = 4; totalControllers = 4; missingControllers = 0; line = "animation source bound" })
    animationPlayback = @([pscustomobject][ordered]@{ ref = "ContractCreature"; group = "idle"; source = "generated/contract-idle.anim"; controllers = 4; playing = $true; line = "play matched ContractCreature" })
    animationBlockers = @()
    creatureEvidence = @(
        [pscustomobject][ordered]@{ kind = "creature-animation"; model = "generated/contract-creature-root.mesh"; line = "inserted creature animation for ContractCreature" },
        [pscustomobject][ordered]@{ kind = "creature-body"; model = "generated/contract-creature-body.mesh"; visibleGeometryCount = 1; line = "forced creature body render mask for ContractCreature rigged=1 static=0 other=0" }
    )
    creatureLines = @("inserted creature animation for ContractCreature", "forced creature body render mask for ContractCreature")
}
$fixtureReport | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $fixtureCase "character-builder-report.json") -Encoding UTF8
$fixtureResult = [pscustomobject][ordered]@{
    case = "creature-animation_front"
    phase = "creature-animation"
    angle = "front"
    runtimeGateStatus = "PASS"
    runtimeGateError = ""
    reportStatus = "PASS"
    proofDir = "generated-proof-only"
    caseDir = $fixtureCase
    failures = @()
    screenshots = @()
}
ConvertTo-Json -InputObject @($fixtureResult) -Depth 12 | Set-Content -LiteralPath (Join-Path $fixtureSuite "character-builder-suite.json") -Encoding UTF8
& python $characterViewerBundle --suite-dir $fixtureSuite | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Creature viewer fixture generation failed." }
$fixtureManifest = Get-Content -LiteralPath (Join-Path $fixtureSuite "character-viewer-manifest.json") -Raw | ConvertFrom-Json
$fixtureActorKit = Get-Content -LiteralPath (Join-Path $fixtureSuite "character-actor-kit.json") -Raw | ConvertFrom-Json
if ($fixtureManifest.schema -ne "nikami-fnv-character-viewer-v2") { throw "Creature viewer fixture wrote unexpected schema: $($fixtureManifest.schema)" }
if ($fixtureActorKit.schema -ne "nikami-fnv-actor-kit-v1") { throw "Creature viewer fixture wrote unexpected actor kit schema: $($fixtureActorKit.schema)" }
if ($fixtureManifest.actorProfile.kind -ne "creature") { throw "Creature viewer fixture did not preserve creature actor profile." }
if ($fixtureActorKit.actorProfile.kind -ne "creature") { throw "Creature viewer actor kit did not preserve creature actor profile." }
if (!@($fixtureManifest.schemaMarkers).Contains("creature-isolation-v1")) { throw "Creature viewer fixture missing creature schema marker." }
if (!@($fixtureManifest.schemaMarkers).Contains("assembly-inventory-v1")) { throw "Creature viewer fixture missing assembly inventory schema marker." }
if ($fixtureManifest.status -ne "PASS") { throw "Creature viewer fixture did not write PASS top-level status: $($fixtureManifest.status)" }
if ($fixtureActorKit.status -ne "PASS") { throw "Creature viewer actor kit did not preserve PASS status: $($fixtureActorKit.status)" }
if (!@($fixtureManifest.layers).Contains("creature-animation")) { throw "Creature viewer fixture missing creature animation layer." }
if (!@($fixtureActorKit.layers).Contains("creature-animation")) { throw "Creature viewer actor kit missing creature animation layer." }
if (@($fixtureManifest.assemblyInventory).Count -eq 0) { throw "Creature viewer fixture did not expose assembly inventory." }
if (@($fixtureActorKit.assemblyInventory).Count -eq 0) { throw "Creature viewer actor kit did not preserve assembly inventory." }
foreach ($row in @($fixtureManifest.assemblyInventory)) {
    if (@($row.classifications).Count -eq 0) { throw "Creature viewer fixture emitted unclassified assembly inventory row." }
}
foreach ($row in @($fixtureActorKit.assemblyInventory)) {
    if (@($row.classifications).Count -eq 0) { throw "Creature viewer actor kit emitted unclassified assembly inventory row." }
}
if (@($fixtureManifest.controls.dialogueControls).Count -ne 0) { throw "Creature viewer fixture should not emit NPC dialogue controls." }
if (@($fixtureManifest.cases[0].creatureEvidence).Count -eq 0) { throw "Creature viewer fixture did not expose creature evidence." }
if (@($fixtureManifest.cases[0].animationPlayback).Count -eq 0) { throw "Creature viewer fixture did not expose target animation playback." }
Write-ProofLine "OK contract: generated creature viewer manifest fixture"

Assert-Text $noRetailArtifacts "Get-TrackedRetailPayloadViolations" "no-retail tracked artifact gate scans tracked payloads"
Assert-Text $noRetailArtifacts "tracked-generated-proof-root" "no-retail gate classifies generated proof roots"
Assert-Text $noRetailArtifacts "tracked-retail-payload-extension" "no-retail gate classifies retail payload extensions"
Assert-Text $proofNoRetailArtifacts "temp-extract" "proof artifact no-retail gate scans temporary extraction roots"
Assert-Text $proofNoRetailArtifacts "base64" "proof artifact no-retail gate scans encoded payload fields"
Assert-Text $proofNoRetailArtifacts ".bsa" "proof artifact no-retail gate rejects BSA proof payloads"
Assert-Text $proofNoRetailArtifacts ".esm" "proof artifact no-retail gate rejects ESM proof payloads"
Assert-Text $proofNoRetailArtifacts ".nif" "proof artifact no-retail gate rejects NIF proof payloads"
Assert-Text $doc "FnvPartMatrixAudit = `$true" "Doc Mitchell actor proof includes runtime part audit"
Assert-Text "scripts/nikami/run-fnv-flat.ps1" "must be generated publish data, not legacy modlist data" "flat proof rejects legacy modlist overlay data"
Assert-NoText "scripts/nikami/run-fnv-flat.ps1" "Auto-detected FNV overlay data" "flat proof legacy overlay autodetect removed"
Assert-Text "scripts/nikami/run-fnv-pcvr-proof.ps1" "not legacy modlist data" "PCVR proof rejects legacy modlist overlay data"
Assert-NoText "scripts/nikami/run-fnv-pcvr-proof.ps1" "LegacyOverlayData" "PCVR legacy overlay autodetect removed"
Assert-Text "scripts/nikami/deploy-fnv-vr-headset.ps1" "must not guess a retail data path" "Android deploy requires explicit FNV data"
Assert-Text "scripts/nikami/deploy-fnv-vr-headset.ps1" "not legacy modlist data" "Android deploy rejects legacy modlist overlay data"
Assert-Text $mugshot "world posture .* verdict=BAD" "mugshot parses bad world posture failures"
Assert-Text $mugshot "standing arm pose .* verdict=BAD" "mugshot parses standing arm bind/T-pose failures"
Assert-Text $mugshot "MachineWorldPostureBad" "mugshot human review includes machine world posture failure column"
Assert-Text $mugshot "MachineArmPoseBad" "mugshot human review includes machine arm pose failure column"
Assert-Text $mugshot "MachineScreenshotStability" "mugshot human review includes screenshot stability column"
Assert-Text $mugshot "MachineScreenshotTiming" "mugshot human review includes post-settle screenshot timing column"
Assert-Text $doc "ActorTarget = `"DocMitchell`"" "Doc Mitchell actor target"
Assert-Text $doc "FNV/ESM4 FACE CHECK DocMitchell:" "Doc Mitchell face asset assertion"
Assert-Text $doc "FNV/ESM4 diag: play matched FormId:0x1104c0c group 'idle'" "Doc Mitchell animation assertion"
Assert-Text $walk "FNV/ESM4 proof walk: summary reached=1 dropped=0" "walk replay completion assertion"
Assert-Text $ui "ProofGuiMode = `"data`"" "UI baseline DATA pane request"
Assert-Text $collision "Movable static physics classification lines:" "movable static removed-classification anchor"
Assert-Text $collision "captured removed MSTT collision surgery" "movable static removed-surgery anchor"

Write-ProofLine ""
Write-ProofLine "FNV proof harness contract PASS"
Write-ProofLine "ProofDir: $ProofDir"
