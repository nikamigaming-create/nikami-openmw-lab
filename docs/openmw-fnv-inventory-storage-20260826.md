# ESM4 inventory storage contract

This slice makes the typed Fallout inventory records usable by the existing
`ContainerStore` ownership path. It registers all eleven ESM4 item kinds that
can occur in Fallout inventories—ammunition, armor, misc, weapons, potions,
books, clothing, ingredients, item mods, keys, and lights—for storage,
iteration, lookup, weight calculation, and inventory save-state loading.

The implementation extends the existing `CellRefList`/iterator API and keeps
the ESM4 FormID and count on the same live reference object used by the native
OpenMW container code. It does not add a second inventory model, clone a
store, or route inventory behavior through Lua/MyGUI. No gameplay IDs are
embedded; the bit masks are named type-category constants alongside the
existing OpenMW container masks.

`testcontainerstoreesm4.cpp` statically checks every registered kind and
exercises insertion, iteration, type identity, and aggregate counts over all
eleven record families. Actor inventory exposure and crafting mutation remain
separate slices because the current ESM4 NPC class still owns an explicit
“InventoryStore not yet supported” boundary.
