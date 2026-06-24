#!/usr/bin/env python3
"""Generate a no-payload batch plan for the FNV character/creature viewer."""

from __future__ import annotations

import argparse
import json
import os
from collections import Counter, defaultdict
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
NPC_PHASES = ["body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"]
CREATURE_PHASES = ["creature-model", "creature-body", "creature-animation", "creature-full"]
COMPONENT_PHASES = {
    "actor-base-record": ["body"],
    "npc-race": ["body", "head", "face"],
    "npc-template": ["body", "head", "face", "equipment"],
    "npc-headpart": ["head", "face"],
    "hair": ["hair"],
    "eyes": ["face"],
    "facegen-symmetric-shape": ["face"],
    "facegen-asymmetric-shape": ["face"],
    "facegen-symmetric-texture": ["face"],
    "npc-model": ["body"],
    "default-outfit": ["equipment"],
    "sleep-outfit": ["equipment"],
    "equipment-armor": ["equipment", "headgear"],
    "equipment-armor-addon": ["equipment", "headgear"],
    "equipment-clothing": ["equipment"],
    "equipment-weapon": ["weapon"],
    "inventory-item": ["equipment"],
    "animation-kffz": ["body"],
    "animation-locomotion-fallback": ["body"],
    "voice-type": ["talk"],
    "creature-model": ["creature-model"],
    "creature-body-nif": ["creature-body"],
    "bodypart-data": ["creature-body"],
    "creature-template": ["creature-model", "creature-body"],
    "placed-reference": [],
}


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def find_latest_ledger(proof_root: Path) -> tuple[Path, Path]:
    ledger_root = proof_root / "fnv-actor-presentation-ledger"
    candidates = sorted([path for path in ledger_root.glob("*") if path.is_dir()], reverse=True)
    for candidate in candidates:
        ledger = candidate / "actor-presentation-ledger.json"
        result = candidate / "result.json"
        if ledger.is_file() and result.is_file():
            return ledger, result
    raise SystemExit(f"No actor presentation ledger found under {ledger_root}")


def as_text(value: Any) -> str:
    return "" if value is None else str(value)


