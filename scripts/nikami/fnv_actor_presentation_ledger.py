#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import os
import struct
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

import fnv_content_ledger as base


DEFAULT_CONTENT = base.DEFAULT_CONTENT

ALLOWED_CLASSIFICATIONS = {
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof",
}

ACTOR_RECORD_TYPES = {"NPC_", "CREA"}
PLACED_ACTOR_TYPES = {"ACHR", "ACRE"}
CELL_CHILD_GROUP_TYPES = {6, 8, 9, 10}
FORM_ID_GROUP_TYPES = {1, 6, 7, 8, 9, 10}
GROUP_TYPE_NAMES = {
    0: "record-type",
    1: "world-child",
    2: "interior-cell-block",
    3: "interior-cell-sub-block",
    4: "exterior-cell-block",
    5: "exterior-cell-sub-block",
    6: "cell-child",
    7: "topic-child",
    8: "cell-persistent-child",
    9: "cell-temporary-child",
    10: "cell-visible-dist-child",
}
SUPPORT_RECORD_TYPES = {
    "RACE",
    "HDPT",
    "HAIR",
    "EYES",
    "ARMO",
    "ARMA",
    "CLOT",
    "BPTD",
    "DIAL",
    "INFO",
    "IDLE",
    "IDLM",
    "ANIO",
    "OTFT",
    "VTYP",
}

RACE_FACE_ROLES = {
    0: "head",
    1: "ears",
    2: "mouth",
    3: "lowerTeeth",
    4: "upperTeeth",
    5: "tongue",
    6: "leftEye",
    7: "rightEye",
    8: "tail",
}

HDPT_TYPES = {
    0: "misc",
    1: "face",
    2: "eyes",
    3: "hair",
    4: "facialHair",
    5: "scar",
    6: "eyebrows",
}

RUNTIME_ANCHORS = {
    "npc-face-assembly": {
        "path": "apps/openmw/mwrender/esm4npcanimation.cpp",
        "needle": "FNV/ESM4 FACE CHECK",
        "description": "runtime logs final face/mouth/teeth/tongue/eye/hair status for FNV NPC presentation",
    },
    "npc-headpart-insert": {
        "path": "apps/openmw/mwrender/esm4npcanimation.cpp",
        "needle": "insertHeadParts",
        "description": "runtime inserts HDPT records and extra parts onto the actor head",
    },
    "npc-equipment-assembly": {
        "path": "apps/openmw/mwrender/esm4npcanimation.cpp",
        "needle": "findArmorAddons",
        "description": "runtime resolves ARMO to ARMA geometry before attaching equipment",
    },
    "npc-kffz-animation": {
        "path": "apps/openmw/mwrender/esm4npcanimation.cpp",
        "needle": "using NPC KFFZ animation list",
        "description": "runtime consumes NPC KFFZ animation paths and fallback locomotion",
    },
    "creature-body-assembly": {
        "path": "apps/openmw/mwrender/creatureanimation.cpp",
        "needle": "bodyPartStore",
        "description": "runtime resolves CREA PNAM body-part data and creature model roots",
    },
    "creature-kffz-animation": {
        "path": "apps/openmw/mwrender/creatureanimation.cpp",
        "needle": "for (const std::string& kf : effective.mKf)",
        "description": "runtime consumes creature KFFZ animation paths",
    },
    "dialogue-runtime": {
        "path": "apps/openmw/mwdialogue/dialoguemanagerimp.cpp",
        "needle": "callback->addResponse",
        "description": "dialogue runtime selects INFO responses for the UI",
    },
    "voice-lip-runtime": {
        "path": "apps/openmw/mwsound/soundmanagerimp.cpp",
        "needle": "loaded LIP sync",
        "description": "sound runtime loads LIP sidecars for active dialogue voice streams",
    },
}


def text_hash(value):
    if not value:
        return ""
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def get_i32(raw, offset):
    if len(raw) < offset + 4:
        return None
    return struct.unpack_from("<i", raw, offset)[0]


def zero_string_array(raw):
    if not raw:
        return []
    result = []
    for item in raw.split(b"\0"):
        if item:
            result.append(item.decode("cp1252", errors="replace"))
    return result


def normalize_plugin_name(name):
    return Path(name).name.lower()


def normalize_archive_path(value, prefix=""):
    if not value:
        return ""
    normalized = value.replace("/", "\\").strip().strip("\\").lower()
    if not normalized:
        return ""
    if prefix and not normalized.startswith(prefix.lower() + "\\"):
        normalized = prefix.lower() + "\\" + normalized
    return normalized


def model_archive_path(value):
    path = normalize_archive_path(value)
    if not path:
        return ""
    if path.startswith("meshes\\"):
        return path
    if path.endswith((".nif", ".kf", ".egm", ".tri", ".ctl", ".psa")):
        return "meshes\\" + path
    return path


def texture_archive_path(value):
    path = normalize_archive_path(value)
    if not path:
        return ""
    if path.startswith("textures\\"):
        return path
    return "textures\\" + path


def form_id_or_empty(value):
    if not value:
        return ""
    return f"0x{value:08x}"


def runtime_form_id_or_empty(value):
    if not value:
        return ""
    if isinstance(value, str):
        try:
            value = int(value, 16)
        except ValueError:
            return ""
    local_file = (int(value) >> 24) & 0xFF
    object_id = int(value) & 0x00FFFFFF
    return form_id_or_empty((((local_file + 1) & 0xFF) << 24) | object_id)


def finite_float(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result):
        return None
    return result


def first_payload(subrecords, rec_type):
    for kind, payload in subrecords:
        if kind == rec_type:
            return payload
    return b""


def placed_position(subrecords):
    payload = first_payload(subrecords, "DATA")
    if len(payload) < 24:
        return {}
    values = [finite_float(value) for value in struct.unpack_from("<6f", payload, 0)]
    if any(value is None for value in values):
        return {}
    return {
        "x": values[0],
        "y": values[1],
        "z": values[2],
        "rotX": values[3],
        "rotY": values[4],
        "rotZ": values[5],
    }


def placed_scale(subrecords):
    payload = first_payload(subrecords, "XSCL")
    if len(payload) < 4:
        return None
    return finite_float(struct.unpack_from("<f", payload, 0)[0])


def cell_grid(subrecords):
    payload = first_payload(subrecords, "XCLC")
    if len(payload) < 8:
        return None
    grid_x, grid_y = struct.unpack_from("<ii", payload, 0)
    return {"x": grid_x, "y": grid_y}


def adjusted_form_id(raw, masters, plugin, content_index):
    if raw is None or raw == 0:
        return 0
    local_file = (raw >> 24) & 0xFF
    object_id = raw & 0x00FFFFFF
    source = plugin
    if local_file < len(masters):
        source = masters[local_file]
    global_file = local_file
    source_key = normalize_plugin_name(source)
    if source_key in content_index:
        global_file = content_index[source_key]
    return ((global_file & 0xFF) << 24) | object_id


