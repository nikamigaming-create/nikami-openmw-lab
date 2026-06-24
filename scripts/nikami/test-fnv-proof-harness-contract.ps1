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
    "[string]`$CharacterBuilderPhase",
    "[switch]`$CharacterBuilderTalk",
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
Assert-Text $flat "OPENMW_FNV_PROOF_MOUTH_FORCE_OPEN" "flat proof can force talk/mouth proof phase"
Assert-Text $flat "CharacterBuilderPhase:" "flat proof summary records character-builder phase"
Assert-Text $flat "ActorViewLocalOffset:" "flat proof summary records actor-local closeup mode"
Assert-Text $flat "fnv-data-provenance.json" "flat proof writes data provenance manifest"
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
Assert-Text $characterBuilder '-split ","' "character builder splits comma-delimited phases instead of creating fake merged phases"
Assert-Text $characterBuilder "FnvPartMatrixAudit = `$true" "character builder always requests runtime part matrix audit"
Assert-Text $characterBuilder "CharacterBuilderPhase = `$phase" "character builder passes selected phase to flat proof"
Assert-Text $characterBuilder "front-left" "character builder captures a left/front camera"
Assert-Text $characterBuilder "front-right" "character builder captures a right/front camera"
Assert-Text $characterBuilder "character-builder-suite.json" "character builder writes aggregate JSON"
Assert-Text $characterBuilder "character-builder-suite.md" "character builder writes aggregate Markdown"
Assert-Text $characterBuilderReport "parse_attachment_bounds" "character builder report parses attachment coordinate bounds"
Assert-Text $characterBuilderReport "parse_runtime_audits" "character builder report parses runtime part audit coordinates"
Assert-Text $characterBuilderReport "summarize_runtime_audits" "character builder report summarizes runtime part drift over time"
Assert-Text $characterBuilderReport "runtime audit regressions after initial OK" "character builder report fails parts that attach then regress during runtime"
Assert-Text $characterBuilderReport '"runtimeAuditSummary"' "character builder report writes runtime audit summary JSON"
Assert-Text $characterBuilderReport "collapsed or empty head source geometry" "character builder report fails collapsed head source geometry"
Assert-Text $characterBuilderReport "missing talk/mouth runtime evidence" "character builder report fails missing talk runtime proof"
Assert-Text $characterBuilderReport "missing equipped weapon evidence" "character builder report fails missing weapon runtime proof"
Assert-Text $characterViewer "fnv_character_viewer_bundle.py" "standalone character viewer launcher builds human/bot viewer bundle"
Assert-Text $characterViewer "character-viewer.html" "standalone character viewer writes human-openable HTML"
Assert-Text $characterViewer "character-viewer-manifest.json" "standalone character viewer writes bot-readable manifest"
Assert-Text $characterViewer "generated proof/viewer output only; no retail assets are committed" "standalone character viewer keeps generated outputs outside repo"
Assert-Text $characterViewerBundle "grid3" "standalone character viewer shows three camera panes together"
Assert-Text $characterViewerBundle "Skin Evidence" "standalone character viewer exposes skin data-flow evidence"
Assert-Text $characterViewerBundle "Hair Headgear Evidence" "standalone character viewer exposes hair/headgear evidence"
Assert-Text $characterViewerBundle "Animation Talk Weapon Evidence" "standalone character viewer exposes animation/talk/weapon evidence"
Assert-Text $characterViewerBundle "attachmentBounds" "standalone character viewer exposes coordinate dumps"
Assert-Text $characterViewerBundle "runtimePartAudits" "standalone character viewer exposes runtime part math audits"
Assert-Text $characterViewerBundle "Runtime Drift" "standalone character viewer exposes temporal part drift"
Assert-Text $characterViewerBundle "runtimeAuditSummary" "standalone character viewer consumes runtime audit summary"
Assert-Text $characterViewerBundle "renderDrift" "standalone character viewer renders runtime drift table"
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