def as_number(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def normalize_form_target(form_id: str) -> str:
    value = form_id.strip()
    if not value:
        return ""
    if value.lower().startswith("formid:"):
        return value
    if value.lower().startswith("0x"):
        return "FormId:" + value
    return value


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def actor_kind_from_record(record_type: str) -> str:
    if record_type == "NPC_":
        return "npc"
    if record_type == "CREA":
        return "creature"
    return "unknown"


def phases_for_kind(actor_kind: str) -> list[str]:
    if actor_kind == "creature":
        return CREATURE_PHASES
    return NPC_PHASES


def phases_for_component(component: str, actor_kind: str) -> list[str]:
    phases = COMPONENT_PHASES.get(component)
    if phases is not None:
        return phases
    if actor_kind == "creature":
        return ["creature-full"]
    return ["body"]


def placement_for_row(row: dict[str, Any]) -> dict[str, Any]:
    position = {
        "x": as_number(row.get("placedPosX")),
        "y": as_number(row.get("placedPosY")),
        "z": as_number(row.get("placedPosZ")),
    }
    rotation = {
        "x": as_number(row.get("placedRotX")),
        "y": as_number(row.get("placedRotY")),
        "z": as_number(row.get("placedRotZ")),
    }
    source_cell = normalize_form_target(as_text(row.get("placedCellFormId")))
    runtime_cell = normalize_form_target(as_text(row.get("placedRuntimeCellFormId"))) or source_cell
    has_position = all(value is not None for value in position.values())
    return {
        "source": as_text(row.get("placedCoordinateSource")),
        "cell": runtime_cell,
        "sourceCell": source_cell,
        "runtimeCell": runtime_cell,
        "cellEditorId": as_text(row.get("placedCellEditorId")),
        "cellGroupType": as_text(row.get("placedCellGroupType")),
        "cellGroupName": as_text(row.get("placedCellGroupName")),
        "cellSource": as_text(row.get("placedCellSource")),
        "cellGridX": as_number(row.get("placedCellGridX")),
        "cellGridY": as_number(row.get("placedCellGridY")),
        "cellFallbackFormId": normalize_form_target(as_text(row.get("placedCellFallbackFormId"))),
        "position": position,
        "rotation": rotation,
        "scale": as_number(row.get("placedScale")),
        "hasPosition": has_position,
        "hasCell": bool(runtime_cell),
        "runtimeBootstrapReady": bool(runtime_cell and has_position),
    }


def placement_command_args(placement: dict[str, Any]) -> str:
    if not placement.get("runtimeBootstrapReady"):
        return ""
    position = placement["position"]
    rotation = placement["rotation"]
    args = [
        f"-BootstrapCell {shell_quote(as_text(placement.get('cell')))}",
        f"-BootstrapX {position['x']}",
        f"-BootstrapY {position['y']}",
        f"-BootstrapZ {position['z']}",
        f"-ActorStageX {position['x']}",
        f"-ActorStageY {position['y']}",
        f"-ActorStageZ {position['z']}",
    ]
    if rotation.get("x") is not None:
        args.append(f"-BootstrapRotX {rotation['x']}")
        args.append(f"-ActorStageRotX {rotation['x']}")
    if rotation.get("y") is not None:
        args.append(f"-BootstrapRotY {rotation['y']}")
        args.append(f"-ActorStageRotY {rotation['y']}")
    if rotation.get("z") is not None:
        args.append(f"-BootstrapRotZ {rotation['z']}")
        args.append(f"-ActorStageRotZ {rotation['z']}")
    return " " + " ".join(args)


def command_for_target(target: str, actor_kind: str, phases: list[str], placement: dict[str, Any]) -> str:
    flags = " -CreatureDiagnostics" if actor_kind == "creature" else ""
    return (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(target)} -ActorKind {actor_kind}{flags} -Phases {','.join(phases)}"
        f"{placement_command_args(placement)} -Serve"
    )


def classify_target(source: str, record_type: str, target: str) -> tuple[str, str, str]:
    actor_kind = actor_kind_from_record(record_type)
    if actor_kind == "unknown":
        return "known-blocked", "actor-kind-resolution", "Source actor is not NPC_ or CREA."
    if not target:
        return "known-blocked", "actor-target-resolution", "No editor id or form id target could be derived."
    if source == "actor-base-record":
        return (
            "loaded-pending-runtime",
            "base-actor-spawn-or-placement-runtime",
            "Base actor is accounted and queued, but still needs spawn/placement runtime support before proof run.",
        )
    return (
        "loaded-pending-runtime",
        "placed-actor-runtime-viewer-proof",
        "Placed actor is queued for runtime viewer proof; loading alone is not claimed as gameplay parity.",
    )


