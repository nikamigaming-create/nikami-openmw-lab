#!/usr/bin/env python3
"""Build a generated no-payload catalog for the FNV live character/item studio."""

from __future__ import annotations

import argparse
import html
import json
import re
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

GAMEPLAY_RECORD_DOMAINS = {
    "ACTI": "activator",
    "ALCH": "consumable",
    "WEAP": "weapon",
    "AMMO": "ammo",
    "ARMA": "armor-addon",
    "ARMO": "armor",
    "BOOK": "book",
    "CLOT": "clothing",
    "CONT": "container",
    "DOOR": "door",
    "PROJ": "projectile",
    "EXPL": "explosion",
    "KEYM": "key",
    "MISC": "misc-item",
    "MSTT": "movable-static",
    "PERK": "perk",
    "STAT": "static",
    "AVIF": "actor-value",
}

HTML_ENTRY_FIELDS = (
    "id",
    "source",
    "domain",
    "kind",
    "recordType",
    "label",
    "plugin",
    "target",
    "selectedTarget",
    "runtimeTarget",
    "placedTarget",
    "baseActorTarget",
    "assemblyTarget",
    "classification",
    "firstFailingGate",
    "formId",
    "actorFormId",
    "placedRefFormId",
    "model",
    "phases",
    "studioGates",
)


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def latest_with(root: Path, child: str, required: tuple[str, ...]) -> Path:
    base = root / child
    candidates = sorted((path for path in base.glob("*") if path.is_dir()), reverse=True)
    for candidate in candidates:
        if all((candidate / name).is_file() for name in required):
            return candidate
    raise SystemExit(f"No generated {child} proof directory with {', '.join(required)} under {base}")


def as_text(value: Any) -> str:
    return "" if value is None else str(value)


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def csv(values: list[Any]) -> str:
    return ",".join(as_text(value) for value in values if as_text(value))


def normalize_form_target(value: Any) -> str:
    text = as_text(value).strip()
    if not text or text.lower().startswith("formid:"):
        return text
    if re.fullmatch(r"0x[0-9A-Fa-f]+", text):
        return f"FormId:{text}"
    return text


def actor_runtime_target(row: dict[str, Any]) -> str:
    runtime_target = as_text(row.get("runtimeTarget"))
    placed_target = as_text(row.get("placedTarget")) or (as_text(row.get("target")) if as_text(row.get("source")) == "placed-reference" else "")
    if as_text(row.get("source")) == "placed-reference" and runtime_target == placed_target:
        runtime_target = ""
    return (
        runtime_target
        or as_text(row.get("assemblyTarget"))
        or as_text(row.get("baseActorTarget"))
        or as_text(row.get("actorEditorId"))
        or normalize_form_target(row.get("actorFormId"))
        or as_text(row.get("target"))
    )


def actor_placed_target(row: dict[str, Any]) -> str:
    if as_text(row.get("source")) != "placed-reference":
        return as_text(row.get("placedTarget"))
    return (
        as_text(row.get("placedTarget"))
        or as_text(row.get("target"))
        or as_text(row.get("placedRefEditorId"))
        or normalize_form_target(row.get("placedRefFormId"))
    )


def placement_command_args(placement: dict[str, Any]) -> str:
    if not isinstance(placement, dict) or not placement.get("runtimeBootstrapReady"):
        return ""
    position = placement.get("position") if isinstance(placement.get("position"), dict) else {}
    rotation = placement.get("rotation") if isinstance(placement.get("rotation"), dict) else {}
    cell = as_text(placement.get("cell"))
    if not cell:
        return ""
    args = [
        f"-BootstrapCell {shell_quote(cell)}",
        f"-BootstrapX {position.get('x')}",
        f"-BootstrapY {position.get('y')}",
        f"-BootstrapZ {position.get('z')}",
        f"-ActorStageX {position.get('x')}",
        f"-ActorStageY {position.get('y')}",
        f"-ActorStageZ {position.get('z')}",
    ]
    for axis in ("x", "y", "z"):
        value = rotation.get(axis)
        if value is not None:
            suffix = axis.upper()
            args.append(f"-BootstrapRot{suffix} {value}")
            args.append(f"-ActorStageRot{suffix} {value}")
    return " ".join(args)


def search_text(*values: Any) -> str:
    flattened: list[str] = []
    for value in values:
        if isinstance(value, dict):
            flattened.extend(search_text(k, v) for k, v in value.items())
        elif isinstance(value, list):
            flattened.extend(search_text(item) for item in value)
        else:
            text = as_text(value).strip()
            if text:
                flattened.append(text)
                spaced = re.sub(r"([a-z])([A-Z0-9])", r"\1 \2", text)
                spaced = re.sub(r"([0-9])([A-Za-z])", r"\1 \2", spaced)
                spaced = re.sub(r"[_\\/\.-]+", " ", spaced)
                if spaced != text:
                    flattened.append(spaced)
                compact = re.sub(r"[^A-Za-z0-9]+", "", text)
                if compact and compact != text:
                    flattened.append(compact)
    return " ".join(flattened).lower()


def normalize_classification(value: str) -> str:
    return value if value in ALLOWED_CLASSIFICATIONS else "known-blocked"


def actor_studio_command(entry: dict[str, Any], angles: str) -> str:
    target = actor_runtime_target(entry)
    actor_kind = as_text(entry.get("actorKind"))
    phases = csv(as_list(entry.get("phases")))
    if not target or actor_kind not in {"npc", "creature"} or not phases:
        return ""
    bootstrap_args = as_text(entry.get("placementCommandArgs")) or placement_command_args(entry.get("placement", {}))
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(target)} -ActorKind {actor_kind} -Phases {shell_quote(phases)} "
    )
    if bootstrap_args:
        command += f"{bootstrap_args} "
    command += f"-Angles {shell_quote(angles)} -OpenViewer -LiveServe"
    if actor_kind == "creature":
        command += " -CreatureDiagnostics"
    return command