def all_adjusted_form_ids(subrecords, rec_type, masters, plugin, content_index, offset=0):
    result = []
    for current_type, payload in subrecords:
        if current_type == rec_type and len(payload) >= offset + 4:
            raw = struct.unpack_from("<I", payload, offset)[0]
            adjusted = adjusted_form_id(raw, masters, plugin, content_index)
            if adjusted:
                result.append(adjusted)
    return result


def form_ids_from_payload(payload, masters, plugin, content_index):
    result = []
    for offset in range(0, len(payload) - 3, 4):
        raw = struct.unpack_from("<I", payload, offset)[0]
        adjusted = adjusted_form_id(raw, masters, plugin, content_index)
        if adjusted:
            result.append(adjusted)
    return result


class RowBuilder:
    def __init__(self, asset_index):
        self.rows = []
        self.asset_index = asset_index

    def add(
        self,
        *,
        plugin,
        actor_kind="",
        actor_form_id="",
        actor_editor_id="",
        placed_ref_form_id="",
        placed_ref_editor_id="",
        placed_cell_form_id="",
        placed_runtime_cell_form_id="",
        placed_cell_editor_id="",
        placed_cell_group_type="",
        placed_cell_group_name="",
        placed_cell_source="",
        placed_cell_grid_x=None,
        placed_cell_grid_y=None,
        placed_cell_fallback_form_id="",
        placed_coordinate_source="",
        placed_pos_x=None,
        placed_pos_y=None,
        placed_pos_z=None,
        placed_rot_x=None,
        placed_rot_y=None,
        placed_rot_z=None,
        placed_scale=None,
        template_chain=None,
        component,
        source_record_type,
        source_form_id,
        source_editor_id="",
        subrecord="",
        resolved_record_type="",
        resolved_form_id="",
        resolved_editor_id="",
        asset_path="",
        required=True,
        coverage_stage="pc-flat-static-ledger",
        classification,
        first_failing_gate="",
        proof_anchor="",
        notes="",
    ):
        if classification not in ALLOWED_CLASSIFICATIONS:
            classification = ""
        archive_path = normalize_archive_path(asset_path)
        asset_status = ""
        asset_archive = ""
        if archive_path:
            asset_status, asset_archive = self.asset_index.status(archive_path)
        self.rows.append(
            {
                "plugin": plugin,
                "actorKind": actor_kind,
                "actorFormId": actor_form_id,
                "actorEditorId": actor_editor_id,
                "placedRefFormId": placed_ref_form_id,
                "placedRefEditorId": placed_ref_editor_id,
                "placedCellFormId": placed_cell_form_id,
                "placedRuntimeCellFormId": placed_runtime_cell_form_id,
                "placedCellEditorId": placed_cell_editor_id,
                "placedCellGroupType": placed_cell_group_type,
                "placedCellGroupName": placed_cell_group_name,
                "placedCellSource": placed_cell_source,
                "placedCellGridX": placed_cell_grid_x,
                "placedCellGridY": placed_cell_grid_y,
                "placedCellFallbackFormId": placed_cell_fallback_form_id,
                "placedCoordinateSource": placed_coordinate_source,
                "placedPosX": placed_pos_x,
                "placedPosY": placed_pos_y,
                "placedPosZ": placed_pos_z,
                "placedRotX": placed_rot_x,
                "placedRotY": placed_rot_y,
                "placedRotZ": placed_rot_z,
                "placedScale": placed_scale,
                "templateChain": template_chain or [],
                "component": component,
                "sourceRecordType": source_record_type,
                "sourceFormId": source_form_id,
                "sourceEditorId": source_editor_id,
                "subrecord": subrecord,
                "resolvedRecordType": resolved_record_type,
                "resolvedFormId": resolved_form_id,
                "resolvedEditorId": resolved_editor_id,
                "assetPath": asset_path,
                "assetStatus": asset_status,
                "assetArchive": asset_archive,
                "required": bool(required),
                "coverageStage": coverage_stage,
                "classification": classification,
                "firstFailingGate": first_failing_gate,
                "proofAnchor": proof_anchor,
                "notes": notes,
            }
        )


class AssetIndex:
    def __init__(self, fnv_data, harvest_dir):
        self.fnv_data = Path(fnv_data)
        self.harvest_dir = Path(harvest_dir) if harvest_dir else None
        self.entries = {}
        if self.harvest_dir and self.harvest_dir.is_dir():
            self._load_harvest_entries()

    def _load_harvest_entries(self):
        ledger_path = self.harvest_dir / "archive-entry-ledger.json"
        if not ledger_path.is_file():
            return
        for archive in json.loads(ledger_path.read_text(encoding="utf-8-sig")):
            list_path = self.harvest_dir / archive.get("entryList", "")
            if not list_path.is_file():
                continue
            archive_name = archive.get("archive", "")
            with list_path.open("r", encoding="utf-8", errors="replace") as stream:
                for line in stream:
                    entry = normalize_archive_path(line.strip())
                    if entry and entry not in self.entries:
                        self.entries[entry] = archive_name

    def status(self, archive_path):
        normalized = normalize_archive_path(archive_path)
        if not normalized:
            return "", ""
        loose_path = self.fnv_data / normalized
        if loose_path.is_file():
            return "loose-file-resolved", ""
        if normalized in self.entries:
            return "archive-entry-resolved", self.entries[normalized]
        if not self.entries:
            return "harvest-not-provided", ""
        return "missing-from-harvest", ""


def group_context(group_label_raw, group_type, masters, plugin, content_index):
    label_raw = struct.unpack_from("<I", group_label_raw, 0)[0]
    grid_y, grid_x = struct.unpack_from("<hh", group_label_raw, 0)
    label_text = ""
    try:
        label_text = base.fourcc(group_label_raw)
    except Exception:
        label_text = ""
    label_form_id = ""
    if group_type in FORM_ID_GROUP_TYPES:
        adjusted = adjusted_form_id(label_raw, masters, plugin, content_index)
        label_form_id = form_id_or_empty(adjusted) if adjusted else ""
    return {
        "type": group_type,
        "typeName": GROUP_TYPE_NAMES.get(group_type, f"unknown-{group_type}"),
        "labelRaw": form_id_or_empty(label_raw),
        "labelFormId": label_form_id,
        "labelText": label_text,
        "labelGridX": grid_x,
        "labelGridY": grid_y,
    }


def context_worldspace_form_id(record):
    for context in reversed(record.get("groupContext", [])):
        if int(context.get("type", -1)) == 1 and context.get("labelFormId"):
            return context.get("labelFormId", "")
    return ""


def exterior_grid_from_position(position):
    if not position:
        return None
    x = finite_float(position.get("x"))
    y = finite_float(position.get("y"))
    if x is None or y is None:
        return None
    return {
        "x": math.floor(x / 4096.0),
        "y": math.floor(y / 4096.0),
    }


def build_exterior_cell_index(by_type):
    result = {}
    for record in by_type.get("CELL", []):
        worldspace = context_worldspace_form_id(record)
        grid = cell_grid(record.get("subrecords", []))
        if not worldspace or grid is None:
            continue
        result[(worldspace, grid["x"], grid["y"])] = record
    return result


