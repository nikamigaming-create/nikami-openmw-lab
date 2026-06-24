#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
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


ALLOWED_CONDITION_CLASSIFICATIONS = {
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof",
}


CONDITION_FUNCTION_SEMANTIC_PROOFS = {
    56: {
        "gate": "test-fnv-quest-status-runtime-contract.ps1",
        "runtimeBoundary": "selected-quest-status-opcode-runtime-supported",
        "proof": "GetQuestRunning reads selected real FNV quest status through the MWScript runtime proof.",
    },
    58: {
        "gate": "test-fnv-quest-setstage-runtime-contract.ps1",
        "runtimeBoundary": "selected-setstage-getstage-opcode-runtime-supported",
        "proof": "GetStage reads a selected real FNV quest stage after SetStage drives the runtime journal.",
    },
    59: {
        "gate": "test-fnv-quest-stage-done-runtime-contract.ps1",
        "runtimeBoundary": "selected-stage-done-entry-state-runtime-supported",
        "proof": "GetStageDone distinguishes SetJournalIndex from SetStage against selected real FNV quest entry state.",
    },
    420: {
        "gate": "test-fnv-quest-objective-runtime-contract.ps1",
        "runtimeBoundary": "selected-quest-objective-state-runtime-supported",
        "proof": "GetObjectiveCompleted reads selected FNV objective completed state through journal-backed runtime state.",
    },
    421: {
        "gate": "test-fnv-quest-objective-runtime-contract.ps1",
        "runtimeBoundary": "selected-quest-objective-state-runtime-supported",
        "proof": "GetObjectiveDisplayed reads selected FNV objective displayed state through journal-backed runtime state.",
    },
    546: {
        "gate": "test-fnv-quest-status-runtime-contract.ps1",
        "runtimeBoundary": "selected-quest-status-opcode-runtime-supported",
        "proof": "GetQuestCompleted reads selected real FNV quest completed state through the MWScript runtime proof.",
    },
}


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


def text_hash(value):
    if not value:
        return ""
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def text_summary(prefix, value):
    return {
        f"{prefix}Length": len(value),
        f"{prefix}Hash": text_hash(value),
    }


def get_u16(raw, offset):
    if len(raw) < offset + 2:
        return None
    return struct.unpack_from("<H", raw, offset)[0]


def get_u8(raw, offset):
    if len(raw) < offset + 1:
        return None
    return raw[offset]


def get_u32(raw, offset):
    if len(raw) < offset + 4:
        return None
    return struct.unpack_from("<I", raw, offset)[0]


def get_float(raw, offset):
    if len(raw) < offset + 4:
        return None
    return struct.unpack_from("<f", raw, offset)[0]


def load_condition_function_names(repo_root):
    path = repo_root / "components" / "esm4" / "script.hpp"
    result = {}
    if not path.is_file():
        return result
    pattern = re.compile(r"\b(FUN_[A-Za-z0-9_]+)\s*=\s*([0-9]+)")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.search(line)
        if match:
            result[int(match.group(2))] = match.group(1)
    return result


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


def all_form_ids_at_offset(subrecords, rec_type, offset):
    result = []
    for current_type, payload in subrecords:
        if current_type == rec_type and len(payload) >= offset + 4:
            result.append(hex_form(struct.unpack_from("<I", payload, offset)[0]))
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


def condition_schema(record_type):
    if record_type == "PACK":
        return "PACK-CTDA"
    return "TargetCondition"


def condition_from_payload(rec_type, payload, index, schema="TargetCondition"):
    condition_value = get_u32(payload, 0)
    function = get_u32(payload, 8)
    param1 = get_u32(payload, 12)
    param2 = get_u32(payload, 16)
    tail_value = get_u32(payload, 20) if len(payload) >= 24 else None
    reference = get_u32(payload, 24) if schema == "TargetCondition" and len(payload) >= 28 else None
    row = {
        "index": index,
        "subrecord": rec_type,
        "size": len(payload),
        "conditionSchema": schema,
        "conditionFlags": hex_form(condition_value) if condition_value is not None else "",
        "comparisonValue": get_float(payload, 4),
        "function": function,
        "param1": hex_form(param1) if param1 is not None else "",
        "param2": hex_form(param2) if param2 is not None else "",
        "reference": hex_form(reference) if reference is not None else "",
    }
    if schema == "PACK-CTDA":
        row["unknownTail"] = hex_form(tail_value) if tail_value is not None else ""
    else:
        row["runOn"] = tail_value
    return row


def conditions(subrecords, record_type=""):
    result = []
    index = 0
    schema = condition_schema(record_type)
    for rec_type, payload in subrecords:
        if rec_type not in ("CTDA", "CTDT"):
            continue
        result.append(condition_from_payload(rec_type, payload, index, schema))
        index += 1
    return result


def quest_top_level_conditions(subrecords):
    result = []
    current_entry = None
    current_target = None
    index = 0

    for rec_type, payload in subrecords:
        if rec_type == "INDX":
            current_entry = None
            current_target = None
        elif rec_type == "QSDT":
            current_entry = True
            current_target = None
        elif rec_type == "QOBJ":
            current_entry = None
            current_target = None
        elif rec_type == "QSTA":
            current_target = True
        elif rec_type in ("CTDA", "CTDT") and current_entry is None and current_target is None:
            result.append(condition_from_payload(rec_type, payload, index, "TargetCondition"))
            index += 1
    return result


