# OpenMW Fallout lab zero-magic audit — 2026-08-27

This audit covers the Fallout-specific production and test surface added to
the clean public lane through `5b22b49243`. It does not rewrite unrelated
upstream OpenMW numerics. A literal is acceptable only when it is one of:

- a named Fallout file-format fact or protocol enum;
- a structural value required by an algorithm (`0`, `1`, or a type width);
- injected policy supplied by the caller; or
- synthetic fixture data used to assert an authored record.

## Ownership checks

`components/esm4/falloutformat.hpp` is the single owner for Fallout ESM4 and
LIP layout facts used by the public slices. This includes record widths,
version gates, FormID widths, masks, LIP container flags/header sizes, and
authored LIP target count/frame rate. The LIP decoder now consumes those
shared constants instead of maintaining a second local table.

Runtime policy is injected at its boundary: collision behavior uses
`NifBullet::FalloutCollisionFilterConfig`; crafting station/category mapping
uses `FnvCraftingStationRule`; crafting session page and redraw limits use
`FnvCraftingSessionPolicy`; LIP resource limits use `LipAnimation::DecodeLimits`.
No retail FormID, UI toolkit choice, localization string, or gameplay
threshold is embedded in those contracts.

Numeric values in `apps/*_tests` are fixture bytes, authored IDs, expected
coordinates, and assertions. They are not runtime policy and are intentionally
kept beside the fixture that proves the reader or API contract.

## Review result

The touched Fallout production paths have named ownership for every value that
controls parsing, collision conversion, crafting navigation, or resource
safety. The generic movement/physics literals that appear in the diff are
existing OpenMW engine behavior or structural math; changing them would be an
unrelated engine rewrite, not Fallout data injection.

This audit is a public-history hygiene gate, not a claim of retail visual
parity. Station activation, production inventory wiring, camera projection,
animation publication, and UI presentation remain separate evidence-backed
work.

