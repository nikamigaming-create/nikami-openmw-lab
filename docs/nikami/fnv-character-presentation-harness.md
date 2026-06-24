# FNV Character Presentation Harness Plan

This is the contract for proving Fallout: New Vegas actors look and behave correctly in Nikami OpenMW. It covers humans, ghouls, creatures, robots, heads, teeth, mouths, eyes, hair, hats/headgear, animation, dialogue, and lip sync. It is PC-flat first, PCVR second, Android flat/VR last.

The harness must not commit retail or mod assets. Retail/mod bytes are harvested on disk into generated proof outputs, then consumed by tools and runtime probes.

## Non-Negotiable Gate

Loaded does not mean working. A row is not complete because an ESM/ESP/BSA entry was parsed or a mesh was visible once.

Every actor-facing item must be classified as exactly one of:

- `runtime-supported`
- `loaded-pending-runtime`
- `known-blocked`
- `non-runtime-support-file`
- `intentionally-excluded-with-proof`

Any unclassified skip is a failing gate.

## GECK Runtime Facts To Model

- Facial animation is driven by baked `.lip` files beside voice audio. New Vegas voice OGGs are 24 kHz, VBR, mono, named from valid dialogue, under `Data\Sound\Voice\[plugin]\[voicetype]`.
  Source: https://geckwiki.com/index.php/Facial_Animation
- Armor forms own gameplay flags and biped object slots. Armor addons provide the 3D geometry, but headwear/hat behavior must be controlled by the base armor biped flags.
  Source: https://geckwiki.com/index.php/Armor
- Head, hair, hat, mask, glasses, mouth object, and related biped slots affect what body/head parts are hidden or attached.
  Source: https://geckwiki.com/index.php/Armor
- Idle animations are picked by skeleton family, conditions, and animation group. Dialogue can trigger idles for both speaker and listener.
  Source: https://geckwiki.com/index.php/Idle_Animations

## Current Repo Starting Point

Existing pieces:

- `scripts/nikami/run-fnv-actor-mugshot-sweep.ps1` stages selected actors, captures screenshots, crops faces, builds a contact sheet, and checks `FACE CHECK` plus controller audit logs.
- `scripts/nikami/test-fnv-dialogue-voice-lip-ledger.ps1` accounts dialogue voice and `.lip` sidecars by INFO row.
- `scripts/nikami/test-fnv-lip-runtime-contract.ps1` proves the runtime has LIP loading/sampling hooks and mouth-animation consumption.
- `scripts/nikami/test-fnv-facegen-ctl-contract.ps1` and `scripts/nikami/test-fnv-egt-runtime-contract.ps1` prove FaceGen CTL/EGT parsing and partial application contracts.
- `scripts/nikami/run-fnv-flat-proof.ps1` already provides the PC-flat proof shell, screenshot gates, no-silent-skip integration, actor staging env vars, and runtime log collection.

Known gaps:

- The mugshot sweep is a sample Goodsprings human proof, not an exhaustive actor/creature/robot proof.
- Visual review is still manual/review-required; it is not a hard pass gate yet.
- Dialogue/LIP proves sidecar loading and mouth-value plumbing, but not yet per-actor mouth open/closed visual deltas across dialogue.
- Hat/headgear coverage is not exhaustive across ARMO/ARMA biped slots and race/gender head attachment variants.
- Creature and robot skeleton families need their own animation/pose families, not human-only assumptions.
- PCVR and Android must inherit only after PC-flat passes.

## Harness Architecture

### 1. Character Dependency Ledger

Add `scripts/nikami/harvest-fnv-character-presentation-ledger.ps1`.

Generated outputs go under proof, not the repo. The ledger must include:

- All `NPC_`, `CREA`, `ACHR`, `ACRE`, `RACE`, `HDPT`, `HAIR`, `EYES`, `ARMO`, `ARMA`, `BPTD`, `DIAL`, and `INFO` records.
- Actor editor ID, form ID, source plugin, placed refs, race, sex, voice type, skeleton family, base outfit, inventory outfit, head parts, eyes, hair, face texture, face normal, FaceGen morph/tint/control files.
- Creature/robot model, skeleton root, controller/KF family, sound/voice data where present.
- All referenced NIF, KF, TRI, EGM, EGT, CTL, DDS, OGG/WAV/MP3, and LIP paths.
- All armor/headwear biped flags and all armor-addon mesh paths by sex/race where applicable.
- Load-order provenance for retail, DFNV, and Komi content layers.

The ledger fails if any referenced item has no classification and no first failing gate.

### 2. Runtime Actor Sweep