def actor_entry(row: dict[str, Any], index: int) -> dict[str, Any]:
    selected_target = as_text(row.get("selectedTarget") or row.get("target"))
    runtime_target = actor_runtime_target(row)
    placed_target = actor_placed_target(row)
    actor_form_target = normalize_form_target(row.get("actorFormId"))
    placed_ref_form_target = normalize_form_target(row.get("placedRefFormId"))
    placement_args = as_text(row.get("placementCommandArgs")) or placement_command_args(row.get("placement", {}))
    if not selected_target:
        selected_target = placed_target or runtime_target
    label = (
        as_text(row.get("placedRefEditorId"))
        or as_text(row.get("actorEditorId"))
        or selected_target
        or actor_form_target
        or f"actor-{index:06d}"
    )
    classification = normalize_classification(as_text(row.get("classification")))
    entry = {
        "id": f"actor:{index:06d}",
        "domain": "actor",
        "kind": as_text(row.get("actorKind")) or "unknown",
        "recordType": as_text(row.get("recordType")),
        "label": label,
        "target": selected_target,
        "selectedTarget": selected_target,
        "runtimeTarget": runtime_target,
        "placedTarget": placed_target,
        "baseActorTarget": as_text(row.get("baseActorTarget")) or runtime_target,
        "assemblyTarget": as_text(row.get("assemblyTarget") or row.get("baseActorTarget")) or runtime_target,
        "actorFormTarget": actor_form_target,
        "placedRefFormTarget": placed_ref_form_target,
        "targetMapping": {
            "selectedTarget": selected_target,
            "placedTarget": placed_target,
            "runtimeTarget": runtime_target,
            "actorFormTarget": actor_form_target,
            "placedRefFormTarget": placed_ref_form_target,
            "source": as_text(row.get("source")),
        },
        "phases": as_list(row.get("phases")),
        "plugin": as_text(row.get("plugin")),
        "formId": as_text(row.get("actorFormId")),
        "editorId": as_text(row.get("actorEditorId")),
        "actorFormId": as_text(row.get("actorFormId")),
        "actorEditorId": as_text(row.get("actorEditorId")),
        "placedRefFormId": as_text(row.get("placedRefFormId")),
        "placedRefEditorId": as_text(row.get("placedRefEditorId")),
        "source": as_text(row.get("source")),
        "classification": classification,
        "readiness": classification,
        "firstFailingGate": as_text(row.get("firstFailingGate")),
        "model": "",
        "icon": "",
        "componentCounts": row.get("componentCounts", {}),
        "componentPhases": row.get("componentPhases", {}),
        "componentEvidence": as_list(row.get("componentEvidence")),
        "placement": row.get("placement", {}),
        "placementCommandArgs": placement_args,
        "studioGates": [
            {
                "gate": "runtime-world-session",
                "classification": "loaded-pending-runtime",
                "proof": "Current runnable command uses the real OpenMW/FNV runtime viewer; visual correctness still requires generated screenshots and runtime audits.",
            },
            {
                "gate": "neutral-stage-summon",
                "classification": "loaded-pending-runtime",
                "proof": "Clean isolated plane/studio summon mode is not implemented yet; this catalog exposes it as the next required runtime gate.",
            },
        ],
        "commands": {
            "runtimeThreeCamera": actor_studio_command(row, "front,front-left,front-right"),
            "runtimeFrontOnly": actor_studio_command(row, "front"),
            "neutralStage": "",
        },
        "sourceEntry": row.get("id", ""),
        "sourceProvenance": row.get("sourceProvenance", {}),
        "proofAnchor": as_text(row.get("proofAnchor")),
        "notes": as_text(row.get("notes")),
        "payloadPolicy": "generated identifiers, commands, and asset path provenance only; no retail payload bytes",
    }
    entry["searchText"] = search_text(
        entry["domain"],
        entry["kind"],
        entry["recordType"],
        entry["label"],
        entry["target"],
        entry["runtimeTarget"],
        entry["placedTarget"],
        entry["baseActorTarget"],
        entry["assemblyTarget"],
        entry["formId"],
        entry["editorId"],
        entry["plugin"],
        entry["source"],
        entry["componentCounts"],
        entry["componentPhases"],
        entry["componentEvidence"],
        entry["placement"],
        entry["placementCommandArgs"],
        entry["proofAnchor"],
    )
    return entry


def gameplay_entry(row: dict[str, Any], index: int) -> dict[str, Any]:
    record_type = as_text(row.get("recordType"))
    domain = GAMEPLAY_RECORD_DOMAINS.get(record_type, record_type.lower() or "gameplay")
    label = as_text(row.get("editorId")) or as_text(row.get("formId")) or f"{domain}-{index:06d}"
    classification = normalize_classification(as_text(row.get("classification") or row.get("readiness")))
    entry = {
        "id": f"gameplay:{index:06d}",
        "domain": "gameplay",
        "kind": domain,
        "recordType": record_type,
        "label": label,
        "target": as_text(row.get("editorId") or row.get("formId")),
        "plugin": as_text(row.get("plugin")),
        "formId": as_text(row.get("formId")),
        "editorId": as_text(row.get("editorId")),
        "source": "content-ledger",
        "classification": classification,
        "readiness": normalize_classification(as_text(row.get("readiness") or classification)),
        "firstFailingGate": as_text(row.get("firstFailingGate") or "item-studio-spawn-command"),
        "model": as_text(row.get("model")),
        "icon": as_text(row.get("icon")),
        "componentCounts": {},
        "placement": {},
        "studioGates": [
            {
                "gate": "item-studio-spawn-command",
                "classification": "loaded-pending-runtime",
                "proof": "Record bytes are cataloged, but a generic real-time studio summon/equip/manipulate command for this item domain is pending.",
            }
        ],
        "commands": {
            "runtimeThreeCamera": "",
            "runtimeFrontOnly": "",
            "neutralStage": "",
        },
        "gameplay": {
            "ammo": as_text(row.get("ammo")),
            "projectile": as_text(row.get("projectile")),
            "worldModel": as_text(row.get("worldModel")),
            "equipType": as_text(row.get("equipType")),
            "runtimeProofGate": as_text(row.get("runtimeProofGate")),
            "unprovenGameplayGates": as_list(row.get("unprovenGameplayGates")),
        },
        "payloadPolicy": "generated identifiers and paths only; no retail payload bytes",
    }
    entry["searchText"] = search_text(
        entry["domain"],
        entry["kind"],
        entry["recordType"],
        entry["label"],
        entry["target"],
        entry["formId"],
        entry["editorId"],
        entry["plugin"],
        entry["model"],
        entry["icon"],
        entry["gameplay"],
    )
    return entry


def build_catalog(plan: dict[str, Any], gameplay_rows: list[dict[str, Any]], limit: int) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    for row in as_list(plan.get("entries")):
        if isinstance(row, dict):
            entries.append(actor_entry(row, len(entries) + 1))
    for row in gameplay_rows:
        if isinstance(row, dict) and as_text(row.get("recordType")) in GAMEPLAY_RECORD_DOMAINS:
            entries.append(gameplay_entry(row, len(entries) + 1))
    if limit > 0:
        entries = entries[:limit]

    classifications = Counter(entry["classification"] for entry in entries)
    domains = Counter(entry["domain"] for entry in entries)
    kinds = Counter(entry["kind"] for entry in entries)
    missing_search = [entry for entry in entries if not entry["searchText"]]
    invalid = [entry for entry in entries if entry["classification"] not in ALLOWED_CLASSIFICATIONS]
    status = "PASS" if entries and not missing_search and not invalid else "FAIL"
    return {
        "schema": "nikami-fnv-character-studio-catalog-v1",
        "schemaMarkers": [
            "searchable-studio-catalog-v1",
            "runtime-session-commands-v1",
            "live-studio-workbench-v1",
            "three-camera-session-strip-v1",
            "component-selector-job-payload-v1",
            "component-review-rows-v1",
            "compact-html-index-v1",
            "live-api-catalog-search-v1",
            "placed-runtime-target-map-v1",
            "placement-bootstrap-job-args-v1",
            "neutral-stage-gate-pending-v1",
            "no-retail-payload-v1",
        ],
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "status": status,
        "payloadPolicy": "generated search/session metadata only; no retail assets or payload bytes",
        "sourcePlan": plan.get("artifacts", {}).get("plan", ""),
        "sourceContentLedger": "",
        "counts": {
            "total": len(entries),
            "domains": dict(sorted(domains.items())),
            "kinds": dict(sorted(kinds.items())),
            "classifications": dict(sorted(classifications.items())),
            "missingSearchText": len(missing_search),
            "invalidClassifications": len(invalid),
        },
        "commands": {
            "buildCatalog": "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-studio-catalog.ps1 -OpenStudio",
            "liveRuntimeNote": "Actor rows expose runtimeThreeCamera/runtimeFrontOnly commands; item rows are cataloged pending generic item studio summon/equip support.",
        },
        "entries": entries,
    }


