# Fallout crafting station catalog contract

This slice freezes the recipes authored for one Fallout: New Vegas crafting
station into an immutable native snapshot. The station-to-category mapping is
supplied by `FnvCraftingStationRule`; no workbench, script, category, player,
or currency FormID is embedded in the engine.

`prepareFnvCraftingCatalog` validates the Fallout game gate, exact store
ownership, station mapping, category record, recipe grouping, skill-gate shape,
item record type, and signed quantities. Every live recipe in the mapped
category is retained. Unsupported recipes carry the exact fail-closed
`FnvCraftingPreparationError` so a later presentation layer can explain why a
recipe is blocked without silently changing authored data.

The snapshot contains authored recipe/category names, subcategory names,
skill metadata, and ingredient/output names and quantities. It does not inspect
inventory, construct outputs, open a window, execute a transaction, or claim
retail or visual parity. Station activation and presentation remain the next
native action contract.

Synthetic tests cover injected mapping, deterministic names and quantities,
retention of a condition-blocked recipe, and rejection when the mapping is not
provided. The full four-platform CI matrix is authoritative for compilation and
test execution.
