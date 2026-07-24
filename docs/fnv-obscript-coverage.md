# Fallout: New Vegas retail script coverage

Rendering a New Vegas cell is not proof that its gameplay works. Retail quests,
dialogue, AI, combat, inventory, and world interactions depend on ObScript
commands and events. This project therefore tracks those dependencies against
the scripts extracted locally from the ten official FNV plugins.

The aggregate was produced with
[BarryThePirate/obscript-pipeline](https://gitlab.com/BarryThePirate/obscript-pipeline).
It covers 3,708 scripts and 165,335 lines from `FalloutNV.esm` plus all official
DLC. No retail script source or Bethesda asset is stored in this repository.

`files/data/openmw_aux/obscript/fnv_retail_coverage.lua` is the generated,
machine-readable result. The current baseline is:

| Kind | In corpus | Implemented | Required gap | Explicitly unsupported |
| --- | ---: | ---: | ---: | ---: |
| Commands | 166 | 40 | 8 | 118 |
| Events | 23 | 9 | 5 | 9 |

The required command gaps are:

- dialogue: `Say`, `SayTo`, `SayToDone`, `StartConversation`
- actor packages: `AddScriptPackage`, `RemoveScriptPackage`, `EVP`, `ResetAI`

The required event gaps are `OnTrigger`, `OnStartCombat`, `OnCombatEnd`,
`OnPackageStart`, and `OnPackageDone`.

These are implementation gaps, not passing features. Unsupported commands
remain visible through the runtime's first-use diagnostics and coverage
telemetry. The component test verifies that every generated `implemented`
entry is actually bound by the runtime and that every implemented event has an
authoritative engine event path.

Regenerate the report after extracting the official scripts locally:

```powershell
python tools/fnv_obscript_coverage.py `
  C:\path\to\pipeline-output\roadmap.md `
  files\data\openmw_aux\obscript\bindings.lua `
  files\data\openmw_aux\obscript\runtime.lua `
  files\data\openmw_aux\obscript\fnv_retail_coverage.lua
```

The gameplay target is to reduce the required-gap count to zero and then prove
the affected behavior from a native save in the actual Goodsprings runtime.
