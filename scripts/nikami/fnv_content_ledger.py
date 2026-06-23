#!/usr/bin/env python3
import argparse
import json
import os
import struct
from collections import Counter
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
    "FNVR.esp",
]


def fourcc(raw):
    return raw.decode("ascii", errors="replace")


def hex_form(value):
    return f"0x{value:08x}"


def zstring(raw):
    if not raw:
        return ""
    end = raw.find(b"\0")
    if end < 0:
        end = len(raw)
    if end == 0:
        return ""
    return raw[:end].decode("cp1252", errors="replace")


def get_u16(raw, offset):
    if len(raw) < offset + 2:
        return None
    return struct.unpack_from("<H", raw, offset)[0]


def get_u32(raw, offset):
    if len(raw) < offset + 4:
        return None
    return struct.unpack_from("<I", raw, offset)[0]


def get_float(raw, offset):
    if len(raw) < offset + 4:
        return None
    return struct.unpack_from("<f", raw, offset)[0]


def read_subrecords(data):
    items = []
    offset = 0
    extended_size = None
    size_limit = len(data)
    while offset + 6 <= size_limit:
        rec_type = fourcc(data[offset : offset + 4])
        size = struct.unpack_from("<H", data, offset + 4)[0]
        offset += 6

        if rec_type == "XXXX":
            if offset + 4 > size_limit:
                break
            extended_size = struct.unpack_from("<I", data, offset)[0]
            offset += size
            continue

        if extended_size is not None:
            size = extended_size
            extended_size = None
        if offset + size > size_limit:
            break
        items.append((rec_type, data[offset : offset + size]))
        offset += size
    return items


def first(subrecords, rec_type):
    for current_type, payload in subrecords:
        if current_type == rec_type:
            return payload
    return None


def first_zstring(subrecords, rec_type):
    return zstring(first(subrecords, rec_type) or b"")


def all_form_ids(subrecords, rec_type):
    result = []
    for current_type, payload in subrecords:
        if current_type == rec_type and len(payload) >= 4:
            result.append(hex_form(struct.unpack_from("<I", payload, 0)[0]))
    return result


def subrecord_sizes(subrecords, rec_type):
    return [len(payload) for current_type, payload in subrecords if current_type == rec_type]


def add_reference(references, plugin, owner_type, owner_form_id, field, value):
    if value is None or value == 0:
        return
    references.append(
        {
            "plugin": plugin,
            "ownerType": owner_type,
            "ownerFormId": owner_form_id,
            "field": field,
            "targetFormId": hex_form(value),
        }
    )


def add_form_id_subrecord_refs(references, plugin, owner_type, owner_form_id, subrecords, fields):
    for subrecord_type, field_name in fields:
        index = 0
        for current_type, payload in subrecords:
            if current_type == subrecord_type and len(payload) >= 4:
                suffix = "" if index == 0 else f"[{index}]"
                add_reference(
                    references,
                    plugin,
                    owner_type,
                    owner_form_id,
                    field_name + suffix,
                    struct.unpack_from("<I", payload, 0)[0],
                )
                index += 1


def conditions(subrecords):
    result = []
    index = 0
    for rec_type, payload in subrecords:
        if rec_type not in ("CTDA", "CTDT"):
            continue
        function = None
        param1 = None
        param2 = None
        reference = None
        if len(payload) >= 28:
            function = get_u16(payload, 8)
            param1 = get_u32(payload, 12)
            param2 = get_u32(payload, 16)
            reference = get_u32(payload, 24)
        elif len(payload) >= 20:
            function = get_u16(payload, 8)
            param1 = get_u32(payload, 12)
            param2 = get_u32(payload, 16)
        result.append(
            {
                "index": index,
                "subrecord": rec_type,
                "size": len(payload),
                "comparisonValue": get_float(payload, 0),
                "function": function,
                "param1": hex_form(param1) if param1 is not None else "",
                "param2": hex_form(param2) if param2 is not None else "",
                "reference": hex_form(reference) if reference is not None else "",
            }
        )
        index += 1
    return result