def group_placed_cell_context(record, by_form):
    fallback = None
    for context in reversed(record.get("groupContext", [])):
        if int(context.get("type", -1)) not in CELL_CHILD_GROUP_TYPES:
            continue
        form_id = context.get("labelFormId", "")
        if not form_id:
            continue
        if int(context.get("type", -1)) != 6:
            if fallback is None:
                fallback = context
            continue
        cell_record = resolve(by_form, form_id)
        return {
            "formId": form_id,
            "runtimeFormId": runtime_form_id_or_empty(form_id),
            "editorId": cell_record.get("editorId", "") if cell_record else "",
            "groupType": str(context.get("type", "")),
            "groupName": str(context.get("typeName", "")),
            "source": "cell-child-grup",
            "gridX": None,
            "gridY": None,
            "fallbackFormId": "",
        }
    if fallback is not None:
        form_id = fallback.get("labelFormId", "")
        cell_record = resolve(by_form, form_id)
        return {
            "formId": form_id,
            "runtimeFormId": runtime_form_id_or_empty(form_id),
            "editorId": cell_record.get("editorId", "") if cell_record else "",
            "groupType": str(fallback.get("type", "")),
            "groupName": str(fallback.get("typeName", "")),
            "source": "child-subgroup-fallback",
            "gridX": None,
            "gridY": None,
            "fallbackFormId": "",
        }
    return {
        "formId": "",
        "runtimeFormId": "",
        "editorId": "",
        "groupType": "",
        "groupName": "",
        "source": "",
        "gridX": None,
        "gridY": None,
        "fallbackFormId": "",
    }


def placed_cell_context(record, by_form, exterior_cell_index=None, position=None):
    group_cell = group_placed_cell_context(record, by_form)
    grid = exterior_grid_from_position(position)
    worldspace = context_worldspace_form_id(record)
    if exterior_cell_index is not None and grid is not None and worldspace:
        cell_record = exterior_cell_index.get((worldspace, grid["x"], grid["y"]))
        if cell_record is not None:
            form_id = cell_record.get("formId", "")
            return {
                "formId": form_id,
                "runtimeFormId": runtime_form_id_or_empty(form_id),
                "editorId": cell_record.get("editorId", ""),
                "groupType": "xclc-grid",
                "groupName": "exterior-grid-cell",
                "source": "worldspace-xclc-from-position",
                "gridX": grid["x"],
                "gridY": grid["y"],
                "fallbackFormId": group_cell.get("formId", ""),
            }
    return group_cell


def read_plugin_records(path, plugin, content_index):
    data = path.read_bytes()
    masters = []
    records = []
    counts = Counter()

    def read_range(offset, end, context):
        nonlocal masters
        while offset + 24 <= end:
            record_start = offset
            rec_type = base.fourcc(data[offset : offset + 4])
            size, flags, raw_form_id = struct.unpack_from("<III", data, offset + 4)
            offset += 24
            if rec_type == "GRUP":
                group_end = record_start + size
                if group_end <= record_start or group_end > end:
                    raise ValueError(
                        f"Invalid GRUP range in {path}: start={record_start} size={size} end={group_end} limit={end}"
                    )
                group_label_raw = data[record_start + 8 : record_start + 12]
                group_type = struct.unpack_from("<i", data, record_start + 12)[0]
                child_context = context + [group_context(group_label_raw, group_type, masters, plugin, content_index)]
                offset = read_range(offset, group_end, child_context)
                continue

            next_offset = offset + size
            if next_offset < offset or next_offset > end:
                raise ValueError(
                    f"Invalid ESM4 record range in {path}: type={rec_type} start={record_start} "
                    f"size={size} next={next_offset} limit={end}"
                )

            payload = data[offset:next_offset]
            subrecords = base.read_subrecords(payload)
            if rec_type == "TES4":
                masters = [base.zstring(payload) for kind, payload in subrecords if kind == "MAST"]
            form_id = adjusted_form_id(raw_form_id, masters, plugin, content_index)
            record = {
                "plugin": plugin,
                "path": str(path),
                "type": rec_type,
                "rawFormId": form_id_or_empty(raw_form_id),
                "formId": form_id_or_empty(form_id),
                "flags": flags,
                "editorId": base.first_zstring(subrecords, "EDID"),
                "fullNameHash": text_hash(base.first_zstring(subrecords, "FULL")),
                "subrecords": subrecords,
                "masters": list(masters),
                "groupContext": list(context),
            }
            records.append(record)
            counts[rec_type] += 1
            offset = next_offset
        return offset

    read_range(0, len(data), [])
    return {
        "plugin": plugin,
        "path": str(path),
        "length": path.stat().st_size,
        "masters": masters,
        "counts": counts,
        "records": records,
    }


def index_records(plugin_ledgers):
    by_form = {}
    by_type = defaultdict(list)
    for ledger in plugin_ledgers:
        for record in ledger["records"]:
            by_type[record["type"]].append(record)
            if record["formId"]:
                by_form[record["formId"]] = record
    return by_form, by_type


def resolve(by_form, form_id):
    if isinstance(form_id, int):
        form_id = form_id_or_empty(form_id)
    return by_form.get(form_id)


def record_type(by_form, form_id):
    record = resolve(by_form, form_id)
    return record["type"] if record else ""


def record_editor(by_form, form_id):
    record = resolve(by_form, form_id)
    return record.get("editorId", "") if record else ""


def base_component_classification(asset_path="", asset_index=None):
    if not asset_path or asset_index is None:
        return "loaded-pending-runtime", "actor-presentation-runtime-sweep"
    status, _archive = asset_index.status(asset_path)
    if status == "missing-from-harvest":
        return "known-blocked", "fnv-asset-resolve-harvest"
    return "loaded-pending-runtime", "actor-presentation-runtime-sweep"


def add_asset_row(
    rows,
    record,
    *,
    component,
    subrecord,
    asset_path,
    classification=None,
    first_failing_gate=None,
    proof_anchor="",
    notes="",
    actor=None,
):
    classification = classification or "loaded-pending-runtime"
    first_failing_gate = first_failing_gate or "actor-presentation-runtime-sweep"
    if asset_path:
        asset_status, _asset_archive = rows.asset_index.status(asset_path)
        if asset_status == "missing-from-harvest" and classification != "known-blocked":
            classification = "known-blocked"
            first_failing_gate = "fnv-asset-resolve-harvest"
            notes = (notes + " " if notes else "") + "Referenced asset path was not found in the latest harvest or loose Data tree."
    rows.add(
        plugin=record["plugin"],
        actor_kind=(actor or {}).get("type", ""),
        actor_form_id=(actor or {}).get("formId", ""),
        actor_editor_id=(actor or {}).get("editorId", ""),
        component=component,
        source_record_type=record["type"],
        source_form_id=record["formId"],
        source_editor_id=record.get("editorId", ""),
        subrecord=subrecord,
        asset_path=asset_path,
        classification=classification,
        first_failing_gate=first_failing_gate,
        proof_anchor=proof_anchor,
        notes=notes,
    )


