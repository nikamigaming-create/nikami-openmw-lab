#!/usr/bin/env python3
"""Generate no-payload FNV sky texture channel/orientation proof stats."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageStat


TEXTURES = [
    {
        "key": "sun",
        "archive": "Fallout - Textures2.bsa",
        "entry": "textures\\sky\\sun.dds",
        "expectedSize": [512, 512],
    },
    {
        "key": "sunglare",
        "archive": "Fallout - Textures2.bsa",
        "entry": "textures\\sky\\nv_sunglare.dds",
        "expectedSize": [512, 512],
    },
    {
        "key": "moon",
        "archive": "Fallout - Textures2.bsa",
        "entry": "textures\\sky\\skymoonfull.dds",
        "expectedSize": [512, 512],
    },
    {
        "key": "clearCloud",
        "archive": "Fallout - Textures2.bsa",
        "entry": "textures\\sky\\nvcloudlight.dds",
        "expectedSize": [1024, 1024],
    },
    {
        "key": "wastelandUpperSky",
        "archive": "Fallout - Textures2.bsa",
        "entry": "textures\\sky\\nv_wastelanduppersky.dds",
        "expectedSize": [1024, 1024],
    },
]


def round_float(value: float) -> float:
    return round(float(value), 4)


def rgb_luma(rgb: list[float]) -> float:
    return (0.2126 * rgb[0]) + (0.7152 * rgb[1]) + (0.0722 * rgb[2])


def crop_mean_rgba(image: Image.Image, box: tuple[int, int, int, int]) -> list[float]:
    stat = ImageStat.Stat(image.crop(box))
    return [round_float(v) for v in stat.mean[:4]]


def texture_stats(image_path: Path) -> dict:
    with Image.open(image_path) as raw_image:
        image = raw_image.convert("RGBA")
        width, height = image.size
        stat = ImageStat.Stat(image)
        mean = [round_float(v) for v in stat.mean]
        extrema = stat.extrema
        top = crop_mean_rgba(image, (0, 0, width, max(1, height // 8)))
        bottom = crop_mean_rgba(image, (0, max(0, height - max(1, height // 8)), width, height))
        left = crop_mean_rgba(image, (0, 0, max(1, width // 8), height))
        right = crop_mean_rgba(image, (max(0, width - max(1, width // 8)), 0, width, height))

    avg_rgb = mean[:3]
    top_rgb = top[:3]
    bottom_rgb = bottom[:3]
    left_rgb = left[:3]
    right_rgb = right[:3]
    return {
        "size": [width, height],
        "mode": "RGBA",
        "avgRgb": avg_rgb,
        "avgAlpha": mean[3],
        "alphaMin": int(extrema[3][0]),
        "alphaMax": int(extrema[3][1]),
        "avgLuma": round_float(rgb_luma(avg_rgb)),
        "topRgb": top_rgb,
        "topLuma": round_float(rgb_luma(top_rgb)),
        "bottomRgb": bottom_rgb,
        "bottomLuma": round_float(rgb_luma(bottom_rgb)),
        "topMinusBottomLuma": round_float(rgb_luma(top_rgb) - rgb_luma(bottom_rgb)),
        "leftRgb": left_rgb,
        "rightRgb": right_rgb,
        "leftMinusRightLuma": round_float(rgb_luma(left_rgb) - rgb_luma(right_rgb)),
    }


def extract_entry(bsa_tool: Path, archive: Path, entry: str, temp_dir: Path) -> Path:
    subprocess.run(
        [str(bsa_tool), "extract", str(archive), entry, str(temp_dir)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    extracted = temp_dir / Path(entry).name
    if extracted.exists():
        return extracted
    matches = list(temp_dir.rglob(Path(entry).name))
    if not matches:
        raise FileNotFoundError(f"bsatool did not extract {entry} from {archive}")
    return matches[0]


def add_assertions(key: str, row: dict) -> dict:
    avg = row["avgRgb"]
    alpha_min = row["alphaMin"]
    alpha_max = row["alphaMax"]
    top_minus_bottom_luma = row["topMinusBottomLuma"]

    if key == "sun":
        return {
            "sunWarmChannelOrder": avg[0] > avg[1] + 8.0 and avg[1] > avg[2] + 20.0,
            "sunHasCutoutAlpha": alpha_min == 0 and alpha_max == 255,
        }
    if key == "sunglare":
        return {
            "sunglareNeutralChannelBalance": max(avg) - min(avg) <= 2.0,
            "sunglareHasCutoutAlpha": alpha_min == 0 and alpha_max == 255,
        }
    if key == "moon":
        return {
            "moonCoolChannelOrder": avg[2] > avg[1] + 3.0 and avg[1] > avg[0] + 5.0,
            "moonHasCutoutAlpha": alpha_min == 0 and alpha_max == 255,
        }
    if key == "clearCloud":
        return {
            "clearCloudBlueGreenRedOrder": avg[2] > avg[1] + 1.0 and avg[1] > avg[0] + 8.0,
            "clearCloudTopBrighterThanBottom": top_minus_bottom_luma >= 8.0,
            "clearCloudHasCutoutAlpha": alpha_min == 0 and alpha_max == 255,
        }
    if key == "wastelandUpperSky":
        return {
            "wastelandUpperSkyBlueDominance": avg[2] > avg[1] + 30.0 and avg[1] > avg[0] + 20.0,
            "wastelandUpperSkyLowAlphaMask": alpha_min == 0 and 40 <= alpha_max <= 100,
        }
    raise KeyError(key)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fnv-data", required=True, type=Path)
    parser.add_argument("--bsa-tool", required=True, type=Path)
    parser.add_argument("--proof-dir", required=True, type=Path)
    args = parser.parse_args()

    if not args.fnv_data.is_dir():
        raise SystemExit(f"Missing FNV Data directory: {args.fnv_data}")
    if not args.bsa_tool.is_file():
        raise SystemExit(f"Missing bsatool: {args.bsa_tool}")

    args.proof_dir.mkdir(parents=True, exist_ok=True)
    temp_dir = Path(tempfile.mkdtemp(prefix="nikami_fnv_sky_texture_stats_"))
    rows: list[dict] = []
    failures: list[str] = []
    try:
        for texture in TEXTURES:
            archive = args.fnv_data / texture["archive"]
            if not archive.is_file():
                failures.append(f"missing archive {archive}")
                continue
            extracted = extract_entry(args.bsa_tool, archive, texture["entry"], temp_dir)
            stats = texture_stats(extracted)
            assertions = add_assertions(texture["key"], stats)
            size_matches = stats["size"] == texture["expectedSize"]
            if not size_matches:
                failures.append(f"{texture['key']} size {stats['size']} != {texture['expectedSize']}")
            for name, passed in assertions.items():
                if not passed:
                    failures.append(f"{texture['key']} assertion failed: {name}")
            rows.append(
                {
                    "key": texture["key"],
                    "archive": texture["archive"],
                    "entry": texture["entry"].replace("\\", "/"),
                    "classification": "runtime-supported",
                    "runtimeBoundary": (
                        "Derived numeric DDS stats prove source dimensions/channel signatures only; live sky "
                        "visibility and shader/material binding are proved by the FNV sky runtime contract."
                    ),
                    "expectedSize": texture["expectedSize"],
                    "sizeMatches": size_matches,
                    **stats,
                    "assertions": assertions,
                }
            )
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)

    result = {
        "status": "PASS" if not failures else "FAIL",
        "payloadPolicy": "derived-sky-texture-stats-no-retail-payloads",
        "tempExtractionPolicy": "retail DDS entries are extracted only under the process temp directory and deleted",
        "sourceFormat": "FNV DDS sky texture entries from Fallout - Textures2.bsa",
        "textures": rows,
        "failures": failures,
    }
    output = args.proof_dir / "fnv-sky-texture-stats.json"
    output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"FNV sky texture stats JSON: {output}")
    print(f"FNV sky texture stats status: {result['status']}")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
