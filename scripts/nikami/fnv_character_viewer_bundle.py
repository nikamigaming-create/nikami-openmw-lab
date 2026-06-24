#!/usr/bin/env python3
"""Generate a standalone FNV character/creature viewer from runtime proof cases."""

from __future__ import annotations

import argparse
import html
import json
import os
import re
from pathlib import Path
from typing import Any


PHASE_ORDER = ["body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk", "full"]
ANGLE_ORDER = ["front", "front-left", "front-right"]
LAYER_ORDER = ["all", "body-skin", "head-skin", "face-organs", "hair-beard", "equipment-body", "weapon", "headgear"]
SKIN_NEEDLES = (
    "skin",
    "FaceGen",
    "EGT",
    "EGM",
    "normal map",
    "subsurface",
    "material tint",
    "diffuse",
    "detail overlay",
    "complexion",
)
HAIR_NEEDLES = ("hair", "beard", "brow", "headgear", "hat", "covered")
ANIMATION_NEEDLES = ("animation", "idle", ".kf", "mouth driver", "dialogue", "TRI morph", "weapon")


def compact_line(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def read_json(path: Path, default: Any) -> Any:
    if not path.is_file():
        return default
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def read_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def rel_path(path: Path, base: Path) -> str:
    try:
        return os.path.relpath(path, base).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")


def unique_ordered(values: list[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def line_matches_actor(line: str, patterns: list[str]) -> bool:
    if not patterns:
        return True
    return any(pattern and pattern in line for pattern in patterns)


def evidence_lines(lines: list[str], patterns: list[str], needles: tuple[str, ...], limit: int = 240) -> list[str]:
    result: list[str] = []
    for line in lines:
        if not any(needle.lower() in line.lower() for needle in needles):
            continue
        if not line_matches_actor(line, patterns):
            continue
        result.append(compact_line(line))
        if len(result) >= limit:
            break
    return result


def normalize_phase(phase: str) -> str:
    lowered = (phase or "full").strip().lower()
    return lowered or "full"


def sort_phases(phases: list[str]) -> list[str]:
    order = {name: index for index, name in enumerate(PHASE_ORDER)}
    return sorted(unique_ordered(phases), key=lambda value: (order.get(value, 999), value))


def sort_angles(angles: list[str]) -> list[str]:
    order = {name: index for index, name in enumerate(ANGLE_ORDER)}
    return sorted(unique_ordered(angles), key=lambda value: (order.get(value, 999), value))


def first_image(case_dir: Path, names: list[str]) -> str:
    for name in names:
        if name.lower().endswith(".png") and (case_dir / name).is_file():
            return name
    pngs = sorted(path.name for path in case_dir.glob("*.png"))
    return pngs[0] if pngs else ""


def load_suite(suite_dir: Path) -> dict[str, Any]:
    suite_json = suite_dir / "character-builder-suite.json"
    raw_results = as_list(read_json(suite_json, []))
    cases: list[dict[str, Any]] = []
    actor = ""
    phases: list[str] = []
    angles: list[str] = []

    for raw in raw_results:
        if not isinstance(raw, dict):
            continue
        case_dir = Path(str(raw.get("caseDir", "")))
        if not case_dir.is_absolute():
            case_dir = suite_dir / case_dir
        report = read_json(case_dir / "character-builder-report.json", {})
        patterns = as_list(report.get("actorPatterns"))
        if not actor:
            actor = str(report.get("actor") or "")
        phase = normalize_phase(str(raw.get("phase") or report.get("phase") or "full"))
        angle = str(raw.get("angle") or "")
        phases.append(phase)
        angles.append(angle)
        log_lines = read_lines(case_dir / "openmw.log")
        screenshots = sorted(
            unique_ordered([str(item) for item in as_list(raw.get("screenshots"))] + [p.name for p in case_dir.glob("*.png")])
        )
        image = first_image(case_dir, screenshots)
        case_data: dict[str, Any] = {
            "case": str(raw.get("case") or f"{phase}_{angle}"),
            "phase": phase,
            "angle": angle,
            "runtimeGateStatus": str(raw.get("runtimeGateStatus") or "MISSING"),
            "runtimeGateError": str(raw.get("runtimeGateError") or ""),
            "reportStatus": str(raw.get("reportStatus") or report.get("status") or "MISSING"),
            "failures": [str(item) for item in as_list(raw.get("failures") or report.get("failures"))],
            "proofDir": str(raw.get("proofDir") or report.get("proofDir") or ""),
            "caseDir": str(case_dir),
            "caseDirRelative": rel_path(case_dir, suite_dir),
            "openmwLog": rel_path(case_dir / "openmw.log", suite_dir),
            "reportJson": rel_path(case_dir / "character-builder-report.json", suite_dir),
            "reportMarkdown": rel_path(case_dir / "character-builder-report.md", suite_dir),
            "screenshots": [{"name": name, "path": rel_path(case_dir / name, suite_dir)} for name in screenshots],
            "mainImage": rel_path(case_dir / image, suite_dir) if image else "",
            "counts": {
                "gates": len(as_list(report.get("gates"))),
                "attachmentBounds": len(as_list(report.get("attachmentBounds"))),
                "runtimePartAudits": len(as_list(report.get("runtimePartAudits"))),
                "runtimeAuditSummary": len(as_list(report.get("runtimeAuditSummary"))),
                "faceDrawables": len(as_list(report.get("faceDrawables"))),
                "morphLines": len(as_list(report.get("morphLines"))),
                "weaponLines": len(as_list(report.get("weaponLines"))),
            },
            "gates": as_list(report.get("gates")),
            "attachmentBounds": as_list(report.get("attachmentBounds")),
            "runtimePartAudits": as_list(report.get("runtimePartAudits")),
            "runtimeAuditSummary": as_list(report.get("runtimeAuditSummary")),
            "faceDrawables": as_list(report.get("faceDrawables")),
            "morphLines": as_list(report.get("morphLines")),
            "weaponLines": as_list(report.get("weaponLines")),
            "skinLines": evidence_lines(log_lines, patterns, SKIN_NEEDLES),
            "hairLines": evidence_lines(log_lines, patterns, HAIR_NEEDLES),
            "animationLines": evidence_lines(log_lines, patterns, ANIMATION_NEEDLES),
        }
        cases.append(case_data)

    overall = "PASS"
    if not cases:
        overall = "MISSING"
    elif any(case["runtimeGateStatus"] != "PASS" or case["reportStatus"] != "PASS" for case in cases):
        overall = "FAIL"

    return {
        "schema": "nikami-fnv-character-viewer-v1",
        "actor": actor,
        "suiteDir": str(suite_dir),
        "overallStatus": overall,
        "phases": sort_phases(phases),
        "angles": sort_angles(angles),
        "layers": LAYER_ORDER,
        "cases": cases,
    }


def html_doc(manifest: dict[str, Any]) -> str:
    data = json.dumps(manifest, ensure_ascii=False)
    data = data.replace("</", "<\\/")
    title = f"FNV Character Viewer - {manifest.get('actor') or 'Actor'}"
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>
:root {{
  color-scheme: dark;
  --bg: #111316;
  --panel: #1a1d22;
  --panel2: #20242a;
  --line: #363c45;
  --text: #eceff3;
  --muted: #aeb6c2;
  --bad: #ff6f61;
  --ok: #64d488;
  --warn: #e8c86a;
  --accent: #70a7ff;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--text); font: 13px/1.4 Segoe UI, Arial, sans-serif; }}
header {{ display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 12px 16px; border-bottom: 1px solid var(--line); background: #15181c; position: sticky; top: 0; z-index: 5; }}
h1 {{ margin: 0; font-size: 16px; font-weight: 600; }}
main {{ padding: 14px 16px 28px; display: grid; gap: 14px; }}
.toolbar, .section {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; }}
.toolbar {{ padding: 10px; display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }}
.group {{ display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }}
.label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }}
button, select {{ border: 1px solid var(--line); border-radius: 5px; background: var(--panel2); color: var(--text); padding: 6px 9px; font: inherit; }}
button.active {{ border-color: var(--accent); background: #26354d; }}
.status {{ border-radius: 99px; padding: 3px 8px; font-weight: 600; }}
.PASS {{ color: #07120b; background: var(--ok); }}
.FAIL {{ color: #190502; background: var(--bad); }}
.MISSING {{ color: #181102; background: var(--warn); }}
.grid3 {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }}
.pane {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; overflow: hidden; min-width: 0; }}
.paneHead {{ display: flex; justify-content: space-between; gap: 8px; padding: 8px 10px; border-bottom: 1px solid var(--line); color: var(--muted); }}
.pane img {{ display: block; width: 100%; height: auto; background: #050607; }}
.empty {{ min-height: 240px; display: grid; place-items: center; color: var(--muted); background: #0b0d10; }}
.section {{ padding: 10px; overflow: auto; }}
.section h2 {{ margin: 0 0 8px; font-size: 14px; }}
table {{ width: 100%; border-collapse: collapse; }}
th, td {{ padding: 6px 7px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }}
th {{ color: var(--muted); font-weight: 600; position: sticky; top: 49px; background: var(--panel); }}
code {{ color: #d8e6ff; }}
.failures {{ color: var(--bad); white-space: pre-wrap; }}
.lineList {{ display: grid; gap: 5px; max-height: 340px; overflow: auto; }}
.line {{ font-family: Consolas, monospace; font-size: 12px; color: #dce6f7; background: #101216; border: 1px solid #2b3038; border-radius: 4px; padding: 5px 6px; }}
a {{ color: #9fc2ff; }}
@media (max-width: 980px) {{ .grid3 {{ grid-template-columns: 1fr; }} header {{ align-items: flex-start; flex-direction: column; }} }}
</style>
</head>
<body>
<header>
  <h1>{html.escape(title)}</h1>
  <div><span id="overall" class="status"></span></div>
</header>
<main>
  <div class="toolbar">
    <div class="group"><span class="label">Phase</span><span id="phaseButtons"></span></div>
    <div class="group"><span class="label">Math Angle</span><span id="angleButtons"></span></div>
    <div class="group"><span class="label">Layer</span><select id="layerSelect"></select></div>
  </div>
  <div id="cameraGrid" class="grid3"></div>
  <div class="section">
    <h2>Case Status</h2>
    <div id="caseStatus"></div>
  </div>
  <div class="section">
    <h2>Assembly Gates</h2>
    <div id="gateTable"></div>
  </div>
  <div class="section">
    <h2>Coordinates</h2>
    <div id="coordTable"></div>
  </div>
  <div class="section">
    <h2>Runtime Drift</h2>
    <div id="driftTable"></div>
  </div>
  <div class="section">
    <h2>Skin Evidence</h2>
    <div id="skinLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Hair Headgear Evidence</h2>
    <div id="hairLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Animation Talk Weapon Evidence</h2>
    <div id="animLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Face Drawables</h2>
    <div id="faceTable"></div>
  </div>
</main>
<script>
const MANIFEST = {data};
const state = {{
  phase: MANIFEST.phases[0] || "full",
  angle: MANIFEST.angles[0] || "front",
  layer: "all"
}};
function esc(value) {{
  return String(value ?? "").replace(/[&<>"']/g, c => ({{"&":"&amp;","<":"&lt;",">":"&gt;","\\"":"&quot;","'":"&#39;"}}[c]));
}}
function statusSpan(value) {{
  const v = value || "MISSING";
  return `<span class="status ${{esc(v)}}">${{esc(v)}}</span>`;
}}
function caseFor(phase, angle) {{
  return MANIFEST.cases.find(c => c.phase === phase && c.angle === angle);
}}
function selectedCase() {{
  return caseFor(state.phase, state.angle) || MANIFEST.cases.find(c => c.phase === state.phase) || MANIFEST.cases[0];
}}
function buttonRow(id, values, current, setter) {{
  const host = document.getElementById(id);
  host.innerHTML = values.map(v => `<button type="button" class="${{v === current ? "active" : ""}}" data-value="${{esc(v)}}">${{esc(v)}}</button>`).join("");
  host.querySelectorAll("button").forEach(btn => btn.addEventListener("click", () => {{ setter(btn.dataset.value); render(); }}));
}}
function table(headers, rows) {{
  if (!rows.length) return `<div class="empty">No data</div>`;
  return `<table><thead><tr>${{headers.map(h => `<th>${{esc(h)}}</th>`).join("")}}</tr></thead><tbody>${{rows.join("")}}</tbody></table>`;
}}
function filteredGates(c) {{
  const gates = c?.gates || [];
  if (state.layer === "all") return gates;
  return gates.filter(g => g.category === state.layer);
}}
function renderImages() {{
  const host = document.getElementById("cameraGrid");
  host.innerHTML = (MANIFEST.angles || []).map(angle => {{
    const c = caseFor(state.phase, angle);
    const image = c?.mainImage || "";
    const body = image ? `<a href="${{esc(image)}}"><img src="${{esc(image)}}" alt="${{esc(c.case)}}"></a>` : `<div class="empty">No screenshot</div>`;
    const failures = (c?.failures || []).join("\\n");
    return `<div class="pane">
      <div class="paneHead"><span>${{esc(angle)}}</span><span>${{statusSpan(c?.reportStatus || "MISSING")}}</span></div>
      ${{body}}
      ${{failures ? `<div class="section failures">${{esc(failures)}}</div>` : ""}}
    </div>`;
  }}).join("");
}}
function renderStatus(c) {{
  const rows = (MANIFEST.angles || []).map(angle => {{
    const item = caseFor(state.phase, angle);
    return `<tr><td>${{esc(item?.case || angle)}}</td><td>${{statusSpan(item?.runtimeGateStatus)}}</td><td>${{statusSpan(item?.reportStatus)}}</td><td><a href="${{esc(item?.openmwLog || "#")}}">log</a></td><td><a href="${{esc(item?.reportJson || "#")}}">json</a></td></tr>`;
  }});
  document.getElementById("caseStatus").innerHTML = table(["Case", "Runtime", "Report", "Log", "JSON"], rows);
}}
function renderGates(c) {{
  const rows = filteredGates(c).map(g => `<tr><td>${{esc(g.action)}}</td><td>${{esc(g.category)}}</td><td>${{esc(g.classification)}}</td><td><code>${{esc(g.model)}}</code></td></tr>`);
  document.getElementById("gateTable").innerHTML = table(["Action", "Category", "Classification", "Model"], rows);
}}
function renderCoords(c) {{
  const bounds = c?.attachmentBounds || [];
  const audits = c?.runtimePartAudits || [];
  const boundRows = bounds.map(b => `<tr><td>attachment</td><td><code>${{esc(b.model)}}</code></td><td>${{esc(b.parent)}}</td><td>${{esc(JSON.stringify(b.headRel))}}</td><td>${{esc(JSON.stringify(b.extent))}}</td><td>${{esc(b.verdict)}}</td></tr>`);
  const auditRows = audits.map(a => `<tr><td>runtime</td><td><code>${{esc(a.part)}}</code></td><td>${{esc(a.class)}}</td><td>${{esc(JSON.stringify(a.relLocal))}}</td><td>${{esc(a.distance)}} / ${{esc(a.limit)}}</td><td>${{esc(a.verdict)}}</td></tr>`);
  document.getElementById("coordTable").innerHTML = table(["Kind", "Part", "Parent/Class", "Head/Rel", "Extent/Distance", "Verdict"], boundRows.concat(auditRows));
}}
function renderDrift(c) {{
  const rows = (c?.runtimeAuditSummary || []).map(a => `<tr><td><code>${{esc(a.part)}}</code></td><td>${{esc(a.class)}}</td><td>${{esc(a.firstVerdict)}} -> ${{esc(a.lastVerdict)}}</td><td>${{esc(a.badCount)}} / ${{esc(a.count)}}</td><td>${{esc(a.firstTimestamp)}} -> ${{esc(a.lastTimestamp)}}</td><td>${{esc(a.maxDistance)}}</td><td>${{esc(JSON.stringify(a.firstRelLocal))}}</td><td>${{esc(JSON.stringify(a.lastRelLocal))}}</td></tr>`);
  document.getElementById("driftTable").innerHTML = table(["Part", "Class", "Verdict", "Bad", "Time", "Max Distance", "First Rel", "Last Rel"], rows);
}}
function renderLines(id, lines) {{
  document.getElementById(id).innerHTML = (lines || []).length
    ? lines.map(line => `<div class="line">${{esc(line)}}</div>`).join("")
    : `<div class="empty">No evidence lines</div>`;
}}
function renderFace(c) {{
  const rows = (c?.faceDrawables || []).map(f => `<tr><td><code>${{esc(f.model)}}</code></td><td>${{esc(f.drawable)}}</td><td>${{esc(f.texture)}}</td><td>${{esc(f.sourceVertices)}}/${{esc(f.renderVertices)}}</td><td>${{esc(JSON.stringify(f.sourceExtent))}}</td><td>${{esc(JSON.stringify(f.renderExtent))}}</td></tr>`);
  document.getElementById("faceTable").innerHTML = table(["Model", "Drawable", "Texture", "Verts", "Source Extent", "Render Extent"], rows);
}}
function render() {{
  document.getElementById("overall").className = `status ${{MANIFEST.overallStatus || "MISSING"}}`;
  document.getElementById("overall").textContent = MANIFEST.overallStatus || "MISSING";
  buttonRow("phaseButtons", MANIFEST.phases, state.phase, value => state.phase = value);
  buttonRow("angleButtons", MANIFEST.angles, state.angle, value => state.angle = value);
  const select = document.getElementById("layerSelect");
  select.innerHTML = MANIFEST.layers.map(layer => `<option value="${{esc(layer)}}">${{esc(layer)}}</option>`).join("");
  select.value = state.layer;
  select.onchange = () => {{ state.layer = select.value; render(); }};
  const c = selectedCase();
  renderImages();
  renderStatus(c);
  renderGates(c);
  renderCoords(c);
  renderDrift(c);
  renderLines("skinLines", c?.skinLines);
  renderLines("hairLines", c?.hairLines);
  renderLines("animLines", [...(c?.animationLines || []), ...(c?.morphLines || []), ...(c?.weaponLines || [])]);
  renderFace(c);
}}
render();
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-html", type=Path)
    args = parser.parse_args()

    suite_dir = args.suite_dir.resolve()
    manifest = load_suite(suite_dir)
    out_json = args.out_json or (suite_dir / "character-viewer-manifest.json")
    out_html = args.out_html or (suite_dir / "character-viewer.html")
    out_json.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    out_html.write_text(html_doc(manifest), encoding="utf-8")
    print(f"viewer-html={out_html}")
    print(f"viewer-json={out_json}")
    print(f"status={manifest['overallStatus']} cases={len(manifest['cases'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