def compact_entry(entry: dict[str, Any]) -> dict[str, Any]:
    compact = {key: entry.get(key) for key in HTML_ENTRY_FIELDS if key in entry}
    commands = entry.get("commands") if isinstance(entry.get("commands"), dict) else {}
    compact["runnable"] = bool(commands.get("runtimeThreeCamera")) and entry.get("domain") == "actor"
    compact["hasFullDetails"] = False
    return compact


def html_catalog(catalog: dict[str, Any]) -> dict[str, Any]:
    markers = [as_text(marker) for marker in as_list(catalog.get("schemaMarkers")) if as_text(marker)]
    for marker in ("compact-html-index-v1", "live-api-catalog-search-v1"):
        if marker not in markers:
            markers.append(marker)
    return {
        "schema": catalog.get("schema", "nikami-fnv-character-studio-catalog-v1"),
        "schemaMarkers": markers,
        "createdAt": catalog.get("createdAt", ""),
        "status": catalog.get("status", "FAIL"),
        "payloadPolicy": catalog.get("payloadPolicy", ""),
        "sourcePlan": catalog.get("sourcePlan", ""),
        "sourceContentLedger": catalog.get("sourceContentLedger", ""),
        "counts": catalog.get("counts", {}),
        "commands": catalog.get("commands", {}),
        "htmlEntryMode": "compact-index-full-row-on-select",
        "entries": [compact_entry(entry) for entry in as_list(catalog.get("entries")) if isinstance(entry, dict)],
    }


