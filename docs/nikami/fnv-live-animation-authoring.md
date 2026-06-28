# FNV Live Animation Authoring Contract

This branch treats Fallout New Vegas NPC animation parity as a live runtime authoring problem first and a C++ promotion problem second.

## Canonical Line

- Repository: `D:\code\vulkanOpenMW\nikami-openmw-lab-publish`
- Branch: `nikami/fnv-vr-hands-hud`
- Merge destination: `main`, only after the live workflow produces repeatable proof artifacts.
- Quarantine: `D:\code\vulkanOpenMW\nikami-openmw-lab` and `D:\code\vulkanOpenMW\vsgopenmw` are not animation sources for this merge unless a specific change is reviewed and cherry-picked.

## Non-Negotiables

1. OpenMW stays running during hand and finger diagnosis.
2. Every experimental transform starts from a T-pose or explicitly named neutral pose.
3. Humans and bots use the same live authoring surface.
4. Runtime proof beats screenshots.
5. The live actor, mirrored controls, current manipulations, bones, and weight evidence must be visible or logged before a result is trusted.
6. No transform, rotation mode, IK tweak, or skinning workaround is promoted to C++ until it has a saved live recipe, runtime audit lines, and a repeatable visual proof.

## Live Loop

The live authoring runner creates a JSON control file, a runtime command file, the studio server, and a long-running OpenMW process. The UI and bots both edit the same JSON file through the studio server.

Primary entry point:

```powershell
scripts\nikami\run-fnv-live-character-authoring.ps1
```

Repeatable runtime contract:

```powershell
scripts\nikami\test-fnv-live-bone-authoring-runtime.ps1
```

Self-contained close-up contract:

```powershell
scripts\nikami\test-fnv-live-bone-authoring-closeup.ps1
```

Per-finger close-up sweep:

```powershell
scripts\nikami\run-fnv-live-finger-closeup-sweep.ps1
```

Merge-readiness gate:

```powershell
scripts\nikami\test-fnv-live-animation-merge-gate.ps1
```

The contract runner launches the runtime proof, tails the live isolated config log, writes hand and finger bone rotations into the generated JSON file while OpenMW is still running, and fails unless the runtime audit records the applied bone transforms, proof-preview bone catalog, and forced mesh-consumer refresh. When given a baseline screenshot, it can also fail unless the post-edit capture has a nonzero pixel delta. It must not wait on the copied proof log, because that file is post-run evidence.

Visual proof is deliberately secondary to telemetry. The useful diagnostic view set is `left,right,top`: side views catch detach/parent-space failures, and the steep top view catches depth curls that front shots hide. Repeating three front-like hand close-ups is not useful; use a single close-up for pixel delta, then switch to `-BaselineAngles left,right,top` only when a human needs spatial context.

Runtime input:

```text
OPENMW_FNV_LIVE_AUTHORING_FILE=<proof>\live-authoring.json
```

Useful visibility aliases:

```text
OPENMW_FNV_SHOW_ALL_BONES=1
OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS=1
OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE=all
```

Bone controls use this schema:

```text
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_ROTATION_X
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_ROTATION_Y
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_ROTATION_Z
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_OFFSET_X
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_OFFSET_Y
OPENMW_FNV_BONE_<SANITIZED_BONE_NAME>_OFFSET_Z
```

Example:

```json
{
  "OPENMW_FNV_BONE_BIP01_R_HAND_ROTATION_Z": 25.0,
  "OPENMW_FNV_BONE_BIP01_R_FINGER11_ROTATION_Z": 15.0
}
```

## Runtime Requirements

- Live bone transforms are reapplied during runtime, not only when the file changes, so animation updates cannot silently stomp the test pose.
- Bone debug telemetry runs after live authoring has been applied.
- Live bone edits require an explicit proof actor target.
- The runtime audit must record `FNV/ESM4 live authoring: frame-applied bone authoring`, `proofPreview:true`, and `runtime-live-bone-authoring-mesh-consumers` before the edit is considered real.
- Snapshots, replay artifacts, and apply-live artifacts must preserve bone controls.

## Current Surface

