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
    "WEAP": "weapon",
    "AMMO": "ammo",
    "PROJ": "projectile",
    "EXPL": "explosion",
    "PERK": "perk",
    "AVIF": "actor-value",
}


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
    target = as_text(entry.get("target"))
    actor_kind = as_text(entry.get("actorKind"))
    phases = csv(as_list(entry.get("phases")))
    if not target or actor_kind not in {"npc", "creature"} or not phases:
        return ""
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(target)} -ActorKind {actor_kind} -Phases {shell_quote(phases)} "
        f"-Angles {shell_quote(angles)} -OpenViewer -LiveServe"
    )
    if actor_kind == "creature":
        command += " -CreatureDiagnostics"
    return command


def actor_entry(row: dict[str, Any], index: int) -> dict[str, Any]:
    label = (
        as_text(row.get("placedRefEditorId"))
        or as_text(row.get("actorEditorId"))
        or as_text(row.get("target"))
        or as_text(row.get("actorFormId"))
        or f"actor-{index:06d}"
    )
    classification = normalize_classification(as_text(row.get("classification")))
    entry = {
        "id": f"actor:{index:06d}",
        "domain": "actor",
        "kind": as_text(row.get("actorKind")) or "unknown",
        "recordType": as_text(row.get("recordType")),
        "label": label,
        "target": as_text(row.get("target")),
        "plugin": as_text(row.get("plugin")),
        "formId": as_text(row.get("actorFormId")),
        "editorId": as_text(row.get("actorEditorId")),
        "source": as_text(row.get("source")),
        "classification": classification,
        "readiness": classification,
        "firstFailingGate": as_text(row.get("firstFailingGate")),
        "model": "",
        "icon": "",
        "componentCounts": row.get("componentCounts", {}),
        "placement": row.get("placement", {}),
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
        "payloadPolicy": "generated identifiers and commands only; no retail payload bytes",
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
        entry["source"],
        entry["componentCounts"],
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


def html_doc(catalog: dict[str, Any]) -> str:
    data = json.dumps(catalog, ensure_ascii=False).replace("</", "<\\/")
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
main {{ padding: 14px 16px 28px; display: grid; gap: 10px; }}
.bar {{ display: grid; grid-template-columns: minmax(240px, 1fr) 170px 170px; gap: 8px; }}
input, select, button {{ border: 1px solid var(--line); border-radius: 5px; background: var(--panel2); color: var(--text); padding: 7px 9px; font: inherit; }}
.summary {{ display: flex; flex-wrap: wrap; gap: 8px; color: var(--muted); }}
.pill {{ border-radius: 99px; padding: 3px 8px; background: var(--panel2); border: 1px solid var(--line); }}
.PASS {{ color: #07120b; background: var(--ok); }}
.FAIL {{ color: #190502; background: var(--bad); }}
.loaded-pending-runtime {{ color: #181102; background: var(--warn); }}
.runtime-supported {{ color: #07120b; background: var(--ok); }}
.known-blocked {{ color: #190502; background: var(--bad); }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(360px, 1fr)); gap: 10px; }}
.card {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 10px; display: grid; gap: 7px; min-width: 0; }}
.head {{ display: flex; justify-content: space-between; gap: 8px; }}
.label {{ font-weight: 600; overflow-wrap: anywhere; }}
.muted {{ color: var(--muted); }}
.meta {{ display: flex; flex-wrap: wrap; gap: 6px; }}
.command {{ font-family: Consolas, monospace; font-size: 12px; background: #0d0f12; border: 1px solid #2b3038; border-radius: 4px; padding: 6px; white-space: pre-wrap; overflow-wrap: anywhere; max-height: 96px; overflow: auto; }}
.gate {{ color: var(--warn); }}
@media (max-width: 760px) {{ .bar {{ grid-template-columns: 1fr; }} }}
</style>
</head>
<body>
<header>
  <h1>FNV Character Studio Catalog <span id="status" class="pill"></span></h1>
  <div class="bar">
    <input id="search" placeholder="Search actors, creatures, weapons, ammo, projectiles, IDs, models">
    <select id="domain"><option value="">all domains</option></select>
    <select id="kind"><option value="">all kinds</option></select>
  </div>
  <div id="summary" class="summary"></div>
</header>
<main>
  <div id="cards" class="grid"></div>
</main>
<script>
const CATALOG = {data};
const state = {{ query: "", domain: "", kind: "" }};
function esc(v) {{ return String(v ?? "").replace(/[&<>"']/g, c => ({{"&":"&amp;","<":"&lt;",">":"&gt;","\\"":"&quot;","'":"&#39;"}}[c])); }}
function fillSelect(id, values) {{
  const node = document.getElementById(id);
  node.innerHTML = `<option value="">all ${{id}}s</option>` + values.map(v => `<option value="${{esc(v)}}">${{esc(v)}}</option>`).join("");
}}
function commandBlock(command) {{
  return command ? `<div class="command">${{esc(command)}}</div>` : `<div class="gate">pending generic studio command</div>`;
}}
function matches(e) {{
  if (state.domain && e.domain !== state.domain) return false;
  if (state.kind && e.kind !== state.kind) return false;
  const q = state.query.trim().toLowerCase();
  if (!q) return true;
  const haystack = String(e.searchText || "");
  return q.split(/\\s+/).filter(Boolean).every(token => haystack.includes(token));
}}
function render() {{
  document.getElementById("status").textContent = CATALOG.status;
  document.getElementById("status").className = `pill ${{CATALOG.status}}`;
  const filtered = (CATALOG.entries || []).filter(matches).slice(0, 250);
  document.getElementById("summary").innerHTML = [
    `showing ${{filtered.length}} / ${{CATALOG.counts.total}}`,
    `actors ${{CATALOG.counts.domains.actor || 0}}`,
    `gameplay ${{CATALOG.counts.domains.gameplay || 0}}`,
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
      <div class="muted">target: <code>${{esc(e.target)}}</code> form: <code>${{esc(e.formId)}}</code></div>
      ${{e.model ? `<div class="muted">model: <code>${{esc(e.model)}}</code></div>` : ""}}
      <div><b>Three Camera Runtime</b>${{commandBlock(e.commands?.runtimeThreeCamera || "")}}</div>
      <div><b>Front Runtime</b>${{commandBlock(e.commands?.runtimeFrontOnly || "")}}</div>
      <div class="muted">gate: ${{esc((e.studioGates || [{{gate:""}}])[0].gate)}} / ${{esc(e.firstFailingGate || "")}}</div>
    </article>`).join("") || `<div class="card">No matching generated catalog entries</div>`;
}}
fillSelect("domain", [...new Set((CATALOG.entries || []).map(e => e.domain).filter(Boolean))].sort());
fillSelect("kind", [...new Set((CATALOG.entries || []).map(e => e.kind).filter(Boolean))].sort());
document.getElementById("search").addEventListener("input", e => {{ state.query = e.target.value; render(); }});
document.getElementById("domain").addEventListener("change", e => {{ state.domain = e.target.value; render(); }});
document.getElementById("kind").addEventListener("change", e => {{ state.kind = e.target.value; render(); }});
render();
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