def row_key(row: dict[str, Any], fields: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(as_text(row.get(field)) for field in fields)


def build_component_index(rows: list[dict[str, Any]]) -> dict[tuple[str, str], Counter[str]]:
    components: dict[tuple[str, str], Counter[str]] = defaultdict(Counter)
    for row in rows:
        actor_kind = as_text(row.get("actorKind"))
        actor_form_id = as_text(row.get("actorFormId"))
        component = as_text(row.get("component"))
        if actor_kind and actor_form_id and component:
            components[(actor_kind, actor_form_id)][component] += 1
    return components


def component_evidence(row: dict[str, Any]) -> dict[str, Any]:
    component = as_text(row.get("component"))
    actor_kind = actor_kind_from_record(as_text(row.get("actorKind")))
    return {
        "component": component,
        "phases": phases_for_component(component, actor_kind),
        "sourceRecordType": as_text(row.get("sourceRecordType")),
        "sourceFormId": as_text(row.get("sourceFormId")),
        "sourceEditorId": as_text(row.get("sourceEditorId")),
        "subrecord": as_text(row.get("subrecord")),
        "resolvedRecordType": as_text(row.get("resolvedRecordType")),
        "resolvedFormId": as_text(row.get("resolvedFormId")),
        "resolvedEditorId": as_text(row.get("resolvedEditorId")),
        "assetPath": as_text(row.get("assetPath")),
        "assetStatus": as_text(row.get("assetStatus")),
        "assetArchive": as_text(row.get("assetArchive")),
        "classification": as_text(row.get("classification")),
        "firstFailingGate": as_text(row.get("firstFailingGate")),
        "proofAnchor": as_text(row.get("proofAnchor")),
        "notes": as_text(row.get("notes")),
    }


def build_component_evidence(rows: list[dict[str, Any]]) -> dict[tuple[str, str], list[dict[str, Any]]]:
    evidence: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        actor_kind = as_text(row.get("actorKind"))
        actor_form_id = as_text(row.get("actorFormId"))
        component = as_text(row.get("component"))
        if actor_kind and actor_form_id and component:
            evidence[(actor_kind, actor_form_id)].append(component_evidence(row))
    return evidence


def component_phase_index(component_counts: Counter[str], actor_kind: str) -> dict[str, list[str]]:
    return {
        component: phases_for_component(component, actor_kind)
        for component in sorted(component_counts)
    }


def make_entry(
    *,
    source: str,
    row: dict[str, Any],
    component_counts: Counter[str],
    sequence: int,
) -> dict[str, Any]:
    record_type = as_text(row.get("actorKind") or row.get("resolvedRecordType"))
    actor_kind = actor_kind_from_record(record_type)
    actor_editor_id = as_text(row.get("actorEditorId") or row.get("resolvedEditorId"))
    actor_form_id = as_text(row.get("actorFormId") or row.get("resolvedFormId"))
    placed_ref_editor_id = as_text(row.get("placedRefEditorId"))
    placed_ref_form_id = as_text(row.get("placedRefFormId"))
    base_actor_target = actor_editor_id or normalize_form_target(actor_form_id)
    placed_target = ""
    if source == "placed-reference":
        placed_target = placed_ref_editor_id or normalize_form_target(placed_ref_form_id)
        target = placed_target or base_actor_target
    else:
        target = base_actor_target
    runtime_target = base_actor_target or target
    phases = phases_for_kind(actor_kind)
    placement = placement_for_row(row) if source == "placed-reference" else {
        "source": "base actor has no placed DATA",
        "cell": "",
        "cellEditorId": "",
        "cellGroupType": "",
        "cellGroupName": "",
        "position": {"x": None, "y": None, "z": None},
        "rotation": {"x": None, "y": None, "z": None},
        "scale": None,
        "hasPosition": False,
        "hasCell": False,
        "runtimeBootstrapReady": False,
    }
    placement_args = placement_command_args(placement).strip()
    classification, first_failing_gate, notes = classify_target(source, record_type, runtime_target)
    command = command_for_target(runtime_target, actor_kind, phases, placement) if runtime_target and actor_kind != "unknown" else ""
    if classification not in ALLOWED_CLASSIFICATIONS:
        classification = "known-blocked"
        first_failing_gate = "batch-plan-classification"
        notes = "Batch planner produced an invalid classification."

    return {
        "id": f"{source}:{sequence:06d}",
        "source": source,
        "plugin": as_text(row.get("plugin")),
        "actorKind": actor_kind,
        "recordType": record_type,
        "target": target,
        "selectedTarget": target,
        "runtimeTarget": runtime_target,
        "placedTarget": placed_target,
        "baseActorTarget": base_actor_target,
        "assemblyTarget": base_actor_target,
        "phases": phases,
        "componentPhases": component_phase_index(component_counts, actor_kind),
        "classification": classification,
        "firstFailingGate": first_failing_gate,
        "proofAnchor": as_text(row.get("proofAnchor")),
        "notes": notes,
        "actorFormId": actor_form_id,
        "actorEditorId": actor_editor_id,
        "placedRefFormId": placed_ref_form_id,
        "placedRefEditorId": placed_ref_editor_id,
        "resolvedRecordType": as_text(row.get("resolvedRecordType")),
        "resolvedFormId": as_text(row.get("resolvedFormId")),
        "resolvedEditorId": as_text(row.get("resolvedEditorId")),
        "placement": placement,
        "placementCommandArgs": placement_args,
        "componentCounts": dict(sorted(component_counts.items())),
        "componentEvidence": [],
        "sourceProvenance": component_evidence(row),
        "command": command,
        "payloadPolicy": "generated commands, identifiers, and asset path provenance only; no retail payload bytes are written by this batch plan",
    }


def build_plan(rows: list[dict[str, Any]], result: dict[str, Any], limit: int) -> dict[str, Any]:
    component_index = build_component_index(rows)
    evidence_index = build_component_evidence(rows)
    entries: list[dict[str, Any]] = []

    base_rows = [
        row for row in rows if as_text(row.get("component")) == "actor-base-record" and as_text(row.get("actorKind")) in {"NPC_", "CREA"}
    ]
    placed_rows = [row for row in rows if as_text(row.get("component")) == "placed-reference"]

    seen_base: set[tuple[str, ...]] = set()
    for row in base_rows:
        key = row_key(row, ("actorKind", "actorFormId"))
        if key in seen_base:
            continue
        seen_base.add(key)
        entry = make_entry(
            source="actor-base-record",
            row=row,
            component_counts=component_index[(as_text(row.get("actorKind")), as_text(row.get("actorFormId")))],
            sequence=len(entries) + 1,
        )
        entry["componentEvidence"] = evidence_index[(as_text(row.get("actorKind")), as_text(row.get("actorFormId")))]
        entries.append(entry)

    seen_placed: set[tuple[str, ...]] = set()
    for row in placed_rows:
        key = row_key(row, ("sourceRecordType", "placedRefFormId", "actorKind", "actorFormId"))
        if key in seen_placed:
            continue
        seen_placed.add(key)
        entry = make_entry(
            source="placed-reference",
            row=row,
            component_counts=component_index[(as_text(row.get("actorKind")), as_text(row.get("actorFormId")))],
            sequence=len(entries) + 1,
        )
        entry["componentEvidence"] = evidence_index[(as_text(row.get("actorKind")), as_text(row.get("actorFormId")))]
        entries.append(entry)

    if limit > 0:
        entries = entries[:limit]

    classification_counts = Counter(entry["classification"] for entry in entries)
    actor_kind_counts = Counter(entry["actorKind"] for entry in entries)
    source_counts = Counter(entry["source"] for entry in entries)
    invalid = [entry for entry in entries if entry["classification"] not in ALLOWED_CLASSIFICATIONS]
    missing_targets = [entry for entry in entries if not entry["target"]]
    missing_commands = [entry for entry in entries if entry["actorKind"] != "unknown" and not entry["command"]]
    missing_placement = [
        entry
        for entry in entries
        if entry["source"] == "placed-reference" and not entry.get("placement", {}).get("runtimeBootstrapReady")
    ]

    expected_base = int(result.get("npcBaseRecords", 0)) + int(result.get("creatureBaseRecords", 0))
    expected_placed = int(result.get("placedNpcCreatureRefs", 0))
    complete_base = len(seen_base) == expected_base if expected_base else len(seen_base) > 0
    complete_placed = len(seen_placed) == expected_placed if expected_placed else len(seen_placed) > 0
    coverage_status = "complete" if complete_base and complete_placed else "partial"
    status = (
        "PASS"
        if not invalid and not missing_targets and not missing_commands and not missing_placement and coverage_status == "complete"
        else "FAIL"
    )

    return {
        "schema": "nikami-fnv-character-viewer-batch-plan-v1",
        "status": status,
        "coverageStatus": coverage_status,
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "sourceLedger": result.get("artifacts", {}).get("ledger", ""),
        "sourceResult": result.get("artifacts", {}).get("result", ""),
        "content": result.get("content", []),
        "payloadPolicy": "generated commands, identifiers, and asset path provenance only; no retail payload bytes",
        "expectedCounts": {
            "baseActors": expected_base,
            "npcBaseRecords": int(result.get("npcBaseRecords", 0)),
            "creatureBaseRecords": int(result.get("creatureBaseRecords", 0)),
            "placedActorRefs": expected_placed,
        },
        "plannedCounts": {
            "total": len(entries),
            "baseActors": source_counts.get("actor-base-record", 0),
            "placedActorRefs": source_counts.get("placed-reference", 0),
            "placedActorRefsWithRuntimeBootstrap": source_counts.get("placed-reference", 0) - len(missing_placement),
            "npc": actor_kind_counts.get("npc", 0),
            "creature": actor_kind_counts.get("creature", 0),
            "unknown": actor_kind_counts.get("unknown", 0),
        },
        "classificationCounts": dict(sorted(classification_counts.items())),
        "failures": {
            "invalidClassification": len(invalid),
            "missingTarget": len(missing_targets),
            "missingCommand": len(missing_commands),
            "missingPlacementContext": len(missing_placement),
        },
        "commands": {
            "regeneratePlan": "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer-batch-plan.ps1 -RequirePass",
            "runSingleEntry": "Use entries[].command; each entry carries its own target, actor kind, phases, and diagnostics.",
        },
        "entries": entries,
    }


def write_markdown(path: Path, plan: dict[str, Any]) -> None:
    lines = [
        "# FNV Character Viewer Batch Plan",
        "",
        f"Status: **{plan['status']}**",
        f"Coverage: `{plan['coverageStatus']}`",
        f"Schema: `{plan['schema']}`",
        "",
        "## Counts",
        "",
        f"Expected base actors: {plan['expectedCounts']['baseActors']}",
        f"Expected placed refs: {plan['expectedCounts']['placedActorRefs']}",
        f"Planned total: {plan['plannedCounts']['total']}",
        f"Planned NPC: {plan['plannedCounts']['npc']}",
        f"Planned creature: {plan['plannedCounts']['creature']}",
        "",
        "## Classifications",
        "",
    ]
    for key, value in sorted(plan["classificationCounts"].items()):
        lines.append(f"- `{key}`: {value}")
    lines.extend(["", "## First Entries", "", "| Source | Kind | Target | Classification | Gate |", "|---|---|---|---|---|"])
    for entry in plan["entries"][:64]:
        lines.append(
            f"| {entry['source']} | {entry['actorKind']} | `{entry['target']}` | "
            f"{entry['classification']} | {entry['firstFailingGate']} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ledger-json", type=Path)
    parser.add_argument("--result-json", type=Path)
    parser.add_argument("--proof-root", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--require-pass", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    proof_root = args.proof_root or repo_root.parent / "proof"
    if args.ledger_json and args.result_json:
        ledger_json = args.ledger_json
        result_json = args.result_json
    else:
        ledger_json, result_json = find_latest_ledger(proof_root)

    rows = read_json(ledger_json)
    result = read_json(result_json)
    result.setdefault("artifacts", {})
    result["artifacts"]["ledger"] = str(ledger_json)
    result["artifacts"]["result"] = str(result_json)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = args.out_dir or proof_root / "fnv-character-viewer-batch-plan" / stamp
    out_dir.mkdir(parents=True, exist_ok=True)
    plan = build_plan(rows, result, args.limit)
    plan["artifacts"] = {
        "plan": str(out_dir / "viewer-batch-plan.json"),
        "markdown": str(out_dir / "viewer-batch-plan.md"),
    }
    write_json(out_dir / "viewer-batch-plan.json", plan)
    write_markdown(out_dir / "viewer-batch-plan.md", plan)
    print(f"{plan['status']} {out_dir / 'viewer-batch-plan.json'}")
    print(
        "planned total={total} npc={npc} creature={creature} base={base} placed={placed}".format(
            total=plan["plannedCounts"]["total"],
            npc=plan["plannedCounts"]["npc"],
            creature=plan["plannedCounts"]["creature"],
            base=plan["plannedCounts"]["baseActors"],
            placed=plan["plannedCounts"]["placedActorRefs"],
        )
    )
    if args.require_pass and plan["status"] != "PASS":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