- Studio server: `scripts\nikami\fnv_character_viewer_live_server.py`
- Studio UI: `scripts\nikami\fnv_character_studio_catalog.py`
- Live runner: `scripts\nikami\run-fnv-live-character-authoring.ps1`
- Runtime application: `apps\openmw\mwrender\esm4npcanimation.cpp`
- Preview rig debug hooks: `apps\openmw\mwrender\characterpreview.cpp`

The first live controls cover both hands, thumbs, and major finger segments. That is a useful starting panel, not the finished target.

## Latest Baseline

- T-pose proof works from the character builder command path with IK bones visible.
- Detached/static hand weight telemetry now survives staticization through `runtime-fnv-static-hand-weight-debug`.
- `OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE=fingers` proves 15 finger slots and 719 weighted vertices on staticized hands.
- `OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE=all` proves 17 strongest-bone slots and 780 weighted vertices on staticized hands when skinning debug data is available.
- The all-bone overlay can now be requested directly with `OPENMW_FNV_SHOW_ALL_BONES=1` / `OPENMW_FNV_ALL_BONE_DEBUG=1` and logs `runtime-fnv-all-bone-overlay` with sampled live bone names.
- The skin-weight view can now be requested directly with `OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS=1`; the old `OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS=1` alias still works.
- Live JSON bone authoring reaches the target skeleton and logs a 70-bone runtime catalog plus `frame-applied bone authoring` for `Bip01 R Hand` and `Bip01 R Finger11`.
- The live runtime and close-up contracts now accept explicit `HandBone` and `FingerBone` names, derive their `OPENMW_FNV_BONE_*` prefixes from the names, and assert telemetry for the selected bones instead of hard-coding only `Bip01 R Finger11`.
- `run-fnv-live-finger-closeup-sweep.ps1` can run repeated T-pose close-up contracts across selected thumb/finger bones and writes `live-finger-closeup-sweep.json` plus `summary.md`.
- The sweep defaults cover right thumb, index, middle, ring, and pinky chain bones and can require selected-digit authoring plus rigged `RightHand:0` matrix or vertex movement telemetry in the live OpenMW log.
- `test-fnv-live-animation-merge-gate.ps1` records branch distance from `origin/main`, dirty-worktree merge-promotion status, quarantined non-canonical sources, a non-mutating `git merge-tree` conflict forecast, PowerShell syntax, Python compile, proof harness status, latest passing finger sweep artifact, and optional OpenMW Release build/live finger smoke.
- Actor rig three-camera diagnostics now default to `left,right,top` so twisted hands/cloth cannot hide behind three near-front views. `front,front-left,front-right` remains available for presentation-style checks.
- Staticized detached hands do not consume later skeleton bone rotations visually, so runtime bone-motion proofs must use `OPENMW_FNV_KEEP_RIGGED_HAND_PARTS=1` until staticized meshes have a deliberate pose-baking path.
- Keeping rigged hand parts with `OPENMW_FNV_KEEP_RIGGED_HAND_PARTS=1` now produces a repeatable post-edit pixel delta after live bone authoring forces SceneUtil skeleton matrices and rig geometry consumers to refresh.
- Runtime rigged hand weight inspection now runs inside `SceneUtil::RigGeometry::cull`, so the same live hand mesh that consumes bone edits can emit and display skin weights.
- Latest live rigged weight proof: `D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_162030\live-finger-closeup-sweep.json`.
- Latest live rigged weight log: `D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-live-bone-authoring-contract-20260627_162125\openmw.log`, including `live RigGeometry weight debug rig='RightHand:0'`, `bones=17`, `vertices=780`, `weightedVertices=780`, and `runtime-fnv-live-rig-weight-debug`.
- Runtime fabric integrity now has a named draw-path gate: `runtime-fnv-fabric-no-twist`. It compares source mesh edge lengths against the current skinned frame and logs `maxEdgeStretchRatio`, stretched/collapsed edge counts, the worst edge endpoints, source/skinned edge lengths, endpoint bone weights, node path, and `verdict=OK/BAD`.
- Flat proof and character-builder aggregation now fail/report `Target fabric no-twist BAD lines`, so ribbon-like clothing or limb curls cannot hide behind a passing finger pixel delta.
- The live finger sweep now fails on `runtime-fnv-fabric-no-twist` BAD lines only after the selected live bone authoring marker. Startup/pre-authoring BAD lines remain diagnostic, but they do not prove the finger edit itself broke fabric.
- Fresh post-authoring one-finger smoke passed for `GSEasyPete` / `Bip01 R Finger21`: `D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_174644\live-finger-closeup-sweep.json`.
- The same run still logged useful startup diagnostics before live authoring was applied: `LeftHand:0` and `upperbody:0` briefly reported max-edge-stretch BAD lines, then post-authoring `LeftHand:0`, `RightHand:0`, `arms:0`, and `upperbody:0` reported `verdict=OK`.
- Latest passing live contract: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_145230\runtime-contract.json`.
- Latest live screenshot: `D:\code\vulkanOpenMW\proof\runtime\fnv-flat-clean-live-bone-authoring-contract-20260627_145230\screenshots\screenshot000.png`.
- Same-build no-edit baseline: `D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_144612_934_35436_a8ed0e2c\t-pose_front\screenshot000.png`.
- Pixel delta proof: `meanAbsDelta=0.14109696502057614`, `changedPixels=2937`, `width=1920`, `height=1080`.
- Zoom proof: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_145230\auto-diff-zoom-baseline-vs-live.png`.
- Hand-close camera profiles now auto-aim at posed `Bip01 R Hand` / `Bip01 L Hand` bones instead of fixed ribcage coordinates.
- Latest passing close-up contract: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_150725\runtime-contract.json`.
- Close-up no-edit baseline: `D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_150638_200_36816_cb11d081\t-pose_front\screenshot000.png`.
- Close-up live screenshot: `D:\code\vulkanOpenMW\proof\runtime\fnv-flat-clean-live-bone-authoring-contract-20260627_150725\screenshots\screenshot000.png`.
- Close-up pixel delta proof: `meanAbsDelta=0.53595518261316877`, `changedPixels=23415`, `width=1920`, `height=1080`.
- Close-up visual diff: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_150725\right-hand-close-baseline-vs-live-diff.png`.
- Second-NPC close-up smoke passed on `DocMitchell` in `GSDocMitchellHouse` using the same right-hand live edit.
- Doc Mitchell close-up contract: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_151502\runtime-contract.json`.
- Doc Mitchell no-edit baseline: `D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_151409_233_30696_d723f98c\t-pose_front\screenshot000.png`.
- Doc Mitchell live screenshot: `D:\code\vulkanOpenMW\proof\runtime\fnv-flat-clean-live-bone-authoring-contract-20260627_151502\screenshots\screenshot000.png`.
- Doc Mitchell pixel delta proof: `meanAbsDelta=0.51145383230452679`, `changedPixels=23658`, `width=1920`, `height=1080`.
- Doc Mitchell visual diff: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_151502\docmitchell-right-hand-close-baseline-vs-live-diff.png`.
- Parameterized non-default finger proof passed on `GSEasyPete` for `Bip01 R Finger21` using the same `Bip01 R Hand` close-up contract.
- `Bip01 R Finger21` contract: `D:\code\vulkanOpenMW\proof\fnv-live-bone-authoring-contract\20260627_152601\runtime-contract.json`.
- `Bip01 R Finger21` no-edit baseline: `D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_152513_973_17940_67a4d5ae\t-pose_front\screenshot000.png`.
- `Bip01 R Finger21` live screenshot: `D:\code\vulkanOpenMW\proof\runtime\fnv-flat-clean-live-bone-authoring-contract-20260627_152601\screenshots\screenshot000.png`.
- Isolated `Bip01 R Finger21` sweep passed with `HandRotationZ=0`, `FingerRotationZ=15`, selected-digit authoring, and rigged `RightHand:0` consumption telemetry.
- Isolated finger sweep: `D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_153616\live-finger-closeup-sweep.json`.
- Isolated finger live log: `D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-live-bone-authoring-contract-20260627_153707\openmw.log`.
- Isolated finger evidence includes `bone="Bip01 R Finger21"`, `prefix=OPENMW_FNV_BONE_BIP01_R_FINGER21`, `runtime-live-bone-authoring-mesh-consumers`, and `RightHand:0 skinned vertices this frame maxVertexSkinDelta=1.19105`.
- Latest merge-readiness gate passed with Release build included: `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_154234\merge-gate.json`.
- Latest full refreshed animation/build proof gate on the real merged branch passes every proof/build row: `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_182805\merge-gate.json`, `172 ahead / 0 behind`, with `worktree-clean-for-merge-promotion=PASS`, `origin-main-integrated=PASS`, `live-finger-smoke=PASS`, `t-pose-bones-weights-baseline=PASS`, and `release-build-openmw=PASS`.
- Main fast-forward promotion rehearsal from `origin/main` to `origin/nikami/fnv-vr-hands-hud` passed in `D:\code\vulkanOpenMW\proof\scratch\fnv-main-ff-promotion-sandbox-20260627_183558`, with gate PASS at `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_183615\merge-gate.json`.
- Throwaway merge rehearsal committed the pre-merge checkpoint in `D:\code\vulkanOpenMW\proof\scratch\fnv-live-animation-merge-sandbox-20260627_175206`, merged `origin/main` cleanly into detached merge commit `e0bf2b6fcb`, and passed the post-merge Release build gate at `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_175503\merge-gate.json` with `worktree-clean-for-merge-promotion=PASS`, `origin-main-integrated=PASS`, and `release-build-openmw=PASS`.
- Repeatable rehearsal runner added at `scripts\nikami\run-fnv-live-animation-merge-rehearsal.ps1`; latest runner-generated no-build rehearsal manifest is `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-rehearsal\20260627_182245\merge-rehearsal.json`, with sandbox gate PASS at `D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_182249\merge-gate.json`.
- Current strict live smoke artifact: `D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_182924\live-finger-closeup-sweep.json`, `Bip01 R Finger21`, `passCount=1`, `failCount=0`, `meanAbsDelta=76.536058706275725`, `changedPixels=2073600`, rigged hand consumption audited, live rig weight debug audited, `fabricNoTwistAuditLines=21`, and post-authoring fabric no-twist BAD count `0`.
- Fresh staticized T-pose bones/weights proof: `D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_182826_723_41392_86878e2e\character-builder-suite.json`, actorTarget `GSEasyPete`, actorKind `npc`, with visible hand geometry PASS, fabric no-twist BAD lines `0`, IK bone overlay proof lines, and staticized hand weight debug evidence accepted by the merge gate.
- Current checkpoint handoff: `docs\nikami\fnv-live-animation-checkpoint-handoff.md`.
- The earlier frame-720 close-up capture missed screenshots on the current build speed, so close-up defaults now capture baseline at frame 240 and live at frame 360 with longer run/timeout windows.