def make_condition_output_row(plugin, record, editor_id, owner_scope, condition, raw_index, **owner_fields):
    row = {
        "plugin": plugin,
        "recordType": record["type"],
        "formId": record["formId"],
        "editorId": editor_id,
        "ownerScope": owner_scope,
        "rawConditionIndex": raw_index,
    }
    row.update(owner_fields)
    row.update(condition)
    row["conditionIndex"] = row.pop("index")
    return row


def quest_condition_rows_for_record(plugin, record, subrecords):
    result = []
    editor_id = first_zstring(subrecords, "EDID")
    raw_index = 0
    top_level_index = 0
    current_stage_index = None
    current_entry_index = None
    current_entry_condition_index = 0
    current_objective_index = None
    current_target_index = None
    current_target_form_id = ""
    current_target_condition_index = 0

    for rec_type, payload in subrecords:
        if rec_type == "INDX":
            current_stage_index = get_u16(payload, 0)
            current_entry_index = None
            current_entry_condition_index = 0
            current_objective_index = None
            current_target_index = None
            current_target_form_id = ""
            current_target_condition_index = 0
        elif rec_type == "QSDT":
            if current_stage_index is None:
                current_stage_index = 0
            current_entry_index = 0 if current_entry_index is None else current_entry_index + 1
            current_entry_condition_index = 0
            current_objective_index = None
            current_target_index = None
            current_target_form_id = ""
            current_target_condition_index = 0
        elif rec_type == "QOBJ":
            current_stage_index = None
            current_entry_index = None
            current_entry_condition_index = 0
            current_objective_index = get_u32(payload, 0)
            current_target_index = None
            current_target_form_id = ""
            current_target_condition_index = 0
        elif rec_type == "QSTA":
            current_target_index = 0 if current_target_index is None else current_target_index + 1
            target = get_u32(payload, 0)
            current_target_form_id = hex_form(target) if target is not None else ""
            current_target_condition_index = 0
        elif rec_type in ("CTDA", "CTDT"):
            if current_objective_index is not None and current_target_index is not None:
                condition = condition_from_payload(rec_type, payload, current_target_condition_index, "TargetCondition")
                result.append(
                    make_condition_output_row(
                        plugin,
                        record,
                        editor_id,
                        "QUST-objective-target",
                        condition,
                        raw_index,
                        objectiveIndex=current_objective_index,
                        targetIndex=current_target_index,
                        targetFormId=current_target_form_id,
                    )
                )
                current_target_condition_index += 1
            elif current_entry_index is not None:
                condition = condition_from_payload(rec_type, payload, current_entry_condition_index, "TargetCondition")
                result.append(
                    make_condition_output_row(
                        plugin,
                        record,
                        editor_id,
                        "QUST-stage-entry",
                        condition,
                        raw_index,
                        stageIndex=current_stage_index,
                        entryIndex=current_entry_index,
                    )
                )
                current_entry_condition_index += 1
            else:
                condition = condition_from_payload(rec_type, payload, top_level_index, "TargetCondition")
                result.append(
                    make_condition_output_row(
                        plugin,
                        record,
                        editor_id,
                        "QUST-top-level",
                        condition,
                        raw_index,
                    )
                )
                top_level_index += 1
            raw_index += 1
    return result


def condition_rows_for_record(plugin, record, subrecords):
    if record["type"] == "QUST":
        return quest_condition_rows_for_record(plugin, record, subrecords)
    result = []
    editor_id = first_zstring(subrecords, "EDID")
    for condition in conditions(subrecords, record["type"]):
        result.append(
            make_condition_output_row(
                plugin,
                record,
                editor_id,
                record["type"] if record["type"] in ("INFO", "PERK", "PACK") else "record",
                condition,
                condition.get("index"),
            )
        )
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
            }
            local_vars.append(last_local)
        elif rec_type == "SCVR" and last_local is not None:
            name = zstring(payload)
            last_local.pop("name", None)
            last_local.update(text_summary("name", name))
    return {
        "sourceLength": len(source),
        "sourceHash": text_hash(source),
        "localVariables": local_vars,
        "referencedForms": all_form_ids(subrecords, "SCRO"),
    }


