#!/usr/bin/env python3
"""Offline proof for FNV weapon IK telemetry.

Reads OpenMW weapon IK log lines, recomputes arm-chain distances, and writes a
small SVG projection so bad math is visible before running another screenshot.
"""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any


VEC_RE = r"\(([-+0-9.eE]+),([-+0-9.eE]+),([-+0-9.eE]+)\)"


def vec(line: str, key: str) -> tuple[float, float, float] | None:
    match = re.search(rf"{re.escape(key)}={VEC_RE}", line)
    if not match:
        return None
    return (float(match.group(1)), float(match.group(2)), float(match.group(3)))


def number(line: str, key: str) -> float | None:
    match = re.search(rf"{re.escape(key)}=([-+0-9.eE]+)", line)
    return float(match.group(1)) if match else None


def subtract(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def scale(a: tuple[float, float, float], s: float) -> tuple[float, float, float]:
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def length(a: tuple[float, float, float]) -> float:
    return math.sqrt(dot(a, a))


def normalize(a: tuple[float, float, float]) -> tuple[float, float, float]:
    l = length(a)
    if l <= 1e-6:
        return (0.0, 0.0, 0.0)
    return (a[0] / l, a[1] / l, a[2] / l)


def angle_degrees(a: tuple[float, float, float], b: tuple[float, float, float]) -> float | None:
    na = normalize(a)
    nb = normalize(b)
    if length(na) <= 1e-6 or length(nb) <= 1e-6:
        return None
    return math.degrees(math.acos(max(-1.0, min(1.0, dot(na, nb)))))


def parse_line(line: str) -> dict[str, Any] | None:
    if "FNV/ESM4 proof: weapon IK solver active" not in line and "FNV/ESM4 telemetry: weapon IK frame" not in line:
        return None
    fields: dict[str, Any] = {"raw": line.strip()}
    for key in (
        "chest",
        "head",
        "solverBasisForward",
        "solverBasisRight",
        "actorForward",
        "actorRight",
        "rightTarget",
        "leftTarget",
        "rightUpperBefore",
        "rightForearmBefore",
        "rightHandBefore",
        "leftUpperBefore",
        "leftForearmBefore",
        "leftHandBefore",
        "rightUpperAfter",
        "rightForearmAfter",
        "rightHandAfter",
        "leftUpperAfter",
        "leftForearmAfter",
        "leftHandAfter",
        "weaponForwardAfter",
        "weaponAxisX",
        "weaponAxisY",
        "weaponAxisZ",
        "weaponBoundsExtent",
    ):
        value = vec(line, key)
        if value is not None:
            fields[key] = value
    for key in (
        "rightFabrikError",
        "leftFabrikError",
        "weaponAimAngleAfter",
        "rightTargetDistanceAfter",
        "leftTargetDistanceAfter",
    ):
        value = number(line, key)
        if value is not None:
            fields[key] = value
    axis = re.search(r"weaponAimAxisName=([^ ]+)", line)
    if axis:
        fields["weaponAimAxisName"] = axis.group(1)
    return fields


def analyze(sample: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {"raw": sample["raw"]}
    forward = sample.get("actorForward") or sample.get("solverBasisForward")
    for side in ("right", "left"):
        upper = sample.get(f"{side}UpperAfter")
        forearm = sample.get(f"{side}ForearmAfter")
        hand = sample.get(f"{side}HandAfter")
        target = sample.get(f"{side}Target")
        if upper and forearm and hand and target:
            upper_len = length(subtract(forearm, upper))
            lower_len = length(subtract(hand, forearm))
            target_dist = length(subtract(target, upper))
            error = length(subtract(hand, target))
            out[f"{side}UpperLength"] = upper_len
            out[f"{side}LowerLength"] = lower_len
            out[f"{side}Reach"] = upper_len + lower_len
            out[f"{side}TargetDistanceFromShoulder"] = target_dist
            out[f"{side}EndpointError"] = error
            out[f"{side}ReachSlack"] = upper_len + lower_len - target_dist
            out[f"{side}Verdict"] = "OK" if error <= 8.0 and target_dist <= upper_len + lower_len + 0.25 else "BAD"
    if forward:
        axis_scores = {}
        for name in ("weaponAxisX", "weaponAxisY", "weaponAxisZ"):
            if name in sample:
                axis_scores[name] = angle_degrees(sample[name], forward)
        out["weaponAxisAngles"] = axis_scores
        if axis_scores:
            best = min(axis_scores, key=lambda key: axis_scores[key] if axis_scores[key] is not None else 999.0)
            out["bestWeaponAxis"] = best
            out["bestWeaponAxisAngle"] = axis_scores[best]
    return out


def make_svg(sample: dict[str, Any], analysis: dict[str, Any], path: Path) -> None:
    forward = normalize(sample.get("actorForward") or sample.get("solverBasisForward") or (1.0, 0.0, 0.0))
    right = normalize(sample.get("actorRight") or sample.get("solverBasisRight") or (0.0, -1.0, 0.0))
    up = (0.0, 0.0, 1.0)
    origin = sample.get("chest") or sample.get("rightUpperAfter") or (0.0, 0.0, 0.0)

    def project(p: tuple[float, float, float]) -> tuple[float, float]:
        rel = subtract(p, origin)
        return (dot(rel, right) * 8.0 + 400.0, 360.0 - dot(rel, up) * 8.0)

    items: list[str] = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="800" height="520" viewBox="0 0 800 520">',
        '<rect width="800" height="520" fill="#e8eef5"/>',
        '<text x="18" y="26" font-family="Consolas" font-size="14" fill="#111">FNV weapon IK offline proof: projection = actor right/up</text>',
    ]

    def line(a: tuple[float, float, float], b: tuple[float, float, float], color: str, width: int = 4) -> None:
        ax, ay = project(a)
        bx, by = project(b)
        items.append(f'<line x1="{ax:.1f}" y1="{ay:.1f}" x2="{bx:.1f}" y2="{by:.1f}" stroke="{color}" stroke-width="{width}"/>')

    def point(p: tuple[float, float, float], color: str, label: str) -> None:
        x, y = project(p)
        items.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="{color}"/>')
        items.append(f'<text x="{x + 7:.1f}" y="{y - 7:.1f}" font-family="Consolas" font-size="11" fill="{color}">{label}</text>')

    for side, color in (("right", "#d7191c"), ("left", "#2c7bb6")):
        upper = sample.get(f"{side}UpperAfter")
        forearm = sample.get(f"{side}ForearmAfter")
        hand = sample.get(f"{side}HandAfter")
        target = sample.get(f"{side}Target")
        if upper and forearm and hand:
            line(upper, forearm, color)
            line(forearm, hand, color)
            point(upper, color, f"{side} shoulder")
            point(forearm, color, f"{side} elbow")
            point(hand, color, f"{side} wrist")
        if hand and target:
            line(hand, target, "#111", 2)
            point(target, "#111", f"{side} target")

    chest = sample.get("chest")
    head = sample.get("head")
    if chest:
        point(chest, "#159447", "chest")
        line(chest, add(chest, scale(forward, 22.0)), "#159447", 3)
    if chest and head:
        line(chest, head, "#159447", 5)
        point(head, "#159447", "head")

    y = 50
    for key, value in analysis.items():
        if key == "raw":
            continue
        items.append(f'<text x="18" y="{y}" font-family="Consolas" font-size="12" fill="#111">{key}: {value}</text>')
        y += 15
        if y > 500:
            break
    items.append("</svg>")
    path.write_text("\n".join(items), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    lines = args.log.read_text(encoding="utf-8", errors="replace").splitlines()
    samples = [sample for line in lines if (sample := parse_line(line))]
    if not samples:
        raise SystemExit(f"no weapon IK samples found in {args.log}")
    sample = samples[-1]
    result = {
        "sourceLog": str(args.log),
        "sampleCount": len(samples),
        "analysis": analyze(sample),
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "weapon-ik-offline-proof.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    make_svg(sample, result["analysis"], args.out / "weapon-ik-offline-proof.svg")
    print(json.dumps(result["analysis"], indent=2))
    print(f"wrote {args.out / 'weapon-ik-offline-proof.json'}")
    print(f"wrote {args.out / 'weapon-ik-offline-proof.svg'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
