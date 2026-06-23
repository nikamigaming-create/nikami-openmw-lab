#!/usr/bin/env python3
import argparse
import json
import os
import struct
from collections import OrderedDict
from datetime import datetime
from pathlib import Path


DEFAULT_CONTENT = [
    "FalloutNV.esm",
    "DeadMoney.esm",
    "HonestHearts.esm",
    "OldWorldBlues.esm",
    "LonesomeRoad.esm",
    "GunRunnersArsenal.esm",
    "CaravanPack.esm",
    "ClassicPack.esm",
    "MercenaryPack.esm",
    "TribalPack.esm",
]

WEATHER_SELECTIONS = OrderedDict(
    [
        ("Clear", ("NVWastelandGS", "NVWastelandClear", "DefaultWeather")),
        ("Cloudy", ("DefaultWeather", "gWastelandCloudy", "NVWastelandHazy", "NVWastelandClear")),
        ("Foggy", ("NVWastelandHazy", "DefaultWeather", "NVWastelandClear")),
        ("Thunderstorm", ("NVLegateBattleWeather", "UrbanOvercast", "DefaultWeather", "NVWastelandHazy")),
        ("Rain", ("UrbanOvercast", "DefaultWeather", "NVWastelandHazy")),
        ("Overcast", ("UrbanOvercast", "DefaultWeather", "NVWastelandHazy")),
        ("Snow", ("NVJacobstownWeather", "DefaultWeather", "NVWastelandClear")),
        ("Blizzard", ("NVJacobstownWeather", "DefaultWeather", "NVWastelandClear")),
    ]
)

NAM0_GROUPS = OrderedDict(
    [
        ("SkyUpper", 0),
        ("Fog", 1),
        ("Unused0", 2),
        ("Ambient", 3),
        ("Sunlight", 4),
        ("Sun", 5),
        ("Stars", 6),
        ("SkyLower", 7),
        ("Horizon", 8),
        ("Unused1", 9),
    ]
)

TIME_INDICES = OrderedDict(
    [
        ("Sunrise", 0),
        ("Day", 1),
        ("Sunset", 2),
        ("Night", 3),
        ("HighNoon", 4),
        ("Midnight", 5),
    ]
)

OPENMW_TIME_KEYS = OrderedDict(
    [
        ("Sunrise", "Sunrise"),
        ("Day", "Day"),
        ("Sunset", "Sunset"),
        ("Night", "Night"),
    ]
)


def fourcc(raw):
    return raw.decode("ascii", errors="replace")


def zstring(raw):
    if not raw:
        return ""
    end = raw.find(b"\0")
    if end < 0:
        end = len(raw)
    return raw[:end].decode("cp1252", errors="replace")


def read_subrecords(data):
    items = []
    offset = 0
    extended_size = None
    while offset + 6 <= len(data):
        rec_type = fourcc(data[offset : offset + 4])
        size = struct.unpack_from("<H", data, offset + 4)[0]
        offset += 6

        if rec_type == "XXXX":
            if offset + 4 > len(data):
                break
            extended_size = struct.unpack_from("<I", data, offset)[0]
            offset += size
            continue

        if extended_size is not None:
            size = extended_size
            extended_size = None
        if offset + size > len(data):
            break
        items.append((rec_type, data[offset : offset + size]))
        offset += size
    return items


def iter_records(data, start=0, end=None):
    if end is None:
        end = len(data)
    offset = start
    while offset + 24 <= end:
        record_start = offset
        rec_type = fourcc(data[offset : offset + 4])
        size = struct.unpack_from("<I", data, offset + 4)[0]
        offset += 24

        if rec_type == "GRUP":
            group_end = record_start + size
            if group_end <= record_start or group_end > end:
                raise ValueError(f"Invalid GRUP range start={record_start} size={size} end={end}")
            yield from iter_records(data, offset, group_end)
            offset = group_end
            continue

        payload_end = offset + size
        if payload_end > end:
            raise ValueError(f"Invalid record range type={rec_type} start={record_start} size={size} end={end}")
        yield {
            "type": rec_type,
            "formId": struct.unpack_from("<I", data, record_start + 12)[0],
            "payload": data[offset:payload_end],
        }
        offset = payload_end