def script_block(subrecords):
    source = first_zstring(subrecords, "SCTX")
    local_vars = []
    last_local = None
    for rec_type, payload in subrecords:
        if rec_type == "SLSD" and len(payload) >= 20:
            last_local = {
                "index": struct.unpack_from("<I", payload, 0)[0],
                "type": struct.unpack_from("<I", payload, 16)[0],
                "name": "",
            }
            local_vars.append(last_local)
        elif rec_type == "SCVR" and last_local is not None:
            last_local["name"] = zstring(payload)
    return {
        "source": source,
        "sourceLength": len(source),
        "localVariables": local_vars,
        "referencedForms": all_form_ids(subrecords, "SCRO"),
    }


def quest_row(plugin, record, subrecords):
    data = first(subrecords, "DATA")
    return {
        "plugin": plugin,
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "questName": first_zstring(subrecords, "FULL"),
        "flags": record["flagsHex"],
        "questFlags": data[0] if data and len(data) >= 1 else None,
        "priority": data[1] if data and len(data) >= 2 else None,
        "questDelay": get_float(data, 4) if data and len(data) >= 8 else None,
        "questScript": (all_form_ids(subrecords, "SCRI") or [None])[0],
        "targetConditions": conditions(subrecords),
        "embeddedScript": script_block(subrecords),
        "objectiveSubrecords": [
            {"type": rec_type, "size": len(payload), "text": zstring(payload)}
            for rec_type, payload in subrecords
            if rec_type in ("INDX", "QSDT", "CNAM", "NNAM", "QOBJ", "QSTA", "NAM0")
        ],
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-quest-execution",
    }


def dialogue_row(plugin, record, subrecords):
    data = first(subrecords, "DATA")
    pnam = first(subrecords, "PNAM")
    return {
        "plugin": plugin,
        "recordType": "DIAL",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "topicName": first_zstring(subrecords, "FULL"),
        "quests": all_form_ids(subrecords, "QSTI"),
        "removedQuests": all_form_ids(subrecords, "QSTR"),
        "dialDataSize": len(data) if data else 0,
        "priority": get_float(pnam, 0) if pnam else None,
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-info-selection",
    }


def info_row(plugin, record, subrecords):
    data = first(subrecords, "DATA")
    sounds = all_form_ids(subrecords, "SNDD") + all_form_ids(subrecords, "SNAM")
    return {
        "plugin": plugin,
        "recordType": "INFO",
        "formId": record["formId"],
        "quest": (all_form_ids(subrecords, "QSTI") or [None])[0],
        "sound": (sounds or [None])[0],
        "response": first_zstring(subrecords, "NAM1"),
        "notes": first_zstring(subrecords, "NAM2"),
        "edits": first_zstring(subrecords, "NAM3"),
        "dialType": data[0] if data and len(data) >= 1 else None,
        "nextSpeaker": data[1] if data and len(data) >= 2 else None,
        "infoFlags": get_u16(data, 2) if data and len(data) >= 4 else None,
        "choices": all_form_ids(subrecords, "TCLT"),
        "addTopics": all_form_ids(subrecords, "NAME"),
        "conditions": conditions(subrecords),
        "resultScript": script_block(subrecords),
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "missing-esm4-info-store",
    }


def script_row(plugin, record, subrecords):
    script = script_block(subrecords)
    return {
        "plugin": plugin,
        "recordType": "SCPT",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "source": script["source"],
        "sourceLength": script["sourceLength"],
        "localVariables": script["localVariables"],
        "referencedForms": script["referencedForms"],
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "missing-fnv-script-runtime",
    }


def perk_row(plugin, record, subrecords):
    script = script_block(subrecords)
    return {
        "plugin": plugin,
        "recordType": "PERK",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "fullName": first_zstring(subrecords, "FULL"),
        "descriptionLength": len(first_zstring(subrecords, "DESC")),
        "icon": first_zstring(subrecords, "ICON"),
        "dataSizes": subrecord_sizes(subrecords, "DATA"),
        "conditionCount": len(conditions(subrecords)),
        "effectTypeCount": len(subrecord_sizes(subrecords, "EPFT")),
        "effectDataSizes": subrecord_sizes(subrecords, "EPFD"),
        "effectRankCount": len(subrecord_sizes(subrecords, "PRKC")),
        "effectEntryCount": len(subrecord_sizes(subrecords, "PRKE")),
        "scriptSourceLength": script["sourceLength"],
        "scriptReferenceCount": len(script["referencedForms"]),
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-player-perk-trait-binding",
    }


