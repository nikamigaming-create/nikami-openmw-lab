# FNV VR Harvest Plan

Goal: preserve and re-land the Fallout: New Vegas VR work after the flat baseline, without losing the current clean checkpoint.

Current protected flat checkpoint:

- Branch: `nikami/fnv-flat-baseline`
- Last proof gate commit: `8005974df0 Add FNV UI baseline proof gate`
- Proof folder: `D:\code\vulkanOpenMW\proof\fnv-ui-baseline-proof\20260619_081855`
- Result: `FNV UI baseline proof PASS`

Old FNV VR source to harvest:

- Repo: `D:\Modlists\fnv\openmw-source`
- Branch: `openmw-vr`
- Important commits:
  - `cf0a858295 Align Pip-Boy UI quads to wrist mount`
  - `7ea3db69da Route Fallout VR UI panes to Pip-Boy quads`
  - `8d45908075 Stabilize VR hand deformation`
  - `d37a607b7e Align Fallout VR hands to Pip-Boy socket`
  - `f6b3f5a8f2 fix: attach fnv vr equipped weapon`
  - `406210f5b3 feat: lock fnv vr pip-boy alignment`
  - `1246f8f610 feat: attach fnv vr hand surfaces`

Harvest order:

1. VR hands visible and data-backed.
   - Reuse FNV hand rig/source selection only where it is backed by loaded FNV assets.
   - Keep fallback and placeholder states logged.
   - Acceptance: left and right hands visible in VR proof captures, with logs naming source mesh/skeleton/bind path.

2. Wrist/on-arm HUD.
   - Reuse the old `openmw_hud_vr.layout` direction: compact HP, AP, AMMO, weapon slots, and compass heading.
   - Preserve the flat HUD data feeds already passing in `hud.png`.
   - Acceptance: wrist HUD shows HP/AP/AMMO plus compass heading in VR proof capture.

3. Pip-Boy wrist panels.
   - Re-land routing for `InventoryWindow`, `StatsWindow`, `MapWindow`, and `SpellWindow` onto wrist/Pip-Boy quads.
   - Preserve the flat Pip-Boy analog page mapping:
     - MAP = `MapWindow`
     - ITEMS = `InventoryWindow`
     - DATA = `SpellWindow`
     - STATUS = `StatsWindow`
   - Acceptance: each panel can be focused/clicked in VR and logs its active pane.

4. Equipped weapon attach.
   - Harvest weapon attach only after hands are stable.
   - Acceptance: equipped FNV weapon attaches to the correct hand/socket and has proof logs.

Known files to compare first:

- `apps/openmw/mwgui/hud.cpp`
- `apps/openmw/mwgui/hud.hpp`
- `files/data/mygui/openmw_hud_vr.layout`
- `apps/openmw/mwvr/vrgui.cpp`
- `apps/openmw/mwvr/vranimation.cpp`
- `apps/openmw/mwvr/vrinputmanager.cpp`
- `apps/openmw/mwvr/vrpointer.cpp`
- `apps/openmw/mwvr/vrpointer.hpp`
- `files/data/scripts/omw/vr/ui/common.lua`
- `files/data/scripts/omw/vr/ui/player.lua`
- `files/data/scripts/omw/vr/inputs/player.lua`

Do not start Vulkan until this VR checkpoint has its own proof gate or has been explicitly parked.