def first(subrecords, rec_type):
    for current_type, payload in subrecords:
        if current_type == rec_type:
            return payload
    return None


def first_zstring(subrecords, rec_type):
    return zstring(first(subrecords, rec_type) or b"")


def normalized_texture_path(value):
    value = value.replace("\\", "/").strip()
    if not value:
        return ""
    value = value.lower()
    if not value.startswith("textures/"):
        value = "textures/" + value
    return value


def choose_cloud_texture(record):
    candidates = []
    for field in ("BNAM", "DNAM", "CNAM", "ANAM"):
        texture = normalized_texture_path(record["strings"].get(field, ""))
        if not texture:
            continue
        candidates.append({"field": field, "texture": texture})
        if not texture.endswith("/alpha.dds"):
            return {"field": field, "texture": texture, "candidates": candidates}
    if candidates:
        return {"field": candidates[0]["field"], "texture": candidates[0]["texture"], "candidates": candidates}
    raise ValueError(f"WTHR {record['editorId']} has no cloud texture strings")


def parse_wthr_record(plugin, record):
    subrecords = read_subrecords(record["payload"])
    editor_id = first_zstring(subrecords, "EDID")
    if not editor_id:
        return None
    nam0 = first(subrecords, "NAM0")
    if nam0 is None or len(nam0) != 240:
        return {
            "plugin": plugin,
            "formId": f"0x{record['formId']:08x}",
            "editorId": editor_id,
            "unsupportedReason": f"unsupported NAM0 size {0 if nam0 is None else len(nam0)}",
        }
    data = first(subrecords, "DATA") or b""
    strings = {key: first_zstring(subrecords, key) for key in ("DNAM", "CNAM", "ANAM", "BNAM")}
    return {
        "plugin": plugin,
        "formId": f"0x{record['formId']:08x}",
        "editorId": editor_id,
        "strings": strings,
        "nam0": nam0,
        "weatherClassification": data[11] if len(data) >= 12 else None,
    }


def read_wthr_records(fnv_data, content):
    records_by_editor = OrderedDict()
    sources = []
    unsupported = []
    for name in content:
        path = fnv_data / name
        if not path.is_file():
            continue
        data = path.read_bytes()
        count = 0
        for record in iter_records(data):
            if record["type"] != "WTHR":
                continue
            parsed = parse_wthr_record(name, record)
            if parsed is None:
                continue
            if "unsupportedReason" in parsed:
                unsupported.append(parsed)
                continue
            records_by_editor[parsed["editorId"].lower()] = parsed
            count += 1
        sources.append({"plugin": name, "weatherRecords": count})
    return records_by_editor, sources, unsupported


def color_at(record, group_name, time_name):
    group_index = NAM0_GROUPS[group_name]
    time_index = TIME_INDICES[time_name]
    offset = ((group_index * len(TIME_INDICES)) + time_index) * 4
    rgba = record["nam0"][offset : offset + 4]
    return tuple(int(component) for component in rgba[:3])


def color_text(rgb):
    return ",".join(f"{component:03d}" for component in rgb)


def select_record(records_by_editor, candidates):
    for editor_id in candidates:
        record = records_by_editor.get(editor_id.lower())
        if record is not None:
            return record, editor_id
    raise ValueError(f"None of the requested WTHR records exist: {', '.join(candidates)}")


