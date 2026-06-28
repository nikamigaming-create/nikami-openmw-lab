# FNV Character Presentation Harness

This is the current harness contract for proving Fallout New Vegas actors, creatures, and animated assets in OpenMW.

## Purpose

The harness exists to prove that visible runtime data is correct from source record to pixel. It covers humans, ghouls, creatures, robots, heads, hair, eyes, mouths, teeth, outfits, weapons, props, animations, dialogue, and placement.

## Order

1. Sunny Smiles hair/face.
2. One random non-Pete human actor using the same workflow.
3. One non-hair target on a human actor.
4. Broader human actor coverage.
5. Creatures and robots.
6. Weapons, held props, doors, containers, projectiles, and animated props.
7. PCVR and world placement after flat evidence is trusted.

## Required Evidence

Each target needs:

- Source record, form id, editor id, plugin, and asset paths.
- Loaded mesh, texture, animation, skeleton, and controller provenance.
- Parent frame, skeleton bone or helper node, local matrix, world matrix, target-relative matrix.
- Origin, X/Y/Z axes, bounds, visible vertices, and drawable state.
- Current animation source, sampled time, controller group, and blend state when animated.
- IK solver state, target, pole hint, basis, and endpoint error when IK is involved.
- Front, left, and top images from the same command revision.
- Runtime telemetry from the same frame as the image.
- Human review mark until automated gates are strong enough to replace it.

## Gates

A target is not complete unless all applicable gates pass:

- Resolve gate: every referenced source record and asset is accounted for.
- Frame gate: every visible part has current origin, axes, matrices, and bounds.
- Visual gate: front, left, and top panes are clear and nonblank.
- Animation gate: sampled animation is identified and active, not accidentally bind pose.
- Skinning gate: weighted vertices and post-skin bounds are sane.
- IK gate: solver targets, endpoint error, and final orientation are logged.
- Dialogue/LIP gate: voice, LIP samples, mouth movement, teeth, and tongue remain coherent.
- Human review gate: a reviewer marks PASS, FAIL, or BLOCKED.

## First Human Fixture

Sunny is not special-case code. Sunny is the first proving fixture because the hair issue exposes the generic problem:

- The part can be visible and still have the wrong frame.
- A visually obvious error must be explainable by origin, axis, matrix, or parent-frame data.
- A transform must replay cleanly before it becomes C++.

After Sunny passes, select one random non-Pete actor and repeat the same evidence path.

## Outputs

Each run should write generated proof artifacts only:

- Current combined image.
- Current telemetry JSON.
- Previous/current surface-frame audit JSON.
- Human review row.
- Runtime log.
- Replay recipe.

No retail or mod asset payload bytes are committed to the repo.
