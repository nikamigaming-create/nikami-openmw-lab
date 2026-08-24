# OpenMW lab zero-magic audit

This audit covers the Nikami OpenMW lab surface from the official OpenMW base
(`e47ad7782c6a3204f1bae0bcb42356e467319168`) through the audit branch. It does
not attempt to rewrite unrelated upstream OpenMW code. Numeric literals in
generic engine code are not automatically game policy; the review question is
whether a value controls Fallout behavior, an on-disk layout, or an API
contract.

## Value ownership

* Fallout ESM4 record widths, version gates, masks, and FormID widths are
  centralized in `components/esm4/falloutformat.hpp`. These are immutable file
  format facts. Moving them to a user config would make the reader accept a
  different binary schema and would be unsafe.
* Fallout NIF/Havok conversion boundaries are named in
  `components/nifbullet/falloutphysicsconstants.hpp`. The unit scale, material
  mask, traversal limit, tolerance, matrix dimensions, and triangle/shape
  boundaries are documented constants rather than anonymous literals.
* Collision lookup sentinels and the compatibility primary-body ordinal are
  API contracts. They are named in `components/resource/bulletshape.hpp` and
  `apps/openmw/mwphysics/constants.hpp`; they are not retail policy.
* Fallout collision-filter policy is injected through
  `NifBullet::FalloutCollisionFilterConfig`. The review fixture is committed at
  `apps/components_tests/data/fallout_collision_filter.cfg`; the production
  evaluator contains no anonymous retail lookup table or default policy.
* Synthetic ESM4 test payloads are test data. Their record sizes and plugin
  versions use the same named schema/version constants as the reader. Expected
  values remain in tests because they are assertions about the fixture, not
  runtime behavior.

## Review invariants

1. A loader must reject a Fallout payload when the plugin version or exact
   schema width is unsupported, and must skip the subrecord to preserve reader
   alignment.
2. A caller that has no injected collision policy, or addresses a layer outside
   the injected tables, receives `std::nullopt` rather than an invented answer.
3. A collision body, shape part, or triangle that is absent is represented by a
   named API sentinel; no caller relies on an unexplained `-1` or `0`.
4. Private retail binaries and observation artifacts remain outside the public
   repository. The fixture records only the reviewed contract needed by the
   deterministic unit test.

## Gate

The branch is ready for the normal upstream-style sequence only after the
component and OpenMW test targets compile, the deterministic tests pass, and
the public topic is fast-forwarded into `main`. A clean diff is necessary but
is not a substitute for those build and CI gates.