## Open Gaps

1. Enumerate every runtime bone into the studio UI instead of relying on a hand-written hand/finger prefix list.
2. Run the per-finger close-up sweep across thumb/index/middle/ring/pinky representative bones on Easy Pete and Doc Mitchell, then add tighter camera selectors for any chain whose movement is hard to inspect.
3. Add a true per-bone skin influence view for non-hand meshes. Staticized hand proofs are now covered, but full parity work needs selectable per-bone vertex influence display across body, clothing, and armor.
4. Preserve screenshot capture as an archive artifact only. The decision loop is live telemetry plus direct manipulation.
5. Run rotation and skinning sweeps only after the live T-pose chain proves which bone, transform basis, or weight interpretation is wrong.
6. Add missing-bone and transform-stack blockers to the live gate so hands, feet, clothing, and armor cannot silently detach or inherit the wrong parent space.
7. Promote only the refresh and proof-preview fixes that survive broader actor coverage; Easy Pete and Doc Mitchell are smoke proof, not full parity.

## Promotion Rule

A candidate fix can move from live authoring into C++ only when all of these exist:

- A saved live authoring JSON recipe.
- A runtime audit showing the target actor, target bone, rotation/offset, and live gate.
- A passing live runtime contract or an explicitly documented reason it could not run on the current machine.
- A visual proof with close camera framing.
- A passing focused server contract test.
- A clean Release build.
- A written note explaining whether the fix is bone hierarchy, transform basis, animation playback, skinning weights, IK, or asset binding.

This is the baseline until FNV detached NPC animation reaches parity.