def build_weather_fallbacks(records_by_editor):
    lines = []
    selected = OrderedDict()
    for openmw_name, candidates in WEATHER_SELECTIONS.items():
        record, requested = select_record(records_by_editor, candidates)
        cloud = choose_cloud_texture(record)
        selected[openmw_name] = {
            "requestedCandidates": list(candidates),
            "selectedEditorId": record["editorId"],
            "selectedPlugin": record["plugin"],
            "selectedFormId": record["formId"],
            "weatherClassification": record["weatherClassification"],
            "cloudTexture": cloud,
        }

        lines.append(f"fallback=Weather_{openmw_name}_Cloud_Texture,{cloud['texture']}")
        for openmw_time, source_time in OPENMW_TIME_KEYS.items():
            lines.append(
                f"fallback=Weather_{openmw_name}_Sky_{openmw_time}_Color,"
                f"{color_text(color_at(record, 'SkyUpper', source_time))}"
            )
            lines.append(
                f"fallback=Weather_{openmw_name}_Fog_{openmw_time}_Color,"
                f"{color_text(color_at(record, 'Fog', source_time))}"
            )
            lines.append(
                f"fallback=Weather_{openmw_name}_Ambient_{openmw_time}_Color,"
                f"{color_text(color_at(record, 'Ambient', source_time))}"
            )
            lines.append(
                f"fallback=Weather_{openmw_name}_Sun_{openmw_time}_Color,"
                f"{color_text(color_at(record, 'Sunlight', source_time))}"
            )
        lines.append(
            f"fallback=Weather_{openmw_name}_Sun_Disc_Sunset_Color,"
            f"{color_text(color_at(record, 'Sun', 'Sunset'))}"
        )
    return lines, selected


def main():
    parser = argparse.ArgumentParser(description="Generate OpenMW weather fallback lines from FNV WTHR records.")
    parser.add_argument("--fnv-data", default=os.environ.get("NIKAMI_FNV_DATA", ""))
    parser.add_argument("--content", nargs="+", default=DEFAULT_CONTENT)
    parser.add_argument("--output-lines", required=True)
    parser.add_argument("--output-json", required=True)
    args = parser.parse_args()

    if not args.fnv_data:
        raise SystemExit("Pass --fnv-data or set NIKAMI_FNV_DATA.")
    fnv_data = Path(args.fnv_data)
    if not fnv_data.is_dir():
        raise SystemExit(f"Missing FNV Data directory: {fnv_data}")

    records_by_editor, sources, unsupported = read_wthr_records(fnv_data, args.content)
    if not records_by_editor:
        raise SystemExit(f"No WTHR records found under {fnv_data}")

    lines, selected = build_weather_fallbacks(records_by_editor)
    output_lines = Path(args.output_lines)
    output_json = Path(args.output_json)
    output_lines.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_lines.write_text("\n".join(lines) + "\n", encoding="utf-8")
    result = {
        "status": "PASS",
        "stamp": datetime.now().strftime("%Y%m%d_%H%M%S"),
        "fnvData": str(fnv_data),
        "payloadPolicy": "derived-weather-fallbacks-no-retail-assets",
        "sourceFormat": "FNV WTHR NAM0/PNAM layout",
        "sourceRecordCount": len(records_by_editor),
        "unsupportedSourceRecordCount": len(unsupported),
        "sources": sources,
        "unsupportedSourceRecords": unsupported,
        "fallbackLineCount": len(lines),
        "selectedWeather": selected,
        "fallbacks": lines,
        "approximations": [
            "OpenMW weather fallback keys support sunrise/day/sunset/night; FNV WTHR high-noon and midnight colors are harvested but not emitted by this compatibility bridge.",
            "OpenMW exposes one cloud texture per weather; the generator selects the first non-alpha FNV WTHR cloud layer, preferring layer 3 then layers 0-2.",
            "OpenMW directional sun color is sourced from FNV NAM0 Sunlight; sunset sun-disc color is sourced from FNV NAM0 Sun.",
        ],
        "classification": "loaded-pending-runtime",
        "runtimeBoundary": "Generated fallbacks repair the current OpenMW WeatherManager palette path, but full CLMT/WTHR/REGN runtime weather binding remains a separate gate.",
    }
    output_json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(str(output_lines))
    print(str(output_json))


if __name__ == "__main__":
    main()
