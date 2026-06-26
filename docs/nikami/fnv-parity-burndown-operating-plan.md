# FNV Parity Burn-Down Operating Plan

This is the working plan for Fallout New Vegas parity in OpenMW. The target is not "loads without crashing." The target is runtime gameplay proof for actors, creatures, robots, props, weapons, projectiles, sounds, UI, quests, perks, traits, scripts, records, configs, and every asset family needed for a real New Vegas run.

## Priority Order

1. PC flat OpenMW/FNV runtime.
2. PCVR runtime after PC-flat gates are stable.
3. Android flat and Android VR after PC and PCVR gates are stable.

Android-specific code cannot drive PC character, animation, skin, headgear, or gameplay assumptions.

## Classification Rule

Every ESM, ESP, BSA entry, INI setting, record type, asset type, script, quest, perk, trait, projectile, sound, animation, UI file, runtime config, actor, creature, robot, prop, and runtime state must be classified as exactly one of:

- `runtime-supported`
- `loaded-pending-runtime`
- `known-blocked`
- `non-runtime-support-file`
- `intentionally-excluded-with-proof`

Any unclassified item is a failing gate. Any empty row selection is a failing gate. `loaded-pending-runtime` means accounted and queued for runtime proof; it never means visually correct or gameplay complete.

## Actor-First Burn-Down

Actors are the first visible parity surface because they exercise the highest number of systems at once: records, race, FaceGen, skin tint, wrinkles, eyes, mouth, teeth, tongue, hair, beard, brows, hats, outfits, weapons, projectiles, hands, animations, dialogue, LIP, voice, sounds, and AI state.

The actor burn-down source of truth is generated from harvested/derived metadata only:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-actor-parity-burndown.ps1 -RegeneratePlan -RequirePass
```

It writes generated proof metadata under the repo-adjacent proof root, normally `<repo parent>/proof/fnv-actor-parity-burndown/<stamp>/`, and does not commit retail payload bytes.

Each actor row can then be selected and routed into the runtime viewer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-actor-parity-burndown.ps1 -BurnDownJson <actor-parity-burndown.json> -DryRun -Phase weapon -Gate projectile-muzzle-sound -MaxRows 1 -RequirePass
```

Dry-runs prove row selection, commands, filters, and no-retail metadata without launching the runtime. They first revalidate that the input burn-down matrix is `PASS`, has no invalid classifications, has no unclassified rows, and has no missing runtime commands. Non-dry runs must launch `run-fnv-character-viewer.ps1`, capture viewer artifacts, and fail if the child runtime manifest fails.

Each selected row writes a `nikami-fnv-actor-row-gate-audit-v1` object. The audit separates process status from runtime support: a viewer phase can pass while the selected gate remains `loaded-pending-runtime` if the child manifest lacks exact gate/state evidence such as screenshots, mouth runtime evidence, projectile evidence, weapon lines, material evidence, face drawables, animation playback, creature evidence, attachment bounds, or runtime part audits.

Named actor rows must be generated with source-plan filters instead of front-slicing the plan:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-actor-parity-burndown.ps1 -PlanJson <viewer-batch-plan.json> -OutDir <proof-out> -ActorKind npc -Target EasyPeteRef -RequirePass
```

That command filters the full source plan before row expansion, producing the placed Easy Pete row ladder directly. The first real Easy Pete face run proved target FACE CHECK and screenshots with capture frames `120,180,240,300`; it still correctly stayed `loaded-pending-runtime` because exact FNV FaceGen/skin synthesis and hand finger articulation remain pending runtime work.

After rows have run, generate the row coverage report:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-actor-row-audit-report.ps1 -BurnDownJson <actor-parity-burndown.json>
```

The report writes `nikami-fnv-actor-row-audit-report-v1` JSON/HTML/Markdown/CSV and is the human/bot burn-down view for actor rows. It counts runtime-supported rows, loaded-pending rows, failed rows, unrun runtime-required rows, source-blocked rows, intentionally excluded rows, and missing evidence kinds.

## Runtime Proof Loop

For each selected row:

1. Select the exact actor/creature/robot row from the generated matrix.
2. Record target, placed target, base target, phase, gate, runtime states, classification, first failing gate, and command.
3. Run or dry-run the standalone viewer.
4. Write generated JSON, HTML, Markdown, selected-row JSON, and child viewer links.
5. Fail on missing row, failed child viewer, missing child manifest, invalid classification, or unclassified data.
6. Treat broad phase-level viewer success as pending exact gate-state audit unless the child proof explicitly covers the selected gate and runtime states.
7. Only after runtime proof improves, promote generic parser/runtime logic into C++.

The proof output is data for humans and bots. The repo stores tools and contracts; generated screenshots, logs, manifests, and harvested asset references stay outside Git.

## Actor Gates

The actor gate list starts with:

- Body, hands, fingers, skinning, and skeleton anchor proof.
- Face skin tone, age, wrinkles, FaceGen morphs, EGT/CTL/normal/bump handling.
- Eyes, mouth, teeth, tongue, LIP, voice, and dialogue animation.
- Hair, beard, brows, headgear, hat/hair occlusion, biped slot composition.
- Outfit, armor-addon, weapon prop, muzzle, projectile, sound, reload and attack states.
- Idle, walk, run, turn, talk, attack, hit, death, and pose transitions.
- Creature and robot model, bodypart, skeleton, KF/controller, idle/walk/run/attack/hit/death families.

Easy Pete, Sunny Smiles, representative Goodsprings actors, one ghoul, one robot, one creature, and headwear/weapon/dialogue edge cases are smoke fixtures. The final pass is every actor row, then every non-actor asset row.

## Promotion Rule

No screenshot-only guess becomes runtime code. A fix can be promoted only when the proof says:

- Which bytes/records/config produced the input.
- Which transform/material/animation/sound/runtime state changed.
- Which row and gate moved from pending/blocked to supported.
- Which generated artifacts prove it.
- Which no-retail gates passed.

If proof cannot identify the data flow from disk to runtime output, the fix remains pending and does not become a generic rule.