def add_reference_row(
    rows,
    record,
    by_form,
    form_id,
    *,
    component,
    subrecord,
    classification="loaded-pending-runtime",
    first_failing_gate="actor-presentation-runtime-sweep",
    proof_anchor="",
    notes="",
    actor=None,
):
    resolved = resolve(by_form, form_id)
    if form_id and not resolved:
        classification = "known-blocked"
        first_failing_gate = "fnv-formid-resolve"
        notes = (notes + " " if notes else "") + "Referenced form ID was not found in active content."
    rows.add(
        plugin=record["plugin"],
        actor_kind=(actor or {}).get("type", ""),
        actor_form_id=(actor or {}).get("formId", ""),
        actor_editor_id=(actor or {}).get("editorId", ""),
        component=component,
        source_record_type=record["type"],
        source_form_id=record["formId"],
        source_editor_id=record.get("editorId", ""),
        subrecord=subrecord,
        resolved_record_type=resolved["type"] if resolved else "",
        resolved_form_id=form_id_or_empty(form_id) if isinstance(form_id, int) else form_id,
        resolved_editor_id=resolved.get("editorId", "") if resolved else "",
        classification=classification,
        first_failing_gate=first_failing_gate,
        proof_anchor=proof_anchor,
        notes=notes,
    )


def npc_gender(record):
    acbs = base.first(record["subrecords"], "ACBS")
    if not acbs or len(acbs) < 4:
        return "unknown"
    flags = struct.unpack_from("<I", acbs, 0)[0]
    return "female" if flags & 0x00000001 else "male"


def collect_actor_inventory_ids(record, content_index):
    result = []
    for kind, payload in record["subrecords"]:
        if kind != "CNTO" or len(payload) < 4:
            continue
        item = adjusted_form_id(struct.unpack_from("<I", payload, 0)[0], record["masters"], record["plugin"], content_index)
        count = get_i32(payload, 4) if len(payload) >= 8 else None
        if item:
            result.append((item, count))
    return result


def collect_strings(record, subrecord):
    result = []
    for kind, payload in record["subrecords"]:
        if kind == subrecord:
            result.extend(zero_string_array(payload))
    return result


def emit_actor_rows(rows, by_form, content_index, record):
    actor_kind = record["type"]
    actor = {"type": actor_kind, "formId": record["formId"], "editorId": record.get("editorId", "")}
    rows.add(
        plugin=record["plugin"],
        actor_kind=actor_kind,
        actor_form_id=record["formId"],
        actor_editor_id=record.get("editorId", ""),
        component="actor-base-record",
        source_record_type=actor_kind,
        source_form_id=record["formId"],
        source_editor_id=record.get("editorId", ""),
        classification="loaded-pending-runtime",
        first_failing_gate="actor-presentation-runtime-sweep",
        proof_anchor="npc-face-assembly" if actor_kind == "NPC_" else "creature-body-assembly",
        notes="Base actor is parsed; runtime presentation still needs visual and animation proof.",
    )

    if actor_kind == "NPC_":
        race = (all_adjusted_form_ids(record["subrecords"], "RNAM", record["masters"], record["plugin"], content_index) or [0])[0]
        add_reference_row(
            rows,
            record,
            by_form,
            race,
            component="npc-race",
            subrecord="RNAM",
            proof_anchor="npc-face-assembly",
            notes="Race selects body/head part families and face defaults.",
            actor=actor,
        )
        for field, component, anchor, note in (
            ("HNAM", "hair", "npc-face-assembly", "NPC hair form and fallback attachment."),
            ("ENAM", "eyes", "npc-face-assembly", "NPC eye form and eye texture path."),
            ("TPLT", "npc-template", "npc-face-assembly", "Template chain can override actor presentation fields."),
            ("DOFT", "default-outfit", "npc-equipment-assembly", "Default outfit contributes worn gear."),
            ("SOFT", "sleep-outfit", "npc-equipment-assembly", "Sleep outfit contributes alternate worn gear."),
            ("VTCK", "voice-type", "voice-lip-runtime", "Voice type is required for correct dialogue voice and lip routing."),
        ):
            ids = all_adjusted_form_ids(record["subrecords"], field, record["masters"], record["plugin"], content_index)
            for form_id in ids:
                classification = "known-blocked" if field == "VTCK" else "loaded-pending-runtime"
                failing_gate = "loadnpc-vtck-runtime-binding" if field == "VTCK" else "actor-presentation-runtime-sweep"
                add_reference_row(
                    rows,
                    record,
                    by_form,
                    form_id,
                    component=component,
                    subrecord=field,
                    classification=classification,
                    first_failing_gate=failing_gate,
                    proof_anchor=anchor,
                    notes=note,
                    actor=actor,
                )
        for form_id in all_adjusted_form_ids(record["subrecords"], "PNAM", record["masters"], record["plugin"], content_index):
            add_reference_row(
                rows,
                record,
                by_form,
                form_id,
                component="npc-headpart",
                subrecord="PNAM",
                proof_anchor="npc-headpart-insert",
                notes="NPC-specific head part must attach and respect extra-part rules.",
                actor=actor,
            )
        for subrecord, component in (("FGGS", "facegen-symmetric-shape"), ("FGGA", "facegen-asymmetric-shape"), ("FGTS", "facegen-symmetric-texture")):
            if base.first(record["subrecords"], subrecord):
                rows.add(
                    plugin=record["plugin"],
                    actor_kind=actor_kind,
                    actor_form_id=record["formId"],
                    actor_editor_id=record.get("editorId", ""),
                    component=component,
                    source_record_type=actor_kind,
                    source_form_id=record["formId"],
                    source_editor_id=record.get("editorId", ""),
                    subrecord=subrecord,
                    classification="loaded-pending-runtime",
                    first_failing_gate="facegen-runtime-visual-delta",
                    proof_anchor="npc-face-assembly",
                    notes="FaceGen coefficients are loaded; actor-specific visual deltas need runtime proof.",
                )
        model = base.first_zstring(record["subrecords"], "MODL")
        if model:
            add_asset_row(
                rows,
                record,
                component="npc-model",
                subrecord="MODL",
                asset_path=model_archive_path(model),
                proof_anchor="npc-face-assembly",
                notes="NPC model override path.",
                actor=actor,
            )
        for kf in collect_strings(record, "KFFZ"):
            add_asset_row(
                rows,
                record,
                component="animation-kffz",
                subrecord="KFFZ",
                asset_path=model_archive_path(kf),
                proof_anchor="npc-kffz-animation",
                notes="Authored per-NPC animation source; must play, not just load.",
                actor=actor,
            )
        rows.add(
            plugin=record["plugin"],
            actor_kind=actor_kind,
            actor_form_id=record["formId"],
            actor_editor_id=record.get("editorId", ""),
            component="animation-locomotion-fallback",
            source_record_type=actor_kind,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="npc-animation-active-pose-sweep",
            proof_anchor="npc-kffz-animation",
            notes="Fallback locomotion paths are wired; each actor still needs non-T-pose runtime proof.",
        )
    else:
        model = base.first_zstring(record["subrecords"], "MODL")
        if model:
            add_asset_row(
                rows,
                record,
                component="creature-model",
                subrecord="MODL",
                asset_path=model_archive_path(model),
                proof_anchor="creature-body-assembly",
                notes="Creature/robot root model.",
                actor=actor,
            )
        for nif in collect_strings(record, "NIFZ"):
            add_asset_row(
                rows,
                record,
                component="creature-body-nif",
                subrecord="NIFZ",
                asset_path=model_archive_path(nif),
                proof_anchor="creature-body-assembly",
                notes="Creature/robot supplemental body NIF.",
                actor=actor,
            )
        for form_id in all_adjusted_form_ids(record["subrecords"], "PNAM", record["masters"], record["plugin"], content_index):
            add_reference_row(
                rows,
                record,
                by_form,
                form_id,
                component="bodypart-data",
                subrecord="PNAM",
                classification="loaded-pending-runtime",
                first_failing_gate="creature-bodypart-runtime-sweep",
                proof_anchor="creature-body-assembly",
                notes="BPTD is parsed; gore/limb/VATS presentation remains pending.",
                actor=actor,
            )
        for field, component, gate in (
            ("TPLT", "creature-template", "actor-presentation-runtime-sweep"),
            ("VTCK", "voice-type", "loadcrea-vtck-runtime-binding"),
        ):
            for form_id in all_adjusted_form_ids(record["subrecords"], field, record["masters"], record["plugin"], content_index):
                add_reference_row(
                    rows,
                    record,
                    by_form,
                    form_id,
                    component=component,
                    subrecord=field,
                    classification="known-blocked" if field == "VTCK" else "loaded-pending-runtime",
                    first_failing_gate=gate,
                    proof_anchor="voice-lip-runtime" if field == "VTCK" else "creature-body-assembly",
                    notes="Creature voice/runtime presentation dependency." if field == "VTCK" else "Template can override creature presentation fields.",
                    actor=actor,
                )
        for kf in collect_strings(record, "KFFZ"):
            add_asset_row(
                rows,
                record,
                component="animation-kffz",
                subrecord="KFFZ",
                asset_path=model_archive_path(kf),
                proof_anchor="creature-kffz-animation",
                notes="Creature/robot animation source; must play, not just load.",
                actor=actor,
            )

    for item, count in collect_actor_inventory_ids(record, content_index):
        target_type = record_type(by_form, item)
        if target_type == "ARMO":
            component = "equipment-armor"
            anchor = "npc-equipment-assembly"
        elif target_type == "CLOT":
            component = "equipment-clothing"
            anchor = "npc-equipment-assembly"
        elif target_type == "WEAP":
            component = "equipment-weapon"
            anchor = "npc-kffz-animation"
        else:
            component = "inventory-item"
            anchor = "npc-equipment-assembly" if actor_kind == "NPC_" else "creature-body-assembly"
        add_reference_row(
            rows,
            record,
            by_form,
            item,
            component=component,
            subrecord="CNTO",
            classification="loaded-pending-runtime",
            first_failing_gate="actor-inventory-presentation-sweep",
            proof_anchor=anchor,
            notes=f"Inventory item count={count}; actor presentation must prove visible/equipped behavior when applicable.",
            actor=actor,
        )


