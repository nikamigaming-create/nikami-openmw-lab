# Fallout ESM4 item condition contract

This slice publishes authored maximum condition for Fallout ESM4 weapons and
armor through OpenMW's existing native `MWWorld::Class` item-health API.

## Contract

- `ESM4::Weapon::mData.health` and `ESM4::Armor::mData.health` are the sole
  source of maximum condition.
- A weapon or armor reports item health only when its authored maximum is
  positive.
- The public `int` API is saturated at `std::numeric_limits<int>::max()` so a
  malformed or future record cannot wrap into a negative condition.
- Other ESM4 records keep the base class behavior and do not acquire synthetic
  durability.
- Current condition remains in the existing `CellRef` charge state, matching
  the native OpenMW item-health contract.

## Scope boundary

This is a data-to-native-API slice. It does not add quest commands, actor
inventory ownership, repair UI, Lua bindings, or a combat bridge. Those require
separate evidence-backed slices once ESM4 actor/container ownership is ready.

## Evidence and tests

`apps/openmw_tests/mwworld/testcontainerstoreesm4.cpp` constructs typed ESM4
weapon, armor, and miscellaneous records and verifies the class contract. The
full OpenMW CI matrix remains authoritative for compilation and test execution.
