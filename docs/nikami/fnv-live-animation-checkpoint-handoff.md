# FNV Live Animation Checkpoint Handoff

Date: 2026-06-27

## Canonical Merge Line

- Current repo: `D:\code\vulkanOpenMW\nikami-openmw-lab-publish`
- Current branch: `nikami/fnv-vr-hands-hud`
- Merge target: `main`
- Current committed branch distance from fetched `origin/main`: `170 ahead / 2 behind`
- Current branch is synced with `origin/nikami/fnv-vr-hands-hud`.
- `origin/main` advanced to `fa90f3886488e9d69eb2cf5bdfcfe8cf2b29581c` via two merge commits from this same branch line; integrate current `origin/main` before final promotion.

Do not merge from the older workspace copies by default. Treat `D:\code\vulkanOpenMW\nikami-openmw-lab`, `D:\code\vulkanOpenMW\vsgopenmw`, `old-fnv/*`, and `backup/*` as quarantine/reference sources unless a change is deliberately reviewed and cherry-picked.

## Branches Not Coalesced

- `backup/nikami-fnv-vr-hands-hud-20260622-preclean`: 97 commits ahead of `main`; not in the current canonical branch.
- `nikami/fnv-flat-baseline`: 5 commits ahead of `main`; not in the current canonical branch.
- `old-fnv/*` remote refs point at `D:\Modlists\fnv\openmw-source`; reference only.

## Current Dirty Bundle

The live animation checkpoint is not committed yet. It includes runtime C++ changes, live studio/server changes, proof scripts, and docs.

Tracked modified files:

- `apps/openmw/mwrender/characterpreview.cpp`
- `apps/openmw/mwrender/esm4npcanimation.cpp`
- `apps/openmw/mwrender/esm4npcanimation.hpp`
- `components/sceneutil/riggeometry.cpp`
- `scripts/nikami/fnv_character_studio_catalog.py`
- `scripts/nikami/fnv_character_viewer_live_server.py`
- `scripts/nikami/run-fnv-live-character-authoring.ps1`
- `scripts/nikami/run-fnv-skinning-mode-sweep.ps1`
- `scripts/nikami/test-fnv-character-studio-live-server.py`
- `scripts/nikami/test-fnv-proof-harness-contract.ps1`

Untracked files to include if promoting this checkpoint:

- `docs/nikami/fnv-live-animation-authoring.md`
- `docs/nikami/fnv-live-animation-checkpoint-handoff.md`
- `scripts/nikami/run-fnv-animation-rotation-sweep.ps1`
- `scripts/nikami/run-fnv-live-finger-closeup-sweep.ps1`
- `scripts/nikami/test-fnv-live-animation-merge-gate.ps1`
- `scripts/nikami/test-fnv-live-bone-authoring-closeup.ps1`
- `scripts/nikami/test-fnv-live-bone-authoring-runtime.ps1`

## Latest Proof State

Latest animation/build proof gate with Release build passed:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\nikami\test-fnv-live-animation-merge-gate.ps1 -BuildDir build-clean -Configuration Release -VcpkgRoot D:\code\c\FMODS\vcpkg -RunBuild -NoSound
```

Artifact:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_174523\merge-gate.json
```

Fresh live finger smoke:

```text
D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_174644\live-finger-closeup-sweep.json
```

Meaning: current dirty checkpoint still compiles in Release, `GSEasyPete` T-pose bones/weights baseline was refreshed, `Bip01 R Finger21` live finger smoke was refreshed, live rigged hand movement/weight proof is accepted by the gate, and `release-build-openmw` passes.

Current merge-readiness gate after fetching `origin`:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_182438\merge-gate.json
```

Meaning: remote tracking is fresh, canonical branch tracking is fresh, cached refreshed T-pose/live-finger evidence passes, and the non-mutating `origin-main-merge-conflict-forecast` passes. Final merge readiness is red because `worktree-clean-for-merge-promotion` fails while this checkpoint is dirty and `origin-main-integrated` fails with `170 ahead / 2 behind`. The full refreshed build/live gate above remains the Release-build proof for this checkpoint.

Throwaway merge rehearsal:

```text
D:\code\vulkanOpenMW\proof\scratch\fnv-live-animation-merge-sandbox-20260627_175206
```

Meaning: the current dirty checkpoint was copied into a detached worktree, committed there as `63451d978a`, and merged with `origin/main` into `e0bf2b6fcb` with no conflicts. The no-build merge gate then passed in the sandbox:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_175218\merge-gate.json
```

That sandbox PASS proves the expected post-checkpoint state has `worktree-clean-for-merge-promotion=PASS`, `origin-main-integrated=PASS`, and the cached T-pose/live-finger proof artifacts still accepted by the merge gate.

The same post-merge sandbox then passed the Release build gate:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_175503\merge-gate.json
```

That build-inclusive sandbox PASS proves the rehearsed merged state also has `release-build-openmw=PASS`. The actual branch still needs the real checkpoint commit and real `origin/main` integration.

Repeatable merge rehearsal runner:

```text
scripts\nikami\run-fnv-live-animation-merge-rehearsal.ps1
```

Latest runner-generated rehearsal manifest:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-rehearsal\20260627_182245\merge-rehearsal.json
```

Meaning: the script created sandbox `D:\code\vulkanOpenMW\proof\scratch\fnv-live-animation-merge-sandbox-20260627_182245`, copied the tracked and untracked checkpoint, committed it as `61c568f38b24fdd160d52a196fe00c515bf15736`, merged `origin/main` into `519beba4f80df60fd2887dbad529764f495b5fa7`, and passed the sandbox merge gate:

```text
D:\code\vulkanOpenMW\proof\fnv-live-animation-merge-gate\20260627_182249\merge-gate.json
```

Fresh staticized T-pose bones/weights proof:

```text
D:\code\vulkanOpenMW\proof\fnv-character-builder\20260627_174541_460_12728_53ae5264\character-builder-suite.json
```

Meaning: actor-matched `GSEasyPete` T-pose proof, explicit all-bone overlay proof lines present, sampled live bone names logged, staticized hand weights loaded, `runtime-fnv-static-hand-weight-debug` logged with `weightedVertices=780`, and `runtime-fnv-fabric-no-twist` reported no BAD lines.

Fresh live rigged hand weight proof after the strongest merge gate:

```text
D:\code\vulkanOpenMW\proof\fnv-live-finger-closeup-sweep\20260627_162030\live-finger-closeup-sweep.json
```

Runtime log:

```text
D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-live-bone-authoring-contract-20260627_162125\openmw.log
```

Meaning: `GSEasyPete`, `Bip01 R Finger21`, live JSON edited while OpenMW was running, selected-finger authoring logged, `RightHand:0` skinned vertex movement logged, and the actual rendered `RigGeometry` cull path emitted `live RigGeometry weight debug rig='RightHand:0'` with `weightedVertices=780`.

New no-ribbon fabric guardrail in this dirty checkpoint:

- `components\sceneutil\riggeometry.cpp` emits `runtime-fnv-fabric-no-twist` from the real skinned draw path.
- The gate compares source mesh edge lengths against the current skinned frame and logs stretched/collapsed edge counts, `maxEdgeStretchRatio`, worst-edge source/skinned lengths, endpoint coordinates, endpoint bone weights, and node path.
- `scripts\nikami\run-fnv-flat-proof.ps1` now reports and fails on `Target fabric no-twist BAD lines`.
- `scripts\nikami\run-fnv-live-finger-closeup-sweep.ps1` now fails live finger proofs when the same runtime fabric gate reports `verdict=BAD` after the selected live bone authoring marker.
- `scripts\nikami\test-fnv-live-animation-merge-gate.ps1` now rejects stale live finger sweep artifacts unless they were run with `requireFabricNoTwist=true` and per-result `fabricNoTwistAudited=true`, and it requires T-pose logs with `runtime-fnv-fabric-no-twist` and no BAD fabric verdict.
- `scripts\nikami\test-fnv-live-animation-merge-gate.ps1` now has an explicit `worktree-clean-for-merge-promotion` row, so merge readiness cannot look green while source/docs/proof scripts are uncommitted.
- Actor rig three-camera diagnostics now default to `left,right,top`, not three near-front shots. Presentation-style `front,front-left,front-right` remains available explicitly. Catalog and live-server contracts now prove that both generated actor commands and structured no-override actor jobs use the orthogonal diagnostic default.
- Latest live log evidence from `D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-live-bone-authoring-contract-20260627_170406\openmw.log` shows `LeftHand:0` and `upperbody:0` can report startup `reason=max-edge-stretch` before live authoring applies, but the post-authoring fabric lines for the same one-finger proof are OK.

## Current Technical Truth

The branch has two separate proof modes right now:

- `OPENMW_FNV_KEEP_RIGGED_HAND_PARTS=1` is still needed for live bone edits to visibly move rigged hand vertices.
- Staticized hand proof remains the clean T-pose all-bone/all-weight baseline.
- Live rigged hands can now show runtime weight colors and emit weighted-vertex telemetry from the actual draw path while live bone authoring is active.

That split is intentional for diagnosis. It is not parity. The next implementation milestone is to rerun the full merge gate after the stricter sweep assertion, then broaden the live rigged weight/movement proof across more finger chains and NPC fixtures, or add a deliberate staticized pose-bake path that consumes live skeleton edits.

## Required Gate Before Merge

Before promoting to `main`, rerun:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\nikami\test-fnv-live-animation-merge-gate.ps1 -BuildDir build-clean -Configuration Release -VcpkgRoot D:\code\c\FMODS\vcpkg -RunTposeWeightBaseline -RunLiveFingerSmoke -RunBuild -NoSound
```

That gate must pass with:

- `t-pose-bones-weights-baseline`
- `live-finger-smoke`
- `release-build-openmw`
- `proof-harness-contract`
- `origin-main-tracking-fresh`
- `worktree-clean-for-merge-promotion`
- `origin-main-integrated`
- `origin-main-merge-conflict-forecast`
- `canonical-branch-tracking-fresh`

The latest animation/build proof above already includes `-RunBuild`, but the current merge-readiness gate is red until the branch incorporates fetched `origin/main`. Rerun the full gate after integration and after any additional source changes before promoting to `main`.

## Merge Path

1. Commit the dirty bundle on `nikami/fnv-vr-hands-hud`.
2. Integrate fetched `origin/main` into `nikami/fnv-vr-hands-hud` after the dirty bundle is safely committed or shelved. The `20260627_175206` sandbox rehearsed this exact shape successfully.
3. Rerun the full merge gate with `-RunTposeWeightBaseline -RunLiveFingerSmoke -RunBuild`.
4. Push `nikami/fnv-vr-hands-hud`.
5. Update `main` with `git pull --ff-only origin main`.
6. Merge or fast-forward from `nikami/fnv-vr-hands-hud` as allowed by the reconciled history, then push `main`.

If the dirty bundle is not ready, stash with untracked files before touching `main`:

```powershell
git stash push -u -m "pre-merge FNV live animation WIP 2026-06-27"
```

Prefer committing this checkpoint before merge-back, because the untracked proof scripts and docs define the current animation baseline.