def projectile_row(plugin, record, subrecords):
    return {
        "plugin": plugin,
        "recordType": "PROJ",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "fullName": first_zstring(subrecords, "FULL"),
        "model": first_zstring(subrecords, "MODL"),
        "dataSizes": subrecord_sizes(subrecords, "DATA"),
        "objectBoundsSizes": subrecord_sizes(subrecords, "OBND"),
        "modelDataSizes": subrecord_sizes(subrecords, "MODT"),
        "nameData1Sizes": subrecord_sizes(subrecords, "NAM1"),
        "nameData2Sizes": subrecord_sizes(subrecords, "NAM2"),
        "soundLevelValues": [get_u32(payload, 0) for current_type, payload in subrecords if current_type == "VNAM"],
        "destructibleChunkCount": len(subrecord_sizes(subrecords, "DEST"))
        + len(subrecord_sizes(subrecords, "DSTD")),
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-projectile-definition-binding",
    }


def explosion_row(plugin, record, subrecords):
    return {
        "plugin": plugin,
        "recordType": "EXPL",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        "fullName": first_zstring(subrecords, "FULL"),
        "model": first_zstring(subrecords, "MODL"),
        "dataSizes": subrecord_sizes(subrecords, "DATA"),
        "magicEffects": all_form_ids(subrecords, "EITM"),
        "impactDataSets": all_form_ids(subrecords, "MNAM"),
        "modelDataSizes": subrecord_sizes(subrecords, "MODT"),
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-explosion-effect-binding",
    }


def simple_value(record_type, editor_id, data):
    if data is None:
        return None
    if record_type == "GMST":
        if editor_id.startswith("s"):
            return zstring(data)
        if editor_id.startswith("f"):
            return get_float(data, 0)
        if editor_id.startswith(("i", "b")) and len(data) >= 4:
            return struct.unpack_from("<i", data, 0)[0]
    if record_type == "GLOB":
        return get_float(data, 0)
    return None


def simple_row(plugin, record, subrecords):
    editor_id = first_zstring(subrecords, "EDID")
    data = first(subrecords, "DATA")
    return {
        "plugin": plugin,
        "recordType": record["type"],
        "formId": record["formId"],
        "editorId": editor_id,
        "fullName": first_zstring(subrecords, "FULL"),
        "value": simple_value(record["type"], editor_id, data),
        "dataSize": len(data) if data else 0,
        "subrecordTypes": [rec_type for rec_type, _ in subrecords],
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "missing-esm4-runtime-store",
    }


