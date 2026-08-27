# Fallout special inventory records

Fallout: New Vegas stores casino chips, Caravan cards, and Caravan money under
the `CHIP`, `CCRD`, and `CMNY` ESM4 record tags. They are physical inventory
objects and share the native `MiscItem` store, but their authored record tag is
retained on the typed record for later inventory and crafting policy.

The loader accepts these aliases only for the Fallout game profile, decodes
their authored value payloads, and rejects alias-only fields on ordinary MISC
records. `ESMStore` indexes the aliases as native `REC_MISC4` inventory
records, so existing `ContainerStore` lookup and persistence paths remain the
single owner. No Lua/MyGUI adapter, product ID, or private asset is added.

`testesm4specialinventorystore.cpp` covers FNV alias indexing and the
non-Fallout fail-closed gate with synthetic ESM4 records.
