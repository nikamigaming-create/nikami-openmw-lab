#!/usr/bin/env python3
"""Build a math-first FNV character builder report from a runtime proof directory."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


FLOAT3_RE = r"\(([^)]*)\)"


def parse_vec3(text: str) -> list[float]:
    values: list[float] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            values.append(float(part))
        except ValueError:
            values.append(float("nan"))
    return values


def compact_line(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def timestamp_from_line(line: str) -> str:
    match = re.match(r"\[([^\]]+)\]", line)
    return match.group(1) if match else ""


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def actor_patterns(lines: list[str], actor: str) -> list[str]:
    patterns = [actor]
    active_re = re.compile(
        rf'active-cell actor match target="{re.escape(actor)}".*?\bref=([^ ]+)\s+base=([^ ]+)'
    )
    for line in lines:
        match = active_re.search(line)
        if not match:
            continue
        for value in match.groups():
            if value not in patterns:
                patterns.append(value)
    return patterns


def line_matches_actor(line: str, patterns: list[str]) -> bool:
    return any(pattern in line for pattern in patterns)


def parse_builder_gates(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    gate_re = re.compile(
        r"FNV/ESM4 CHARACTER BUILDER (?P<action>include|skip) "
        r"phase=(?P<phase>[^ ]+) category=(?P<category>[^ ]+) actor=(?P<actor>[^ ]+) "
        r"ref=(?P<ref>[^ ]+) model=(?P<model>.*?) classification=(?P<classification>[^ ]+)"
    )
    gates: list[dict[str, Any]] = []
    for line in lines:
        match = gate_re.search(line)
        if not match:
            continue
        item = match.groupdict()
        if not line_matches_actor(f"{item['actor']} {item['ref']}", patterns):
            continue
        gates.append(
            {
                "action": item["action"],
                "phase": item["phase"],
                "category": item["category"],
                "actor": item["actor"],
                "ref": item["ref"],
                "model": item["model"],
                "classification": item["classification"],
                "line": compact_line(line),
            }
        )
    return gates


def parse_attachment_bounds(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    bounds_re = re.compile(
        r"attachment bounds (?P<model>.*?) for (?P<ref>[^ ]+) parent=(?P<parent>.*?) "
        rf"center={FLOAT3_RE} extent={FLOAT3_RE} worldCenter={FLOAT3_RE} "
        rf"attachLocal={FLOAT3_RE} headRel={FLOAT3_RE} headLocal={FLOAT3_RE} "
        rf"headFrame={FLOAT3_RE}.*? centerDistance=(?P<centerDistance>[-+0-9.eE]+) "
        r"diagonal=(?P<diagonal>[-+0-9.eE]+).*? verdict=(?P<verdict>[^ ]+)"
    )
    items: list[dict[str, Any]] = []
    for line in lines:
        match = bounds_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        if not line_matches_actor(data["ref"], patterns):
            continue
        groups = match.groups()
        items.append(
            {
                "model": data["model"],
                "ref": data["ref"],
                "parent": data["parent"],
                "center": parse_vec3(groups[3]),
                "extent": parse_vec3(groups[4]),
                "worldCenter": parse_vec3(groups[5]),
                "attachLocal": parse_vec3(groups[6]),
                "headRel": parse_vec3(groups[7]),
                "headLocal": parse_vec3(groups[8]),
                "headFrame": parse_vec3(groups[9]),
                "centerDistance": float(data["centerDistance"]),
                "diagonal": float(data["diagonal"]),
                "verdict": data["verdict"],
                "line": compact_line(line),
            }
        )
    return items


def parse_runtime_audits(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    audit_re = re.compile(
        r"runtime part audit (?P<ref>[^ ]+) part='(?P<part>[^']*)' class=(?P<class>[^ ]+) "
        rf"center={FLOAT3_RE} anchor={FLOAT3_RE} .*? relLocal={FLOAT3_RE} "
        r"distance=(?P<distance>[-+0-9.eE]+) limit=(?P<limit>[-+0-9.eE]+).*? verdict=(?P<verdict>[^ ]+)"
    )
    items: list[dict[str, Any]] = []
    for line in lines:
        match = audit_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        if not line_matches_actor(data["ref"], patterns):
            continue
        groups = match.groups()
        items.append(
            {
                "ref": data["ref"],
                "part": data["part"],
                "class": data["class"],
                "timestamp": timestamp_from_line(line),
                "center": parse_vec3(groups[3]),
                "anchor": parse_vec3(groups[4]),
                "relLocal": parse_vec3(groups[5]),
                "distance": float(data["distance"]),
                "limit": float(data["limit"]),
                "verdict": data["verdict"],
                "line": compact_line(line),
            }
        )
    return items


def parse_face_drawables(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    drawable_re = re.compile(
        r"FACE DRAWABLE AUDIT (?P<ref>.*?) model=(?P<model>.*?) phase=(?P<phase>[^ ]+) "
        r"drawable='(?P<drawable>[^']*)'.*?sourceVertices=(?P<sourceVertices>[0-9]+).*?"
        r"renderVertices=(?P<renderVertices>[0-9]+).*?texture=(?P<texture>[^ ]+).*?"
        rf"renderExtent={FLOAT3_RE}.*?sourceExtent={FLOAT3_RE}"
    )
    items: list[dict[str, Any]] = []
    for line in lines:
        match = drawable_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        ref_text = data["ref"]
        if not line_matches_actor(ref_text, patterns):
            continue
        groups = match.groups()
        items.append(
            {
                "ref": ref_text,
                "model": data["model"],
                "phase": data["phase"],
                "drawable": data["drawable"],
                "sourceVertices": int(data["sourceVertices"]),
                "renderVertices": int(data["renderVertices"]),
                "texture": data["texture"],
                "renderExtent": parse_vec3(groups[7]),
                "sourceExtent": parse_vec3(groups[8]),
                "line": compact_line(line),
            }
        )
    return items


def summarize_runtime_audits(audits: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, str], dict[str, Any]] = {}
    order: list[tuple[str, str, str]] = []
    for item in audits:
        key = (item["ref"], item["part"], item["class"])
        if key not in grouped:
            order.append(key)
            grouped[key] = {
                "ref": item["ref"],
                "part": item["part"],
                "class": item["class"],
                "count": 0,
                "badCount": 0,
                "firstVerdict": item["verdict"],
                "lastVerdict": item["verdict"],
                "firstDistance": item["distance"],
                "lastDistance": item["distance"],
                "maxDistance": item["distance"],
                "firstRelLocal": item["relLocal"],
                "lastRelLocal": item["relLocal"],
                "firstTimestamp": item.get("timestamp", ""),
                "lastTimestamp": item.get("timestamp", ""),
                "firstLine": item["line"],
                "lastLine": item["line"],
                "badLines": [],
                "transitions": [],
            }
        summary = grouped[key]
        previous_verdict = summary["lastVerdict"]
        summary["count"] += 1
        summary["lastVerdict"] = item["verdict"]
        summary["lastDistance"] = item["distance"]
        summary["lastRelLocal"] = item["relLocal"]
        summary["lastTimestamp"] = item.get("timestamp", "")
        summary["lastLine"] = item["line"]
        if item["distance"] > summary["maxDistance"]:
            summary["maxDistance"] = item["distance"]
        if item["verdict"] != "OK":
            summary["badCount"] += 1
            if len(summary["badLines"]) < 4:
                summary["badLines"].append(item["line"])
        if item["verdict"] != previous_verdict:
            summary["transitions"].append(
                {
                    "timestamp": item.get("timestamp", ""),
                    "from": previous_verdict,
                    "to": item["verdict"],
                    "distance": item["distance"],
                    "line": item["line"],
                }
            )

    result = []
    for key in order:
        summary = grouped[key]
        summary["regressed"] = summary["firstVerdict"] == "OK" and summary["badCount"] > 0
        result.append(summary)
    return result


def summarize_categories(gates: list[dict[str, Any]]) -> dict[str, dict[str, int]]:
    summary: dict[str, dict[str, int]] = {}
    for gate in gates:
        category = gate["category"]
        summary.setdefault(category, {"include": 0, "skip": 0})
        summary[category][gate["action"]] += 1
    return summary


def collect_matching(lines: list[str], patterns: list[str], needle: str) -> list[str]:
    result: list[str] = []
    for line in lines:
        if needle in line and line_matches_actor(line, patterns):
            result.append(compact_line(line))
    return result


def evaluate(
    proof_dir: Path,
    actor: str,
    phase: str,
    lines: list[str],
    patterns: list[str],
    gates: list[dict[str, Any]],
    bounds: list[dict[str, Any]],
    audits: list[dict[str, Any]],
    drawables: list[dict[str, Any]],
) -> tuple[str, list[str]]:
    failures: list[str] = []
    screenshots = sorted(p.name for p in proof_dir.glob("*.png"))
    if not screenshots:
        failures.append("missing screenshots")
    if phase and not gates:
        failures.append("missing character builder phase gate lines")
    bad_class = [
        gate
        for gate in gates
        if gate["action"] == "skip" and gate["classification"] != "intentionally-excluded-with-proof"
    ]
    if bad_class:
        failures.append(f"phase skip without proof classification: {len(bad_class)}")
    suspect_bounds = [item for item in bounds if item["verdict"] != "OK"]
    if suspect_bounds:
        failures.append(f"suspect attachment bounds: {len(suspect_bounds)}")
    bad_audits = [item for item in audits if item["verdict"] != "OK"]
    if bad_audits:
        failures.append(f"bad runtime part audits: {len(bad_audits)}")
    regressions = [item for item in summarize_runtime_audits(audits) if item["regressed"]]
    if regressions:
        failures.append(f"runtime audit regressions after initial OK: {len(regressions)}")
    collapsed_heads = []
    for item in drawables:
        model = item["model"].replace("\\", "/").lower()
        if "/head/head" not in model:
            continue
        extent = item["sourceExtent"]
        if item["sourceVertices"] <= 0 or (len(extent) == 3 and max(extent) <= 3.0):
            collapsed_heads.append(item)
    if collapsed_heads:
        failures.append(f"collapsed or empty head source geometry: {len(collapsed_heads)}")
    if phase in {"weapon", "weapons", "headgear", "talk", "dialogue", "full"}:
        weapon_lines = collect_matching(lines, patterns, "equipped NPC weapon")
        if not weapon_lines:
            failures.append("missing equipped weapon evidence")
    if phase in {"talk", "dialogue"}:
        talk_lines = [
            line
            for line in lines
            if (
                "installed mouth open driver" in line
                or "mouth driver active" in line
                or "applied FaceGen TRI morph" in line
                or "installed FaceGen TRI morph driver" in line
            )
            and line_matches_actor(line, patterns)
        ]
        if not talk_lines:
            failures.append("missing talk/mouth runtime evidence")
    if not collect_matching(lines, patterns, "FACE CHECK"):
        failures.append("missing FACE CHECK for actor")

    return ("PASS" if not failures else "FAIL", failures)


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"# FNV Character Builder Report - {report['actor']}")
    lines.append("")
    lines.append(f"Status: **{report['status']}**")
    lines.append(f"Phase: `{report['phase']}`")
    lines.append(f"Proof: `{report['proofDir']}`")
    lines.append("")
    lines.append("## Actor")
    lines.append("")
    lines.append(f"Patterns: `{', '.join(report['actorPatterns'])}`")
    lines.append(f"Screenshots: {len(report['screenshots'])}")
    lines.append("")
    lines.append("## Gates")
    lines.append("")
    lines.append("| Category | Include | Skip |")
    lines.append("|---|---:|---:|")
    for category, counts in sorted(report["categorySummary"].items()):
        lines.append(f"| {category} | {counts.get('include', 0)} | {counts.get('skip', 0)} |")
    lines.append("")
    lines.append("## Math")
    lines.append("")
    lines.append(f"Attachment bounds: {len(report['attachmentBounds'])}")
    lines.append(f"Runtime part audits: {len(report['runtimePartAudits'])}")
    lines.append(f"Runtime audit summaries: {len(report['runtimeAuditSummary'])}")
    lines.append(f"Face drawable audits: {len(report['faceDrawables'])}")
    lines.append(f"TRI/EGM/talk lines: {len(report['morphLines'])}")
    lines.append(f"Weapon lines: {len(report['weaponLines'])}")
    lines.append("")
    if report["failures"]:
        lines.append("## Failures")
        lines.append("")
        for failure in report["failures"]:
            lines.append(f"- {failure}")
        lines.append("")
    lines.append("## First Attachments")
    lines.append("")
    lines.append("| Model | Parent | HeadRel | Extent | Verdict |")
    lines.append("|---|---|---|---|---|")
    for item in report["attachmentBounds"][:24]:
        lines.append(
            f"| {item['model']} | {item['parent']} | {item['headRel']} | {item['extent']} | {item['verdict']} |"
        )
    lines.append("")
    lines.append("## Runtime Drift")
    lines.append("")
    lines.append("| Part | First | Last | Bad | Max Distance | First Rel | Last Rel |")
    lines.append("|---|---|---|---:|---:|---|---|")
    for item in report["runtimeAuditSummary"][:32]:
        lines.append(
            f"| {item['part']} | {item['firstVerdict']} | {item['lastVerdict']} | {item['badCount']} | "
            f"{item['maxDistance']:.4g} | {item['firstRelLocal']} | {item['lastRelLocal']} |"
        )
    lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--proof-dir", required=True, type=Path)
    parser.add_argument("--actor", default="GSEasyPete")
    parser.add_argument("--phase", default="")
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    args = parser.parse_args()

    proof_dir = args.proof_dir.resolve()
    log_path = proof_dir / "openmw.log"
    if not log_path.is_file():
        raise SystemExit(f"Missing OpenMW log: {log_path}")

    lines = read_lines(log_path)
    patterns = actor_patterns(lines, args.actor)
    gates = parse_builder_gates(lines, patterns)
    bounds = parse_attachment_bounds(lines, patterns)
    audits = parse_runtime_audits(lines, patterns)
    drawables = parse_face_drawables(lines, patterns)
    morph_lines = [
        compact_line(line)
        for line in lines
        if (
            "FaceGen TRI" in line
            or "FaceGen EGM" in line
            or "dialogue morph" in line
            or "mouth driver" in line
            or "dialogue pose" in line
        )
        and line_matches_actor(line, patterns)
    ]
    weapon_lines = [
        compact_line(line)
        for line in lines
        if ("equipped NPC weapon" in line or "weapon metadata" in line or "weapon sound files" in line)
        and line_matches_actor(line, patterns)
    ]
    status, failures = evaluate(
        proof_dir,
        args.actor,
        args.phase.lower(),
        lines,
        patterns,
        gates,
        bounds,
        audits,
        drawables,
    )

    report: dict[str, Any] = {
        "status": status,
        "failures": failures,
        "proofDir": str(proof_dir),
        "actor": args.actor,
        "phase": args.phase,
        "actorPatterns": patterns,
        "screenshots": sorted(p.name for p in proof_dir.glob("*.png")),
        "categorySummary": summarize_categories(gates),
        "gates": gates,
        "attachmentBounds": bounds,
        "runtimePartAudits": audits,
        "runtimeAuditSummary": summarize_runtime_audits(audits),
        "faceDrawables": drawables,
        "morphLines": morph_lines,
        "weaponLines": weapon_lines,
    }

    out_json = args.out_json or (proof_dir / "character-builder-report.json")
    out_md = args.out_md or (proof_dir / "character-builder-report.md")
    out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    write_markdown(out_md, report)
    print(f"{status} {out_json}")
    return 0 if status == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