def emit_placed_actor_rows(rows, by_form, content_index, exterior_cell_index, record):
    ids = all_adjusted_form_ids(record["subrecords"], "NAME", record["masters"], record["plugin"], content_index)
    base_id = ids[0] if ids else 0
    base_record = resolve(by_form, base_id)
    position = placed_position(record["subrecords"])
    scale = placed_scale(record["subrecords"])
    cell = placed_cell_context(record, by_form, exterior_cell_index, position)
    placement_ready = bool(position and cell["runtimeFormId"])
    coordinate_source = "incomplete"
    if placement_ready and cell["source"] == "worldspace-xclc-from-position":
        coordinate_source = "ACHR/ACRE DATA + WRLD/XCLC exterior parent cell"
    elif placement_ready:
        coordinate_source = "ACHR/ACRE DATA + GRUP parent cell"
    placement_note = (
        " Placement DATA and parent cell context are decoded for runtime bootstrap/stage planning."
        if placement_ready
        else " Placement DATA or parent cell context is missing; runtime bootstrap/stage planning must not silently assume it."
    )
    rows.add(
        plugin=record["plugin"],
        actor_kind=base_record["type"] if base_record else record["type"],
        actor_form_id=form_id_or_empty(base_id) if base_id else "",
        actor_editor_id=base_record.get("editorId", "") if base_record else "",
        placed_ref_form_id=record["formId"],
        placed_ref_editor_id=record.get("editorId", ""),
        placed_cell_form_id=cell["formId"],
        placed_runtime_cell_form_id=cell["runtimeFormId"],
        placed_cell_editor_id=cell["editorId"],
        placed_cell_group_type=cell["groupType"],
        placed_cell_group_name=cell["groupName"],
        placed_cell_source=cell["source"],
        placed_cell_grid_x=cell["gridX"],
        placed_cell_grid_y=cell["gridY"],
        placed_cell_fallback_form_id=cell["fallbackFormId"],
        placed_coordinate_source=coordinate_source,
        placed_pos_x=position.get("x"),
        placed_pos_y=position.get("y"),
        placed_pos_z=position.get("z"),
        placed_rot_x=position.get("rotX"),
        placed_rot_y=position.get("rotY"),
        placed_rot_z=position.get("rotZ"),
        placed_scale=scale,
        component="placed-reference",
        source_record_type=record["type"],
        source_form_id=record["formId"],
        source_editor_id=record.get("editorId", ""),
        subrecord="NAME",
        resolved_record_type=base_record["type"] if base_record else "",
        resolved_form_id=form_id_or_empty(base_id) if base_id else "",
        resolved_editor_id=base_record.get("editorId", "") if base_record else "",
        classification="loaded-pending-runtime" if base_record else "known-blocked",
        first_failing_gate="placed-actor-runtime-sweep" if base_record else "fnv-formid-resolve",
        proof_anchor="npc-face-assembly" if base_record and base_record["type"] == "NPC_" else "creature-body-assembly",
        notes="Placed actor must spawn, present correctly, and be movable/interactable in runtime." + placement_note,
    )