def quest_stages(subrecords):
    stages = []
    current_stage = None
    current_entry = None
    current_target = None

    for rec_type, payload in subrecords:
        if rec_type == "QOBJ":
            current_stage = None
            current_entry = None
            current_target = None
            continue
        if rec_type == "INDX":
            current_stage = {
                "stageIndex": get_u16(payload, 0),
                "logEntries": [],
            }
            stages.append(current_stage)
            current_entry = None
            current_target = None
        elif rec_type == "QSDT":
            if current_stage is None:
                current_stage = {
                    "stageIndex": None,
                    "logEntries": [],
                }
                stages.append(current_stage)
            current_entry = {
                "flags": payload[0] if payload else None,
                "conditionCount": 0,
                "conditions": [],
                "nextQuest": "",
                **text_summary("log", ""),
                "script": {
                    "sourceLength": 0,
                    "sourceHash": "",
                    "localVariableCount": 0,
                    "referencedFormCount": 0,
                },
            }
            current_stage["logEntries"].append(current_entry)
            current_target = None
        elif rec_type == "CNAM" and current_entry is not None:
            current_entry.update(text_summary("log", zstring(payload)))
        elif rec_type == "NAM0" and current_entry is not None and len(payload) >= 4:
            current_entry["nextQuest"] = hex_form(struct.unpack_from("<I", payload, 0)[0])
        elif rec_type in ("CTDA", "CTDT") and current_entry is not None and current_target is None:
            current_entry["conditions"].append(condition_from_payload(rec_type, payload, current_entry["conditionCount"]))
            current_entry["conditionCount"] = len(current_entry["conditions"])
        elif rec_type == "SCHR" and current_entry is not None:
            current_entry["script"]["headerSize"] = len(payload)
        elif rec_type == "SCTX" and current_entry is not None:
            source = zstring(payload)
            current_entry["script"]["sourceLength"] = len(source)
            current_entry["script"]["sourceHash"] = text_hash(source)
        elif rec_type == "SLSD" and current_entry is not None:
            current_entry["script"]["localVariableCount"] += 1
        elif rec_type == "SCRO" and current_entry is not None:
            current_entry["script"]["referencedFormCount"] += 1
    return stages


def quest_objectives(subrecords):
    objectives = []
    current_objective = None
    current_target = None

    for rec_type, payload in subrecords:
        if rec_type == "INDX":
            current_objective = None
            current_target = None
            continue
        if rec_type == "QOBJ":
            current_objective = {
                "objectiveIndex": get_u32(payload, 0),
                **text_summary("description", ""),
                "targets": [],
            }
            objectives.append(current_objective)
            current_target = None
        elif rec_type == "NNAM" and current_objective is not None:
            current_objective.update(text_summary("description", zstring(payload)))
        elif rec_type == "QSTA" and current_objective is not None:
            target = get_u32(payload, 0)
            current_target = {
                "targetFormId": hex_form(target) if target is not None else "",
                "flags": payload[4] if len(payload) >= 5 else None,
                "conditionCount": 0,
                "conditions": [],
            }
            current_objective["targets"].append(current_target)
        elif rec_type in ("CTDA", "CTDT") and current_target is not None:
            current_target["conditions"].append(condition_from_payload(rec_type, payload, current_target["conditionCount"]))
            current_target["conditionCount"] = len(current_target["conditions"])
    return objectives


