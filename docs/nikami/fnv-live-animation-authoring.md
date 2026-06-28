# FNV Live Authoring Contract

This is the current live runtime authoring contract for Fallout New Vegas data in OpenMW.

## Purpose

The live rig exists so we can observe, change, and verify runtime 3D state without restarting the whole scene or trusting screenshots by themselves. It must work for every visible actor, creature, weapon, held object, door, container, prop, and animated part.

## Current Loop

1. Keep one OpenMW worker alive.
2. Keep one website judge surface alive.
3. Show one current front, left, and top image.
4. Emit current-frame telemetry for every drawn target part.
5. Apply one live transform or animation change.
6. Wait for runtime acknowledgement, telemetry, and screenshot hash.
7. Compare previous and current origins, axes, bounds, matrices, and image.
8. Human-gate the result until the data gates are strong enough to automate.

## Required Runtime Data

Every visible part needs:

- Source record and asset path.
- Parent frame and skeleton bone or helper node.
- Local, parent, world, and target-relative matrices.
- Origin and X/Y/Z axes in local, parent, world, and target space.
- Local, world, and target-space bounds.
- Visible drawable and vertex counts.
- Current animation source, group, sampled time, controller, and blend mask when animated.
- IK solver targets, pole hints, basis, error, and final endpoint positions when IK is involved.
- Previous command, current command, runtime ack frame, telemetry frame, and screenshot hash.

If a part is visible but any of this data is missing or stale, it is not trusted.

## Current First Fixture

Sunny Smiles hair/face is the first fixture. The goal is not a Sunny-specific hack. The goal is to prove the generic part-frame pipeline:

1. Head and hair origins are known.
2. Head and hair axes are known.
3. The current bad transform is numerically explainable.
4. The corrected transform is replayable.
5. The same method works on a random non-Pete actor and one non-hair target.

## Commands

Start live authoring:

```powershell
scripts\nikami\run-fnv-live-character-authoring.ps1
```

Audit live surface frames:

```powershell
python scripts\nikami\fnv_live_surface_frame_audit.py <openmw.log> --latest --pretty
```

Audit a single model family:

```powershell
python scripts\nikami\fnv_live_surface_frame_audit.py <openmw.log> --model-contains hairbun --latest --pretty
```

## Promotion Rule

A runtime tweak can become code only after:

1. The current image and telemetry agree.
2. The previous/current audit explains the change in matrix terms.
3. The recipe replays in a clean run.
4. Sunny passes.
5. One random non-Pete human actor passes.
6. One non-hair target passes.
7. Release build and focused proof gates pass.

No screenshot-only pass, legacy pass condition, or one-actor workaround can promote code.
