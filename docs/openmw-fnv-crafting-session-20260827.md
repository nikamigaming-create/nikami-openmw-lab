# Fallout crafting session controller — 2026-08-27

This slice adds the native/headless controller that drives a prepared Fallout
crafting catalog. It owns only page navigation, blocked-entry handling,
confirmation ordering, and the one backend request after explicit craft
confirmation.

The presentation and mutation seams are injected. A MyGUI adapter, a future
headless test harness, or another front end can implement the presenter without
putting widgets, localization strings, input policy, or toolkit dependencies in
the controller. The backend remains responsible for re-preparing and
committing the existing all-or-none transaction.

Page size and redraw limits are required policy inputs; there are no controller
defaults or embedded retail IDs. Empty/invalid policy, invalid navigation,
cancellation, blocked records, backend outcomes, and redraw exhaustion are
explicitly typed and covered by synthetic tests. No inventory mutation or UI
side effect occurs in this slice.

The later integration contract may connect this controller to native activator
actions and a localized presentation adapter after retail station activation
evidence is recorded. This commit intentionally does not add Lua, MyGUI, or
visual-parity behavior.
