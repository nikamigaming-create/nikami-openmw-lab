# Nikami Roadmap

Nikami OpenMW Lab is a clean OpenMW-based lab for staged rendering and VR experiments.

## Current Baseline

- Keep `main` clean and buildable.
- Start from a flat OpenMW/Morrowind baseline before adding other game data or VR paths.
- Store local proof output, generated configs, and build trees outside Git-tracked files.
- Treat `vsgopenmw` as a simple reference example only.

## Staging Order

1. OpenMW flat Morrowind baseline.
2. FNV flat baseline.
3. FNV VR smoke baseline.
4. Clean branch break before renderer work.
5. Vulkan work in the lab repo after the flat and VR baselines are proven.

## Commit Rules

- Keep commits small and explainable.
- Do not commit build outputs, proof logs, generated runtime configs, game assets, or private machine paths.
- Put Nikami-specific notes under `docs/` or `scripts/nikami/`.
- Avoid rebranding upstream OpenMW files unless a change is needed for the lab.
