# OpenMW Flat Proof

Proof status: passed.

The clean OpenMW flat baseline builds from the Nikami public repo and reaches the Morrowind chargen name dialog with the prisoner body visible.

## Scope

- OpenMW flat desktop path only.
- Stock Morrowind, Tribunal, and Bloodmoon content only.
- No FNV, VR, Vulkan, or `vsgopenmw` code in this proof.
- Build and proof outputs remain ignored by Git.

## Local Inputs

The helper scripts take local paths from parameters or environment variables:

- `NIKAMI_VCPKG_ROOT` or `-VcpkgRoot`
- `NIKAMI_MORROWIND_DATA` or `-MorrowindData`
- `NIKAMI_EXTRA_OSG_PLUGIN_DIR` or `-ExtraOsgPluginDir` when a local OpenSceneGraph plugin directory is needed

## Verified Behavior

- Clean Release build produced `openmw.exe`.
- Runtime MyGUI DLL and OSG plugin directory were present.
- OpenMW launched into `Imperial Prison Ship`.
- Loaded content was limited to `builtin.omwscripts`, `Morrowind.esm`, `Tribunal.esm`, and `Bloodmoon.esm`.
- Visual inspection confirmed the name dialog and visible body baseline.
