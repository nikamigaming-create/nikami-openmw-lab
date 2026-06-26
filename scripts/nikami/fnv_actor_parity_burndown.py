#!/usr/bin/env python3
"""Generate an actor-first FNV parity burn-down matrix from the viewer batch plan."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any


ALLOWED_CLASSIFICATIONS = {
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof",
}

NPC_STATES = [
    "neutral",
    "idle",
    "walk",
    "run",
    "turn",
    "talk",
    "mouth-open",
    "weapon-draw",
    "attack",
    "reload",
    "projectile-fire",
    "hit",
    "death",
    "sleep",
    "sit",
]

CREATURE_STATES = [
    "neutral",
    "idle",
    "walk",
    "run",
    "turn",
    "attack",
    "projectile-fire",
    "hit",
    "death",
]

PHASE_GATES: dict[str, list[dict[str, Any]]] = {
    "body": [
        {"gate": "actor-base-record", "components": ["actor-base-record"], "states": ["neutral"]},
        {"gate": "race-body-skeleton", "components": ["npc-race", "npc-model"], "states": ["neutral"]},
        {"gate": "skin-material-tone", "components": ["npc-race", "facegen-symmetric-texture"], "states": ["neutral"]},
        {"gate": "full-body-screenshot", "components": ["actor-base-record"], "states": ["neutral", "turn"]},
    ],
    "head": [
        {"gate": "headpart-stack", "components": ["npc-headpart", "npc-race"], "states": ["neutral"]},
        {"gate": "facegen-morph-targets", "components": ["facegen-symmetric-shape", "facegen-asymmetric-shape"], "states": ["neutral"]},
        {"gate": "head-transform-pivot", "components": ["npc-headpart"], "states": ["neutral", "turn"]},
    ],
    "face": [
        {"gate": "face-skin-tone-wrinkles", "components": ["facegen-symmetric-texture", "npc-race"], "states": ["neutral"]},
        {"gate": "eyes-mouth-teeth-tongue", "components": ["eyes", "npc-race"], "states": ["neutral", "mouth-open"]},
        {"gate": "face-closeup-screenshot", "components": ["actor-base-record"], "states": ["neutral", "talk"]},
    ],
    "hair": [
        {"gate": "hair-beard-brow", "components": ["hair", "npc-headpart"], "states": ["neutral"]},
        {"gate": "hair-under-headgear-policy", "components": ["hair", "equipment-armor", "equipment-armor-addon"], "states": ["neutral"]},
    ],
    "equipment": [
        {"gate": "outfit-inventory-resolution", "components": ["default-outfit", "sleep-outfit", "inventory-item"], "states": ["neutral"]},
        {"gate": "armor-addon-geometry", "components": ["equipment-armor", "equipment-armor-addon", "equipment-clothing"], "states": ["neutral", "turn"]},
        {"gate": "biped-slot-visibility", "components": ["equipment-armor", "equipment-armor-addon"], "states": ["neutral"]},
    ],
    "weapon": [
        {"gate": "weapon-prop-attachment", "components": ["equipment-weapon"], "states": ["weapon-draw", "attack"]},
        {"gate": "weapon-animation-family", "components": ["equipment-weapon", "animation-kffz"], "states": ["attack", "reload"]},
        {"gate": "projectile-muzzle-sound", "components": ["equipment-weapon"], "states": ["projectile-fire"]},
        {"gate": "hand-weapon-transform", "components": ["equipment-weapon"], "states": ["weapon-draw", "turn"]},
    ],
    "headgear": [
        {"gate": "headgear-slot-composition", "components": ["equipment-armor", "equipment-armor-addon"], "states": ["neutral"]},
        {"gate": "hat-hair-occlusion", "components": ["equipment-armor", "equipment-armor-addon", "hair"], "states": ["neutral", "turn"]},
        {"gate": "headgear-transform-pivot", "components": ["equipment-armor-addon"], "states": ["neutral", "turn"]},
    ],
    "talk": [
        {"gate": "dialogue-info-selection", "components": ["voice-type"], "states": ["talk"]},
        {"gate": "voice-lip-sidecar", "components": ["voice-type"], "states": ["talk", "mouth-open"]},
        {"gate": "mouth-teeth-lip-sync", "components": ["voice-type", "npc-race"], "states": ["talk", "mouth-open"]},
    ],
    "creature-model": [
        {"gate": "creature-model-root", "components": ["creature-model", "creature-template"], "states": ["neutral"]},
        {"gate": "creature-bounds-camera", "components": ["creature-model"], "states": ["neutral", "turn"]},
    ],
    "creature-body": [
        {"gate": "creature-bodypart-data", "components": ["bodypart-data", "creature-body-nif"], "states": ["neutral"]},
        {"gate": "creature-material-texture", "components": ["creature-body-nif"], "states": ["neutral"]},
    ],
    "creature-animation": [
        {"gate": "creature-idle-walk-run", "components": ["animation-kffz", "animation-locomotion-fallback"], "states": ["idle", "walk", "run"]},
        {"gate": "creature-attack-hit-death", "components": ["animation-kffz"], "states": ["attack", "hit", "death"]},
    ],
    "creature-full": [
        {"gate": "creature-full-runtime-view", "components": ["creature-model", "creature-body-nif"], "states": CREATURE_STATES},
    ],
}

CRITICAL_TARGET_HINTS = {
    "easypete": "easy-pete",
    "easy pete": "easy-pete",
    "gseasypete": "easy-pete",
    "sunnysmiles": "sunny-smiles",
    "sunny smiles": "sunny-smiles",
    "gssunnysmiles": "sunny-smiles",
    "docmitchell": "doc-mitchell",
    "doc mitchell": "doc-mitchell",
    "victor": "victor-robot",
    "trudy": "trudy",
    "ringo": "ringo",
}


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def as_text(value: Any) -> str:
    return "" if value is None else str(value)


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def latest_batch_plan(proof_root: Path) -> Path:
    root = proof_root / "fnv-character-viewer-batch-plan"
    for candidate in sorted((path for path in root.glob("*") if path.is_dir()), reverse=True):
        plan = candidate / "viewer-batch-plan.json"
        if plan.is_file():
            return plan
    raise SystemExit(f"No viewer batch plan found under {root}")


def component_counts(entry: dict[str, Any]) -> Counter[str]:
    result: Counter[str] = Counter()
    raw = entry.get("componentCounts")
    if isinstance(raw, dict):
        for key, value in raw.items():
            try:
                result[as_text(key)] += int(value)
            except (TypeError, ValueError):
                result[as_text(key)] += 1
    for evidence in as_list(entry.get("componentEvidence")):
        if isinstance(evidence, dict):
            component = as_text(evidence.get("component"))
            if component:
                result[component] += 1
    return result


def gate_components_present(counts: Counter[str], gate: dict[str, Any]) -> list[str]:
    return [component for component in as_list(gate.get("components")) if counts.get(as_text(component), 0) > 0]


def actor_priority(entry: dict[str, Any]) -> str:
    haystack = " ".join(
        as_text(entry.get(key)).lower()
        for key in (
            "target",
            "runtimeTarget",
            "placedTarget",
            "baseActorTarget",
            "actorEditorId",
            "placedRefEditorId",
        )
    )
    for needle, priority in CRITICAL_TARGET_HINTS.items():
        if needle in haystack:
            return priority
    return "normal"


def phase_command(entry: dict[str, Any], phase: str, states: list[str]) -> str:
    target = as_text(entry.get("runtimeTarget") or entry.get("target"))
    actor_kind = as_text(entry.get("actorKind") or "npc")
    if not target or actor_kind == "unknown":
        return ""
    flags = " -CreatureDiagnostics" if actor_kind == "creature" else ""
    placement_args = as_text(entry.get("placementCommandArgs"))
    if placement_args:
        placement_args = f" {placement_args}"
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets '{target.replace(chr(39), chr(39) + chr(39))}' -ActorKind {actor_kind}{flags} "
        f"-Phases {phase} -Angles front,front-left,front-right{placement_args} -Serve"
    )
    if states:
        command += f" # states={','.join(states)}"
    return command


def classify_gate(entry: dict[str, Any], gate: dict[str, Any], present: list[str]) -> tuple[str, str, str]:
    entry_classification = as_text(entry.get("classification"))
    if entry_classification in {"known-blocked", "non-runtime-support-file", "intentionally-excluded-with-proof"}:
        return entry_classification, as_text(entry.get("firstFailingGate")) or "entry-classification", as_text(entry.get("notes"))

    gate_name = as_text(gate.get("gate"))
    actor_kind = as_text(entry.get("actorKind"))
    required_components = [as_text(value) for value in as_list(gate.get("components"))]

    optional_if_absent = {
        "weapon-prop-attachment",
        "weapon-animation-family",
        "projectile-muzzle-sound",
        "hand-weapon-transform",
        "dialogue-info-selection",
        "voice-lip-sidecar",
        "mouth-teeth-lip-sync",
        "headgear-slot-composition",
        "hat-hair-occlusion",
        "headgear-transform-pivot",
    }
    if not present and gate_name in optional_if_absent:
        return (
            "intentionally-excluded-with-proof",
            f"{gate_name}-not-referenced-by-actor-ledger",
            "The actor ledger has no component evidence for this optional prop/dialogue/headgear path; absence is explicit, not a silent skip.",
        )

    if actor_kind == "creature" and gate_name.startswith(("face-", "eyes-", "hair-", "headgear-", "weapon-", "dialogue-", "mouth-")):
        return (
            "intentionally-excluded-with-proof",
            "creature-non-human-gate",
            "Creature actor uses the creature presentation ladder; human face/equipment gates are excluded with actor-kind proof.",
        )

    if required_components and not present:
        return (
            "known-blocked",
            f"{gate_name}-missing-ledger-component",
            "Required source components were not present in the harvested actor presentation ledger.",
        )

    return (
        "loaded-pending-runtime",
        f"{gate_name}-runtime-proof",
        "Bytes and source components are accounted, but the gate is not promoted until runtime screenshots/log evidence prove working gameplay presentation.",
    )


def burn_rows_for_entry(entry: dict[str, Any], sequence: int) -> list[dict[str, Any]]:
    counts = component_counts(entry)
    rows: list[dict[str, Any]] = []
    phases = [as_text(phase) for phase in as_list(entry.get("phases")) if as_text(phase)]
    if not phases:
        phases = ["creature-full"] if as_text(entry.get("actorKind")) == "creature" else ["body", "head", "face"]

    for phase in phases:
        gates = PHASE_GATES.get(phase, [{"gate": f"{phase}-runtime-proof", "components": [], "states": [phase]}])
        for gate_index, gate in enumerate(gates, start=1):
            gate_name = as_text(gate.get("gate"))
            states = [as_text(state) for state in as_list(gate.get("states")) if as_text(state)]
            present = gate_components_present(counts, gate)
            classification, first_failing_gate, notes = classify_gate(entry, gate, present)
            rows.append(
                {
                    "id": f"{sequence:06d}:{phase}:{gate_index:02d}:{gate_name}",
                    "entryId": as_text(entry.get("id")),
                    "priority": actor_priority(entry),
                    "source": as_text(entry.get("source")),
                    "actorKind": as_text(entry.get("actorKind")),
                    "target": as_text(entry.get("target")),
                    "runtimeTarget": as_text(entry.get("runtimeTarget") or entry.get("target")),
                    "placedTarget": as_text(entry.get("placedTarget")),
                    "baseActorTarget": as_text(entry.get("baseActorTarget")),
                    "actorFormId": as_text(entry.get("actorFormId")),
                    "actorEditorId": as_text(entry.get("actorEditorId")),
                    "placement": entry.get("placement", {}),
                    "placementCommandArgs": as_text(entry.get("placementCommandArgs")),
                    "phase": phase,
                    "gate": gate_name,
                    "runtimeStates": states,
                    "requiredComponents": [as_text(value) for value in as_list(gate.get("components"))],
                    "presentComponents": present,
                    "classification": classification,
                    "firstFailingGate": first_failing_gate,
                    "notes": notes,
                    "proofExpectations": [
                        "runtime log gate",
                        "front/left/right screenshot set",
                        "transform/bounds/pivot telemetry where geometry is attached",
                        "actor-kit component evidence",
                    ],
                    "runGateCommand": phase_command(entry, phase, states),
                    "payloadPolicy": "generated IDs, commands, classifications, and asset path provenance only; no retail payload bytes",
                }
            )
    return rows


def build_burndown(plan: dict[str, Any], limit: int) -> dict[str, Any]:
    entries = [entry for entry in as_list(plan.get("entries")) if isinstance(entry, dict)]
    if limit > 0:
        entries = entries[:limit]
    rows: list[dict[str, Any]] = []
    for sequence, entry in enumerate(entries, start=1):
        rows.extend(burn_rows_for_entry(entry, sequence))

    classification_counts = Counter(row["classification"] for row in rows)
    gate_counts = Counter(row["gate"] for row in rows)
    phase_counts = Counter(row["phase"] for row in rows)
    invalid = [row for row in rows if row["classification"] not in ALLOWED_CLASSIFICATIONS]
    unclassified = [row for row in rows if not row["classification"]]
    missing_commands = [
        row
        for row in rows
        if row["classification"] in {"runtime-supported", "loaded-pending-runtime"} and not row["runGateCommand"]
    ]
    status = "PASS" if not invalid and not unclassified and not missing_commands else "FAIL"
    return {
        "schema": "nikami-fnv-actor-parity-burndown-v1",
        "status": status,
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "sourcePlan": as_text(plan.get("artifacts", {}).get("plan")) or as_text(plan.get("sourcePlan")),
        "payloadPolicy": "generated proof/control metadata only; no retail assets or mod payload bytes",
        "runtimeBoundary": "This is an actor-first parity burn-down matrix. loaded-pending-runtime means the item is accounted and queued, not visually/gameplay complete.",
        "sourceReferences": {
            "geckNpcCategory": "https://geckwiki.com/index.php/Category:NPC",
            "geckArmorSlots": "https://geckwiki.com/index.php/Armor",
            "geckFacialAnimation": "https://geckwiki.com/index.php/Facial_Animation",
            "geckIdleAnimations": "https://geckwiki.com/index.php/Idle_Animations",
            "geckAnimationTab": "https://geckwiki.com/index.php/Animation_Tab",
        },
        "counts": {
            "entries": len(entries),
            "rows": len(rows),
            "criticalRows": sum(1 for row in rows if row["priority"] != "normal"),
            "invalidClassification": len(invalid),
            "unclassified": len(unclassified),
            "missingRuntimeCommand": len(missing_commands),
        },
        "classificationCounts": dict(sorted(classification_counts.items())),
        "phaseCounts": dict(sorted(phase_counts.items())),
        "gateCounts": dict(sorted(gate_counts.items())),
        "failures": {
            "invalidClassification": [row["id"] for row in invalid[:32]],
            "unclassified": [row["id"] for row in unclassified[:32]],
            "missingRuntimeCommand": [row["id"] for row in missing_commands[:32]],
        },
        "rows": rows,
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = [
        "id",
        "priority",
        "actorKind",
        "source",
        "target",
        "runtimeTarget",
        "phase",
        "gate",
        "runtimeStates",
        "classification",
        "firstFailingGate",
        "presentComponents",
        "requiredComponents",
        "runGateCommand",
        "notes",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writable = dict(row)
            writable["runtimeStates"] = ",".join(row.get("runtimeStates", []))
            writable["presentComponents"] = ",".join(row.get("presentComponents", []))
            writable["requiredComponents"] = ",".join(row.get("requiredComponents", []))
            writer.writerow({field: writable.get(field, "") for field in fieldnames})


def write_markdown(path: Path, burn: dict[str, Any]) -> None:
    lines = [
        "# FNV Actor Parity Burn-Down",
        "",
        f"Status: **{burn['status']}**",
        f"Schema: `{burn['schema']}`",
        f"Runtime boundary: {burn['runtimeBoundary']}",
        f"Policy: {burn['payloadPolicy']}",
        "",
        "## Counts",
        "",
        f"Entries: {burn['counts']['entries']}",
        f"Rows: {burn['counts']['rows']}",
        f"Critical rows: {burn['counts']['criticalRows']}",
        f"Unclassified: {burn['counts']['unclassified']}",
        "",
        "## Classifications",
        "",
    ]
    for key, value in burn["classificationCounts"].items():
        lines.append(f"- `{key}`: {value}")
    lines.extend(["", "## First Runtime Rows", "", "| Priority | Actor | Phase | Gate | Classification | First Gate |", "|---|---|---|---|---|---|"])
    for row in burn["rows"][:96]:
        lines.append(
            f"| {row['priority']} | `{row['runtimeTarget']}` | {row['phase']} | {row['gate']} | "
            f"{row['classification']} | {row['firstFailingGate']} |"
        )
    lines.extend(
        [
            "",
            "## References",
            "",
            "- GECK NPC category: https://geckwiki.com/index.php/Category:NPC",
            "- GECK armor and biped slots: https://geckwiki.com/index.php/Armor",
            "- GECK facial animation and LIP files: https://geckwiki.com/index.php/Facial_Animation",
            "- GECK idle animations: https://geckwiki.com/index.php/Idle_Animations",
            "- GECK animation tab/KF preview: https://geckwiki.com/index.php/Animation_Tab",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proof-root", default="")
    parser.add_argument("--plan-json", default="")
    parser.add_argument("--out-dir", default="")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--require-pass", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    proof_root = Path(args.proof_root) if args.proof_root else repo_root.parent / "proof"
    plan_path = Path(args.plan_json) if args.plan_json else latest_batch_plan(proof_root)
    plan = read_json(plan_path)
    plan.setdefault("sourcePlan", str(plan_path))
    burn = build_burndown(plan, args.limit)
    burn["sourcePlan"] = str(plan_path)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.out_dir) if args.out_dir else proof_root / "fnv-actor-parity-burndown" / stamp
    out_dir.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "json": str(out_dir / "actor-parity-burndown.json"),
        "markdown": str(out_dir / "actor-parity-burndown.md"),
        "csv": str(out_dir / "actor-parity-burndown.csv"),
    }
    burn["artifacts"] = artifacts
    write_json(out_dir / "actor-parity-burndown.json", burn)
    write_markdown(out_dir / "actor-parity-burndown.md", burn)
    write_csv(out_dir / "actor-parity-burndown.csv", burn["rows"])
    print(f"{burn['status']} {out_dir / 'actor-parity-burndown.json'} rows={burn['counts']['rows']}")
    if args.require_pass and burn["status"] != "PASS":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