def parse_plugin(path, plugin):
    data = path.read_bytes()
    counts = Counter()
    quests = []
    dialogue = []
    scripts = []
    globals_ = []
    game_settings = []
    gameplay_systems = []
    references = []

    def read_range(offset, end):
        while offset + 24 <= end:
            record_start = offset
            rec_type = fourcc(data[offset : offset + 4])
            size, flags, form_id = struct.unpack_from("<III", data, offset + 4)
            offset += 24

            if rec_type == "GRUP":
                group_end = record_start + size
                if group_end <= record_start or group_end > end:
                    raise ValueError(
                        f"Invalid GRUP range in {path}: start={record_start} size={size} end={group_end} limit={end}"
                    )
                offset = read_range(offset, group_end)
                continue

            next_offset = offset + size
            if next_offset < offset or next_offset > end:
                raise ValueError(
                    f"Invalid ESM4 record range in {path}: type={rec_type} start={record_start} "
                    f"size={size} next={next_offset} limit={end}"
                )

            counts[rec_type] += 1
            if rec_type in {"QUST", "DIAL", "INFO", "SCPT", "GLOB", "GMST", "PERK", "PROJ", "EXPL"}:
                payload = data[offset:next_offset]
                subrecords = read_subrecords(payload)
                record = {
                    "type": rec_type,
                    "formId": hex_form(form_id),
                    "flags": flags,
                    "flagsHex": f"0x{flags:08x}",
                }
                if rec_type == "QUST":
                    quests.append(quest_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references, plugin, "QUST", record["formId"], subrecords, [("SCRI", "questScript")]
                    )
                elif rec_type == "DIAL":
                    dialogue.append(dialogue_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references,
                        plugin,
                        "DIAL",
                        record["formId"],
                        subrecords,
                        [("QSTI", "quest"), ("QSTR", "removedQuest")],
                    )
                elif rec_type == "INFO":
                    dialogue.append(info_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references,
                        plugin,
                        "INFO",
                        record["formId"],
                        subrecords,
                        [
                            ("QSTI", "quest"),
                            ("SNDD", "sound"),
                            ("SNAM", "sound"),
                            ("TCLT", "choice"),
                            ("NAME", "addTopic"),
                            ("SCRO", "scriptRef"),
                        ],
                    )
                elif rec_type == "SCPT":
                    scripts.append(script_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references, plugin, "SCPT", record["formId"], subrecords, [("SCRO", "scriptRef")]
                    )
                elif rec_type == "GLOB":
                    globals_.append(simple_row(plugin, record, subrecords))
                elif rec_type == "GMST":
                    game_settings.append(simple_row(plugin, record, subrecords))
                elif rec_type == "PERK":
                    gameplay_systems.append(perk_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references, plugin, "PERK", record["formId"], subrecords, [("SCRO", "scriptRef")]
                    )
                elif rec_type == "PROJ":
                    gameplay_systems.append(projectile_row(plugin, record, subrecords))
                elif rec_type == "EXPL":
                    gameplay_systems.append(explosion_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references,
                        plugin,
                        "EXPL",
                        record["formId"],
                        subrecords,
                        [("EITM", "magicEffect"), ("MNAM", "impactDataSet")],
                    )
            offset = next_offset
        return offset

    read_range(0, len(data))
    return {
        "plugin": plugin,
        "path": str(path),
        "length": path.stat().st_size,
        "records": [{"type": key, "count": counts[key]} for key in sorted(counts)],
        "quests": quests,
        "dialogue": dialogue,
        "scripts": scripts,
        "globals": globals_,
        "gameSettings": game_settings,
        "gameplaySystems": gameplay_systems,
        "references": references,
    }


