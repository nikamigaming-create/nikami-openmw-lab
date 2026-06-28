#!/usr/bin/env python3
"""Audit FNV live surface-frame telemetry from OpenMW logs.

The truth rig rule is simple: if OpenMW draws a part, we need its frame.
This parser turns runtime lines into JSON that can be compared across live
iterations: previous state, current state, parent/head frame, axes, bounds,
and alignment errors.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Any


FRAME_MARKER = "FNV/ESM4 telemetry: live surface frame "
FIELD_RE = re.compile(r"([A-Za-z][A-Za-z0-9]*)=(\[[^\]]*\]|\([^\)]*\)|\"[^\"]*\"|\S+)")
FLOAT_RE = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


VECTOR_FIELDS = {
    "offset",
    "rotation",
    "pivot",
    "surfaceOriginWorld",
    "headOriginWorld",
    "surfaceOriginHead",
    "boundsCenterHead",
    "boundsExtent",
    "pointCenterHead",
    "pointMinHead",
    "pointMaxHead",
    "pointXMinBandCenterHead",
    "pointXMaxBandCenterHead",
    "pointYMinBandCenterHead",
    "pointYMaxBandCenterHead",
    "pointZMinBandCenterHead",
    "pointZMaxBandCenterHead",
    "surfaceAxisXWorld",
    "surfaceAxisYWorld",
    "surfaceAxisZWorld",
    "headAxisXWorld",
    "headAxisYWorld",
    "headAxisZWorld",
}

MATRIX_FIELDS = {"surfaceWorldMatrix", "headWorldMatrix"}


def numbers(text: str) -> list[float]:
    return [float(match.group(0)) for match in FLOAT_RE.finditer(text)]


def parse_value(key: str, raw: str) -> Any:
    if raw.startswith('"') and raw.endswith('"'):
        return raw[1:-1]
    if key in VECTOR_FIELDS:
        return numbers(raw)
    if key in MATRIX_FIELDS:
        values = numbers(raw)
        return [values[index : index + 4] for index in range(0, len(values), 4)]
    if raw in {"0", "1"} and key.endswith("Mode"):
        return raw == "1"
    return raw


def parse_frame_line(line: str, line_number: int) -> dict[str, Any] | None:
    if FRAME_MARKER not in line:
        return None
    payload = line.split(FRAME_MARKER, 1)[1]
    fields: dict[str, Any] = {"lineNumber": line_number, "raw": line.rstrip()}
    for match in FIELD_RE.finditer(payload):
        key = match.group(1)
        fields[key] = parse_value(key, match.group(2))
    return fields


def dot(a: list[float], b: list[float]) -> float | None:
    if len(a) != 3 or len(b) != 3:
        return None
    return sum(x * y for x, y in zip(a, b))


def length(v: list[float]) -> float | None:
    if len(v) != 3:
        return None
    return math.sqrt(sum(x * x for x in v))


def subtract(a: list[float], b: list[float]) -> list[float] | None:
    if len(a) != 3 or len(b) != 3:
        return None
    return [a[index] - b[index] for index in range(3)]


def axis_summary(frame: dict[str, Any]) -> dict[str, Any]:
    surface_axes = {
        "x": frame.get("surfaceAxisXWorld", []),
        "y": frame.get("surfaceAxisYWorld", []),
        "z": frame.get("surfaceAxisZWorld", []),
    }
    head_axes = {
        "x": frame.get("headAxisXWorld", []),
        "y": frame.get("headAxisYWorld", []),
        "z": frame.get("headAxisZWorld", []),
    }
    alignment: dict[str, dict[str, float | None]] = {}
    for surface_name, surface_axis in surface_axes.items():
        alignment[surface_name] = {
            head_name: dot(surface_axis, head_axis) for head_name, head_axis in head_axes.items()
        }
    return {
        "surfaceAxisLengths": {name: length(axis) for name, axis in surface_axes.items()},
        "headAxisLengths": {name: length(axis) for name, axis in head_axes.items()},
        "surfaceToHeadAxisDot": alignment,
    }


def frame_summary(frame: dict[str, Any]) -> dict[str, Any]:
    origin_world_delta = subtract(frame.get("surfaceOriginWorld", []), frame.get("headOriginWorld", []))
    summary = {
        "lineNumber": frame.get("lineNumber"),
        "ref": frame.get("ref"),
        "model": frame.get("model"),
        "prefix": frame.get("prefix"),
        "parentHead": frame.get("parentHead"),
        "offset": frame.get("offset"),
        "rotation": frame.get("rotation"),
        "pivot": frame.get("pivot"),
        "pivotMode": frame.get("pivotMode"),
        "surfaceOriginHead": frame.get("surfaceOriginHead"),
        "boundsCenterHead": frame.get("boundsCenterHead"),
        "boundsExtent": frame.get("boundsExtent"),
        "pointCloud": {
            "count": int(frame.get("pointCount", 0)),
            "drawables": int(frame.get("pointDrawableCount", 0)),
            "centerHead": frame.get("pointCenterHead"),
            "minHead": frame.get("pointMinHead"),
            "maxHead": frame.get("pointMaxHead"),
            "xMinBandCenterHead": frame.get("pointXMinBandCenterHead"),
            "xMinBandCount": int(frame.get("pointXMinBandCount", 0)),
            "xMaxBandCenterHead": frame.get("pointXMaxBandCenterHead"),
            "xMaxBandCount": int(frame.get("pointXMaxBandCount", 0)),
            "yMinBandCenterHead": frame.get("pointYMinBandCenterHead"),
            "yMinBandCount": int(frame.get("pointYMinBandCount", 0)),
            "yMaxBandCenterHead": frame.get("pointYMaxBandCenterHead"),
            "yMaxBandCount": int(frame.get("pointYMaxBandCount", 0)),
            "zMinBandCenterHead": frame.get("pointZMinBandCenterHead"),
            "zMinBandCount": int(frame.get("pointZMinBandCount", 0)),
            "zMaxBandCenterHead": frame.get("pointZMaxBandCenterHead"),
            "zMaxBandCount": int(frame.get("pointZMaxBandCount", 0)),
        },
        "surfaceMinusHeadWorld": origin_world_delta,
        "axes": axis_summary(frame),
        "runtime": frame.get("runtime"),
        "gate": frame.get("gate"),
    }
    return summary


def load_frames(log_path: Path) -> list[dict[str, Any]]:
    frames = []
    with log_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            frame = parse_frame_line(line, line_number)
            if frame is not None:
                frames.append(frame)
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--model-contains", default="")
    parser.add_argument("--prefix", default="")
    parser.add_argument("--latest", action="store_true")
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args()

    frames = load_frames(args.log)
    if args.model_contains:
        needle = args.model_contains.lower()
        frames = [frame for frame in frames if needle in str(frame.get("model", "")).lower()]
    if args.prefix:
        frames = [frame for frame in frames if str(frame.get("prefix", "")) == args.prefix]

    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for frame in frames:
        key = f"{frame.get('ref','')}|{frame.get('prefix','')}|{frame.get('model','')}"
        groups[key].append(frame)

    parts = []
    for key, grouped in sorted(groups.items()):
        selected = grouped[-1:] if args.latest else grouped
        part = {
            "key": key,
            "frameCount": len(grouped),
            "previous": frame_summary(grouped[-2]) if len(grouped) >= 2 else None,
            "current": frame_summary(grouped[-1]) if grouped else None,
        }
        if not args.latest:
            part["frames"] = [frame_summary(frame) for frame in selected]
        parts.append(part)

    result = {
        "schema": "nikami-fnv-live-surface-frame-audit-v1",
        "log": str(args.log),
        "totalFrames": len(frames),
        "partCount": len(parts),
        "parts": parts,
    }
    print(json.dumps(result, indent=2 if args.pretty else None))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
