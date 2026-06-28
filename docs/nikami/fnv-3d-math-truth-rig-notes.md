# FNV 3D Math Truth Rig Notes

This is the working notebook for making OpenMW understand Fallout New Vegas runtime data from byte to pixel.

## Standard

Every runtime thing we draw must have a frame:

- Source provenance: plugin or archive path, model path, record id, resolved actor/object, and selected variant.
- Parent chain: record part, attachment node, skeleton bone, helper node, local parent, world parent.
- Matrices: local, parent, world, inverse parent, and target-relative matrices.
- Axes: local X/Y/Z, parent X/Y/Z, world X/Y/Z, and target-space X/Y/Z.
- Bounds: local bounds, world bounds, target-space bounds, center, extent, and visible vertex count.
- State history: initial state, previous live command, current live command, runtime ack frame, screenshot hash.
- Evidence: front, left, and top views plus telemetry from the same command revision.

If any visible item has no current frame, the shot is not trusted.

## Live Iteration Loop

1. Observe current front/left/top image.
2. Parse current runtime telemetry with `scripts/nikami/fnv_live_surface_frame_audit.py`.
3. Compare previous and current origins, axes, bounds, and parent matrices.
4. Apply exactly one transform hypothesis.
5. Wait for runtime ack and a new screenshot hash.
6. Re-audit the same part and record the delta.
7. Promote only a replayable rule, never an eye-fit tweak.

## Transform Discipline

- Pick the rotation frame first. A correct angle around the wrong origin is still wrong.
- Align origins before judging a rotation result.
- Do not mix rotation and offset in the same hypothesis unless the test explicitly names both.
- For head parts, distinguish the surface helper origin, mesh bounds center, head bone origin, and skull/scalp target center.
- For rigged parts, record bind pose, current pose, skinning matrices, and weighted vertex deltas.
- For animation, record sampled time, group, controller source, blend mask, and post-skin bounds.

## Review Lanes

Treat every fix as if several specialists are reviewing the same evidence:

- Transform algebra: local, parent, world, inverse, basis, handedness, determinant, and axis dot products.
- Skinning: bind pose, inverse bind matrices, weighted vertices, per-bone consumers, and post-skin bounds.
- IK and animation: FABRIK targets, pole hints, solver error, controller source, sampled time, blend masks, and pose sanity.
- Anatomy and object affordance: head, face, mouth, hands, feet, weapons, held props, doors, containers, and creature analogs must obey believable physical constraints.
- Provenance: ESM/BSA source record, asset path, variant selection, parent node, skeleton bone, texture/material, and generated command revision.
- Rendering: visible drawables, culling, alpha state, camera matrices, pane hashes, and byte-to-pixel traceability.

No lane can silently pass by itself. A result is trusted only when the lanes agree.

## Current Sunny Finding

The old `part 3d space` line was construction-time telemetry and was stale after live edits. The runtime now emits `FNV/ESM4 telemetry: live surface frame` after live authoring applies. That line is the current source of truth for each live surface part.

For Sunny hair at the restored baseline, the current post-transform telemetry showed the hair surface origin and bounds center in head space, proving that the rotation problem is also an origin/frame alignment problem.

## Scripts

Run a latest-frame audit:

```powershell
python scripts\nikami\fnv_live_surface_frame_audit.py D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-fnv-live-authoring-20260628_095454\openmw.log --latest --pretty
```

Run only Sunny hair frames:

```powershell
python scripts\nikami\fnv_live_surface_frame_audit.py D:\code\vulkanOpenMW\proof\configs\fnv-flat-clean-fnv-live-authoring-20260628_095454\openmw.log --model-contains hairbun --latest --pretty
```

## References

- MIT 6.837 notes on coordinates and transformations: https://ocw.mit.edu/courses/6-837-computer-graphics-fall-2012/5cbb1bf32a92fad91e8ad6c37a473240_MIT6_837F12_Lec03.pdf
- UCSD CSE 167 notes on 3D transforms and rotations: https://cseweb.ucsd.edu/classes/wi18/cse167-a/lec3.pdf
- OpenSceneGraph `MatrixTransform` reference: https://podsvirov.github.io/osg/reference/openscenegraph/a00518.html
- OpenSceneGraph `Transform` reference: https://podsvirov.github.io/osg/reference/openscenegraph/a00955.html
- GECK Wiki NIF block reference: https://geckwiki.com/index.php?title=NIF_Block_Types
- SIGGRAPH quaternion course notes mirror: https://docslib.org/doc/13285084/visualizing-quaternions-course-notes-for-siggraph