def write_json(path, value, depth=None):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Build a structured Fallout New Vegas ESM4 content ledger.")
    parser.add_argument("--fnv-root", default=os.environ.get("NIKAMI_FNV_ROOT", ""))
    parser.add_argument("--fnv-data", default=os.environ.get("NIKAMI_FNV_DATA", ""))
    parser.add_argument("--proof-root", default="")
    parser.add_argument("--repo-root", default="")
    parser.add_argument("--content", nargs="+", default=DEFAULT_CONTENT)
    args = parser.parse_args()

    if not args.fnv_root and not args.fnv_data:
        raise SystemExit("Set --fnv-root, --fnv-data, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof.")
    fnv_data = Path(args.fnv_data or Path(args.fnv_root) / "Data")
    fnv_root = Path(args.fnv_root or fnv_data.parent)
    repo_root = Path(args.repo_root or Path(__file__).resolve().parents[2])
    proof_root = Path(args.proof_root or repo_root.parent / "proof")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    proof_dir = proof_root / "fnv-content-ledger" / stamp
    proof_dir.mkdir(parents=True, exist_ok=True)

    summary_file = proof_dir / "summary.txt"

    def proof_line(text=""):
        print(text, flush=True)
        with summary_file.open("a", encoding="utf-8") as stream:
            stream.write(text + "\n")

    proof_line(f"FNV structured content ledger proof {stamp}")
    proof_line(f"RepoRoot: {repo_root}")
    proof_line(f"FnvRoot: {fnv_root}")
    proof_line(f"FnvData: {fnv_data}")
    proof_line(f"ProofDir: {proof_dir}")
    proof_line()

    missing = []
    plugin_ledgers = []
    for name in args.content:
        path = fnv_data / name
        if not path.is_file():
            missing.append(name)
            proof_line(f"FAIL missing content: {name} -> {path}")
            continue
        proof_line(f"Parsing content: {name}")
        plugin_ledgers.append(parse_plugin(path, name))
    if missing:
        raise SystemExit(f"Missing active content files: {', '.join(missing)}")

    records = [
        {"plugin": row["plugin"], "path": row["path"], "length": row["length"], "records": row["records"]}
        for row in plugin_ledgers
    ]
    quests = [item for row in plugin_ledgers for item in row["quests"]]
    dialogue = [item for row in plugin_ledgers for item in row["dialogue"]]
    scripts = [item for row in plugin_ledgers for item in row["scripts"]]
    globals_ = [item for row in plugin_ledgers for item in row["globals"]]
    game_settings = [item for row in plugin_ledgers for item in row["gameSettings"]]
    gameplay_systems = [item for row in plugin_ledgers for item in row["gameplaySystems"]]
    references = [item for row in plugin_ledgers for item in row["references"]]
    total_records = sum(record["count"] for plugin in records for record in plugin["records"])
    gameplay_counts = Counter(item["recordType"] for item in gameplay_systems)

    artifacts = {
        "records": proof_dir / "records.json",
        "quests": proof_dir / "quests.json",
        "dialogue": proof_dir / "dialogue.json",
        "scripts": proof_dir / "scripts.json",
        "globals": proof_dir / "globals.json",
        "gameSettings": proof_dir / "game-settings.json",
        "gameplaySystems": proof_dir / "gameplay-systems.json",
        "references": proof_dir / "references.json",
        "summary": summary_file,
        "result": proof_dir / "result.json",
    }
    write_json(artifacts["records"], records)
    write_json(artifacts["quests"], quests)
    write_json(artifacts["dialogue"], dialogue)
    write_json(artifacts["scripts"], scripts)
    write_json(artifacts["globals"], globals_)
    write_json(artifacts["gameSettings"], game_settings)
    write_json(artifacts["gameplaySystems"], gameplay_systems)
    write_json(artifacts["references"], references)

    result = {
        "status": "PASS",
        "stamp": stamp,
        "repoRoot": str(repo_root),
        "fnvData": str(fnv_data),
        "content": args.content,
        "pluginCount": len(plugin_ledgers),
        "recordTotal": total_records,
        "questCount": len(quests),
        "dialogueRowCount": len(dialogue),
        "scriptCount": len(scripts),
        "globalCount": len(globals_),
        "gameSettingCount": len(game_settings),
        "gameplaySystemCount": len(gameplay_systems),
        "gameplaySystemCounts": dict(sorted(gameplay_counts.items())),
        "referenceCount": len(references),
        "artifacts": {key: str(value) for key, value in artifacts.items()},
    }
    write_json(artifacts["result"], result)

    proof_line()
    proof_line("FNV structured ledger artifacts:")
    for key in (
        "records",
        "quests",
        "dialogue",
        "scripts",
        "globals",
        "gameSettings",
        "gameplaySystems",
        "references",
        "result",
    ):
        proof_line(f"{key}: {artifacts[key]}")
    proof_line()
    proof_line("FNV structured content ledger proof PASS")
    proof_line(
        "plugins={plugins} records={records} quests={quests} dialogueRows={dialogue} scripts={scripts} "
        "globals={globals} gameSettings={settings} gameplaySystems={gameplay} references={references}".format(
            plugins=len(plugin_ledgers),
            records=total_records,
            quests=len(quests),
            dialogue=len(dialogue),
            scripts=len(scripts),
            globals=len(globals_),
            settings=len(game_settings),
            gameplay=len(gameplay_systems),
            references=len(references),
        )
    )
    proof_line("gameplaySystemCounts=" + json.dumps(dict(sorted(gameplay_counts.items())), sort_keys=True))


if __name__ == "__main__":
    main()
