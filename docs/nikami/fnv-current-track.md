# FNV Current Track

This is the current project track for the published Nikami OpenMW lab branch.

## Direction

The current priority is Fallout New Vegas parity from a flat, fully observable runtime first, then PCVR and world placement using the same runtime data rules.

The order is:

1. Flat OpenMW truth rig.
2. Every actor and character part.
3. Every creature.
4. Every animated or placeable asset: weapons, doors, containers, props, projectiles, and interactables.
5. Re-run the same evidence in PCVR and world placement.
6. Promote only after the same data-backed gates pass from flat and VR perspectives.

## Current Gate

The active gate is the FNV parity truth rig:

- One live OpenMW worker.
- One website judge surface.
- One current combined front, left, and top image.
- Current-frame telemetry for every drawn part.
- Explicit origin, X/Y/Z axes, parent frame, local/world matrices, bounds, and screenshot hashes.
- Human judgment in the loop until the data gates become strong enough to automate.

Sunny Smiles hair/face is the first proving fixture because it exposed the exact class of problem we must solve generically: a visible part can look close while its origin, axes, or parent frame are wrong.

The accepted generic FNV scalp-hair basis is now `rotation=(90,90,0)`, `offset=(0,0,0)`, with pivot rotation off. This is not Sunny-specific; it is the default `OPENMW_FNV_HAIR` head-frame basis and should be visible in flat runtime and PCVR/world placement without loading a live authoring JSON.

## Non-Goals Right Now

- Do not promote one-off Sunny hair magic numbers.
- Do not move to broad actor sweeps until the accepted generic hair basis is checked in flat runtime and PCVR/world placement.
- Do not trust screenshot-only or legacy pass conditions.
- Do not let PCVR/world placement weaken the flat truth rig gates.
- Do not weaken runtime evidence into screenshot-only pass conditions.

## VR Check

The next proof is a PCVR/world walkaround check from main: walk to Sunny in the world and verify the same generic hair basis is visible without live overrides. A VR issue creates a platform-specific failure row; it does not weaken the flat data gate.

## Promotion Path

Before pushing and proposing a merge to `main`:

1. Release build passes.
2. Truth rig page shows front, left, and top clearly.
3. Runtime logs current-frame part telemetry after every live command.
4. `scripts/nikami/fnv_live_surface_frame_audit.py` reports previous/current state for the target part.
5. Sunny passes by image, telemetry, and replayed generic hair basis.
6. Main carries the accepted generic hair basis.
7. A fresh branch from main proves Sunny in PCVR/world placement without live overrides.
8. One random non-Pete human actor passes the same workflow.
9. One non-hair target passes the same workflow.

## Canonical Docs

Read these first:

- [FNV parity truth rig contract](fnv-parity-truth-rig-contract.md)
- [FNV 3D math truth rig notes](fnv-3d-math-truth-rig-notes.md)
- [FNV live animation authoring](fnv-live-animation-authoring.md)
- [FNV character presentation harness](fnv-character-presentation-harness.md)
