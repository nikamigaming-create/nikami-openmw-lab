# FNV Flat 001: Easy Pete at Goodsprings

Stable FNV flat-render baseline for the Nikami OpenMW lab.

This pin proves:

- Fallout: New Vegas content can boot through the lab OpenMW build.
- Goodsprings exterior and the Prospector Saloon render.
- The real placed actor ref `EasyPeteRef` resolves to base actor `GSEasyPete`.
- The proof harness can force the target cell, stage the actor, align the camera, and capture native screenshots/logs.
- FNV NPCs have enough actor/class/render plumbing to appear as visible characters in the flat renderer.

Known limits:

- This is a flat-render proof, not VR.
- This is not final FNV character animation or FaceGen parity.
- Easy Pete may appear in a neutral/T-pose baseline pose until the fuller FNV animation path is layered in.

## Proof Command

Set these paths for the local machine:

```powershell
$env:NIKAMI_FNV_DATA = "<Fallout New Vegas Data path>"
$env:NIKAMI_FNV_CONFIG_DATA = "<optional generated FNV config data path>"
$env:NIKAMI_VCPKG_ROOT = "<vcpkg root>"
$env:NIKAMI_EXTRA_OSG_PLUGIN_DIR = "<optional OSG plugin directory>"
```

Then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\nikami\run-fnv-flat-proof.ps1 `
  -FnvData $env:NIKAMI_FNV_DATA `
  -FnvConfigData $env:NIKAMI_FNV_CONFIG_DATA `
  -VcpkgRoot $env:NIKAMI_VCPKG_ROOT `
  -ExtraOsgPluginDir $env:NIKAMI_EXTRA_OSG_PLUGIN_DIR `
  -RunSeconds 24 `
  -ScreenshotFrames "760,920,1080" `
  -BootstrapCell "FormId:0x10daeb9" `
  -BootstrapX -67480 `
  -BootstrapY 2200 `
  -BootstrapZ 8425 `
  -BootstrapRotX 0 `
  -BootstrapRotZ 1.5708 `
  -ActorTarget GSEasyPete `
  -StageActor `
  -ActorFrame 620 `
  -ActorStageX -67480 `
  -ActorStageY 2200 `
  -ActorStageZ 8425 `
  -ActorStageRotZ 1.5708 `
  -ActorViewOffsetX 34 `
  -ActorViewOffsetY 0 `
  -ActorViewOffsetZ 102 `
  -ActorViewTargetZ 116 `
  -NoSound
```

## Expected Log Markers

The run is a valid pin when `openmw.log` includes:

- `loaded placed actor ref FormId:0x1104c80 editor 'EasyPeteRef'`
- `assembling minimal FONV NPC parts for GSEasyPete`
- `active-cell actor match target="GSEasyPete"`
- `staged actor target="GSEasyPete"`
- `aligned player camera to actor target="GSEasyPete"`

The proof runner writes screenshots, summary, stdout, stderr, and OpenMW logs under the ignored proof directory.
