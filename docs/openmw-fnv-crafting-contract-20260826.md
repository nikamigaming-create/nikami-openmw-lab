# Fallout recipe transaction contract

This slice defines the native, headless boundary for Fallout: New Vegas
recipe execution. It is intentionally narrower than a crafting UI or station
activation feature: a later runtime adapter owns `ContainerStore` mutation and
the presentation layer owns menus. The contract keeps those concerns out of
the ESM4 loader and makes every policy that is not authored in Fallout data an
explicit input.

## Authored inputs

`FnvCraftingTransactionSource` accepts the active `ESMStore`, station
`ACTI`, station and recipe `RCCT` categories, and the `RCPE` recipe. The
planner validates exact store identity, deletion flags, category/subcategory
links, condition absence, supported item record types, authored quantities,
and the Fallout skill actor-value range. It preserves each authored output
row and aggregates duplicate ingredient rows for consumption.

Station base/script/category mappings are supplied as
`FnvCraftingStationRule` values. Currency is recognized by the typed ESM4
record kind; no runtime FormID is embedded in the implementation. Skill values
come from the injected `FnvCraftingSkillProvider`, so player-save and
actor-value policy remain separate contracts.

## Commit boundary

Preparation snapshots all ingredient/output counts. Commit consumes the
move-only plan exactly once, rechecks actor ownership and those counts, then
calls one `FnvCraftingInventory::apply` operation. An inventory adapter must
apply all deltas or none of them; it must not clone a `ContainerStore` to fake
transactionality. Stale plans and failed adapter mutations are reported
without silently continuing.

The production `ContainerStore` adapter and station action are deliberately a
follow-up slice. Until that adapter is present, this contract is not presented
as user-visible crafting or as retail parity.

## Verification

`apps/openmw_tests/mwworld/testfnvcraftingruntime.cpp` covers injected station
and skill policies, typed/conditional recipe rejection before mutation,
snapshot revalidation, one-shot plan ownership, and the all-or-none mutation
boundary. The focused source and test translation units compile with C++20
and warnings-as-errors on MSVC.
