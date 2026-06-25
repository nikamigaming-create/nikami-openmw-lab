#!/usr/bin/env python3
"""Build a math-first FNV character builder report from a runtime proof directory."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


FLOAT3_RE = r"\(([^)]*)\)"
FLOAT_RE = r"[-+0-9.eE]+"
CREATURE_RUNTIME_NEEDLES = (
    "inserted creature animation for",
    "creature animation groups",
    "attached creature body nif",
    "forced creature body render mask",
    "creature bounds",
    "creature KF candidate",
    "creature KF global candidate",
)
NEUTRAL_PREVIEW_PANES = (
    {
        "name": "full-body",
        "centerNdcX": -0.64,
        "widthNdc": 0.58,
        "heightNdc": 1.03,
        "minForegroundFraction": 0.025,
        "maxForegroundFraction": 0.35,
        "allowedTouchEdges": ("bottom",),
    },
    {
        "name": "face-hat",
        "centerNdcX": 0.0,
        "widthNdc": 0.58,
        "heightNdc": 1.03,
        "minForegroundFraction": 0.05,
        "maxForegroundFraction": 0.42,
        "allowedTouchEdges": ("bottom",),
    },
    {
        "name": "right-hand-weapon",
        "centerNdcX": 0.64,
        "widthNdc": 0.58,
        "heightNdc": 1.03,
        "minForegroundFraction": 0.010,
        "maxForegroundFraction": 0.25,
        "allowedTouchEdges": (),
    },
)

NEUTRAL_PREVIEW_PHASE_PANES = {
    "body": {"full-body"},
    "equipment": {"full-body"},
    "face": {"face-hat"},
    "head": {"face-hat"},
    "hair": {"face-hat"},
    "headgear": {"face-hat"},
    "talk": {"face-hat"},
    "dialogue": {"face-hat"},
    "weapon": {"right-hand-weapon"},
    "weapons": {"right-hand-weapon"},
    "full": {"full-body", "face-hat", "right-hand-weapon"},
    "": {"full-body", "face-hat", "right-hand-weapon"},
}


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


def vec_delta(first: list[float] | None, last: list[float] | None) -> list[float]:
    if not first or not last or len(first) != len(last):
        return []
    return [b - a for a, b in zip(first, last)]


def max_abs_vec_delta(delta: list[float]) -> float:
    if not delta:
        return 0.0
    return max(abs(value) for value in delta)


def compact_line(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def timestamp_from_line(line: str) -> str:
    match = re.match(r"\[([^\]]+)\]", line)
    return match.group(1) if match else ""


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def matches_known_actor_value(value: str | None, patterns: list[str]) -> bool:
    if not value:
        return False
    normalized = value.strip().strip('"')
    if not normalized:
        return False
    return any(normalized == pattern.strip().strip('"') for pattern in patterns)


def actor_patterns(lines: list[str], actor: str) -> list[str]:
    patterns = [actor]
    active_re = re.compile(
        rf'active-cell actor match target="{re.escape(actor)}".*?\bref=([^ ]+)\s+base=([^ ]+)'
    )
    assembly_re = re.compile(
        rf'actor part assembly target match target="{re.escape(actor)}".*?\bactor=([^ ]+) '
        r"refAlias=([^ ]+) ref=([^ ]+) baseEditor=([^ ]+) baseForm=([^ ]+)"
    )
    quoted_re = re.compile(r'\b(baseEditor|baseFull)="([^"]*)"')
    for line in lines:
        matched_actor_line = False
        match = active_re.search(line)
        if match:
            matched_actor_line = True
            for value in match.groups():
                if value and value not in patterns:
                    patterns.append(value)
        assembly_match = assembly_re.search(line)
        if assembly_match and any(matches_known_actor_value(value, patterns) for value in assembly_match.groups()):
            matched_actor_line = True
            for value in assembly_match.groups():
                if value and value not in patterns:
                    patterns.append(value)
        if not matched_actor_line:
            continue
        for quoted in quoted_re.finditer(line):
            value = quoted.group(2)
            if value and value not in patterns:
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


def parse_head_surface_offsets(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    offset_re = re.compile(
        r"applied head frame surface offset model=(?P<model>.*?) "
        rf"offset={FLOAT3_RE} rotationPrefix=(?P<rotationPrefix>[^ ]+) "
        rf"pivot={FLOAT3_RE} pivotMode=(?P<pivotMode>[^ ]+) for (?P<ref>.+)$"
    )
    items: list[dict[str, Any]] = []
    for line in lines:
        match = offset_re.search(line)
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
                "offset": parse_vec3(groups[1]),
                "rotationPrefix": data["rotationPrefix"],
                "pivot": parse_vec3(groups[3]),
                "pivotMode": data["pivotMode"],
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
    matrix_re = re.compile(
        r"PART MATRIX AUDIT (?P<ref>[^ ]+) part='(?P<part>[^']*)' class=(?P<class>[^ ]+) "
        rf"center={FLOAT3_RE} anchor={FLOAT3_RE} "
        rf"partWorldTrans={FLOAT3_RE} parentWorldTrans={FLOAT3_RE} "
        rf"partInParentTrans={FLOAT3_RE} partInAnchorTrans={FLOAT3_RE} "
        rf"partWorldQuat={FLOAT3_RE} anchorWorldQuat={FLOAT3_RE} partInAnchorQuat={FLOAT3_RE} "
        rf"anchorAngleDeg=(?P<anchorAngleDeg>{FLOAT_RE}) "
        r"partHandedness=(?P<partHandedness>[-+0-9.eE]+) anchorHandedness=(?P<anchorHandedness>[-+0-9.eE]+)"
    )
    active_re = re.compile(
        r"manually applied (?P<applied>[0-9]+) active keyframe controller\(s\) for (?P<ref>[^ ]+) "
        r".*?activeGroups=\[(?P<activeGroups>[^\]]*)\]"
    )
    group_re = re.compile(
        rf"(?P<blendMask>[0-9]+):(?P<group>[^@|]+)@t=(?P<time>{FLOAT_RE})(?: src=(?P<source>[^|]+))?"
    )
    items: list[dict[str, Any]] = []
    pending_matrices: dict[tuple[str, str, str], dict[str, Any]] = {}
    sample_counts: dict[tuple[str, str, str], int] = {}
    active_state_by_ref: dict[str, dict[str, Any]] = {}
    for line in lines:
        active_match = active_re.search(line)
        if active_match:
            ref = active_match.group("ref")
            groups = []
            for group_match in group_re.finditer(active_match.group("activeGroups")):
                groups.append(
                    {
                        "blendMask": int(group_match.group("blendMask")),
                        "group": group_match.group("group").strip(),
                        "time": float(group_match.group("time")),
                        "source": (group_match.group("source") or "").strip(),
                    }
                )
            active_state_by_ref[ref] = {
                "timestamp": timestamp_from_line(line),
                "appliedControllers": int(active_match.group("applied")),
                "groups": groups,
                "line": compact_line(line),
            }

        matrix_match = matrix_re.search(line)
        if matrix_match:
            data = matrix_match.groupdict()
            if line_matches_actor(data["ref"], patterns):
                groups = matrix_match.groups()
                key = (data["ref"], data["part"], data["class"])
                pending_matrices[key] = {
                    "timestamp": timestamp_from_line(line),
                    "center": parse_vec3(groups[3]),
                    "anchor": parse_vec3(groups[4]),
                    "partWorldTrans": parse_vec3(groups[5]),
                    "parentWorldTrans": parse_vec3(groups[6]),
                    "partInParentTrans": parse_vec3(groups[7]),
                    "partInAnchorTrans": parse_vec3(groups[8]),
                    "partWorldQuat": parse_vec3(groups[9]),
                    "anchorWorldQuat": parse_vec3(groups[10]),
                    "partInAnchorQuat": parse_vec3(groups[11]),
                    "anchorAngleDeg": float(data["anchorAngleDeg"]),
                    "partHandedness": float(data["partHandedness"]),
                    "anchorHandedness": float(data["anchorHandedness"]),
                    "line": compact_line(line),
                }

        match = audit_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        if not line_matches_actor(data["ref"], patterns):
            continue
        groups = match.groups()
        key = (data["ref"], data["part"], data["class"])
        sample_counts[key] = sample_counts.get(key, 0) + 1
        active_state = active_state_by_ref.get(data["ref"], {})
        active_groups = active_state.get("groups", [])
        primary_group = active_groups[0] if active_groups else {}
        item = {
            "ref": data["ref"],
            "part": data["part"],
            "class": data["class"],
            "sampleIndex": sample_counts[key],
            "timestamp": timestamp_from_line(line),
            "activeAnimationTimestamp": active_state.get("timestamp", ""),
            "activeControllers": active_state.get("appliedControllers", 0),
            "activeGroups": active_groups,
            "animationGroup": primary_group.get("group", ""),
            "animationTime": primary_group.get("time"),
            "animationSource": primary_group.get("source", ""),
            "center": parse_vec3(groups[3]),
            "anchor": parse_vec3(groups[4]),
            "relLocal": parse_vec3(groups[5]),
            "distance": float(data["distance"]),
            "limit": float(data["limit"]),
            "verdict": data["verdict"],
            "line": compact_line(line),
        }
        matrix = pending_matrices.get(key)
        if matrix:
            item["matrix"] = matrix
            item["partWorldTrans"] = matrix["partWorldTrans"]
            item["parentWorldTrans"] = matrix["parentWorldTrans"]
            item["partInParentTrans"] = matrix["partInParentTrans"]
            item["partInAnchorTrans"] = matrix["partInAnchorTrans"]
            item["partWorldQuat"] = matrix["partWorldQuat"]
            item["anchorWorldQuat"] = matrix["anchorWorldQuat"]
            item["partInAnchorQuat"] = matrix["partInAnchorQuat"]
            item["anchorAngleDeg"] = matrix["anchorAngleDeg"]
            item["partHandedness"] = matrix["partHandedness"]
            item["anchorHandedness"] = matrix["anchorHandedness"]
            item["matrixLine"] = matrix["line"]
        items.append(item)
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
                "okCount": 0,
                "regressedAfterOk": False,
                "firstVerdict": item["verdict"],
                "lastVerdict": item["verdict"],
                "firstDistance": item["distance"],
                "lastDistance": item["distance"],
                "deltaDistance": 0.0,
                "maxDistance": item["distance"],
                "firstRelLocal": item["relLocal"],
                "lastRelLocal": item["relLocal"],
                "deltaRelLocal": [],
                "maxAbsDeltaRelLocal": 0.0,
                "firstPartInAnchorTrans": item.get("partInAnchorTrans", []),
                "lastPartInAnchorTrans": item.get("partInAnchorTrans", []),
                "deltaPartInAnchorTrans": [],
                "maxAbsDeltaPartInAnchorTrans": 0.0,
                "firstAnchorAngleDeg": item.get("anchorAngleDeg"),
                "lastAnchorAngleDeg": item.get("anchorAngleDeg"),
                "deltaAnchorAngleDeg": 0.0,
                "firstSampleIndex": item.get("sampleIndex", 0),
                "lastSampleIndex": item.get("sampleIndex", 0),
                "firstTimestamp": item.get("timestamp", ""),
                "lastTimestamp": item.get("timestamp", ""),
                "firstAnimationGroup": item.get("animationGroup", ""),
                "lastAnimationGroup": item.get("animationGroup", ""),
                "firstAnimationTime": item.get("animationTime"),
                "lastAnimationTime": item.get("animationTime"),
                "firstBadSampleIndex": 0,
                "firstBadTimestamp": "",
                "firstBadAnimationTime": None,
                "firstBadDistance": None,
                "firstBadRelLocal": [],
                "firstBadPartInAnchorTrans": [],
                "firstLine": item["line"],
                "lastLine": item["line"],
                "firstBadLine": "",
                "badLines": [],
                "transitions": [],
            }
        summary = grouped[key]
        previous_verdict = summary["lastVerdict"]
        summary["count"] += 1
        summary["lastVerdict"] = item["verdict"]
        summary["lastDistance"] = item["distance"]
        summary["deltaDistance"] = summary["lastDistance"] - summary["firstDistance"]
        summary["lastRelLocal"] = item["relLocal"]
        summary["deltaRelLocal"] = vec_delta(summary["firstRelLocal"], summary["lastRelLocal"])
        summary["maxAbsDeltaRelLocal"] = max_abs_vec_delta(summary["deltaRelLocal"])
        summary["lastPartInAnchorTrans"] = item.get("partInAnchorTrans", [])
        summary["deltaPartInAnchorTrans"] = vec_delta(
            summary["firstPartInAnchorTrans"], summary["lastPartInAnchorTrans"]
        )
        summary["maxAbsDeltaPartInAnchorTrans"] = max_abs_vec_delta(summary["deltaPartInAnchorTrans"])
        summary["lastAnchorAngleDeg"] = item.get("anchorAngleDeg")
        if summary["firstAnchorAngleDeg"] is not None and summary["lastAnchorAngleDeg"] is not None:
            summary["deltaAnchorAngleDeg"] = summary["lastAnchorAngleDeg"] - summary["firstAnchorAngleDeg"]
        summary["lastSampleIndex"] = item.get("sampleIndex", 0)
        summary["lastTimestamp"] = item.get("timestamp", "")
        summary["lastAnimationGroup"] = item.get("animationGroup", "")
        summary["lastAnimationTime"] = item.get("animationTime")
        summary["lastLine"] = item["line"]
        if item["distance"] > summary["maxDistance"]:
            summary["maxDistance"] = item["distance"]
        if item["verdict"] == "OK":
            summary["okCount"] += 1
        elif summary["okCount"] > 0:
            summary["regressedAfterOk"] = True
        if item["verdict"] != "OK":
            summary["badCount"] += 1
            if summary["firstBadSampleIndex"] == 0:
                summary["firstBadSampleIndex"] = item.get("sampleIndex", 0)
                summary["firstBadTimestamp"] = item.get("timestamp", "")
                summary["firstBadAnimationTime"] = item.get("animationTime")
                summary["firstBadDistance"] = item["distance"]
                summary["firstBadRelLocal"] = item["relLocal"]
                summary["firstBadPartInAnchorTrans"] = item.get("partInAnchorTrans", [])
                summary["firstBadLine"] = item["line"]
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
        summary["regressed"] = summary["regressedAfterOk"]
        result.append(summary)
    return result


def build_runtime_part_timelines(audits: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    order: list[tuple[str, str, str]] = []
    for item in audits:
        key = (item["ref"], item["part"], item["class"])
        if key not in grouped:
            order.append(key)
            grouped[key] = []
        grouped[key].append(item)

    timelines: list[dict[str, Any]] = []
    for key in order:
        samples = grouped[key]
        first = samples[0]
        last = samples[-1]
        bad_samples = [sample for sample in samples if sample["verdict"] != "OK"]
        first_bad = bad_samples[0] if bad_samples else None
        sample_rows = []
        for sample in samples:
            sample_rows.append(
                {
                    "sampleIndex": sample.get("sampleIndex", 0),
                    "timestamp": sample.get("timestamp", ""),
                    "animationGroup": sample.get("animationGroup", ""),
                    "animationTime": sample.get("animationTime"),
                    "activeControllers": sample.get("activeControllers", 0),
                    "distance": sample.get("distance"),
                    "limit": sample.get("limit"),
                    "verdict": sample.get("verdict", ""),
                    "center": sample.get("center", []),
                    "anchor": sample.get("anchor", []),
                    "relLocal": sample.get("relLocal", []),
                    "partInAnchorTrans": sample.get("partInAnchorTrans", []),
                    "partInParentTrans": sample.get("partInParentTrans", []),
                    "anchorAngleDeg": sample.get("anchorAngleDeg"),
                    "line": sample.get("line", ""),
                    "matrixLine": sample.get("matrixLine", ""),
                }
            )
        timelines.append(
            {
                "ref": key[0],
                "part": key[1],
                "class": key[2],
                "count": len(samples),
                "badCount": len(bad_samples),
                "regressed": first.get("verdict") == "OK" and bool(bad_samples),
                "firstSampleIndex": first.get("sampleIndex", 0),
                "lastSampleIndex": last.get("sampleIndex", 0),
                "firstTimestamp": first.get("timestamp", ""),
                "lastTimestamp": last.get("timestamp", ""),
                "firstAnimationTime": first.get("animationTime"),
                "lastAnimationTime": last.get("animationTime"),
                "firstBadSampleIndex": first_bad.get("sampleIndex", 0) if first_bad else 0,
                "firstBadTimestamp": first_bad.get("timestamp", "") if first_bad else "",
                "firstBadAnimationTime": first_bad.get("animationTime") if first_bad else None,
                "firstDistance": first.get("distance"),
                "lastDistance": last.get("distance"),
                "deltaDistance": (last.get("distance") or 0.0) - (first.get("distance") or 0.0),
                "firstRelLocal": first.get("relLocal", []),
                "lastRelLocal": last.get("relLocal", []),
                "deltaRelLocal": vec_delta(first.get("relLocal"), last.get("relLocal")),
                "firstPartInAnchorTrans": first.get("partInAnchorTrans", []),
                "lastPartInAnchorTrans": last.get("partInAnchorTrans", []),
                "deltaPartInAnchorTrans": vec_delta(
                    first.get("partInAnchorTrans"), last.get("partInAnchorTrans")
                ),
                "samples": sample_rows,
            }
        )
    return timelines


def axis_range(center: float, extent: float) -> tuple[float, float]:
    half = abs(extent) * 0.5
    return (center - half, center + half)


def first_face_point(audits: list[dict[str, Any]], class_name: str, needles: tuple[str, ...]) -> list[float] | None:
    for item in audits:
        part = item.get("part", "").replace("\\", "/").lower()
        if item.get("class") != class_name:
            continue
        if needles and not any(needle in part for needle in needles):
            continue
        rel_local = item.get("relLocal")
        if isinstance(rel_local, list) and len(rel_local) == 3:
            return rel_local
    return None


def find_face_occlusion_findings(bounds: list[dict[str, Any]], audits: list[dict[str, Any]]) -> list[dict[str, Any]]:
    left_eye = first_face_point(audits, "faceEye", ("eyelefthuman", "eyeleft"))
    right_eye = first_face_point(audits, "faceEye", ("eyerighthuman", "eyeright"))
    mouth = first_face_point(audits, "faceMouth", ("mouth",))
    if not left_eye or not right_eye or not mouth:
        return []

    eye_y = (left_eye[1] + right_eye[1]) * 0.5
    eye_z = (left_eye[2] + right_eye[2]) * 0.5
    mouth_y = mouth[1]
    mouth_z = mouth[2]
    findings: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()

    for item in bounds:
        model = item.get("model", "").replace("\\", "/").lower()
        head_rel = item.get("headRel") or []
        extent = item.get("extent") or []
        if len(head_rel) != 3 or len(extent) != 3:
            continue

        x_range = axis_range(head_rel[0], extent[0])
        y_range = axis_range(head_rel[1], extent[1])
        z_range = axis_range(head_rel[2], extent[2])
        reason = ""

        if "/hair/" in model and "eyebrow" not in model:
            broad = max(abs(value) for value in extent) >= 10.0
            forward_of_mouth = head_rel[1] >= mouth_y + 2.0
            low_against_mouth = head_rel[2] <= mouth_z + 0.75
            covers_mouth_z = z_range[0] <= mouth_z + 0.75 and z_range[1] >= mouth_z
            if broad and forward_of_mouth and low_against_mouth and covers_mouth_z:
                reason = "hair bounds are forward/low enough to cover the mouth/face corridor"

        if "/headgear/" in model:
            overlaps_eye_y = y_range[0] <= eye_y <= y_range[1]
            drops_to_eye = z_range[0] < eye_z - 0.75
            if overlaps_eye_y and drops_to_eye:
                reason = "headgear bounds intersect eye corridor"
            elif ("cap" in model or "hat1950" in model) and y_range[1] < eye_y - 1.0:
                reason = "cap/headgear forward extent is behind eye plane"

        if not reason:
            continue
        key = (model, reason)
        if key in seen:
            continue
        seen.add(key)
        findings.append(
            {
                "model": item.get("model", ""),
                "parent": item.get("parent", ""),
                "reason": reason,
                "headRel": head_rel,
                "extent": extent,
                "xRange": list(x_range),
                "yRange": list(y_range),
                "zRange": list(z_range),
                "eyeY": eye_y,
                "eyeZ": eye_z,
                "mouthY": mouth_y,
                "mouthZ": mouth_z,
            }
        )
    return findings


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


def parse_actor_weapon_states(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    state_re = re.compile(r'initialized actor shell for NPC "(?P<editor>[^"]+)".*? weapon=(?P<weapon>[^ ]*)')
    states: list[dict[str, Any]] = []
    for line in lines:
        match = state_re.search(line)
        if not match:
            continue
        editor = match.group("editor")
        if not line_matches_actor(line, patterns) and not matches_known_actor_value(editor, patterns):
            continue
        states.append(
            {
                "timestamp": timestamp_from_line(line),
                "editor": editor,
                "weapon": match.group("weapon").strip(),
                "line": compact_line(line),
            }
        )
    return states


def has_neutral_preview_runtime(lines: list[str], actor: str) -> bool:
    return any(f'neutral actor preview assembled target="{actor}"' in line for line in lines)


def pane_box(width: int, height: int, pane: dict[str, Any]) -> tuple[int, int, int, int]:
    pane_width = pane["widthNdc"] * width / 2.0
    pane_height = pane["heightNdc"] * height / 2.0
    center_x = (pane["centerNdcX"] + 1.0) * width / 2.0
    left = max(0, int(round(center_x - pane_width / 2.0)))
    right = min(width, int(round(center_x + pane_width / 2.0)))
    top = max(0, int(round((1.0 - pane["heightNdc"] / 2.0) * height / 2.0)))
    bottom = min(height, int(round((1.0 + pane["heightNdc"] / 2.0) * height / 2.0)))
    return (left, top, right, bottom)


def rects_intersect(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> bool:
    return min(a[2], b[2]) > max(a[0], b[0]) and min(a[3], b[3]) > max(a[1], b[1])


def foreground_components(mask: list[bool], width: int, height: int, min_pixels: int) -> list[dict[str, Any]]:
    seen = bytearray(len(mask))
    components: list[dict[str, Any]] = []
    for index, is_foreground in enumerate(mask):
        if not is_foreground or seen[index]:
            continue
        stack = [index]
        seen[index] = 1
        count = 0
        min_x = width
        min_y = height
        max_x = 0
        max_y = 0
        while stack:
            current = stack.pop()
            y, x = divmod(current, width)
            count += 1
            min_x = min(min_x, x)
            min_y = min(min_y, y)
            max_x = max(max_x, x)
            max_y = max(max_y, y)
            for neighbor in (current - 1, current + 1, current - width, current + width):
                if neighbor < 0 or neighbor >= len(mask) or seen[neighbor] or not mask[neighbor]:
                    continue
                neighbor_y, neighbor_x = divmod(neighbor, width)
                if abs(neighbor_x - x) + abs(neighbor_y - y) != 1:
                    continue
                seen[neighbor] = 1
                stack.append(neighbor)
        if count >= min_pixels:
            components.append({"pixels": count, "bbox": [min_x, min_y, max_x + 1, max_y + 1]})
    components.sort(key=lambda item: int(item["pixels"]), reverse=True)
    return components


def neutral_preview_active_panes(phase: str) -> set[str]:
    return set(NEUTRAL_PREVIEW_PHASE_PANES.get(phase.strip().lower(), NEUTRAL_PREVIEW_PHASE_PANES["full"]))


def analyze_neutral_preview_image(path: Path, weapon_present: bool = False, phase: str = "") -> dict[str, Any]:
    from collections import Counter

    from PIL import Image

    with Image.open(path) as raw_image:
        original_width, original_height = raw_image.size
        scale = min(1.0, 480.0 / max(1, original_width))
        image = raw_image.convert("RGB")
        if scale < 1.0:
            image = image.resize((480, max(1, int(round(original_height * scale)))))
        width, height = image.size
        flattened = getattr(image, "get_flattened_data", None)
        pixels = list(flattened() if flattened else image.getdata())

    sample_step = 2
    quantized_background: Counter[tuple[int, int, int]] = Counter()
    for y in range(0, height, sample_step):
        offset = y * width
        for x in range(0, width, sample_step):
            r, g, b = pixels[offset + x]
            quantized_background[(r // 8, g // 8, b // 8)] += 1
    background_bin, background_samples = quantized_background.most_common(1)[0]
    background = [value * 8 + 4 for value in background_bin]
    background_sample_count = sum(quantized_background.values())
    background_fraction = background_samples / max(1, background_sample_count)

    foreground_mask: list[bool] = []
    for r, g, b in pixels:
        distance = abs(r - background[0]) + abs(g - background[1]) + abs(b - background[2])
        foreground_mask.append(distance > 45)

    components = foreground_components(foreground_mask, width, height, min_pixels=8)
    pane_results: list[dict[str, Any]] = []
    findings: list[str] = []
    pane_boxes = [pane_box(width, height, pane) for pane in NEUTRAL_PREVIEW_PANES]
    active_panes = neutral_preview_active_panes(phase)
    edge_tolerance = 2

    for pane, box in zip(NEUTRAL_PREVIEW_PANES, pane_boxes):
        left, top, right, bottom = box
        area = max(1, (right - left) * (bottom - top))
        foreground_pixels = 0
        for y in range(top, bottom):
            row = y * width
            for x in range(left, right):
                if foreground_mask[row + x]:
                    foreground_pixels += 1
        foreground_fraction = foreground_pixels / area
        pane_components = [
            item for item in components if rects_intersect(tuple(item["bbox"]), (left, top, right, bottom))
        ]
        largest = pane_components[0] if pane_components else {"pixels": 0, "bbox": []}
        touch_edges: list[str] = []
        bbox = largest.get("bbox") or []
        if bbox:
            if bbox[0] <= left + edge_tolerance:
                touch_edges.append("left")
            if bbox[2] >= right - edge_tolerance:
                touch_edges.append("right")
            if bbox[1] <= top + edge_tolerance:
                touch_edges.append("top")
            if bbox[3] >= bottom - edge_tolerance:
                touch_edges.append("bottom")

        pane_findings: list[str] = []
        allowed_edges = set(pane["allowedTouchEdges"])
        if pane["name"] == "right-hand-weapon" and weapon_present:
            # A long gun or other held prop may intentionally extend upward or rightward in the isolated weapon pane.
            allowed_edges.update({"right", "top"})
        active_for_phase = pane["name"] in active_panes
        if active_for_phase:
            if foreground_fraction < pane["minForegroundFraction"]:
                pane_findings.append(
                    f"foreground fraction {foreground_fraction:.4f} below {pane['minForegroundFraction']:.4f}"
                )
            if foreground_fraction > pane["maxForegroundFraction"]:
                pane_findings.append(
                    f"foreground fraction {foreground_fraction:.4f} above {pane['maxForegroundFraction']:.4f}"
                )
            disallowed_edges = [edge for edge in touch_edges if edge not in allowed_edges]
            if disallowed_edges:
                pane_findings.append(f"foreground touches disallowed pane edge(s): {','.join(disallowed_edges)}")
        if pane_findings:
            findings.extend([f"{pane['name']}: {finding}" for finding in pane_findings])

        pane_results.append(
            {
                "name": pane["name"],
                "box": [left, top, right, bottom],
                "foregroundPixels": foreground_pixels,
                "foregroundFraction": foreground_fraction,
                "componentCount": len(pane_components),
                "largestComponentPixels": int(largest.get("pixels", 0)),
                "largestComponentBox": bbox,
                "touchEdges": touch_edges,
                "allowedTouchEdges": sorted(allowed_edges),
                "weaponPresent": weapon_present if pane["name"] == "right-hand-weapon" else None,
                "activeForPhase": active_for_phase,
                "phase": phase,
                "findings": pane_findings,
                "status": "PASS" if not pane_findings else "FAIL",
            }
        )

    outside_components = [
        item
        for item in components
        if int(item["pixels"]) >= 16 and not any(rects_intersect(tuple(item["bbox"]), box) for box in pane_boxes)
    ]
    if outside_components:
        findings.append(f"foreground components outside neutral preview panes: {len(outside_components)}")

    return {
        "image": path.name,
        "status": "PASS" if not findings else "FAIL",
        "originalSize": [original_width, original_height],
        "analysisSize": [width, height],
        "backgroundRgb": background,
        "backgroundDominantFraction": background_fraction,
        "phase": phase,
        "activePanes": sorted(active_panes),
        "foregroundComponentCount": len(components),
        "outsideComponentCount": len(outside_components),
        "outsideComponents": outside_components[:12],
        "panes": pane_results,
        "findings": findings,
        "runtime": "runtime-supported" if not findings else "loaded-pending-runtime",
    }


def analyze_neutral_preview_composition(
    proof_dir: Path, lines: list[str], actor: str, weapon_present: bool = False, phase: str = ""
) -> list[dict[str, Any]]:
    if not has_neutral_preview_runtime(lines, actor):
        return []
    screenshots = sorted(proof_dir.glob("*.png"))
    if not screenshots:
        return []
    try:
        return [analyze_neutral_preview_image(screenshots[-1], weapon_present=weapon_present, phase=phase)]
    except Exception as exc:
        return [
            {
                "image": screenshots[-1].name,
                "status": "FAIL",
                "runtime": "known-blocked",
                "findings": [f"neutral preview composition analysis failed: {exc}"],
            }
        ]


def parse_actor_matches(lines: list[str], actor: str) -> list[dict[str, Any]]:
    match_re = re.compile(
        rf'active-cell actor match target="{re.escape(actor)}".*?\bframe=(?P<frame>[0-9]+) '
        r"ref=(?P<ref>[^ ]+) base=(?P<base>[^ ]+) name=\"(?P<name>[^\"]*)\" "
        r"baseEditor=\"(?P<baseEditor>[^\"]*)\" (?:baseForm=(?P<baseForm>[^ ]+) )?baseFull=\"(?P<baseFull>[^\"]*)\" "
        r"pos=\((?P<pos>[^)]*)\) rot=\((?P<rot>[^)]*)\)"
    )
    assembly_re = re.compile(
        rf'actor part assembly target match target="{re.escape(actor)}" actor=(?P<name>[^ ]+) '
        r"refAlias=(?P<refAlias>[^ ]+) ref=(?P<ref>[^ ]+) baseEditor=(?P<baseEditor>[^ ]+) "
        r"baseForm=(?P<baseForm>[^ ]+) traitsForm=(?P<traitsForm>[^ ]+)"
    )
    matches: list[dict[str, Any]] = []
    known_patterns = [actor]
    for line in lines:
        match = match_re.search(line)
        if match:
            data = match.groupdict()
            for value in (data["ref"], data["base"], data["name"], data["baseEditor"], data.get("baseForm") or "", data["baseFull"]):
                if value and value not in known_patterns:
                    known_patterns.append(value)
            matches.append(
                {
                    "frame": int(data["frame"]),
                    "source": "active-cell",
                    "ref": data["ref"],
                    "base": data["base"],
                    "name": data["name"],
                    "baseEditor": data["baseEditor"],
                    "baseForm": data.get("baseForm") or "",
                    "baseFull": data["baseFull"],
                    "pos": parse_vec3(data["pos"]),
                    "rot": parse_vec3(data["rot"]),
                    "line": compact_line(line),
                }
            )
            continue
        assembly_match = assembly_re.search(line)
        if assembly_match:
            data = assembly_match.groupdict()
            if not any(matches_known_actor_value(value, known_patterns) for value in data.values()):
                continue
            for value in data.values():
                if value and value not in known_patterns:
                    known_patterns.append(value)
            matches.append(
                {
                    "frame": 0,
                    "source": "actor-part-assembly",
                    "ref": data["ref"],
                    "refAlias": data["refAlias"],
                    "base": data["baseForm"],
                    "name": data["name"],
                    "baseEditor": data["baseEditor"],
                    "baseForm": data["baseForm"],
                    "traitsForm": data["traitsForm"],
                    "baseFull": "",
                    "pos": [],
                    "rot": [],
                    "line": compact_line(line),
                }
            )
    return matches


def parse_animation_sources(lines: list[str]) -> list[dict[str, Any]]:
    source_re = re.compile(
        r"animation source (?P<source>.*?) bound (?P<matched>[0-9]+)/(?P<total>[0-9]+) "
        r"controller\(s\) to (?P<model>.*?), missing (?P<missing>[0-9]+)"
    )
    sources: list[dict[str, Any]] = []
    for line in lines:
        match = source_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        sources.append(
            {
                "source": data["source"],
                "model": data["model"],
                "matchedControllers": int(data["matched"]),
                "totalControllers": int(data["total"]),
                "missingControllers": int(data["missing"]),
                "line": compact_line(line),
            }
        )
    return sources


def parse_animation_playback(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    play_re = re.compile(
        r"play matched (?P<ref>[^ ]+) group '(?P<group>[^']*)' source=(?P<source>.*?) "
        r"checkedSources=(?P<checkedSources>[0-9]+) controllers=(?P<controllers>[0-9]+) "
        r"startTime=(?P<startTime>[-+0-9.eE]+) loopStart=(?P<loopStart>[-+0-9.eE]+) "
        r"loopStop=(?P<loopStop>[-+0-9.eE]+) stopTime=(?P<stopTime>[-+0-9.eE]+) "
        r"playing=(?P<playing>[01])"
    )
    playback: list[dict[str, Any]] = []
    for line in lines:
        match = play_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        if not line_matches_actor(data["ref"], patterns):
            continue
        playback.append(
            {
                "ref": data["ref"],
                "group": data["group"],
                "source": data["source"],
                "checkedSources": int(data["checkedSources"]),
                "controllers": int(data["controllers"]),
                "startTime": float(data["startTime"]),
                "loopStart": float(data["loopStart"]),
                "loopStop": float(data["loopStop"]),
                "stopTime": float(data["stopTime"]),
                "playing": data["playing"] == "1",
                "line": compact_line(line),
            }
        )
    return playback


def parse_animation_requests(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    request_re = re.compile(
        r"actor-kit animation request actor=(?P<actor>[^ ]+) ref=(?P<ref>[^ ]+) "
        r"group=(?P<group>[^ ]+) available=(?P<available>[01]) runtime=(?P<runtime>[^ ]+)"
    )
    requests: list[dict[str, Any]] = []
    for line in lines:
        match = request_re.search(line)
        if not match:
            continue
        data = match.groupdict()
        if not line_matches_actor(f"{data['actor']} {data['ref']}", patterns):
            continue
        requests.append(
            {
                "actor": data["actor"],
                "ref": data["ref"],
                "group": data["group"],
                "available": data["available"] == "1",
                "runtime": data["runtime"],
                "line": compact_line(line),
            }
        )
    return requests


def collect_animation_blockers(lines: list[str], patterns: list[str]) -> list[str]:
    blockers: list[str] = []
    needles = ("play request", "idle animation missing", "movement animation missing")
    for line in lines:
        lowered = line.lower()
        if not any(needle in lowered for needle in needles):
            continue
        if (" ignored " not in lowered and " missing" not in lowered) or not line_matches_actor(line, patterns):
            continue
        blockers.append(compact_line(line))
    return blockers


def parse_creature_evidence(lines: list[str], patterns: list[str]) -> list[dict[str, Any]]:
    model_re = re.compile(r"\b(?:model|path)=([^ ]+)")
    mask_re = re.compile(r"rigged=(?P<rigged>[0-9]+) static=(?P<static>[0-9]+) other=(?P<other>[0-9]+)")
    bounds_re = re.compile(rf"creature (?P<label>[^ ]+) bounds .*? path=(?P<path>[^ ]+) center={FLOAT3_RE} extent={FLOAT3_RE}")
    evidence: list[dict[str, Any]] = []
    for line in lines:
        lowered = line.lower()
        if not any(needle.lower() in lowered for needle in CREATURE_RUNTIME_NEEDLES):
            continue
        if not line_matches_actor(line, patterns):
            continue

        if "inserted creature animation for" in line:
            kind = "creature-animation"
        elif "creature animation groups" in line:
            kind = "creature-animation-groups"
        elif "attached creature body nif" in line or "forced creature body render mask" in line:
            kind = "creature-body"
        elif "creature bounds" in line:
            kind = "creature-bounds"
        elif "creature KF" in line:
            kind = "creature-kf"
        else:
            kind = "creature-runtime"

        model_match = model_re.search(line)
        item: dict[str, Any] = {
            "kind": kind,
            "model": model_match.group(1) if model_match else "",
            "timestamp": timestamp_from_line(line),
            "line": compact_line(line),
        }
        mask_match = mask_re.search(line)
        if mask_match:
            item["visibleGeometryCount"] = sum(int(mask_match.group(name)) for name in ("rigged", "static", "other"))
        bounds_match = bounds_re.search(line)
        if bounds_match:
            groups = bounds_match.groups()
            item["label"] = bounds_match.group("label")
            item["path"] = bounds_match.group("path")
            item["center"] = parse_vec3(groups[2])
            item["extent"] = parse_vec3(groups[3])
        evidence.append(item)
    return evidence


def evaluate(
    proof_dir: Path,
    actor: str,
    actor_kind: str,
    phase: str,
    lines: list[str],
    patterns: list[str],
    gates: list[dict[str, Any]],
    bounds: list[dict[str, Any]],
    audits: list[dict[str, Any]],
    drawables: list[dict[str, Any]],
    creature_evidence: list[dict[str, Any]],
    actor_matches: list[dict[str, Any]],
    animation_sources: list[dict[str, Any]],
    animation_requests: list[dict[str, Any]],
    animation_playback: list[dict[str, Any]],
    animation_blockers: list[str],
    actor_weapon_states: list[dict[str, Any]],
    face_occlusion_findings: list[dict[str, Any]],
    neutral_preview_composition: list[dict[str, Any]],
) -> tuple[str, list[str]]:
    failures: list[str] = []
    phase_lower = phase.lower()
    requires_animation_playback = bool(animation_requests) or phase_lower in {
        "animation",
        "dialogue",
        "talk",
        "creature-animation",
        "creature-full",
    }
    screenshots = sorted(p.name for p in proof_dir.glob("*.png"))
    if not screenshots:
        failures.append("missing screenshots")
    if not actor_matches:
        failures.append("missing active-cell actor match")
    if not any(item["matchedControllers"] > 0 for item in animation_sources):
        failures.append("missing bound animation controller evidence")
    if requires_animation_playback and not any(item["controllers"] > 0 and item["playing"] for item in animation_playback):
        failures.append("missing target animation playback evidence")
    unavailable_requests = [item for item in animation_requests if not item["available"]]
    if unavailable_requests:
        failures.append(f"selected animation group unavailable: {len(unavailable_requests)}")
    for request in animation_requests:
        if not any(
            item["group"].lower() == request["group"].lower() and item["controllers"] > 0 and item["playing"]
            for item in animation_playback
        ):
            failures.append(f"missing selected animation playback evidence: {request['group']}")
    if animation_blockers:
        failures.append(f"target animation blocker lines: {len(animation_blockers)}")
    if actor_kind != "creature" and phase and not gates:
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
    runtime_audit_summary = summarize_runtime_audits(audits)
    never_settled_audits = [item for item in runtime_audit_summary if item["okCount"] == 0 and item["badCount"] > 0]
    if never_settled_audits:
        failures.append(f"bad runtime part audits: {len(never_settled_audits)}")
    regressions = [item for item in runtime_audit_summary if item["regressed"]]
    if regressions:
        failures.append(f"runtime audit regressions after initial OK: {len(regressions)}")
    neutral_preview_findings = [
        finding
        for item in neutral_preview_composition
        if item.get("status") != "PASS"
        for finding in item.get("findings", [])
    ]
    if neutral_preview_findings:
        failures.append(f"neutral preview composition findings: {len(neutral_preview_findings)}")
    if face_occlusion_findings:
        failures.append(f"face occlusion/headgear orientation findings: {len(face_occlusion_findings)}")
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
        weapon_expected = not actor_weapon_states or any(item["weapon"] for item in actor_weapon_states)
        if weapon_expected and not weapon_lines:
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

    if actor_kind == "creature":
        creature_lines = [item["line"] for item in creature_evidence]
        if not creature_lines:
            failures.append("missing creature runtime evidence")
        if not any(item["kind"] == "creature-animation" for item in creature_evidence):
            failures.append("missing inserted creature animation runtime evidence")
        if not any(item["kind"] == "creature-animation-groups" for item in creature_evidence):
            failures.append("missing creature animation group runtime evidence")
        if any("bounds invalid" in item["line"] for item in creature_evidence):
            failures.append("creature geometry bounds invalid")
        mask_lines = [item for item in creature_evidence if item["kind"] == "creature-body" and "visibleGeometryCount" in item]
        if mask_lines and not any(int(item.get("visibleGeometryCount", 0)) > 0 for item in mask_lines):
            failures.append("creature body render mask has no visible geometry")
        if phase in {"creature-body", "creature-model", "creature-full", "full"} and not any(
            item["kind"] in {"creature-body", "creature-bounds"} for item in creature_evidence
        ):
            failures.append("missing creature body/model runtime evidence")
    elif not collect_matching(lines, patterns, "FACE CHECK"):
        failures.append("missing FACE CHECK for actor")

    return ("PASS" if not failures else "FAIL", failures)


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    lines: list[str] = []
    lines.append(f"# FNV Character Builder Report - {report['actor']}")
    lines.append("")
    lines.append(f"Status: **{report['status']}**")
    lines.append(f"Actor Kind: `{report['actorKind']}`")
    lines.append(f"Phase: `{report['phase']}`")
    lines.append(f"Proof: `{report['proofDir']}`")
    lines.append("")
    lines.append("## Actor")
    lines.append("")
    lines.append(f"Patterns: `{', '.join(report['actorPatterns'])}`")
    lines.append(f"Actor matches: {len(report['actorMatches'])}")
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
    lines.append(f"Head surface offsets: {len(report['headSurfaceOffsets'])}")
    lines.append(f"Runtime part audits: {len(report['runtimePartAudits'])}")
    lines.append(f"Runtime audit summaries: {len(report['runtimeAuditSummary'])}")
    lines.append(f"Neutral preview composition samples: {len(report['neutralPreviewComposition'])}")
    lines.append(f"Face drawable audits: {len(report['faceDrawables'])}")
    lines.append(f"TRI/EGM/talk lines: {len(report['morphLines'])}")
    lines.append(f"Actor weapon states: {len(report['actorWeaponStates'])} expected={report['weaponExpected']}")
    lines.append(f"Weapon lines: {len(report['weaponLines'])}")
    lines.append(f"Creature evidence lines: {len(report['creatureEvidence'])}")
    lines.append(f"Animation sources: {len(report['animationSources'])}")
    lines.append(f"Selected animation requests: {len(report['animationRequests'])}")
    lines.append(f"Target animation playback lines: {len(report['animationPlayback'])}")
    lines.append("")
    if report["failures"]:
        lines.append("## Failures")
        lines.append("")
        for failure in report["failures"]:
            lines.append(f"- {failure}")
        lines.append("")
    if report["neutralPreviewComposition"]:
        lines.append("## Neutral Preview Composition")
        lines.append("")
        for item in report["neutralPreviewComposition"]:
            lines.append(f"- `{item['image']}`: **{item['status']}**")
            for finding in item.get("findings", []):
                lines.append(f"  - {finding}")
            lines.append("")
            lines.append("| Pane | Status | Foreground | Components | Largest Box | Touch Edges |")
            lines.append("|---|---|---:|---:|---|---|")
            for pane in item.get("panes", []):
                lines.append(
                    f"| {pane['name']} | {pane['status']} | {pane['foregroundFraction']:.4f} | "
                    f"{pane['componentCount']} | {pane.get('largestComponentBox', [])} | "
                    f"{pane.get('touchEdges', [])} |"
                )
            lines.append("")
    if report["faceOcclusionFindings"]:
        lines.append("## Face Occlusion Findings")
        lines.append("")
        lines.append("| Model | Reason | HeadRel | Extent | Y Range | Z Range |")
        lines.append("|---|---|---|---|---|---|")
        for item in report["faceOcclusionFindings"][:24]:
            lines.append(
                f"| {item['model']} | {item['reason']} | {item['headRel']} | {item['extent']} | "
                f"{item['yRange']} | {item['zRange']} |"
            )
        lines.append("")
    if report["headSurfaceOffsets"]:
        lines.append("## Head Surface Offsets")
        lines.append("")
        lines.append("| Model | Offset XYZ | Rotation Prefix | Pivot XYZ | Pivot Mode |")
        lines.append("|---|---|---|---|---|")
        for item in report["headSurfaceOffsets"][:48]:
            lines.append(
                f"| {item['model']} | {item['offset']} | {item['rotationPrefix']} | "
                f"{item['pivot']} | {item['pivotMode']} |"
            )
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
    lines.append("| Part | First | Last | Bad | Time | Max Distance | Delta Rel | Delta Anchor |")
    lines.append("|---|---|---|---:|---|---:|---|---|")
    for item in report["runtimeAuditSummary"][:32]:
        lines.append(
            f"| {item['part']} | {item['firstVerdict']} | {item['lastVerdict']} | {item['badCount']} | "
            f"{item.get('firstAnimationTime')} -> {item.get('lastAnimationTime')} | "
            f"{item['maxDistance']:.4g} | {item.get('deltaRelLocal', [])} | "
            f"{item.get('deltaPartInAnchorTrans', [])} |"
        )
    lines.append("")
    if report["runtimePartTimelines"]:
        lines.append("## Runtime Part Timeline")
        lines.append("")
        lines.append("| Part | Sample | Anim | Distance | Verdict | Rel Local | Part In Anchor |")
        lines.append("|---|---:|---|---:|---|---|---|")
        for timeline in report["runtimePartTimelines"][:24]:
            for sample in timeline["samples"][:8]:
                lines.append(
                    f"| {timeline['part']} | {sample['sampleIndex']} | "
                    f"{sample.get('animationGroup')}@{sample.get('animationTime')} | "
                    f"{sample.get('distance')} | {sample.get('verdict')} | "
                    f"{sample.get('relLocal', [])} | {sample.get('partInAnchorTrans', [])} |"
                )
        lines.append("")
    if report["actorWeaponStates"] or report["weaponLines"]:
        lines.append("## Weapon Evidence")
        lines.append("")
        for item in report["actorWeaponStates"][:16]:
            weapon = item["weapon"] if item["weapon"] else "(none)"
            lines.append(f"- `actor-state` {item['editor']} weapon={weapon} {item['line']}")
        for line in report["weaponLines"][:16]:
            lines.append(f"- `equipped` {line}")
        lines.append("")
    if report["creatureEvidence"]:
        lines.append("## Creature Evidence")
        lines.append("")
        for item in report["creatureEvidence"][:48]:
            lines.append(f"- `{item['kind']}` {item['line']}")
        lines.append("")
    if report["animationRequests"] or report["animationPlayback"]:
        lines.append("## Animation Playback")
        lines.append("")
        for item in report["animationRequests"][:48]:
            lines.append(f"- `request` group={item['group']} available={item['available']} {item['line']}")
        for item in report["animationPlayback"][:48]:
            lines.append(f"- `{item['group']}` controllers={item['controllers']} playing={item['playing']} {item['line']}")
        lines.append("")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--proof-dir", required=True, type=Path)
    parser.add_argument("--actor", default="GSEasyPete")
    parser.add_argument("--actor-kind", choices=("npc", "creature", "auto"), default="npc")
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
    head_surface_offsets = parse_head_surface_offsets(lines, patterns)
    audits = parse_runtime_audits(lines, patterns)
    drawables = parse_face_drawables(lines, patterns)
    creature_evidence = parse_creature_evidence(lines, patterns)
    actor_matches = parse_actor_matches(lines, args.actor)
    animation_sources = parse_animation_sources(lines)
    animation_requests = parse_animation_requests(lines, patterns)
    animation_playback = parse_animation_playback(lines, patterns)
    animation_blockers = collect_animation_blockers(lines, patterns)
    actor_weapon_states = parse_actor_weapon_states(lines, patterns)
    weapon_lines = [
        compact_line(line)
        for line in lines
        if ("equipped NPC weapon" in line or "weapon metadata" in line or "weapon sound files" in line)
        and line_matches_actor(line, patterns)
    ]
    weapon_present = any(item["weapon"] for item in actor_weapon_states) or bool(weapon_lines)
    runtime_audit_summary = summarize_runtime_audits(audits)
    runtime_part_timelines = build_runtime_part_timelines(audits)
    face_occlusion_findings = find_face_occlusion_findings(bounds, audits)
    neutral_preview_composition = analyze_neutral_preview_composition(
        proof_dir, lines, args.actor, weapon_present=weapon_present, phase=args.phase.lower()
    )
    actor_kind = args.actor_kind
    if actor_kind == "auto":
        actor_kind = "creature" if creature_evidence else "npc"
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
    status, failures = evaluate(
        proof_dir,
        args.actor,
        actor_kind,
        args.phase.lower(),
        lines,
        patterns,
        gates,
        bounds,
        audits,
        drawables,
        creature_evidence,
        actor_matches,
        animation_sources,
        animation_requests,
        animation_playback,
        animation_blockers,
        actor_weapon_states,
        face_occlusion_findings,
        neutral_preview_composition,
    )

    report: dict[str, Any] = {
        "status": status,
        "failures": failures,
        "proofDir": str(proof_dir),
        "actor": args.actor,
        "actorKind": actor_kind,
        "phase": args.phase,
        "actorPatterns": patterns,
        "actorMatches": actor_matches,
        "screenshots": sorted(p.name for p in proof_dir.glob("*.png")),
        "categorySummary": summarize_categories(gates),
        "gates": gates,
        "attachmentBounds": bounds,
        "headSurfaceOffsets": head_surface_offsets,
        "runtimePartAudits": audits,
        "runtimeAuditSummary": runtime_audit_summary,
        "runtimePartTimelines": runtime_part_timelines,
        "faceOcclusionFindings": face_occlusion_findings,
        "neutralPreviewComposition": neutral_preview_composition,
        "faceDrawables": drawables,
        "morphLines": morph_lines,
        "actorWeaponStates": actor_weapon_states,
        "weaponExpected": (None if not actor_weapon_states else any(item["weapon"] for item in actor_weapon_states)),
        "weaponLines": weapon_lines,
        "creatureEvidence": creature_evidence,
        "creatureLines": [item["line"] for item in creature_evidence],
        "animationSources": animation_sources,
        "animationRequests": animation_requests,
        "animationPlayback": animation_playback,
        "animationBlockers": animation_blockers,
    }

    out_json = args.out_json or (proof_dir / "character-builder-report.json")
    out_md = args.out_md or (proof_dir / "character-builder-report.md")
    out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
    write_markdown(out_md, report)
    print(f"{status} {out_json}")
    return 0 if status == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
