# FNV UI Baseline Proof

This is the judge-ready gate for the flat Fallout: New Vegas baseline.

Run it from the repo root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\nikami\run-fnv-ui-baseline-proof.ps1 -BuildDir build-clean -Configuration Release -FnvData "d:\SteamLibrary\steamapps\common\Fallout New Vegas\Data" -VcpkgRoot D:\code\c\FMODS\vcpkg -ExtraOsgPluginDir D:\code\vulkanOpenMW\nikami-openmw-lab\build-clean\Release\osgPlugins-3.6.5 -NoSound
```

The proof is valid only when the script ends with:

```text
FNV UI baseline proof PASS
```

The script creates a timestamped folder under:

```text
D:\code\vulkanOpenMW\proof\fnv-ui-baseline-proof
```

Judge these images:

- `hud.png`: in-game Fallout HUD with HP, AP, ammo, compass, crosshair, and world content.
- `status.png`: STATUS page with FNV stats, SPECIAL, skills, and paper-doll summary.
- `items.png`: ITEMS page with paper doll, tabs, and FNV proof inventory.
- `map.png`: Mojave map view.
- `data.png`: DATA placeholder page for quests, notes, radio, perks, and alternate ammo contracts.

The proof folder also traps the logs:

- `hud_openmw.log`
- `status_openmw.log`
- `items_openmw.log`
- `map_openmw.log`
- `data_openmw.log`

Recheck an existing proof folder:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\nikami\test-fnv-ui-baseline-proof.ps1 -ProofDir D:\code\vulkanOpenMW\proof\fnv-ui-baseline-proof\<timestamp>
```

Current known tolerance: missing default OpenMW sky/base animation marker meshes are allowed only when the validator classifies them as known tolerated mesh lines. Real blockers such as fatal errors, unknown globals, empty NPC classes, and marker errors fail the gate.