def quest_row(plugin, record, subrecords):
    data = first(subrecords, "DATA")
    quest_name = first_zstring(subrecords, "FULL")
    top_level_conditions = quest_top_level_conditions(subrecords)
    stages = quest_stages(subrecords)
    objectives = quest_objectives(subrecords)
    stage_log_count = sum(len(stage["logEntries"]) for stage in stages)
    stage_text_count = sum(
        1
        for stage in stages
        for entry in stage["logEntries"]
        if entry.get("logLength", 0) > 0
    )
    objective_target_count = sum(len(objective["targets"]) for objective in objectives)
    return {
        "plugin": plugin,
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("questName", quest_name),
        "flags": record["flagsHex"],
        "questFlags": data[0] if data and len(data) >= 1 else None,
        "priority": data[1] if data and len(data) >= 2 else None,
        "questDelay": get_float(data, 4) if data and len(data) >= 8 else None,
        "questScript": (all_form_ids(subrecords, "SCRI") or [None])[0],
        "conditions": top_level_conditions,
        "conditionCount": len(top_level_conditions),
        "targetConditions": top_level_conditions,
        "embeddedScript": script_block(subrecords),
        "stages": stages,
        "objectives": objectives,
        "stageCount": len(stages),
        "stageLogEntryCount": stage_log_count,
        "stageTextEntryCount": stage_text_count,
        "objectiveCount": len(objectives),
        "objectiveTargetCount": objective_target_count,
        "objectiveSubrecords": [
            {"type": rec_type, "size": len(payload), **text_summary("text", zstring(payload))}
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
    topic_name = first_zstring(subrecords, "FULL")
    return {
        "plugin": plugin,
        "recordType": "DIAL",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("topicName", topic_name),
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
    response = first_zstring(subrecords, "NAM1")
    notes = first_zstring(subrecords, "NAM2")
    edits = first_zstring(subrecords, "NAM3")
    return {
        "plugin": plugin,
        "recordType": "INFO",
        "formId": record["formId"],
        "quest": (all_form_ids(subrecords, "QSTI") or [None])[0],
        "sound": (sounds or [None])[0],
        **text_summary("response", response),
        **text_summary("notes", notes),
        **text_summary("edits", edits),
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
        "sourceLength": script["sourceLength"],
        "sourceHash": script["sourceHash"],
        "localVariables": script["localVariables"],
        "referencedForms": script["referencedForms"],
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "missing-fnv-script-runtime",
    }


def perk_row(plugin, record, subrecords):
    script = script_block(subrecords)
    perk_conditions = conditions(subrecords)
    full_name = first_zstring(subrecords, "FULL")
    return {
        "plugin": plugin,
        "recordType": "PERK",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
        "descriptionLength": len(first_zstring(subrecords, "DESC")),
        "icon": first_zstring(subrecords, "ICON"),
        "dataSizes": subrecord_sizes(subrecords, "DATA"),
        "conditionCount": len(perk_conditions),
        "conditions": perk_conditions,
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


def condition_function_state(function):
    if function is None:
        return {
            "classification": "known-blocked",
            "readiness": "known-blocked",
            "functionSemanticsClassification": "known-blocked",
            "runtimeProofGate": "",
            "runtimeBoundary": "condition-function-id-missing",
            "firstFailingGate": "condition-function-id-missing",
            "proof": "The condition row is present but too short to expose a function id; this row cannot be evaluated until the payload shape is decoded.",
        }
    semantic_proof = CONDITION_FUNCTION_SEMANTIC_PROOFS.get(function)
    if semantic_proof:
        return {
            "classification": "loaded-pending-runtime",
            "readiness": "loaded-pending-runtime",
            "functionSemanticsClassification": "runtime-supported",
            "runtimeProofGate": semantic_proof["gate"],
            "runtimeBoundary": semantic_proof["runtimeBoundary"],
            "firstFailingGate": "runtime-condition-evaluator",
            "proof": semantic_proof["proof"]
            + " CTDA/CTDT condition-row evaluation remains a separate runtime gate, so loaded condition rows are not claimed as working gameplay.",
        }
    return {
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "functionSemanticsClassification": "loaded-pending-runtime",
        "runtimeProofGate": "",
        "runtimeBoundary": "condition-function-semantics-pending",
        "firstFailingGate": "runtime-condition-evaluator",
        "proof": "Condition row bytes and function inventory are harvested explicitly; runtime condition evaluation and this function's gameplay semantics remain pending.",
    }


def build_condition_functions(condition_rows, function_names):
    owner_rows = condition_rows
    grouped = {}
    for owner_row in owner_rows:
        function = owner_row.get("function")
        if function not in grouped:
            grouped[function] = {
                "function": function,
                "conditionCount": 0,
                "ownerCounts": Counter(),
                "ownerRecordCounts": Counter(),
                "subrecordCounts": Counter(),
                "payloadSizeCounts": Counter(),
                "conditionSchemaCounts": Counter(),
                "runOnCounts": Counter(),
                "sampleOwners": [],
            }
        bucket = grouped[function]
        bucket["conditionCount"] += 1
        bucket["ownerCounts"][owner_row["ownerScope"]] += 1
        bucket["ownerRecordCounts"][owner_row["recordType"]] += 1
        bucket["subrecordCounts"][owner_row["subrecord"]] += 1
        bucket["payloadSizeCounts"][str(owner_row["size"])] += 1
        bucket["conditionSchemaCounts"][owner_row.get("conditionSchema", "")] += 1
        run_on = owner_row.get("runOn")
        bucket["runOnCounts"]["<none>" if run_on is None else str(run_on)] += 1
        if len(bucket["sampleOwners"]) < 8:
            sample = {
                key: value
                for key, value in owner_row.items()
                if key
                in (
                    "ownerScope",
                    "plugin",
                    "recordType",
                    "formId",
                    "editorId",
                    "stageIndex",
                    "entryIndex",
                    "objectiveIndex",
                    "targetIndex",
                    "targetFormId",
                    "conditionIndex",
                    "subrecord",
                    "size",
                    "conditionSchema",
                    "runOn",
                    "unknownTail",
                )
            }
            bucket["sampleOwners"].append(sample)

    result = []
    for function, bucket in sorted(grouped.items(), key=lambda item: (-1 if item[0] is None else item[0])):
        state = condition_function_state(function)
        classification = state["classification"]
        if classification not in ALLOWED_CONDITION_CLASSIFICATIONS:
            classification = "known-blocked"
            state["readiness"] = "known-blocked"
            state["firstFailingGate"] = "invalid-condition-classification"
            state["proof"] = "Generated condition classification was outside the allowed vocabulary."
        result.append(
            {
                "function": function,
                "functionName": function_names.get(function, f"UNKNOWN_FUN_{function}" if function is not None else "UNKNOWN_FUN"),
                "conditionCount": bucket["conditionCount"],
                "ownerCounts": dict(sorted(bucket["ownerCounts"].items())),
                "ownerScopeCounts": dict(sorted(bucket["ownerCounts"].items())),
                "ownerRecordCounts": dict(sorted(bucket["ownerRecordCounts"].items())),
                "subrecordCounts": dict(sorted(bucket["subrecordCounts"].items())),
                "payloadSizeCounts": dict(sorted(bucket["payloadSizeCounts"].items())),
                "conditionSchemaCounts": dict(sorted(bucket["conditionSchemaCounts"].items())),
                "runOnCounts": dict(sorted(bucket["runOnCounts"].items())),
                "sampleOwners": bucket["sampleOwners"],
                **state,
                "classification": classification,
            }
        )
    return result


def weapon_data(data):
    if data is None:
        return {"size": 0}
    result = {"size": len(data)}
    if len(data) == 15:
        result.update(
            {
                "value": get_u32(data, 0),
                "health": get_u32(data, 4),
                "weight": get_float(data, 8),
                "damage": get_u16(data, 12),
                "clipSize": get_u8(data, 14),
            }
        )
    elif len(data) == 10:
        result.update(
            {
                "value": get_u32(data, 0),
                "weight": get_float(data, 4),
                "damage": get_u16(data, 8),
            }
        )
    elif len(data) >= 26:
        result.update(
            {
                "type": get_u32(data, 0),
                "speed": get_float(data, 4),
                "reach": get_float(data, 8),
                "flags": get_u32(data, 12),
                "value": get_u32(data, 16),
                "health": get_u32(data, 20),
                "weight": get_float(data, 24) if len(data) >= 28 else None,
                "damage": get_u16(data, 28) if len(data) >= 30 else None,
            }
        )
    return result


def weapon_row(plugin, record, subrecords):
    full_name = first_zstring(subrecords, "FULL")
    description = first_zstring(subrecords, "DESC")
    sound_refs = []
    for rec_type, payload in subrecords:
        if rec_type in ("SNAM", "XNAM", "TNAM", "NAM8", "NAM9", "WMS1", "WMS2") and len(payload) >= 4:
            sound_refs.append({"type": rec_type, "formId": hex_form(struct.unpack_from("<I", payload, 0)[0])})
    return {
        "plugin": plugin,
        "recordType": "WEAP",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
        "descriptionLength": len(description),
        "descriptionHash": text_hash(description),
        "model": first_zstring(subrecords, "MODL"),
        "icon": first_zstring(subrecords, "ICON"),
        "miniIcon": first_zstring(subrecords, "MICO"),
        "data": weapon_data(first(subrecords, "DATA")),
        "ammo": (all_form_ids(subrecords, "NAM0") or [""])[0],
        "repairList": (all_form_ids(subrecords, "REPL") or [""])[0],
        "equipType": (all_form_ids(subrecords, "ETYP") or [""])[0],
        "impactDataSet": (all_form_ids(subrecords, "INAM") or [""])[0],
        "worldModel": (all_form_ids(subrecords, "WNAM") or [""])[0],
        "script": (all_form_ids(subrecords, "SCRI") or [""])[0],
        "pickupSound": (all_form_ids(subrecords, "YNAM") or [""])[0],
        "dropSound": (all_form_ids(subrecords, "ZNAM") or [""])[0],
        "modItems": all_form_ids(subrecords, "WMI1")
        + all_form_ids(subrecords, "WMI2")
        + all_form_ids(subrecords, "WMI3"),
        "moddedWeapons": all_form_ids(subrecords, "WNM1")
        + all_form_ids(subrecords, "WNM2")
        + all_form_ids(subrecords, "WNM3")
        + all_form_ids(subrecords, "WNM4")
        + all_form_ids(subrecords, "WNM5")
        + all_form_ids(subrecords, "WNM6")
        + all_form_ids(subrecords, "WNM7"),
        "modModelCount": sum(1 for rec_type, _ in subrecords if rec_type.startswith("MWD")),
        "soundRefs": sound_refs,
        "classification": "runtime-supported",
        "readiness": "runtime-supported",
        "runtimeScope": "record load/store, ManualRef construction, inventory equip, HUD-safe form id, icon lookup, and the real 10mm firing slice",
        "runtimeProofGate": "fnv-real-10mm-runtime-contract",
        "unprovenGameplayGates": [
            "exhaustive-per-weapon-combat-behavior",
            "weapon-mod-application",
            "reload-and-attack-animation-parity",
            "weapon-sound-event-parity",
            "projectile-visual-impact-binding",
        ],
    }


def ammo_data(subrecords):
    data = first(subrecords, "DATA")
    dnam = first(subrecords, "DNAM")
    dat2 = first(subrecords, "DAT2")
    result = {"dataSize": len(data) if data else 0}
    if data and len(data) >= 13:
        result.update(
            {
                "speed": get_float(data, 0),
                "flags": get_u32(data, 4),
                "value": get_u32(data, 8),
                "clipRounds": get_u8(data, 12),
            }
        )
    if dnam and len(dnam) >= 16:
        result.update(
            {
                "projectile": hex_form(get_u32(dnam, 0)),
                "dnamFlags": get_u32(dnam, 4),
                "damage": get_float(dnam, 8),
                "health": get_u32(dnam, 12),
            }
        )
    if dat2 and len(dat2) >= 20:
        result.update(
            {
                "projectilesPerShot": get_u32(dat2, 0),
                "dat2Projectile": hex_form(get_u32(dat2, 4)),
                "weight": get_float(dat2, 8),
                "consumedAmmo": hex_form(get_u32(dat2, 12)),
                "consumedPercentage": get_float(dat2, 16),
            }
        )
    return result


def ammo_row(plugin, record, subrecords):
    full_name = first_zstring(subrecords, "FULL")
    description = first_zstring(subrecords, "DESC")
    data = ammo_data(subrecords)
    projectile = data.get("projectile", "") or data.get("dat2Projectile", "")
    if projectile == "0x00000000":
        projectile_binding_classification = "intentionally-excluded-with-proof"
        projectile_binding_boundary = (
            "AMMO projectile FormID is null in source bytes; this row has no PROJ record to bind. "
            "Hitscan/raycast behavior must be proved by weapon/ammo runtime gates."
        )
    elif projectile:
        projectile_binding_classification = "loaded-pending-runtime"
        projectile_binding_boundary = (
            "AMMO references a PROJ FormID; record bytes are accounted, but spawned projectile visuals, physics, "
            "impacts, and explosions remain separate runtime gates."
        )
    else:
        projectile_binding_classification = "loaded-pending-runtime"
        projectile_binding_boundary = (
            "AMMO row has no decoded projectile field; projectile behavior remains pending explicit parser/runtime proof."
        )
    return {
        "plugin": plugin,
        "recordType": "AMMO",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
        "shortNameLength": len(first_zstring(subrecords, "ONAM")),
        "shortNameHash": text_hash(first_zstring(subrecords, "ONAM")),
        "abbrevLength": len(first_zstring(subrecords, "QNAM")),
        "abbrevHash": text_hash(first_zstring(subrecords, "QNAM")),
        "descriptionLength": len(description),
        "descriptionHash": text_hash(description),
        "model": first_zstring(subrecords, "MODL"),
        "icon": first_zstring(subrecords, "ICON"),
        "miniIcon": first_zstring(subrecords, "MICO"),
        "data": data,
        "projectile": projectile,
        "projectileBindingClassification": projectile_binding_classification,
        "projectileBindingBoundary": projectile_binding_boundary,
        "ammoEffects": all_form_ids(subrecords, "RCIL"),
        "script": (all_form_ids(subrecords, "SCRI") or [""])[0],
        "pickupSound": (all_form_ids(subrecords, "YNAM") or [""])[0],
        "dropSound": (all_form_ids(subrecords, "ZNAM") or [""])[0],
        "classification": "runtime-supported",
        "readiness": "runtime-supported",
        "runtimeScope": "record load/store, ManualRef construction, ammunition slot equip, HUD-safe form id, null-projectile hitscan/raycast accounting, and the real 10mm ammo decrement slice",
        "runtimeProofGate": "fnv-real-10mm-runtime-contract",
        "unprovenGameplayGates": [
            "ammo-effect-binding",
            "nonzero-projectile-form-binding",
            "projectile-visual-impact-binding",
            "all-ammo-variant-ballistics",
            "consumed-ammo-chain-runtime",
        ],
    }


def subrecord_type_counts(subrecords):
    counts = Counter(rec_type for rec_type, _ in subrecords)
    return dict(sorted(counts.items()))


def actor_value_row(plugin, record, subrecords):
    full_name = first_zstring(subrecords, "FULL")
    description = first_zstring(subrecords, "DESC")
    marker_types = ("ANAM", "CNAM", "AVSK")
    marker_counts = {
        marker: sum(1 for rec_type, _ in subrecords if rec_type == marker)
        for marker in marker_types
    }
    return {
        "plugin": plugin,
        "recordType": "AVIF",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
        "descriptionLength": len(description),
        "descriptionHash": text_hash(description),
        "icon": first_zstring(subrecords, "ICON"),
        "dataSizes": subrecord_sizes(subrecords, "DATA"),
        "progressionMarkerCounts": marker_counts,
        "progressionMarkerTotal": sum(marker_counts.values()),
        "subrecordTypeCounts": subrecord_type_counts(subrecords),
        "classification": "loaded-pending-runtime",
        "readiness": "loaded-pending-runtime",
        "firstFailingGate": "runtime-actor-value-progression-binding",
        "runtimeBoundary": "AVIF actor-value and perk-tree bytes are harvested and typed-loaded pending full SPECIAL/skills/level-up/perk-tree runtime binding.",
    }


def projectile_row(plugin, record, subrecords):
    full_name = first_zstring(subrecords, "FULL")
    return {
        "plugin": plugin,
        "recordType": "PROJ",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
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
    full_name = first_zstring(subrecords, "FULL")
    return {
        "plugin": plugin,
        "recordType": "EXPL",
        "formId": record["formId"],
        "editorId": first_zstring(subrecords, "EDID"),
        **text_summary("fullName", full_name),
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
    full_name = first_zstring(subrecords, "FULL")
    value = simple_value(record["type"], editor_id, data)
    if isinstance(value, str):
        value_fields = {
            "valueType": "string",
            **text_summary("value", value),
        }
    else:
        value_fields = {
            "valueType": "numeric" if value is not None else "",
            "value": value,
        }
    return {
        "plugin": plugin,
        "recordType": record["type"],
        "formId": record["formId"],
        "editorId": editor_id,
        **text_summary("fullName", full_name),
        **value_fields,
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
    condition_rows = []

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
            payload = data[offset:next_offset]
            subrecords = read_subrecords(payload)
            record = {
                "type": rec_type,
                "formId": hex_form(form_id),
                "flags": flags,
                "flagsHex": f"0x{flags:08x}",
            }
            condition_rows.extend(condition_rows_for_record(plugin, record, subrecords))
            if rec_type in {"QUST", "DIAL", "INFO", "SCPT", "GLOB", "GMST", "WEAP", "AMMO", "AVIF", "PERK", "PROJ", "EXPL"}:
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
                elif rec_type == "WEAP":
                    gameplay_systems.append(weapon_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references,
                        plugin,
                        "WEAP",
                        record["formId"],
                        subrecords,
                        [
                            ("SCRI", "script"),
                            ("NAM0", "ammo"),
                            ("REPL", "repairList"),
                            ("ETYP", "equipType"),
                            ("INAM", "impactDataSet"),
                            ("WNAM", "worldModel"),
                            ("YNAM", "pickupSound"),
                            ("ZNAM", "dropSound"),
                            ("WMI1", "modItem"),
                            ("WMI2", "modItem"),
                            ("WMI3", "modItem"),
                            ("WNM1", "moddedWeapon"),
                            ("WNM2", "moddedWeapon"),
                            ("WNM3", "moddedWeapon"),
                            ("WNM4", "moddedWeapon"),
                            ("WNM5", "moddedWeapon"),
                            ("WNM6", "moddedWeapon"),
                            ("WNM7", "moddedWeapon"),
                            ("SNAM", "sound"),
                            ("XNAM", "sound"),
                            ("TNAM", "sound"),
                            ("NAM8", "sound"),
                            ("NAM9", "sound"),
                            ("WMS1", "sound"),
                            ("WMS2", "sound"),
                        ],
                    )
                elif rec_type == "AMMO":
                    gameplay_systems.append(ammo_row(plugin, record, subrecords))
                    add_form_id_subrecord_refs(
                        references,
                        plugin,
                        "AMMO",
                        record["formId"],
                        subrecords,
                        [
                            ("DNAM", "projectile"),
                            ("RCIL", "ammoEffect"),
                            ("SCRI", "script"),
                            ("YNAM", "pickupSound"),
                            ("ZNAM", "dropSound"),
                        ],
                    )
                    for target in all_form_ids_at_offset(subrecords, "DAT2", 4):
                        add_reference(references, plugin, "AMMO", record["formId"], "dat2Projectile", int(target, 16))
                    for target in all_form_ids_at_offset(subrecords, "DAT2", 12):
                        add_reference(references, plugin, "AMMO", record["formId"], "consumedAmmo", int(target, 16))
                elif rec_type == "AVIF":
                    gameplay_systems.append(actor_value_row(plugin, record, subrecords))
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
        "conditionRows": condition_rows,
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
    condition_rows = [item for row in plugin_ledgers for item in row["conditionRows"]]
    condition_function_names = load_condition_function_names(repo_root)
    condition_functions = build_condition_functions(condition_rows, condition_function_names)
    total_records = sum(record["count"] for plugin in records for record in plugin["records"])
    gameplay_counts = Counter(item["recordType"] for item in gameplay_systems)
    quest_stage_count = sum(int(item.get("stageCount", 0)) for item in quests)
    quest_stage_log_entry_count = sum(int(item.get("stageLogEntryCount", 0)) for item in quests)
    quest_stage_text_entry_count = sum(int(item.get("stageTextEntryCount", 0)) for item in quests)
    quest_objective_count = sum(int(item.get("objectiveCount", 0)) for item in quests)
    quest_objective_target_count = sum(int(item.get("objectiveTargetCount", 0)) for item in quests)
    condition_subrecord_counts = Counter(item.get("subrecord", "") for item in condition_rows)
    condition_payload_size_counts = Counter(str(item.get("size", "")) for item in condition_rows)
    condition_owner_record_counts = Counter(item.get("recordType", "") for item in condition_rows)
    condition_owner_scope_counts = Counter(item.get("ownerScope", "") for item in condition_rows)
    condition_schema_counts = Counter(item.get("conditionSchema", "") for item in condition_rows)
    condition_function_counts = Counter(
        str(item.get("function")) for item in condition_rows if item.get("function") is not None
    )
    condition_unknown_functions = sorted(
        {
            item.get("function")
            for item in condition_rows
            if item.get("function") is None or item.get("function") not in condition_function_names
        },
        key=lambda value: -1 if value is None else value,
    )
    condition_high_word_nonzero_count = sum(
        1 for item in condition_rows if item.get("function") is not None and (int(item["function"]) & 0xFFFF0000) != 0
    )
    condition_malformed_count = sum(
        1 for item in condition_rows if item.get("function") is None or int(item.get("size", 0)) < 20
    )
    condition_function_use_count = sum(int(item.get("conditionCount", 0)) for item in condition_functions)
    condition_unaccounted_count = len(condition_rows) - condition_function_use_count
    condition_function_class_counts = Counter(item.get("classification", "") for item in condition_functions)
    condition_function_semantic_class_counts = Counter(
        item.get("functionSemanticsClassification", "") for item in condition_functions
    )
    condition_function_unclassified = sum(
        1 for item in condition_functions if item.get("classification") not in ALLOWED_CONDITION_CLASSIFICATIONS
    )

    artifacts = {
        "records": proof_dir / "records.json",
        "quests": proof_dir / "quests.json",
        "dialogue": proof_dir / "dialogue.json",
        "scripts": proof_dir / "scripts.json",
        "globals": proof_dir / "globals.json",
        "gameSettings": proof_dir / "game-settings.json",
        "gameplaySystems": proof_dir / "gameplay-systems.json",
        "references": proof_dir / "references.json",
        "conditions": proof_dir / "conditions.json",
        "conditionFunctions": proof_dir / "condition-functions.json",
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
    write_json(artifacts["conditions"], condition_rows)
    write_json(artifacts["conditionFunctions"], condition_functions)

    result = {
        "status": "PASS",
        "stamp": stamp,
        "repoRoot": str(repo_root),
        "fnvData": str(fnv_data),
        "payloadPolicy": "retail-text-redacted-v1",
        "content": args.content,
        "pluginCount": len(plugin_ledgers),
        "recordTotal": total_records,
        "questCount": len(quests),
        "questStageCount": quest_stage_count,
        "questStageLogEntryCount": quest_stage_log_entry_count,
        "questStageTextEntryCount": quest_stage_text_entry_count,
        "questObjectiveCount": quest_objective_count,
        "questObjectiveTargetCount": quest_objective_target_count,
        "dialogueRowCount": len(dialogue),
        "scriptCount": len(scripts),
        "globalCount": len(globals_),
        "gameSettingCount": len(game_settings),
        "gameplaySystemCount": len(gameplay_systems),
        "gameplaySystemCounts": dict(sorted(gameplay_counts.items())),
        "referenceCount": len(references),
        "conditionRowCount": len(condition_rows),
        "conditionFunctionCount": len(condition_functions),
        "conditionFunctionUseCount": condition_function_use_count,
        "conditionFunctionCounts": dict(sorted(condition_function_counts.items(), key=lambda item: int(item[0]))),
        "conditionFunctionClassCounts": dict(sorted(condition_function_class_counts.items())),
        "conditionFunctionSemanticClassCounts": dict(sorted(condition_function_semantic_class_counts.items())),
        "conditionFunctionUnclassified": condition_function_unclassified,
        "conditionSubrecordCounts": dict(sorted(condition_subrecord_counts.items())),
        "conditionPayloadSizeCounts": dict(sorted(condition_payload_size_counts.items(), key=lambda item: int(item[0]) if item[0].isdigit() else -1)),
        "conditionOwnerRecordCounts": dict(sorted(condition_owner_record_counts.items())),
        "conditionOwnerScopeCounts": dict(sorted(condition_owner_scope_counts.items())),
        "conditionSchemaCounts": dict(sorted(condition_schema_counts.items())),
        "conditionUnknownFunctionCount": len(condition_unknown_functions),
        "conditionUnknownFunctions": condition_unknown_functions,
        "conditionHighWordNonzeroCount": condition_high_word_nonzero_count,
        "conditionMalformedCount": condition_malformed_count,
        "conditionUnaccountedCount": condition_unaccounted_count,
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
        "conditions",
        "conditionFunctions",
        "result",
    ):
        proof_line(f"{key}: {artifacts[key]}")
    proof_line()
    proof_line("FNV structured content ledger proof PASS")
    proof_line(
        "plugins={plugins} records={records} quests={quests} questStages={questStages} "
        "questStageLogs={questStageLogs} questStageTexts={questStageTexts} questObjectives={questObjectives} "
        "questObjectiveTargets={questObjectiveTargets} dialogueRows={dialogue} scripts={scripts} "
        "globals={globals} gameSettings={settings} gameplaySystems={gameplay} references={references} "
        "conditionRows={conditionRows} conditionFunctions={conditionFunctions} conditionUnknownFunctions={unknown} "
        "conditionUnaccounted={unaccounted}".format(
            plugins=len(plugin_ledgers),
            records=total_records,
            quests=len(quests),
            questStages=quest_stage_count,
            questStageLogs=quest_stage_log_entry_count,
            questStageTexts=quest_stage_text_entry_count,
            questObjectives=quest_objective_count,
            questObjectiveTargets=quest_objective_target_count,
            dialogue=len(dialogue),
            scripts=len(scripts),
            globals=len(globals_),
            settings=len(game_settings),
            gameplay=len(gameplay_systems),
            references=len(references),
            conditionRows=len(condition_rows),
            conditionFunctions=len(condition_functions),
            unknown=len(condition_unknown_functions),
            unaccounted=condition_unaccounted_count,
        )
    )
    proof_line("gameplaySystemCounts=" + json.dumps(dict(sorted(gameplay_counts.items())), sort_keys=True))
    proof_line("conditionOwnerRecordCounts=" + json.dumps(dict(sorted(condition_owner_record_counts.items())), sort_keys=True))
    proof_line("conditionOwnerScopeCounts=" + json.dumps(dict(sorted(condition_owner_scope_counts.items())), sort_keys=True))


if __name__ == "__main__":
    main()
