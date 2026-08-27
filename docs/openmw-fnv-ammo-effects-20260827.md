# Fallout AMEF operation contract — 2026-08-27

This slice adds the pure gameplay boundary for Fallout: New Vegas `AMEF`
records. `applyFalloutAmmoEffects` receives the already-resolved `AMMO.RCIL`
records in authored order and applies only the requested effect type. The
caller still owns record lookup, ammunition ownership, and combat state.

The contract is deliberately fail-closed: null records, non-finite authored
values, unknown operations, and non-finite results return an explicit failure
instead of silently inheriting Morrowind behavior or inventing a fallback.
There are no engine settings, UI dependencies, Lua calls, or hard-coded
gameplay thresholds in this slice. Weapon wear and damage integration remain a
separate runtime gate because they require the corresponding retail telemetry
and state-ownership review.

Coverage is in `apps/openmw_tests/mwmechanics/testfalloutammoeffects.cpp`:
ordered add/multiply/subtract behavior, ignored effect types, empty input, and
all fail-closed validation paths are exercised.