Add `scripts/nikami/run-fnv-actor-presentation-sweep.ps1`, extending the mugshot sweep instead of replacing it.

For each target, the sweep stages the actor in a controlled PC-flat cell with fixed weather/time and captures:

- Neutral face closeup.
- Full body outfit view.
- Headgear/profile view.
- Idle pose.
- Walk/turn pose.
- Dialogue speaking pose where voice/LIP exists.
- Creature/robot idle and movement poses for non-human skeleton families.

Targets are sharded:

- Smoke: Doc Mitchell, Easy Pete, one settler male/female, one ghoul, one creature, one robot, one headwear case, one voiced dialogue case.
- Coverage: every unique race/gender/head-part family, every skeleton family, every biped head slot combination, every voice/LIP family.
- Exhaustive: every actor/creature/robot base plus placed refs and modded overrides.

### 3. Required Runtime Gates

An actor passes only when all applicable gates pass:

- **Resolve gate:** every source record and referenced asset resolves or is classified.
- **Face assembly gate:** `FACE CHECK` confirms head, mouth, lower teeth, upper teeth, tongue, left eye, right eye, eye texture, hair record, hair attachment, NPC-specific head parts, face texture, face normal, and tint path status.
- **FaceGen gate:** TRI/EGM/EGT/CTL files are loaded or explicitly classified, applied to the actor, and logged with the exact runtime boundary.
- **Hat/headgear gate:** ARMO biped flags and ARMA geometry agree; head/hair hiding rules are applied; hat/glasses/mask/mouth-object geometry attaches to the expected head frame; screenshots are generated for front/profile review.
- **Animation gate:** controller audit has matched controllers and `missing=0` for the selected family; an active animation group is playing; sampled pose is not bind pose/T-pose/static source pose; idle/walk/dialogue/combat-special families are separately classified.
- **Dialogue/LIP gate:** INFO row resolves to voice audio and `.lip`; audio playback starts; LIP samples vary over time; mouth morph values reach the actor; screenshots show closed/open mouth deltas; teeth/tongue remain attached through the mouth cycle.
- **Visual image gate:** screenshots are nonblank, actor is centered, crop has enough color/detail variance, marker-pink/missing texture threshold is zero, contact sheet is produced.
- **Human review gate:** contact sheet rows must be marked `PASS`, `FAIL`, or `BLOCKED` by target. Machine gates cannot claim perfect aesthetics alone.
- **Performance gate:** manual play uses light diagnostics; heavy actor/terrain/sound spam is proof-only and must not be enabled in normal test runs.

### 4. Output Contract

Each sweep writes:

- `character-presentation-ledger.json`
- `presentation-results.json`
- `presentation-summary.md`
- `presentation-contact-sheet.jpg`
- Per-target screenshots and logs.
- `human-review.csv` with target, gate status, reviewer mark, and notes.

The summary must include counts for total actors, total creatures, total robots, passed, blocked, loaded-pending-runtime, and unclassified. `unclassified` must be zero.

### 5. Promotion Order

PC-flat is the source of truth:

1. Pass smoke PC-flat.
2. Pass coverage PC-flat.
3. Pass exhaustive PC-flat in shards.
4. Re-run smoke and coverage in PCVR using the same target ledger and screenshots.
5. Only after PC-flat and PCVR are clean, port the same gates to Android flat/VR.

PCVR and Android failures do not weaken PC-flat gates. They create platform-specific failing rows with platform proof logs.

### 6. First Implementation Slice

Build the harness in this order:

1. Add the character presentation ledger generator.
2. Add the smoke target list and reuse `run-fnv-flat-proof.ps1` actor staging.
3. Extend log probes to require teeth/tongue and FaceGen status, not just head/mouth/eyes/hair.
4. Add a dialogue/LIP visual target, starting with one known voiced actor.
5. Add headgear targets that exercise Hair, Hat, EyeGlasses, Mask, and MouthObject slots.
6. Add creature/robot skeleton smoke targets.
7. Add human-review CSV gating so a run cannot be called final without review rows.
8. Wire the new sweep into no-retail proof gates.

### 7. Definition Of Done For This Harness

The harness is acceptable when a fresh repo can:

1. Harvest installed retail/mod bytes into proof output.
2. Build a complete character dependency ledger with zero unclassified skips.
3. Run PC-flat smoke and coverage sweeps.
4. Produce screenshots, contact sheets, machine gate JSON, and human review CSV.
5. Fail loudly on any missing face part, missing hat slot rule, missing animation controller, static/T-pose, absent LIP sidecar, mouth that does not move during dialogue, or unreviewed visual row.
6. Leave no retail/mod asset payload committed to the repo.
