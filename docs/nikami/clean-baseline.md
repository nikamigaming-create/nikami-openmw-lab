# Nikami Clean Baseline

The clean baseline is the first gate for this lab. It must run stock OpenMW with vanilla Morrowind content before any FNV, VR, Team Beef, Mads VR, or Vulkan work is layered in.

## Build

```powershell
$env:NIKAMI_VCPKG_ROOT = "<path-to-vcpkg>"
.\scripts\nikami\build-clean-openmw.ps1
```

This configures and builds `build-clean/Release/openmw.exe` with OpenMW only:

- `BUILD_OPENMW=ON`
- `BUILD_OPENMW_VR=OFF`
- launcher, wizard, OpenCS, tools, and tests off
- local vcpkg from `NIKAMI_VCPKG_ROOT` or `-VcpkgRoot`

The helper also copies the MyGUI runtime DLL and OSG plugin directory into the OpenMW output directory, because this local vcpkg layout stores them outside the root `bin` directory.

## Run Flat Morrowind

```powershell
$env:NIKAMI_MORROWIND_DATA = "<path-to-Morrowind-Data-Files>"
.\scripts\nikami\run-morrowind-flat.ps1 -NoSound
```

The script writes clean proof state under a repo-adjacent `proof/` directory by default, or under `-ProofRoot` when supplied.

## Pass Criteria

- The chargen ship scene reaches the first UI prompt.
- The player/NPC body is visible.
- The log does not contain `FNV/ESM4 diag` lines.
- The log uses only the Morrowind, Tribunal, and Bloodmoon content files for this proof.

Only after this gate passes should FNV flat work be layered back in.
