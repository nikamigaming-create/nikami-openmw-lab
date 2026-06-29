# FNV Parity Truth Rig Contract

This contract is the durable operating rule for Fallout New Vegas data parity work in this repository.

## Purpose

The rig exists to prevent guesswork. A visual claim is not trusted unless it comes from the real OpenMW runtime, the exact target is known, and the image is tied to matching telemetry from the same run.

This is an FNV-wide asset contract. A named actor may be a proving fixture, but no rule may be implemented or promoted as a one-actor style. The same contract applies to every FNV actor, creature, body part, equipped item, prop, door, container, projectile, and animated asset.

## Non-Negotiables

1. Use one OpenMW render worker for live iteration.
2. Use the website as the judge surface.
3. Show one current combined image at the top of the website.
4. The combined image must use separate front, left, and top views.
5. The image must be paired with telemetry from the same actor, target part, frame, and command revision.
6. No legacy pass condition can promote a result by itself.
7. Missing image, missing actor match, missing camera data, missing matrix data, or stale telemetry means the result is not trusted.
8. Live tweaks are recipes, not fixes, until replay proves them.
9. Code promotion requires the smallest proven rule, not a one-actor hack.
10. Every drawn runtime object or picked data part must expose an explicit 3D frame: origin, X/Y/Z axes, parent frame, local/world matrices, bounds, and a renderable axis overlay.
11. If a part is visible but its frame is unknown, stale, or not tied to the current command revision, the result is not trusted.
12. Every inspection run is an alignment job with explicit actor/object, source part, target part, optional context parts, view profile, overlay toggles, command revision, image artifact, and telemetry artifact.
13. A part-to-part alignment must show both frames at once. If aligning a hand to a cuff, show hand and cuff. If aligning a cuff to an elbow, show cuff, elbow, shoulder, and torso as requested context.

## Required Judge Targets

The rig must support targetable views for:

- `hair`
- `face`
- `torso`
- `hands`
- `feet`
- `full-body`
- `creature`
- `weapon`
- `animated-prop`
- `door`
- `container`

Each target must define camera framing, expected node families, and required telemetry before it can be used as a parity gate.

Source and target axes must use distinct color families so the operator can tell which frame is moving. Context parts must be selectable without changing the rig or the target actor.

## Required Evidence

Every current judge artifact must identify:

- Requested target and resolved runtime actor/object.
- Alignment job id, source part id/model, target part id/model, and context part ids/models.
- Base form, editor id, kind, race or creature type when available.
- Target part name, model path, variant, visible/hidden geometry counts.
- Parent node, skeleton bone, and attach reason.
- Local, parent, world, and head/bone-relative transforms.
- Local, world, and target-space bounds.
- Pivot, offset, rotation, and basis axes.
- Animation group, source, time, and sampled frame.
- IK solver state when IK is involved.
- Front, left, and top camera matrices.
- Screenshot or rendered image hashes for each pane.
- Command id, command time, runtime ack frame, and telemetry frame.
- Per-drawn-object axis records for every visible target part, not just the currently failing asset.

## First Three Iterations

The rig is not trusted until these pass as evidence, not just as screenshots:

1. First fixture head/part alignment only: no body, no blood, no unrelated rig changes.
2. Same fixture head, hands, and full-body matrices.
3. One random non-Pete human NPC using the same workflow.
4. One non-hair target such as torso, hands, feet, weapon, creature part, or animated prop.

The first fixture must be finished before promotion, but the random non-Pete actor must be exercised immediately after to prove the path is generic.

## Promotion Rule

A live edit can be promoted to C++ only after:

1. The current combined image and telemetry agree.
2. A saved live recipe can replay the result in a clean run.
3. The rule succeeds on at least the required iteration set for that target class.
4. The code change explains the data reason: transform basis, parent space, variant selection, animation controller, skinning, IK, or asset binding.
5. The focused runtime contract passes.

## Drift Rule

If work starts to diverge from this contract, stop and restore the rig first. Do not continue asset fixes on a broken judge surface.