def emit_race_rows(rows, by_form, content_index, record):
    rows.add(
        plugin=record["plugin"],
        component="support-record",
        source_record_type="RACE",
        source_form_id=record["formId"],
        source_editor_id=record.get("editorId", ""),
        classification="loaded-pending-runtime",
        first_failing_gate="race-face-body-runtime-sweep",
        proof_anchor="npc-face-assembly",
        notes="Race is parsed; exact body/head material and tint presentation needs runtime proof.",
    )
    section = ""
    gender = ""
    index = None
    for kind, payload in record["subrecords"]:
        if kind == "NAM0":
            section = "head"
            index = None
        elif kind == "NAM1":
            section = "body"
            index = None
        elif kind == "MNAM":
            gender = "male"
        elif kind == "FNAM":
            gender = "female"
        elif kind == "INDX" and len(payload) >= 4:
            index = struct.unpack_from("<I", payload, 0)[0]
        elif kind == "MODL" and section in {"head", "body"}:
            value = base.zstring(payload)
            role = RACE_FACE_ROLES.get(index, f"index{index}") if section == "head" else f"body{index}"
            add_asset_row(
                rows,
                record,
                component=f"race-{section}",
                subrecord=f"{kind}:{gender}:{role}",
                asset_path=model_archive_path(value),
                proof_anchor="npc-face-assembly",
                notes=f"Race {gender or 'unknown'} {section} mesh role={role}.",
            )
        elif kind == "ICON" and section in {"head", "body"}:
            value = base.zstring(payload)
            role = RACE_FACE_ROLES.get(index, f"index{index}") if section == "head" else f"body{index}"
            add_asset_row(
                rows,
                record,
                component=f"race-{section}-texture",
                subrecord=f"{kind}:{gender}:{role}",
                asset_path=texture_archive_path(value),
                proof_anchor="npc-face-assembly",
                notes=f"Race {gender or 'unknown'} {section} texture role={role}.",
            )
        elif kind in {"HNAM", "ENAM", "DNAM", "HEAD"}:
            for form_id in form_ids_from_payload(payload, record["masters"], record["plugin"], content_index):
                add_reference_row(
                    rows,
                    record,
                    by_form,
                    form_id,
                    component="race-choice" if kind in {"HNAM", "ENAM", "DNAM"} else "race-headpart",
                    subrecord=kind,
                    proof_anchor="npc-face-assembly",
                    notes="Race selectable/default hair, eye, or head-part dependency.",
                )


