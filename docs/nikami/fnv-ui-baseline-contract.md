# FNV UI Baseline Contract

Goal: make Fallout: New Vegas feel first-class inside OpenMW before Vulkan or VR work resumes.

This checkpoint is valid only when it works in-game and the proof folder includes screenshots plus `openmw.log`.

## Gameplay HUD

The flat gameplay HUD is responsible for live, moment-to-moment play:

- HP and AP bars with numeric readouts.
- Compass heading and minimap/map access.
- Equipped weapon icon/status.
- Ammo readout from FNV ammo records or the current proof inventory.
- Crosshair, activate prompts, enemy health, and temporary location/weapon text.

The HUD must not show Morrowind-only concepts for FNV, such as fatigue as a third primary bar or magic as the primary action surface.

## Pip-Boy Analog

Until the real Pip-Boy shell exists, OpenMW's native windows are the Pip-Boy analog:

- `Stats` is STATUS: SPECIAL, skills, HP/AP/WG, paper-doll summary.
- `Inventory` is ITEMS: paper doll, weapons, apparel, aid, misc, ammo.
- `Map` is DATA / MAP: Mojave map first, local map as it becomes reliable.
- `Magic/Spell` is a temporary DATA subpage for perks, traits, effects, notes, radio, and alternate ammo placeholders.
- Journal/dialogue pages become QUESTS and NOTES once FNV quest records are wired.

Every visible value must be one of:

- Real FNV-backed data.
- A deliberate, logged placeholder with the missing backing system named.

## Current Acceptance Gate

A judge-ready proof must include screenshots showing:

- Gameplay HUD with HP/AP, AMMO, compass, crosshair, and world content.
- Pip-Boy analog STATUS / paper doll.
- ITEMS list with FNV proof inventory.
- DATA / MAP showing Mojave map mode.
- Log markers for HUD readouts, inventory panes, stats panes, map binding, alternate ammo/readout state, and any placeholder surfaces.

If any one of those is missing, the checkpoint is not ready.