def html_doc(catalog: dict[str, Any]) -> str:
    data = json.dumps(html_catalog(catalog), ensure_ascii=False).replace("</", "<\\/")
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FNV Character Studio Catalog</title>
<style>
:root {{
  color-scheme: dark;
  --bg: #101215;
  --panel: #1a1d22;
  --panel2: #20242b;
  --line: #363c45;
  --text: #eceff3;
  --muted: #aeb6c2;
  --accent: #74a8ff;
  --bad: #ff6f61;
  --ok: #64d488;
  --warn: #e8c86a;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--text); font: 13px/1.4 Segoe UI, Arial, sans-serif; }}
header {{ position: sticky; top: 0; z-index: 4; background: #15181c; border-bottom: 1px solid var(--line); padding: 12px 16px; display: grid; gap: 10px; }}
h1 {{ margin: 0; font-size: 17px; }}
main {{ padding: 14px 16px 28px; display: grid; gap: 12px; }}
.bar {{ display: grid; grid-template-columns: minmax(240px, 1fr) 170px 170px; gap: 8px; }}
input, select, button {{ border: 1px solid var(--line); border-radius: 5px; background: var(--panel2); color: var(--text); padding: 7px 9px; font: inherit; }}
button:disabled {{ opacity: .45; cursor: not-allowed; }}
.summary {{ display: flex; flex-wrap: wrap; gap: 8px; color: var(--muted); }}
.pill {{ border-radius: 99px; padding: 3px 8px; background: var(--panel2); border: 1px solid var(--line); }}
.PASS {{ color: #07120b; background: var(--ok); }}
.FAIL {{ color: #190502; background: var(--bad); }}
.loaded-pending-runtime {{ color: #181102; background: var(--warn); }}
.runtime-supported {{ color: #07120b; background: var(--ok); }}
.known-blocked {{ color: #190502; background: var(--bad); }}
.workbench {{ display: grid; grid-template-columns: minmax(300px, 380px) minmax(0, 1fr); gap: 10px; align-items: start; }}
.panel {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 10px; display: grid; gap: 9px; min-width: 0; }}
.panel h2 {{ margin: 0; font-size: 14px; }}
.panel h3 {{ margin: 0; font-size: 12px; color: var(--muted); text-transform: uppercase; }}
.actions {{ display: flex; flex-wrap: wrap; gap: 7px; }}
.fieldGrid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 7px; }}
.checkGrid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 6px; }}
.check {{ display: inline-flex; gap: 6px; align-items: center; border: 1px solid var(--line); border-radius: 5px; padding: 6px 7px; background: var(--panel2); }}
.check input {{ margin: 0; }}
.cameraStrip {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }}
.cameraPane {{ background: #090b0e; border: 1px solid var(--line); border-radius: 6px; overflow: hidden; min-width: 0; }}
.cameraHead {{ display: flex; justify-content: space-between; gap: 8px; padding: 8px 9px; border-bottom: 1px solid var(--line); color: var(--muted); }}
.cameraBody {{ min-height: 220px; display: grid; place-items: center; color: var(--muted); }}
.cameraPane img {{ display: block; width: 100%; height: auto; background: #050607; }}
.studioStage {{ min-height: 280px; border: 1px solid var(--line); border-radius: 6px; background: #0b0d10; overflow: hidden; position: relative; display: grid; grid-template-rows: auto 1fr auto; }}
.stageHead, .stageFoot {{ display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 8px; padding: 8px 10px; background: #12151a; border-bottom: 1px solid var(--line); color: var(--muted); }}
.stageFoot {{ border-top: 1px solid var(--line); border-bottom: 0; }}
.stageViewport {{ min-height: 220px; position: relative; display: grid; place-items: center; background:
  linear-gradient(transparent 95%, rgba(116,168,255,.18) 96%),
  linear-gradient(90deg, transparent 95%, rgba(116,168,255,.18) 96%),
  radial-gradient(circle at 50% 40%, rgba(116,168,255,.12), transparent 34%),
  #080a0d; background-size: 32px 32px, 32px 32px, auto, auto; }}
.stageActor {{ width: min(24vw, 110px); min-width: 72px; aspect-ratio: .42 / 1; border: 1px solid #667181; border-radius: 42% 42% 10px 10px; background: linear-gradient(#303844, #171b22); box-shadow: 0 0 0 1px #0f1217, 0 20px 70px rgba(116,168,255,.24); }}
.stageOverlay {{ position: absolute; left: 10px; bottom: 10px; display: grid; gap: 4px; max-width: min(460px, calc(100% - 20px)); }}
.stateRow {{ display: flex; flex-wrap: wrap; gap: 7px; align-items: center; }}
.focusPreset {{ border-color: #4d5a6a; }}
.reviewControl {{ display: grid; grid-template-columns: minmax(150px, 1fr) auto; gap: 7px; }}
.reviewRows {{ display: grid; gap: 5px; max-height: 260px; overflow: auto; }}
.reviewRow {{ display: grid; grid-template-columns: minmax(92px, 1fr) 86px 92px; gap: 6px; align-items: center; background: #101216; border: 1px solid #2b3038; border-radius: 5px; padding: 6px; }}
.reviewRow.active {{ border-color: #5c7ead; }}
.reviewRow .proofLinks {{ grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 5px; color: var(--muted); }}
.policyNote {{ color: var(--muted); font-size: 12px; }}
.jobList, .eventList, .coordList {{ display: grid; gap: 6px; max-height: 240px; overflow: auto; }}
.jobItem, .eventItem, .coordItem {{ background: #101216; border: 1px solid #2b3038; border-radius: 5px; padding: 7px; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(360px, 1fr)); gap: 10px; }}
.card {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 10px; display: grid; gap: 7px; min-width: 0; }}
.head {{ display: flex; justify-content: space-between; gap: 8px; }}
.label {{ font-weight: 600; overflow-wrap: anywhere; }}
.muted {{ color: var(--muted); }}
.meta {{ display: flex; flex-wrap: wrap; gap: 6px; }}
.command {{ font-family: Consolas, monospace; font-size: 12px; background: #0d0f12; border: 1px solid #2b3038; border-radius: 4px; padding: 6px; white-space: pre-wrap; overflow-wrap: anywhere; max-height: 96px; overflow: auto; }}
.gate {{ color: var(--warn); }}
@media (max-width: 980px) {{ .workbench, .cameraStrip {{ grid-template-columns: 1fr; }} }}
@media (max-width: 760px) {{ .bar, .fieldGrid, .checkGrid {{ grid-template-columns: 1fr; }} }}
</style>
</head>
<body>
<header>
  <h1>FNV Character Studio Catalog <span id="status" class="pill"></span></h1>
  <div class="bar">
    <input id="search" placeholder="Search NPCs, creatures, items, weapons, outfits, hair, hats, IDs, models">
    <select id="domain"><option value="">all domains</option></select>
    <select id="kind"><option value="">all kinds</option></select>
  </div>
  <div id="summary" class="summary"></div>
</header>
<main>
  <section class="workbench">
    <div class="panel">
      <h2>Studio Session</h2>
      <div id="liveState" class="muted"></div>
      <div id="selectedEntry" class="jobItem"></div>
      <div class="actions">
        <button id="newSession" type="button">New Session</button>
        <button id="runThree" type="button">Run 3 Camera</button>
        <button id="runFront" type="button">Run Front</button>
      </div>
      <h3>Easy Pete Debug</h3>
      <div class="actions">
        <button class="focusPreset" type="button" data-preset="easy-pete-face">Face</button>
        <button class="focusPreset" type="button" data-preset="easy-pete-hat">Hat</button>
        <button class="focusPreset" type="button" data-preset="easy-pete-skin">Skin</button>
      </div>
      <h3>Component Payload</h3>
      <div class="fieldGrid">
        <label>Phase<select id="phaseSelect"></select></label>
        <label>Part Focus<select id="partFocusSelect"></select></label>
        <label>Job<select id="jobTypeSelect"></select></label>
        <label>Animation<select id="animationSelect"></select></label>
        <label>Dialogue<select id="dialogueSelect"></select></label>
        <label>Angles<select id="angleSelect"></select></label>
      </div>
      <div id="partChecks" class="checkGrid"></div>
      <h3>Save / Review</h3>
      <div class="stateRow">
        <span id="saveState" class="pill">Unsaved</span>
        <span id="proofState" class="pill">No proof</span>
        <span id="reviewState" class="pill">Review pending</span>
      </div>
      <div class="reviewControl">
        <select id="reviewSelect"></select>
        <button id="saveReview" type="button">Save Component Review Rows</button>
      </div>
      <div id="componentReviews" class="reviewRows"></div>
      <div class="policyNote">Generated session/review metadata only; no retail assets or payload bytes are written.</div>
      <h3>Session Events</h3>
      <div id="eventList" class="eventList"></div>
    </div>
    <div class="panel">
      <div class="cameraStrip" id="cameraStrip"></div>
      <div class="studioStage" id="studioStage">
        <div class="stageHead"><b>Neutral 3D Workbench</b><span id="stageStatus">runtime stage pending</span></div>
        <div class="stageViewport">
          <div class="stageActor" aria-label="neutral actor silhouette"></div>
          <div class="stageOverlay" id="stageOverlay"></div>
        </div>
        <div class="stageFoot"><span id="stageTarget">no target selected</span><span id="stageFocus">all parts</span></div>
      </div>
      <div class="fieldGrid">
        <div>
          <h3>Jobs</h3>
          <div id="jobList" class="jobList"></div>
        </div>
        <div>
          <h3>Coordinates</h3>
          <div id="coordList" class="coordList"></div>
        </div>
      </div>
    </div>
  </section>
  <div id="cards" class="grid"></div>
</main>
<script>
const CATALOG = {data};
const PARTS = ["body-skin", "head-skin", "face-organs", "hair-beard", "equipment-body", "weapon", "headgear"];
const PART_LABELS = {{
  "body-skin": "Body / Skin",
  "head-skin": "Head Skin",
  "face-organs": "Face / Eyes / Mouth",
  "hair-beard": "Hair / Beard",
  "equipment-body": "Outfit / Armor",
  "weapon": "Weapon",
  "headgear": "Hat / Headgear"
}};
const PART_PHASES = {{
  "body-skin": "body",
  "head-skin": "head",
  "face-organs": "face",
  "hair-beard": "hair",
  "equipment-body": "equipment",
  "weapon": "weapon",
  "headgear": "headgear"
}};
const REVIEW_STATES = ["review-pending", "pass", "fail", "blocked", "needs-rerun"];
const REVIEW_COMPONENTS = [
  {{ id: "body-skin", label: "Body / Skin", parts: ["body-skin"], phases: ["body"], categories: ["body-skin"] }},
  {{ id: "head-skin", label: "Head Skin", parts: ["head-skin"], phases: ["head"], categories: ["head-skin"] }},
  {{ id: "face", label: "Face / Wrinkles", parts: ["face-organs", "head-skin"], phases: ["face"], categories: ["head-skin", "face-organs"] }},
  {{ id: "eyes", label: "Eyes", parts: ["face-organs"], phases: ["face"], categories: ["face-organs"], classes: ["faceEye"] }},
  {{ id: "mouth", label: "Mouth", parts: ["face-organs"], phases: ["face", "talk"], categories: ["face-organs"], classes: ["faceMouth"] }},
  {{ id: "teeth", label: "Teeth", parts: ["face-organs"], phases: ["face", "talk"], categories: ["face-organs"], classes: ["faceTeeth"] }},
  {{ id: "tongue", label: "Tongue", parts: ["face-organs"], phases: ["face", "talk"], categories: ["face-organs"], classes: ["faceTongue"] }},
  {{ id: "hair-beard", label: "Hair / Beard", parts: ["hair-beard"], phases: ["hair"], categories: ["hair-beard"] }},
  {{ id: "headgear", label: "Hat / Headgear", parts: ["headgear"], phases: ["headgear"], categories: ["headgear"] }},
  {{ id: "equipment-body", label: "Outfit / Armor", parts: ["equipment-body"], phases: ["equipment"], categories: ["equipment-body"] }},
  {{ id: "weapon", label: "Weapon / Gun", parts: ["weapon"], phases: ["weapon"], categories: ["weapon"] }},
  {{ id: "animation", label: "Animation", parts: ["body-skin", "head-skin"], phases: ["body", "head"], categories: ["animation"] }},
  {{ id: "dialogue", label: "Dialogue / Talk", parts: ["face-organs"], phases: ["talk"], categories: ["face-organs"] }}
];
const state = {{
  query: "",
  domain: "",
  kind: "",
  selectedId: "",
  session: null,
  jobs: [],
  events: [],
  reviews: [],
  results: CATALOG.entries || [],
  resultCount: null,
  searchMode: "embedded-index",
  searchSeq: 0,
  entryDetails: {{}},
  latestManifest: null,
  latestJob: null,
  partEnabled: Object.fromEntries(PARTS.map(part => [part, true]))
}};
function esc(v) {{ return String(v ?? "").replace(/[&<>"']/g, c => ({{"&":"&amp;","<":"&lt;",">":"&gt;","\\"":"&quot;","'":"&#39;"}}[c])); }}
function fillSelect(id, values) {{
  const node = document.getElementById(id);
  node.innerHTML = `<option value="">all ${{id}}s</option>` + values.map(v => `<option value="${{esc(v)}}">${{esc(v)}}</option>`).join("");
}}
function commandBlock(command) {{
  return command ? `<div class="command">${{esc(command)}}</div>` : `<div class="gate">pending generic studio command</div>`;
}}
function liveAvailable() {{
  return location.protocol.startsWith("http") && (location.hostname === "127.0.0.1" || location.hostname === "localhost");
}}
function cacheEntry(entry) {{
  if (entry?.id) state.entryDetails[entry.id] = entry;
  return entry;
}}
for (const entry of (CATALOG.entries || [])) cacheEntry(entry);
async function api(path, options = {{}}) {{
  const response = await fetch(path, {{
    cache: "no-store",
    ...options,
    headers: {{ "Content-Type": "application/json", ...(options.headers || {{}}) }}
  }});
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || response.statusText);
  return payload;
}}
function selectedEntry() {{
  return state.entryDetails[state.selectedId]
    || state.results.find(entry => entry.id === state.selectedId)
    || (CATALOG.entries || []).find(entry => entry.id === state.selectedId)
    || null;
}}
async function fetchEntryDetails(id) {{
  if (!id) return null;
  const cached = state.entryDetails[id];
  if (cached?.hasFullDetails) return cached;
  if (!liveAvailable()) return cached || null;
  const entry = await api(`/nikami/catalog/entries/${{encodeURIComponent(id)}}`);
  cacheEntry({{ ...entry, hasFullDetails: true }});
  return state.entryDetails[id];
}}
function localFilteredEntries() {{
  return (CATALOG.entries || []).filter(matches).slice(0, 250);
}}
async function refreshSearch() {{
  const seq = ++state.searchSeq;
  if (!liveAvailable()) {{
    state.searchMode = "embedded-index";
    state.results = localFilteredEntries();
    state.resultCount = state.results.length;
    render();
    return;
  }}
  state.searchMode = "live-api-search";
  state.results = [];
  state.resultCount = null;
  render();
  const params = new URLSearchParams();
  if (state.query.trim()) params.set("q", state.query.trim());
  if (state.domain) params.set("domain", state.domain);
  if (state.kind) params.set("kind", state.kind);
  params.set("limit", "250");
  try {{
    const payload = await api(`/nikami/catalog/search?${{params.toString()}}`);
    if (seq !== state.searchSeq) return;
    state.results = Array.isArray(payload.entries) ? payload.entries : [];
    state.resultCount = Number.isFinite(payload.total) ? payload.total : payload.count;
    state.results.forEach(cacheEntry);
    render();
  }} catch (error) {{
    if (seq !== state.searchSeq) return;
    state.searchMode = "embedded-index";
    state.results = localFilteredEntries();
    state.resultCount = state.results.length;
    addLocalEvent("catalog.search.failed", {{ message: error.message || String(error) }});
    render();
  }}
}}
function selectedParts() {{
  const focus = document.getElementById("partFocusSelect")?.value || "";
  if (focus) return [focus];
  return PARTS.filter(part => state.partEnabled[part] !== false);
}}
function selectedPropSlots() {{
  return selectedParts().filter(part => ["equipment-body", "weapon", "headgear"].includes(part));
}}
function selectedPhases() {{
  const value = document.getElementById("phaseSelect")?.value || "";
  if (value) return [value];
  const focus = document.getElementById("partFocusSelect")?.value || "";
  if (focus && PART_PHASES[focus]) return [PART_PHASES[focus]];
  const entry = selectedEntry();
  return (entry?.phases || []).length ? entry.phases : ["body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"];
}}
function selectedAngles(commandKey) {{
  const value = document.getElementById("angleSelect")?.value || "";
  if (value) return value.split(",");
  return commandKey === "runtimeFrontOnly" ? ["front"] : ["front", "front-left", "front-right"];
}}
function studioPayload(commandKey) {{
  const payload = {{
    entryId: state.selectedId,
    commandKey,
    jobType: document.getElementById("jobTypeSelect")?.value || "appearance",
    partFocus: document.getElementById("partFocusSelect")?.value || "",
    reviewState: document.getElementById("reviewSelect")?.value || "review-pending",
    phases: selectedPhases(),
    angles: selectedAngles(commandKey),
    parts: selectedParts(),
    propSlots: selectedPropSlots()
  }};
  const animation = document.getElementById("animationSelect")?.value || "";
  const dialogue = document.getElementById("dialogueSelect")?.value || "";
  if (animation) payload.animationGroup = animation;
  if (dialogue) payload.dialogueMode = dialogue;
  return payload;
}}
function addLocalEvent(type, payload) {{
  state.events.unshift({{ t: new Date().toISOString(), type, payload }});
  state.events = state.events.slice(0, 80);
  renderEvents();
}}
async function recordEvent(type, payload) {{
  addLocalEvent(type, payload);
  if (!liveAvailable() || !state.session) return;
  try {{
    await api(`/nikami/studio/sessions/${{encodeURIComponent(state.session.id)}}/events`, {{
      method: "POST",
      body: JSON.stringify({{ type, payload }})
    }});
  }} catch (error) {{
    addLocalEvent("event.write.failed", {{ message: error.message || String(error) }});
  }}
}}
async function ensureSession() {{
  if (state.session) return state.session;
  if (!liveAvailable()) throw new Error("live loopback server is not available");
  const entry = selectedEntry();
  state.session = await api("/nikami/studio/sessions", {{
    method: "POST",
    body: JSON.stringify({{ entryId: entry?.id || "" }})
  }});
  addLocalEvent("session.create", {{ id: state.session.id, selectedEntry: entry?.id || "" }});
  renderWorkbench();
  return state.session;
}}
async function launchJob(commandKey) {{
  let entry = selectedEntry();
  if (!entry) return;
  try {{
    entry = await fetchEntryDetails(entry.id) || entry;
    await ensureSession();
    const payload = studioPayload(commandKey);
    addLocalEvent("job.request", payload);
    const job = await api(`/nikami/studio/sessions/${{encodeURIComponent(state.session.id)}}/jobs`, {{
      method: "POST",
      body: JSON.stringify(payload)
    }});
    state.jobs.unshift(job);
    state.latestJob = job;
    renderWorkbench();
    pollJob(job.id);
  }} catch (error) {{
    addLocalEvent("job.failed-to-start", {{ message: error.message || String(error), entryId: entry.id }});
  }}
}}
async function pollJob(id) {{
  for (let attempt = 0; attempt < 720; attempt++) {{
    await new Promise(resolve => setTimeout(resolve, 2000));
    try {{
      const job = await api(`/nikami/actor-kit/jobs/${{encodeURIComponent(id)}}`);
      const index = state.jobs.findIndex(item => item.id === id);
      if (index >= 0) state.jobs[index] = job;
      if (!state.latestJob || state.latestJob.id === id) state.latestJob = job;
      if (job.result?.viewerJsonUrl && !job.manifest) await loadJobManifest(job);
      renderWorkbench();
      if (job.state !== "running") return;
    }} catch (error) {{
      addLocalEvent("job.poll.failed", {{ id, message: error.message || String(error) }});
      return;
    }}
  }}
}}
async function loadJobManifest(job) {{
  const url = job.result?.viewerJsonUrl || "";
  if (!url) return;
  try {{
    const response = await fetch(url, {{ cache: "no-store" }});
    const manifest = await response.json();
    job.manifest = manifest;
    state.latestManifest = manifest;
    state.latestJob = job;
    addLocalEvent("manifest.loaded", {{ jobId: job.id, url }});
  }} catch (error) {{
    addLocalEvent("manifest.load.failed", {{ jobId: job.id, message: error.message || String(error) }});
  }}
}}
function caseImageUrl(job, item) {{
  const image = item?.mainImage || (item?.screenshots || [])[0]?.name || (item?.screenshots || [])[0] || "";
  const base = job?.result?.viewerJsonUrl || location.href;
  if (!image) return "";
  try {{ return new URL(image, new URL(base, location.href)).href; }}
  catch {{ return image; }}
}}
function componentFailureItems(component, manifest) {{
  const failures = Array.isArray(manifest?.failureSummary) ? manifest.failureSummary : [];
  return failures.filter(item => {{
    const category = String(item?.category || "");
    const cls = String(item?.class || "");
    const classes = Array.isArray(item?.classes) ? item.classes.map(String) : [];
    return (component.categories || []).includes(category)
      || (component.classes || []).includes(cls)
      || classes.some(value => (component.classes || []).includes(value));
  }});
}}
function componentCases(component, manifest) {{
  const phases = new Set(component.phases || []);
  return (manifest?.cases || []).filter(item => phases.has(item.phase));
}}
function componentMachineStatus(component) {{
  const job = state.latestJob || {{}};
  const manifest = state.latestManifest || job.manifest || null;
  if (!manifest) return job.state === "running" ? "running" : "not-run";
  const failures = componentFailureItems(component, manifest);
  if (failures.length) return "fail";
  const cases = componentCases(component, manifest);
  if (cases.some(item => item.reportStatus === "FAIL" || item.runtimeGateStatus === "FAIL")) return "fail";
  if (cases.some(item => item.reportStatus === "PASS" || item.runtimeGateStatus === "PASS")) return "pass";
  return "no-proof";
}}
function componentProofUrls(component) {{
  const job = state.latestJob || {{}};
  const manifest = state.latestManifest || job.manifest || null;
  if (!manifest) return [];
  return componentCases(component, manifest)
    .map(item => caseImageUrl(job, item))
    .filter(Boolean)
    .slice(0, 3);
}}
function componentReviewRows(activeOnly = false) {{
  const entry = selectedEntry();
  const activeParts = new Set(selectedParts());
  const payload = studioPayload("runtimeThreeCamera");
  const job = state.latestJob || {{}};
  return REVIEW_COMPONENTS
    .filter(component => !activeOnly || (component.parts || []).some(part => activeParts.has(part)))
    .map(component => {{
      const active = (component.parts || []).some(part => activeParts.has(part));
      return {{
        component: component.id,
        label: component.label,
        active,
        entryId: entry?.id || "",
        jobId: job.id || "",
        reviewState: active ? (document.getElementById("reviewSelect")?.value || "review-pending") : "review-pending",
        machineStatus: componentMachineStatus(component),
        target: entry?.target || "",
        runtimeTarget: entry?.runtimeTarget || entry?.target || "",
        placedTarget: entry?.placedTarget || "",
        phase: (component.phases || []).join(","),
        selectors: {{
          phases: payload.phases,
          angles: payload.angles,
          parts: payload.parts,
          propSlots: payload.propSlots,
          animationGroup: payload.animationGroup || "",
          dialogueMode: payload.dialogueMode || ""
        }},
        manifestUrl: job.result?.viewerJsonUrl || "",
        viewerUrl: job.result?.viewerUrl || "",
        proofUrls: componentProofUrls(component),
        failureCount: componentFailureItems(component, state.latestManifest || job.manifest || null).length
      }};
    }});
}}
function matches(e) {{
  if (state.domain && e.domain !== state.domain) return false;
  if (state.kind && e.kind !== state.kind) return false;
  const q = state.query.trim().toLowerCase();
  if (!q) return true;
  const haystack = [
    e.label,
    e.target,
    e.selectedTarget,
    e.runtimeTarget,
    e.placedTarget,
    e.baseActorTarget,
    e.assemblyTarget,
    e.formId,
    e.actorFormId,
    e.placedRefFormId,
    e.plugin,
    e.recordType,
    e.kind,
    e.model
  ].filter(Boolean).join(" ").toLowerCase();
  return q.split(/\\s+/).filter(Boolean).every(token => haystack.includes(token));
}}
function renderControls() {{
  const phases = ["", "body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk", "creature-model", "creature-body", "creature-animation", "creature-full"];
  document.getElementById("phaseSelect").innerHTML = phases.map(value => `<option value="${{esc(value)}}">${{esc(value || "entry default")}}</option>`).join("");
  document.getElementById("partFocusSelect").innerHTML = [["", "all enabled parts"], ...PARTS.map(part => [part, PART_LABELS[part] || part])]
    .map(([value, label]) => `<option value="${{esc(value)}}">${{esc(label)}}</option>`).join("");
  document.getElementById("jobTypeSelect").innerHTML = [
    ["appearance", "appearance"],
    ["part-isolation", "part isolation"],
    ["weapon", "weapon job"],
    ["animation", "animation job"],
    ["dialogue", "dialogue job"],
    ["easy-pete-face", "Easy Pete face"],
    ["easy-pete-hat", "Easy Pete hat"],
    ["easy-pete-skin", "Easy Pete skin"]
  ].map(([value, label]) => `<option value="${{esc(value)}}">${{esc(label)}}</option>`).join("");
  document.getElementById("animationSelect").innerHTML = ["", "idle", "walkforward", "runforward", "mtidle", "attackleft", "attackright"].map(value => `<option value="${{esc(value)}}">${{esc(value || "none")}}</option>`).join("");
  document.getElementById("dialogueSelect").innerHTML = ["", "mouth-open", "mouth-open-pose"].map(value => `<option value="${{esc(value)}}">${{esc(value || "none")}}</option>`).join("");
  document.getElementById("angleSelect").innerHTML = [
    ["", "default"],
    ["front,front-left,front-right", "front + left + right"],
    ["front", "front only"]
  ].map(([value, label]) => `<option value="${{esc(value)}}">${{esc(label)}}</option>`).join("");
  document.getElementById("reviewSelect").innerHTML = REVIEW_STATES.map(value => `<option value="${{esc(value)}}">${{esc(value.replace(/-/g, " "))}}</option>`).join("");
  document.getElementById("partChecks").innerHTML = PARTS.map(part => `<label class="check"><input type="checkbox" data-part="${{esc(part)}}" ${{state.partEnabled[part] !== false ? "checked" : ""}}> ${{esc(PART_LABELS[part] || part)}}</label>`).join("");
  document.querySelectorAll("#partChecks input").forEach(input => input.onchange = () => {{
    state.partEnabled[input.dataset.part] = input.checked;
    recordEvent("selector.parts", {{ parts: selectedParts() }});
  }});
  ["phaseSelect", "partFocusSelect", "jobTypeSelect", "animationSelect", "dialogueSelect", "angleSelect", "reviewSelect"].forEach(id => {{
    document.getElementById(id).onchange = () => recordEvent("selector.change", studioPayload("runtimeThreeCamera"));
  }});
}}
function renderEvents() {{
  document.getElementById("eventList").innerHTML = state.events.length
    ? state.events.map(event => `<div class="eventItem"><b>${{esc(event.type)}}</b> <span class="muted">${{esc(event.t)}}</span><div class="command">${{esc(JSON.stringify(event.payload || {{}}))}}</div></div>`).join("")
    : `<div class="muted">No session events yet</div>`;
}}
function renderCameras() {{
  const job = state.latestJob;
  const manifest = state.latestManifest || job?.manifest;
  const angles = ["front", "front-left", "front-right"];
  document.getElementById("cameraStrip").innerHTML = angles.map(angle => {{
    const item = (manifest?.cases || []).find(c => c.angle === angle) || null;
    const image = caseImageUrl(job, item);
    const status = item ? (item.reportStatus || item.runtimeGateStatus || "case") : (job ? "not requested" : "idle");
    const message = item
      ? (item.runtimeGateError || (item.failures || []).join("; ") || job?.state || "no screenshot")
      : (job ? "No capture for this angle" : "No capture yet");
    const body = image ? `<a href="${{esc(image)}}"><img src="${{esc(image)}}" alt="${{esc(angle)}}"></a>` : `<div class="cameraBody">${{esc(message)}}</div>`;
    return `<div class="cameraPane"><div class="cameraHead"><span>${{esc(angle)}}</span><span>${{esc(status)}}</span></div>${{body}}</div>`;
  }}).join("");
}}
function renderJobs() {{
  document.getElementById("jobList").innerHTML = state.jobs.length
    ? state.jobs.map(job => {{
      const viewer = job.result?.viewerUrl ? `<a href="${{esc(job.result.viewerUrl)}}">viewer</a>` : "";
      const manifest = job.result?.viewerJsonUrl ? `<a href="${{esc(job.result.viewerJsonUrl)}}">manifest</a>` : "";
      return `<div class="jobItem"><b>${{esc(job.id)}}</b> ${{esc(job.state)}} return=${{esc(job.returnCode ?? "")}} ${{viewer}} ${{manifest}}<div class="command">${{esc(JSON.stringify(job.request || {{}}))}}</div></div>`;
    }}).join("")
    : `<div class="muted">No jobs yet</div>`;
}}
function renderCoords() {{
  const manifest = state.latestManifest || state.latestJob?.manifest;
  const rows = [];
  for (const c of (manifest?.cases || []).slice(0, 12)) {{
    if (c.actorStage) rows.push(`<div class="coordItem"><b>${{esc(c.angle || c.phase)}}</b> stage <code>${{esc(JSON.stringify(c.actorStage))}}</code></div>`);
    if (c.actorCamera) rows.push(`<div class="coordItem"><b>${{esc(c.angle || c.phase)}}</b> camera <code>${{esc(JSON.stringify(c.actorCamera))}}</code></div>`);
    if (c.actorKitSelection) rows.push(`<div class="coordItem"><b>${{esc(c.angle || c.phase)}}</b> selector <code>${{esc(JSON.stringify(c.actorKitSelection))}}</code></div>`);
  }}
  document.getElementById("coordList").innerHTML = rows.join("") || `<div class="muted">No coordinate dump yet</div>`;
}}
function renderStage() {{
  const entry = selectedEntry();
  const manifest = state.latestManifest || state.latestJob?.manifest;
  const firstCase = (manifest?.cases || [])[0] || null;
  const focus = document.getElementById("partFocusSelect")?.value || "";
  const focusLabel = focus ? (PART_LABELS[focus] || focus) : "all enabled parts";
  const jobType = document.getElementById("jobTypeSelect")?.value || "appearance";
  document.getElementById("stageStatus").textContent = manifest ? "latest generated proof loaded" : (entry ? "runtime stage pending" : "select target");
  document.getElementById("stageTarget").innerHTML = entry
    ? `target <code>${{esc(entry.target || entry.label)}}</code> / ${{esc(entry.kind)}}`
    : "no target selected";
  document.getElementById("stageFocus").textContent = `${{focusLabel}} / ${{jobType}}`;
  document.getElementById("stageOverlay").innerHTML = [
    `<span class="pill">${{esc(entry?.label || "no actor")}}</span>`,
    `<span class="pill">${{esc(focusLabel)}}</span>`,
    `<span class="pill">${{esc((firstCase?.phase || selectedPhases()[0] || "phase"))}}</span>`,
    `<span class="pill">${{esc((firstCase?.angle || selectedAngles("runtimeThreeCamera").join("+")))}}</span>`
  ].join("");
}}
function renderSaveReview() {{
  const job = state.latestJob;
  const review = document.getElementById("reviewSelect")?.value || "review-pending";
  document.getElementById("saveState").textContent = state.session ? "Saved session" : "Unsaved";
  document.getElementById("proofState").textContent = job ? `${{job.state || "job"}} / ${{job.result?.status || "no manifest"}}` : "No proof";
  document.getElementById("reviewState").textContent = review.replace(/-/g, " ");
}}
function renderComponentReviews() {{
  const rows = componentReviewRows(false);
  document.getElementById("componentReviews").innerHTML = rows.map(row => {{
    const links = (row.proofUrls || []).map((url, index) => `<a href="${{esc(url)}}">shot ${{index + 1}}</a>`).join("");
    return `<div class="reviewRow ${{row.active ? "active" : ""}}">
      <b>${{esc(row.label)}}</b>
      <span class="pill">${{esc(row.machineStatus)}}</span>
      <span class="pill">${{esc(row.active ? row.reviewState : "inactive")}}</span>
      <div class="proofLinks">${{links || "no proof image yet"}} <span>failures=${{esc(row.failureCount)}}</span></div>
    </div>`;
  }}).join("");
}}
function setSelectValue(id, value) {{
  const node = document.getElementById(id);
  if (node) node.value = value;
}}
function applyPreset(name) {{
  const presets = {{
    "easy-pete-face": {{ query: "easy pete face", phase: "face", part: "face-organs", job: "easy-pete-face", animation: "idle", dialogue: "mouth-open-pose" }},
    "easy-pete-hat": {{ query: "easy pete hat headgear", phase: "headgear", part: "headgear", job: "easy-pete-hat", animation: "idle", dialogue: "" }},
    "easy-pete-skin": {{ query: "easy pete skin body head", phase: "body", part: "body-skin", job: "easy-pete-skin", animation: "idle", dialogue: "" }}
  }};
  const preset = presets[name];
  if (!preset) return;
  document.getElementById("search").value = preset.query;
  state.query = preset.query;
  setSelectValue("phaseSelect", preset.phase);
  setSelectValue("partFocusSelect", preset.part);
  setSelectValue("jobTypeSelect", preset.job);
  setSelectValue("animationSelect", preset.animation);
  setSelectValue("dialogueSelect", preset.dialogue);
  recordEvent("preset.easy-pete", {{ name, payload: studioPayload("runtimeThreeCamera") }});
  refreshSearch();
}}
async function saveReviewEvent() {{
  try {{
    if (!state.session && liveAvailable()) await ensureSession();
    const rows = componentReviewRows(true);
    const payload = {{
      entryId: state.selectedId,
      jobId: state.latestJob?.id || "",
      rows
    }};
    if (liveAvailable() && state.session) {{
      const response = await api(`/nikami/studio/sessions/${{encodeURIComponent(state.session.id)}}/reviews`, {{
        method: "POST",
        body: JSON.stringify(payload)
      }});
      state.reviews = [...(response.rows || []), ...state.reviews].slice(0, 120);
    }}
    await recordEvent("review.mark", {{ ...studioPayload("runtimeThreeCamera"), componentReviewRows: rows }});
    renderWorkbench();
  }} catch (error) {{
    addLocalEvent("review.write.failed", {{ message: error.message || String(error) }});
  }}
}}
function renderWorkbench() {{
  const entry = selectedEntry();
  const live = liveAvailable();
  document.getElementById("liveState").innerHTML = live
    ? `loopback API ready${{state.session ? ` / session <code>${{esc(state.session.id)}}</code>` : ""}}`
    : `open through the live loopback server to run sessions`;
  document.getElementById("selectedEntry").innerHTML = entry
    ? `<b>${{esc(entry.label)}}</b><div class="muted">${{esc(entry.id)}} / ${{esc(entry.kind)}} / ${{esc(entry.classification)}}</div><div class="muted">runtime: <code>${{esc(entry.runtimeTarget || entry.target)}}</code></div><div class="muted">assembly: <code>${{esc(entry.assemblyTarget || "")}}</code> placed: <code>${{esc(entry.placedTarget || "")}}</code></div>`
    : `<span class="muted">Select an actor or creature entry</span>`;
  const runnable = live && entry && entry.domain === "actor";
  document.getElementById("newSession").disabled = !live;
  document.getElementById("runThree").disabled = !runnable;
  document.getElementById("runFront").disabled = !runnable;
  renderCameras();
  renderJobs();
  renderCoords();
  renderStage();
  renderSaveReview();
  renderComponentReviews();
  renderEvents();
}}
function render() {{
  document.getElementById("status").textContent = CATALOG.status;
  document.getElementById("status").className = `pill ${{CATALOG.status}}`;
  const filtered = state.searchMode === "live-api-search" ? state.results : localFilteredEntries();
  const shownTotal = state.searchMode === "live-api-search" && state.resultCount !== null ? state.resultCount : filtered.length;
  document.getElementById("summary").innerHTML = [
    `showing ${{filtered.length}} / ${{shownTotal}}`,
    `actors ${{CATALOG.counts.domains.actor || 0}}`,
    `gameplay ${{CATALOG.counts.domains.gameplay || 0}}`,
    state.searchMode,
    `neutral stage pending`
  ].map(x => `<span class="pill">${{esc(x)}}</span>`).join("");
  document.getElementById("cards").innerHTML = filtered.map(e => `
    <article class="card">
      <div class="head"><div class="label">${{esc(e.label)}}</div><span class="pill ${{esc(e.classification)}}">${{esc(e.classification)}}</span></div>
      <div class="meta">
        <span class="pill">${{esc(e.domain)}}</span>
        <span class="pill">${{esc(e.kind)}}</span>
        <span class="pill">${{esc(e.recordType)}}</span>
        <span class="pill">${{esc(e.plugin)}}</span>
      </div>
      <div class="muted">runtime: <code>${{esc(e.runtimeTarget || e.target)}}</code> form: <code>${{esc(e.formId)}}</code></div>
      ${{e.placedTarget || e.assemblyTarget ? `<div class="muted">placed: <code>${{esc(e.placedTarget || "")}}</code> assembly: <code>${{esc(e.assemblyTarget || "")}}</code></div>` : ""}}
      ${{e.model ? `<div class="muted">model: <code>${{esc(e.model)}}</code></div>` : ""}}
      <div class="actions">
        <button type="button" data-select="${{esc(e.id)}}">Select</button>
        <button type="button" data-run="${{esc(e.id)}}" ${{e.domain === "actor" ? "" : "disabled"}}>Run 3 Camera</button>
      </div>
      <div><b>Three Camera Runtime</b>${{commandBlock(e.commands?.runtimeThreeCamera || "")}}</div>
      <div><b>Front Runtime</b>${{commandBlock(e.commands?.runtimeFrontOnly || "")}}</div>
      <div class="muted">gate: ${{esc((e.studioGates || [{{gate:""}}])[0].gate)}} / ${{esc(e.firstFailingGate || "")}}</div>
    </article>`).join("") || `<div class="card">No matching generated catalog entries</div>`;
  document.querySelectorAll("[data-select]").forEach(button => button.onclick = () => {{
    state.selectedId = button.dataset.select || "";
    recordEvent("entry.select", {{ entryId: state.selectedId }});
    render();
    fetchEntryDetails(state.selectedId).then(() => render()).catch(error => addLocalEvent("entry.detail.failed", {{ entryId: state.selectedId, message: error.message || String(error) }}));
  }});
  document.querySelectorAll("[data-run]").forEach(button => button.onclick = async () => {{
    state.selectedId = button.dataset.run || "";
    render();
    await fetchEntryDetails(state.selectedId).catch(error => addLocalEvent("entry.detail.failed", {{ entryId: state.selectedId, message: error.message || String(error) }}));
    launchJob("runtimeThreeCamera");
  }});
  renderWorkbench();
}}
fillSelect("domain", [...new Set((CATALOG.entries || []).map(e => e.domain).filter(Boolean))].sort());
fillSelect("kind", [...new Set((CATALOG.entries || []).map(e => e.kind).filter(Boolean))].sort());
renderControls();
document.getElementById("newSession").addEventListener("click", async () => {{
  try {{
    state.session = null;
    await ensureSession();
  }} catch (error) {{
    addLocalEvent("session.create.failed", {{ message: error.message || String(error) }});
  }}
}});
document.getElementById("runThree").addEventListener("click", () => launchJob("runtimeThreeCamera"));
document.getElementById("runFront").addEventListener("click", () => launchJob("runtimeFrontOnly"));
document.getElementById("saveReview").addEventListener("click", saveReviewEvent);
document.querySelectorAll("[data-preset]").forEach(button => button.addEventListener("click", () => applyPreset(button.dataset.preset || "")));
document.getElementById("search").addEventListener("input", e => {{ state.query = e.target.value; refreshSearch(); }});
document.getElementById("domain").addEventListener("change", e => {{ state.domain = e.target.value; refreshSearch(); }});
document.getElementById("kind").addEventListener("change", e => {{ state.kind = e.target.value; refreshSearch(); }});
refreshSearch();
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan-json", type=Path)
    parser.add_argument("--content-dir", type=Path)
    parser.add_argument("--proof-root", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--require-pass", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    proof_root = args.proof_root or repo_root.parent / "proof"
    plan_json = args.plan_json
    if not plan_json:
        plan_json = latest_with(proof_root, "fnv-character-viewer-batch-plan", ("viewer-batch-plan.json",)) / "viewer-batch-plan.json"
    content_dir = args.content_dir
    if not content_dir:
        content_dir = latest_with(proof_root, "fnv-content-ledger", ("gameplay-systems.json", "result.json"))

    plan = read_json(plan_json)
    gameplay_rows = read_json(content_dir / "gameplay-systems.json")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = args.out_dir or proof_root / "fnv-character-studio-catalog" / stamp
    out_dir.mkdir(parents=True, exist_ok=True)
    catalog = build_catalog(plan, gameplay_rows, args.limit)
    catalog["sourcePlan"] = str(plan_json)
    catalog["sourceContentLedger"] = str(content_dir / "gameplay-systems.json")
    catalog["artifacts"] = {
        "catalog": str(out_dir / "character-studio-catalog.json"),
        "html": str(out_dir / "character-studio.html"),
    }
    write_json(out_dir / "character-studio-catalog.json", catalog)
    (out_dir / "character-studio.html").write_text(html_doc(catalog), encoding="utf-8")
    print(f"{catalog['status']} {out_dir / 'character-studio-catalog.json'}")
    print(f"studio-html={out_dir / 'character-studio.html'}")
    print(
        "entries total={total} actors={actors} gameplay={gameplay}".format(
            total=catalog["counts"]["total"],
            actors=catalog["counts"]["domains"].get("actor", 0),
            gameplay=catalog["counts"]["domains"].get("gameplay", 0),
        )
    )
    if args.require_pass and catalog["status"] != "PASS":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