def emit_support_rows(rows, by_form, content_index, record):
    rec_type = record["type"]
    if rec_type == "RACE":
        emit_race_rows(rows, by_form, content_index, record)
        return
    if rec_type == "HDPT":
        part_type = None
        pnam = base.first(record["subrecords"], "PNAM")
        if pnam and len(pnam) >= 4:
            part_type = struct.unpack_from("<I", pnam, 0)[0]
        rows.add(
            plugin=record["plugin"],
            component="hair",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="headpart-runtime-sweep",
            proof_anchor="npc-headpart-insert",
            notes=f"Head-part record type={HDPT_TYPES.get(part_type, part_type)}.",
        )
        model = base.first_zstring(record["subrecords"], "MODL")
        if model:
            add_asset_row(rows, record, component="headpart-model", subrecord="MODL", asset_path=model_archive_path(model), proof_anchor="npc-headpart-insert")
        nam1_index = 0
        for kind, payload in record["subrecords"]:
            if kind == "NAM1":
                tri = base.zstring(payload)
                if tri:
                    add_asset_row(
                        rows,
                        record,
                        component="headpart-tri",
                        subrecord=f"NAM1[{nam1_index}]",
                        asset_path=model_archive_path(tri),
                        classification="loaded-pending-runtime",
                        first_failing_gate="headpart-tri-runtime-morph",
                        proof_anchor="npc-headpart-insert",
                    )
                nam1_index += 1
            elif kind in {"HNAM", "TNAM", "CNAM", "RNAM"}:
                for form_id in form_ids_from_payload(payload, record["masters"], record["plugin"], content_index):
                    add_reference_row(
                        rows,
                        record,
                        by_form,
                        form_id,
                        component="headpart-reference",
                        subrecord=kind,
                        proof_anchor="npc-headpart-insert",
                    )
    elif rec_type == "HAIR":
        rows.add(
            plugin=record["plugin"],
            component="hair",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="hair-runtime-sweep",
            proof_anchor="npc-face-assembly",
            notes="Hair record is parsed; per-actor attachment/tint needs visual proof.",
        )
        for subrecord, component, normalizer in (
            ("MODL", "hair-model", model_archive_path),
            ("ICON", "hair-icon-texture", texture_archive_path),
        ):
            value = base.first_zstring(record["subrecords"], subrecord)
            if value:
                add_asset_row(rows, record, component=component, subrecord=subrecord, asset_path=normalizer(value), proof_anchor="npc-face-assembly")
    elif rec_type == "EYES":
        rows.add(
            plugin=record["plugin"],
            component="eyes",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="eye-runtime-sweep",
            proof_anchor="npc-face-assembly",
            notes="Eye record is parsed; eye material/texture needs per-actor visual proof.",
        )
        value = base.first_zstring(record["subrecords"], "ICON")
        if value:
            add_asset_row(rows, record, component="eye-texture", subrecord="ICON", asset_path=texture_archive_path(value), proof_anchor="npc-face-assembly")
    elif rec_type == "ARMO":
        rows.add(
            plugin=record["plugin"],
            component="equipment-armor",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="armor-headgear-runtime-sweep",
            proof_anchor="npc-equipment-assembly",
            notes="Armor flags/addon references are loaded; head/hair hiding and fitted geometry need proof.",
        )
        for kind, payload in record["subrecords"]:
            if kind == "MODL" and len(payload) == 4:
                add_reference_row(
                    rows,
                    record,
                    by_form,
                    adjusted_form_id(struct.unpack_from("<I", payload, 0)[0], record["masters"], record["plugin"], content_index),
                    component="equipment-armor-addon",
                    subrecord="MODL",
                    proof_anchor="npc-equipment-assembly",
                    notes="ARMO addon reference.",
                )
            elif kind in {"MODL", "MOD2", "MOD3", "MOD4"}:
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="equipment-armor-model", subrecord=kind, asset_path=model_archive_path(value), proof_anchor="npc-equipment-assembly")
            elif kind in {"ICON", "ICO2", "MICO", "MIC2"}:
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="equipment-armor-icon", subrecord=kind, asset_path=texture_archive_path(value), proof_anchor="npc-equipment-assembly")
    elif rec_type == "ARMA":
        rows.add(
            plugin=record["plugin"],
            component="equipment-armor-addon",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="armor-addon-runtime-sweep",
            proof_anchor="npc-equipment-assembly",
            notes="Armor addon is parsed; exact race/sex geometry and headgear alignment needs proof.",
        )
        for kind, payload in record["subrecords"]:
            if kind in {"MOD2", "MOD3", "MOD4", "MOD5"}:
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="armor-addon-model", subrecord=kind, asset_path=model_archive_path(value), proof_anchor="npc-equipment-assembly")
            elif kind == "MODL":
                if len(payload) == 4:
                    add_reference_row(
                        rows,
                        record,
                        by_form,
                        adjusted_form_id(struct.unpack_from("<I", payload, 0)[0], record["masters"], record["plugin"], content_index),
                        component="armor-addon-race-reference",
                        subrecord="MODL",
                        proof_anchor="npc-equipment-assembly",
                        notes="TES5-style ARMA race reference.",
                    )
                else:
                    add_asset_row(
                        rows,
                        record,
                        component="armor-addon-fnv-modl",
                        subrecord="MODL",
                        asset_path=model_archive_path(base.zstring(payload)),
                        classification="loaded-pending-runtime",
                        first_failing_gate="armor-addon-runtime-sweep",
                        proof_anchor="npc-equipment-assembly",
                        notes="FO3/FNV ARMA MODL is parsed; exact race/sex equipment fitting still needs runtime sweep proof.",
                    )
    elif rec_type == "CLOT":
        rows.add(
            plugin=record["plugin"],
            component="equipment-clothing",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="clothing-fnv-equipment-binding",
            proof_anchor="npc-equipment-assembly",
            notes="CLOT records parse, but FNV actor clothing/equipment parity needs runtime proof.",
        )
        for kind, payload in record["subrecords"]:
            if kind in {"MODL", "MOD2", "MOD3", "MOD4"}:
                value = base.zstring(payload)
                if value:
                    add_asset_row(
                        rows,
                        record,
                        component="equipment-clothing-model",
                        subrecord=kind,
                        asset_path=model_archive_path(value),
                        classification="loaded-pending-runtime",
                        first_failing_gate="clothing-fnv-equipment-binding",
                        proof_anchor="npc-equipment-assembly",
                    )
            elif kind in {"ICON", "ICO2"}:
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="equipment-clothing-icon", subrecord=kind, asset_path=texture_archive_path(value), proof_anchor="npc-equipment-assembly")
    elif rec_type == "BPTD":
        rows.add(
            plugin=record["plugin"],
            component="support-record",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="creature-bodypart-runtime-sweep",
            proof_anchor="creature-body-assembly",
            notes="BPTD is parsed; limb/gore/VATS/damage presentation remains pending.",
        )
        for kind, payload in record["subrecords"]:
            if kind == "MODL":
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="bodypart-model", subrecord=kind, asset_path=model_archive_path(value), proof_anchor="creature-body-assembly")
            elif kind == "NAM1":
                value = base.zstring(payload)
                if value:
                    add_asset_row(
                        rows,
                        record,
                        component="bodypart-limb-replacement",
                        subrecord=kind,
                        asset_path=model_archive_path(value),
                        classification="loaded-pending-runtime",
                        first_failing_gate="creature-limb-gore-runtime-sweep",
                        proof_anchor="creature-body-assembly",
                    )
    elif rec_type in {"DIAL", "INFO"}:
        component = "dialogue-topic" if rec_type == "DIAL" else "dialogue-info"
        rows.add(
            plugin=record["plugin"],
            component=component,
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="dialogue-selection-runtime-sweep",
            proof_anchor="dialogue-runtime",
            notes="Dialogue row is parsed; runtime selection/conditions/result scripts still need proof.",
        )
        for field in ("QSTI", "QSTR", "TCLT", "NAME", "ANAM", "SNDD", "SNAM"):
            for form_id in all_adjusted_form_ids(record["subrecords"], field, record["masters"], record["plugin"], content_index):
                reference_component = "voice-audio" if field in {"SNDD", "SNAM"} else "dialogue-reference"
                gate = "dialogue-voice-lip-ledger" if field in {"SNDD", "SNAM"} else "dialogue-selection-runtime-sweep"
                add_reference_row(
                    rows,
                    record,
                    by_form,
                    form_id,
                    component=reference_component,
                    subrecord=field,
                    proof_anchor="voice-lip-runtime" if reference_component == "voice-audio" else "dialogue-runtime",
                    first_failing_gate=gate,
                    notes="Voice sound form must resolve to audio and LIP sidecar." if reference_component == "voice-audio" else "Dialogue linked form.",
                )
        if rec_type == "INFO":
            response = base.first_zstring(record["subrecords"], "NAM1")
            rows.add(
                plugin=record["plugin"],
                component="voice-lip",
                source_record_type=rec_type,
                source_form_id=record["formId"],
                source_editor_id=record.get("editorId", ""),
                subrecord="NAM1/TRDT/SNDD",
                classification="loaded-pending-runtime",
                first_failing_gate="dialogue-voice-lip-ledger",
                proof_anchor="voice-lip-runtime",
                notes=f"INFO response hash={text_hash(response)}; exact voice path is proven by dialogue voice/lip ledger.",
            )
    elif rec_type in {"IDLE", "IDLM", "ANIO"}:
        rows.add(
            plugin=record["plugin"],
            component="support-record",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="idle-animation-runtime-sweep",
            proof_anchor="npc-kffz-animation",
            notes="Idle/animated-object record is parsed; runtime selection and pose proof pending.",
        )
        for kind, payload in record["subrecords"]:
            if kind == "MODL":
                value = base.zstring(payload)
                if value:
                    add_asset_row(rows, record, component="animation-idle-model", subrecord=kind, asset_path=model_archive_path(value), proof_anchor="npc-kffz-animation")
            elif kind in {"ANAM", "DATA"}:
                for form_id in form_ids_from_payload(payload, record["masters"], record["plugin"], content_index):
                    add_reference_row(rows, record, by_form, form_id, component="animation-idle-reference", subrecord=kind, proof_anchor="npc-kffz-animation")
    elif rec_type == "OTFT":
        rows.add(
            plugin=record["plugin"],
            component="support-record",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="outfit-equipment-runtime-sweep",
            proof_anchor="npc-equipment-assembly",
            notes="Outfit items are parsed; actor equipment presentation proof pending.",
        )
        for form_id in all_adjusted_form_ids(record["subrecords"], "INAM", record["masters"], record["plugin"], content_index):
            add_reference_row(rows, record, by_form, form_id, component="outfit-item", subrecord="INAM", proof_anchor="npc-equipment-assembly")
    elif rec_type == "VTYP":
        rows.add(
            plugin=record["plugin"],
            component="voice-type",
            source_record_type=rec_type,
            source_form_id=record["formId"],
            source_editor_id=record.get("editorId", ""),
            classification="loaded-pending-runtime",
            first_failing_gate="voice-type-dialogue-runtime-binding",
            proof_anchor="voice-lip-runtime",
            notes="Voice type record is accounted; actor-to-voice selection parity is not yet proven.",
        )


def summarize_rows(rows):
    classification_counts = Counter(row["classification"] for row in rows)
    component_counts = Counter(row["component"] for row in rows)
    record_counts = Counter(row["sourceRecordType"] for row in rows)
    asset_status_counts = Counter(row["assetStatus"] for row in rows if row["assetPath"])
    unclassified = sum(1 for row in rows if row["classification"] not in ALLOWED_CLASSIFICATIONS)
    blocked = sum(1 for row in rows if row["classification"] == "known-blocked")
    placed_rows = [row for row in rows if row["component"] == "placed-reference"]
    placed_with_data = sum(1 for row in placed_rows if row.get("placedPosX") is not None and row.get("placedPosY") is not None and row.get("placedPosZ") is not None)
    placed_with_cell = sum(1 for row in placed_rows if row.get("placedCellFormId"))
    placed_with_runtime_cell = sum(1 for row in placed_rows if row.get("placedRuntimeCellFormId"))
    placed_with_grid_cell = sum(1 for row in placed_rows if row.get("placedCellSource") == "worldspace-xclc-from-position")
    return {
        "rowCount": len(rows),
        "classificationCounts": dict(sorted(classification_counts.items())),
        "componentCounts": dict(sorted(component_counts.items())),
        "sourceRecordCounts": dict(sorted(record_counts.items())),
        "assetStatusCounts": dict(sorted(asset_status_counts.items())),
        "unclassifiedCount": unclassified,
        "knownBlockedCount": blocked,
        "placedActorRefsWithDataPosition": placed_with_data,
        "placedActorRefsWithParentCell": placed_with_cell,
        "placedActorRefsWithRuntimeParentCell": placed_with_runtime_cell,
        "placedActorRefsWithExteriorGridParentCell": placed_with_grid_cell,
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Build a Fallout New Vegas actor presentation dependency ledger.")
    parser.add_argument("--fnv-root", default=os.environ.get("NIKAMI_FNV_ROOT", ""))
    parser.add_argument("--fnv-data", default=os.environ.get("NIKAMI_FNV_DATA", ""))
    parser.add_argument("--harvest-dir", default="")
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
    harvest_dir = Path(args.harvest_dir) if args.harvest_dir else None
    if harvest_dir is None:
        harvest_root = proof_root / "fnv-retail-harvest"
        latest = sorted([path for path in harvest_root.glob("*") if path.is_dir()], reverse=True)
        harvest_dir = latest[0] if latest else None

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    proof_dir = proof_root / "fnv-actor-presentation-ledger" / stamp
    proof_dir.mkdir(parents=True, exist_ok=True)
    summary_file = proof_dir / "summary.txt"

    def proof_line(text=""):
        print(text, flush=True)
        with summary_file.open("a", encoding="utf-8") as stream:
            stream.write(text + "\n")

    proof_line(f"FNV actor presentation ledger proof {stamp}")
    proof_line(f"RepoRoot: {repo_root}")
    proof_line(f"FnvRoot: {fnv_root}")
    proof_line(f"FnvData: {fnv_data}")
    proof_line(f"HarvestDir: {harvest_dir if harvest_dir else '<none>'}")
    proof_line(f"ProofDir: {proof_dir}")
    proof_line()

    missing = []
    content_index = {normalize_plugin_name(name): index for index, name in enumerate(args.content)}
    plugin_ledgers = []
    for name in args.content:
        path = fnv_data / name
        if not path.is_file():
            missing.append(name)
            proof_line(f"FAIL missing content: {name} -> {path}")
            continue
        proof_line(f"Parsing content: {name}")
        plugin_ledgers.append(read_plugin_records(path, name, content_index))
    if missing:
        raise SystemExit(f"Missing active content files: {', '.join(missing)}")

    by_form, by_type = index_records(plugin_ledgers)
    exterior_cell_index = build_exterior_cell_index(by_type)
    asset_index = AssetIndex(fnv_data, harvest_dir)
    builder = RowBuilder(asset_index)

    for record in by_type["NPC_"]:
        emit_actor_rows(builder, by_form, content_index, record)
    for record in by_type["CREA"]:
        emit_actor_rows(builder, by_form, content_index, record)
    for rec_type in sorted(PLACED_ACTOR_TYPES):
        for record in by_type[rec_type]:
            emit_placed_actor_rows(builder, by_form, content_index, exterior_cell_index, record)
    for rec_type in sorted(SUPPORT_RECORD_TYPES):
        for record in by_type[rec_type]:
            emit_support_rows(builder, by_form, content_index, record)

    rows = builder.rows
    summary = summarize_rows(rows)
    record_manifest = [
        {
            "plugin": ledger["plugin"],
            "path": ledger["path"],
            "length": ledger["length"],
            "masters": ledger["masters"],
            "records": [{"type": key, "count": ledger["counts"][key]} for key in sorted(ledger["counts"])],
        }
        for ledger in plugin_ledgers
    ]
    actor_counts = {
        "npcBaseRecords": len(by_type["NPC_"]),
        "creatureBaseRecords": len(by_type["CREA"]),
        "placedNpcCreatureRefs": len(by_type["ACHR"]) + len(by_type["ACRE"]),
        "raceRecords": len(by_type["RACE"]),
        "headPartRecords": len(by_type["HDPT"]),
        "hairRecords": len(by_type["HAIR"]),
        "eyeRecords": len(by_type["EYES"]),
        "armorRecords": len(by_type["ARMO"]),
        "armorAddonRecords": len(by_type["ARMA"]),
        "clothingRecords": len(by_type["CLOT"]),
        "bodyPartDataRecords": len(by_type["BPTD"]),
        "dialogueRecords": len(by_type["DIAL"]),
        "infoRecords": len(by_type["INFO"]),
        "idleRecords": len(by_type["IDLE"]),
        "animObjectRecords": len(by_type["ANIO"]),
    }

    artifacts = {
        "records": proof_dir / "records.json",
        "ledger": proof_dir / "actor-presentation-ledger.json",
        "runtimeAnchors": proof_dir / "runtime-anchors.json",
        "result": proof_dir / "result.json",
        "summary": summary_file,
    }
    result = {
        "status": "PASS" if summary["unclassifiedCount"] == 0 else "FAIL",
        "stamp": stamp,
        "repoRoot": str(repo_root),
        "fnvData": str(fnv_data),
        "harvestDir": str(harvest_dir) if harvest_dir else "",
        "payloadPolicy": "retail-text-redacted-v1; no retail/mod payload bytes are copied by this proof",
        "content": args.content,
        "pluginCount": len(plugin_ledgers),
        **actor_counts,
        **summary,
        "runtimeAnchors": RUNTIME_ANCHORS,
        "artifacts": {key: str(value) for key, value in artifacts.items()},
    }
    write_json(artifacts["records"], record_manifest)
    write_json(artifacts["ledger"], rows)
    write_json(artifacts["runtimeAnchors"], RUNTIME_ANCHORS)
    write_json(artifacts["result"], result)

    proof_line()
    proof_line("FNV actor presentation artifacts:")
    for key in ("records", "ledger", "runtimeAnchors", "result"):
        proof_line(f"{key}: {artifacts[key]}")
    proof_line()
    proof_line("FNV actor presentation ledger proof " + result["status"])
    proof_line(
        "plugins={plugins} npc={npc} crea={crea} placed={placed} rows={rows} unclassified={unclassified} "
        "blocked={blocked}".format(
            plugins=len(plugin_ledgers),
            npc=actor_counts["npcBaseRecords"],
            crea=actor_counts["creatureBaseRecords"],
            placed=actor_counts["placedNpcCreatureRefs"],
            rows=summary["rowCount"],
            unclassified=summary["unclassifiedCount"],
            blocked=summary["knownBlockedCount"],
        )
    )
    proof_line("classificationCounts=" + json.dumps(summary["classificationCounts"], sort_keys=True))
    proof_line("assetStatusCounts=" + json.dumps(summary["assetStatusCounts"], sort_keys=True))
    proof_line("componentCounts=" + json.dumps(summary["componentCounts"], sort_keys=True))

    if summary["unclassifiedCount"]:
        raise SystemExit(f"Actor presentation ledger has {summary['unclassifiedCount']} unclassified rows.")


if __name__ == "__main__":
    main()
