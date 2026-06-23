#!/usr/bin/env python3
import argparse
import hashlib
import json
import re
from collections import Counter
from datetime import datetime
from pathlib import Path


ALLOWED_CLASSIFICATIONS = {
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof",
}


RUNTIME_SUPPORTED_RECORDS = {
    "ACHR": "placed actor references are preprocessed into active cells",
    "ACRE": "placed creature references are preprocessed into active cells",
    "AMMO": "ammo records feed weapon/HUD ammo paths",
    "CELL": "cells are loaded into the world model",
    "CREA": "creature records feed actor shells, rendering, and animation",
    "GMST": "game settings are bridged into runtime settings",
    "GLOB": "globals are bridged into runtime variables",
    "LAND": "land records feed terrain",
    "LTEX": "land textures feed terrain layers",
    "MSTT": "movable statics are loaded and placed",
    "NOTE": "notes are stored for runtime UI/data access",
    "NPC_": "NPC records feed actor shells, rendering, and animation",
    "REFR": "placed object references are loaded into cells",
    "SCOL": "static collections are placed into the world",
    "SNDR": "sound reference records feed sound lookup",
    "SOUN": "sound records feed sound lookup",
    "STAT": "static records feed world geometry",
    "TACT": "talking activators feed radio/runtime activator paths",
    "TES4": "plugin header is consumed by the ESM4 reader",
    "TXST": "texture sets feed material/texture resolution",
    "WEAP": "weapon records feed inventory, actor, and HUD ammo paths",
}


KNOWN_BLOCKED_RECORDS = {
}


LOADED_PENDING_RECORDS = {
    "ACTI": "loader/store exists; object-class/runtime behavior still needs targeted FNV parity gates",
    "ADDN": "addon node bytes are inventoried pending full visual/addon-node runtime binding",
    "ALCH": "loader/store exists; gameplay effects are pending full FNV parity gates",
    "ALOC": "audio location controller bytes are inventoried pending full media/location audio runtime binding",
    "AMEF": "ammo effect bytes are inventoried pending full ammo-effect gameplay binding",
    "ANIO": "animated object bytes are inventoried pending full animation-object runtime binding",
    "ARMA": "armor addon data is loaded; full equipment/body binding remains gated separately",
    "ARMO": "armor data is loaded; full equipment/body binding remains gated separately",
    "ASPC": "acoustic space data is loaded pending runtime audio-space binding",
    "AVIF": "actor value and perk tree records are typed-loaded pending full progression/SPECIAL runtime binding",
    "BOOK": "book records are loaded pending full UI/readable-content parity",
    "BPTD": "body part data is loaded pending dismemberment/body-part runtime parity",
    "CAMS": "camera shot bytes are inventoried pending full camera/scene runtime binding",
    "CCRD": "Caravan card bytes are inventoried pending full Caravan minigame runtime binding",
    "CDCK": "Caravan deck bytes are inventoried pending full Caravan minigame runtime binding",
    "CHAL": "challenge bytes are inventoried pending full challenge/progression runtime binding",
    "CHIP": "casino chip bytes are inventoried pending full casino economy runtime binding",
    "CLAS": "class records are loaded pending player/NPC gameplay parity",
    "CLOT": "clothing records are loaded pending full equipment/body binding",
    "CLMT": "climate bytes are inventoried pending full FNV climate/weather runtime binding",
    "CMNY": "Caravan money bytes are inventoried pending full Caravan economy runtime binding",
    "CONT": "container records are loaded pending full inventory/container behavior parity",
    "CPTH": "camera path bytes are inventoried pending full camera/scene runtime binding",
    "CSNO": "casino bytes are inventoried pending full casino/minigame runtime binding",
    "CSTY": "combat style data is known but combat tuning parity remains pending",
    "DEBR": "debris bytes are inventoried pending full explosion/debris runtime binding",
    "DEHY": "dehydration stage bytes are inventoried pending full hardcore-mode actor effect binding",
    "DIAL": "dialogue topics are stored and partially bridged pending exhaustive FNV INFO selection/result-script parity",
    "DOBJ": "default object manager bytes are inventoried pending full default-object lookup binding",
    "DOOR": "door records are loaded pending full activation/lock/teleport parity",
    "ECZN": "encounter zone bytes are inventoried pending full spawn/AI level-scaling runtime binding",
    "EFSH": "effect shader data is loaded pending full visual-effect runtime parity",
    "ENCH": "enchantment records are known pending full FNV magic/effect bridge",
    "EXPL": "explosion records are source-backed and stored pending runtime effect binding",
    "EYES": "eyes records are loaded pending full character creation/UI parity",
    "FACT": "faction records are known pending full reputation/faction gameplay parity",
    "FLOR": "flora records are loaded pending full harvest/use behavior parity",
    "FLST": "form lists are loaded pending exhaustive referencing-system parity",
    "FURN": "furniture records are loaded pending full package/furniture behavior parity",
    "GRAS": "grass records are known pending FNV grass runtime parity",
    "HAIR": "hair records are loaded and used by character visuals pending full creator parity",
    "HDPT": "head-part records are loaded and used by character visuals pending full creator parity",
    "HUNG": "hunger stage bytes are inventoried pending full hardcore-mode actor effect binding",
    "IDLE": "idle animation records are loaded pending complete package/idle runtime parity",
    "IDLM": "idle marker records are loaded pending complete idle-marker runtime parity",
    "INFO": "dialogue INFO rows are stored and partially bridged pending exhaustive conditions, choices, and result-script parity",
    "IMAD": "image-space modifier records are loaded pending full post-process binding",
    "IMGS": "image-space bytes are inventoried pending full post-process binding",
    "IMOD": "item mod records are loaded pending full weapon-mod gameplay binding",
    "INGR": "ingredient records are loaded pending full effects/crafting parity",
    "IPCT": "impact data records are loaded pending full projectile/impact binding",
    "IPDS": "impact data sets are loaded pending full projectile/impact binding",
    "KEYM": "key records are loaded pending full lock/key gameplay parity",
    "LGTM": "lighting templates are loaded pending full interior lighting parity",
    "LIGH": "light records are loaded pending full lighting behavior parity",
    "LSCR": "load screen records are known pending full load-screen selection parity",
    "LSCT": "load screen type bytes are inventoried pending full load-screen selection runtime binding",
    "LVLC": "leveled creature records are loaded pending full leveled-spawn parity",
    "LVLI": "leveled item records are loaded pending full leveled-list parity",
    "LVLN": "leveled actor records are loaded pending full leveled-actor parity",
    "MESG": "message records are known pending message/UI parity",
    "MGEF": "magic effect records are known pending full effect runtime parity",
    "MICN": "menu icon bytes are inventoried pending full UI icon resolution binding",
    "MISC": "misc item records are loaded pending complete item behavior parity",
    "MSET": "media set records are loaded pending full media selection parity",
    "MUSC": "music type records are loaded pending full radio/music selection parity",
    "NAVI": "navigation master data is loaded pending complete navmesh parity gates",
    "NAVM": "navmesh records are loaded pending complete navmesh parity gates",
    "PACK": "AI packages are loaded and partially acted on pending exhaustive package parity",
    "PERK": "perk/trait records are source-backed and stored pending player progression binding",
    "PGRE": "placed grenade records are loaded pending projectile/explosive runtime parity",
    "PROJ": "projectile records are source-backed and stored pending runtime projectile binding",
    "QUST": "quest records are stored, QUST stage journal entries are bridged, selected SetStage/GetStage and stage fragments execute, selected objective target references can resolve, and objective displayed/completed script state is bound pending full quest completion/HUD-marker/condition parity",
    "PWAT": "placeable water records are loaded pending full water placement parity",
    "RACE": "race records feed actors pending full character creation/body parity",
    "RADS": "radiation stage bytes are inventoried pending full actor effect/status runtime binding",
    "RCCT": "recipe category bytes are inventoried pending full crafting menu/runtime binding",
    "RCPE": "recipe bytes are inventoried pending full crafting menu/runtime binding",
    "REGN": "region bytes are inventoried pending full FNV weather/audio-region runtime binding",
    "REPU": "reputation bytes are inventoried pending full faction/reputation runtime binding",
    "RGDL": "ragdoll bytes are inventoried pending full physics death-pose runtime binding",
    "SCEN": "scene records are known pending full scene runtime parity",
    "SCPT": "script source is stored and bridged as evidence pending full FNV script VM/runtime semantics",
    "SLPD": "sleep deprivation stage bytes are inventoried pending full hardcore-mode actor effect binding",
    "SPEL": "spell records are known pending full effects/runtime parity",
    "TERM": "terminal records are loaded pending full terminal UI/script parity",
    "TREE": "tree records render through the current SpeedTree billboard fallback",
    "VTYP": "voice type records are loaded pending exhaustive voice selection parity",
    "WATR": "water records are loaded pending full water shader/behavior parity",
    "WRLD": "world records are loaded pending full climate/weather/world runtime parity",
    "WTHR": "weather bytes are inventoried pending WeatherManager binding",
}


EXTENSION_RULES = {
    ".bik": ("runtime-supported", "video-menu-intro", "VideoWidget plays BIK menu/intro movies through VFS."),
    ".ctl": ("loaded-pending-runtime", "facegen-control-basis", "CTL bytes are read and validated; full FaceGen control semantics remain pending."),
    ".dat": ("non-runtime-support-file", "lip-generation-support-tables", "LSDATA tables support lip generation tooling; runtime consumes baked LIP sidecars."),
    ".dds": ("runtime-supported", "textures-ui-world", "DDS files are consumed by ImageManager/MyGUI/material texture binding."),
    ".dlodsettings": ("loaded-pending-runtime", "distant-lod", "DLOD settings are read through VFS in PC-flat proof; terrain/object paging binding remains pending."),
    ".egm": ("runtime-supported", "facegen-morphs", "EGM morphs are loaded and applied to NPC geometry."),
    ".egt": ("loaded-pending-runtime", "facegen-tint-maps", "EGT bytes are read and tint is approximated; exact texture synthesis remains pending."),
    ".fnt": ("runtime-supported", "bitmap-fonts", "Bitmap font metadata is consumed by FontLoader."),
    ".kf": ("runtime-supported", "animation-keyframes", "KF animation sources are attached to actor/creature animation."),
    ".lip": ("loaded-pending-runtime", "voice-lip-sync", "LIP sidecars feed a mouth envelope; exact phoneme mapping remains pending."),
    ".mp3": ("runtime-supported", "music-radio", "MP3 files are streamed by the sound/music manager."),
    ".nif": ("runtime-supported", "scene-mesh-collision", "NIF files are loaded for render nodes and collision users."),
    ".ogg": ("runtime-supported", "sound-voice", "OGG files are decoded by the sound manager."),
    ".psa": ("loaded-pending-runtime", "actor-deathpose-animation", "PSA death-pose bytes are read through VFS in PC-flat proof; actor/creature death playback remains pending."),
    ".psd": ("non-runtime-support-file", "source-art-leftover", "PSD source art is not a runtime file; DDS siblings are runtime assets."),
    ".spt": ("loaded-pending-runtime", "speedtree-billboard-fallback", "SPT files render through a billboard fallback pending real SpeedTree geometry/collision."),
    ".tai": ("runtime-supported", "interface-texture-atlas-index", "TAI atlas metadata is parsed for InterfaceShared DDS regions."),
    ".tex": ("runtime-supported", "bitmap-fonts", "Paired bitmap font TEX files are consumed by FontLoader."),
    ".tri": ("runtime-supported", "facegen-tri-morph-targets", "TRI morphs are loaded and applied for face/dialogue morphs."),
    ".txt": ("loaded-pending-runtime", "text-assets", "Text files are VFS-visible; runtime use depends on a referencing system."),
    ".wav": ("runtime-supported", "sound-voice", "WAV files are decoded by the sound manager."),
    ".xml": ("runtime-supported", "menus-ui-layout", "XML UI/menu files are consumed by MyGUI/OpenMW menu loaders."),
    "<none>": ("known-blocked", "extensionless-assets", "Extensionless entries need per-path ownership before runtime support can be claimed."),
}


INI_RUNTIME_KEYS = {
    "sIntroMovie",
    "sWelcomeScreen1",
    "sWelcomeScreen2",
    "sWelcomeScreen3",
    "sWelcomeScreen4",
    "sMainMenuBackground",
    "iSystemColorHUDMainRed",
    "iSystemColorHUDMainGreen",
    "iSystemColorHUDMainBlue",
    "bUsePipboyMode",
}


CONTENT_ITEM_FILES = [
    ("quests.json", "quest", "loaded-pending-runtime", "quest records are parsed; exhaustive quest execution remains gated"),
    (
        "dialogue.json",
        "dialogue-or-info",
        "loaded-pending-runtime",
        "dialogue rows are parsed/bridged; exhaustive INFO selection remains gated",
    ),
    ("scripts.json", "script", "loaded-pending-runtime", "scripts are harvested as source pending full script VM parity"),
    ("globals.json", "global", "runtime-supported", "globals are bridged into runtime global variables"),
    ("game-settings.json", "game-setting-record", "runtime-supported", "GMST values are bridged into runtime game settings"),
    (
        "gameplay-systems.json",
        "gameplay-system",
        "loaded-pending-runtime",
        "gameplay records are loaded/stored pending full gameplay binding",
    ),
    (
        "references.json",
        "form-reference",
        "loaded-pending-runtime",
        "form references are harvested pending exhaustive cross-reference resolution gates",
    ),
]


CONTENT_ARTIFACT_COUNTS = {
    "records.json": "pluginCount",
    "quests.json": "questCount",
    "dialogue.json": "dialogueRowCount",
    "scripts.json": "scriptCount",
    "globals.json": "globalCount",
    "game-settings.json": "gameSettingCount",
    "gameplay-systems.json": "gameplaySystemCount",
    "references.json": "referenceCount",
}


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256_file(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def rel_or_abs(path, root):
    try:
        return str(Path(path).resolve().relative_to(Path(root).resolve())).replace("\\", "/")
    except ValueError:
        return str(path)


def get_ext(entry):
    suffix = Path(entry).suffix.lower()
    return suffix if suffix else "<none>"


def parse_loader_coverage(repo_root):
    esm4_dir = repo_root / "components" / "esm4"
    loader_by_record = {}
    type_by_record = {}
    if esm4_dir.is_dir():
        for path in sorted(esm4_dir.glob("load*.hpp")):
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for i, line in enumerate(lines):
                match = re.search(r"sRecordId\s*=\s*(?:ESM::RecNameInts::|ESM::)REC_([A-Z0-9_]+)4", line)
                if not match:
                    continue
                ctype = ""
                for j in range(i, -1, -1):
                    type_match = re.search(r"^\s{4}struct\s+([A-Za-z0-9_]+)(?:\s|:|$)", lines[j])
                    if type_match:
                        ctype = type_match.group(1)
                        break
                if ctype:
                    record = match.group(1)
                    loader_by_record[record] = path
                    type_by_record[record] = ctype

    records_header = (repo_root / "components" / "esm4" / "records.hpp").read_text(
        encoding="utf-8", errors="replace"
    )
    store_header = (repo_root / "apps" / "openmw" / "mwworld" / "esmstore.hpp").read_text(
        encoding="utf-8", errors="replace"
    )
    store_cpp = (repo_root / "apps" / "openmw" / "mwworld" / "store.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    esmstore_cpp = (repo_root / "apps" / "openmw" / "mwworld" / "esmstore.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    raw_pending_records = set()
    raw_pending_start = esmstore_cpp.find("static bool isLoadedPendingEsm4Record")
    raw_pending_end = esmstore_cpp.find("static bool readLoadedPendingEsm4Record", raw_pending_start)
    if raw_pending_start >= 0 and raw_pending_end > raw_pending_start:
        raw_pending_source = esmstore_cpp[raw_pending_start:raw_pending_end]
        raw_pending_records = set(re.findall(r"case\s+ESM::REC_([A-Z0-9_]+)4\s*:", raw_pending_source))
    return {
        "loaderByRecord": loader_by_record,
        "typeByRecord": type_by_record,
        "rawPendingRecords": raw_pending_records,
        "recordsHeader": records_header,
        "storeHeader": store_header,
        "storeCpp": store_cpp,
        "esmstoreCpp": esmstore_cpp,
    }


def classify_record_type(record, coverage):
    if record in RUNTIME_SUPPORTED_RECORDS:
        return "runtime-supported", RUNTIME_SUPPORTED_RECORDS[record]
    if record in KNOWN_BLOCKED_RECORDS:
        return "known-blocked", KNOWN_BLOCKED_RECORDS[record]

    loader_path = coverage["loaderByRecord"].get(record)
    ctype = coverage["typeByRecord"].get(record, "")
    has_loader = loader_path is not None
    has_records_header = bool(loader_path and loader_path.name in coverage["recordsHeader"])
    has_store = bool(ctype and f"Store<ESM4::{ctype}>" in coverage["storeHeader"])
    has_instantiation = bool(ctype and f"TypedDynamicStore<ESM4::{ctype}" in coverage["storeCpp"])

    if has_loader and has_records_header and has_store and has_instantiation:
        return "loaded-pending-runtime", LOADED_PENDING_RECORDS.get(
            record, "record is loaded/stored but has no explicit full-runtime parity claim yet"
        )
    if record in LOADED_PENDING_RECORDS:
        if record in coverage["rawPendingRecords"]:
            return (
                "loaded-pending-runtime",
                LOADED_PENDING_RECORDS[record]
                + "; ESMStore raw-pending fallback consumes and inventories the record bytes",
            )
        return (
            "known-blocked",
            "declared loaded-pending intent exists, but no typed loader/store or raw-pending runtime fallback is present",
        )
    return "known-blocked", "no complete loader/store/runtime claim is present for this ESM4 record type"


def make_row(scope, item_type, identifier, classification, proof, **extra):
    if classification not in ALLOWED_CLASSIFICATIONS:
        raise ValueError(f"Invalid classification {classification} for {scope}:{identifier}")
    row = {
        "scope": scope,
        "itemType": item_type,
        "identifier": identifier,
        "classification": classification,
        "proof": proof,
    }
    row.update(extra)
    return row


def record_failure(failures, code, message, **extra):
    row = {"code": code, "message": message}
    row.update(extra)
    failures.append(row)


def add_main_row(rows, scope, item_type, identifier, classification, proof, **extra):
    rows.append(make_row(scope, item_type, identifier, classification, proof, **extra))


def classify_content_items(content_dir, content_jsonl, counters, failures):
    with content_jsonl.open("w", encoding="utf-8") as stream:
        for filename, item_type, classification, proof in CONTENT_ITEM_FILES:
            path = content_dir / filename
            if not path.is_file():
                record_failure(
                    failures,
                    "missing-content-classification-artifact",
                    f"Expected content artifact is missing: {filename}",
                    sourceFile=filename,
                )
                continue
            values = read_json(path)
            if not isinstance(values, list):
                values = [values]
            for value in values:
                plugin = value.get("plugin", "")
                record_type = value.get("recordType", "")
                form_id = value.get("formId", "")
                editor_id = value.get("editorId", "")
                identifier_parts = [part for part in (plugin, record_type, form_id, editor_id) if part]
                identifier = ":".join(identifier_parts) or f"{filename}:{counters['contentRows']}"
                row = make_row(
                    "content-ledger",
                    item_type,
                    identifier,
                    classification,
                    proof,
                    sourceFile=filename,
                    plugin=plugin,
                    recordType=record_type,
                    formId=form_id,
                    editorId=editor_id,
                )
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")
                counters["contentRows"] += 1
                counters[f"class:{classification}"] += 1


def validate_content_artifacts(content_dir, content_result, content_records, failures):
    coverage = {
        "expectedArtifacts": sorted(CONTENT_ARTIFACT_COUNTS),
        "checkedCounts": {},
        "missingArtifacts": [],
    }
    for filename, count_key in CONTENT_ARTIFACT_COUNTS.items():
        path = content_dir / filename
        if not path.is_file():
            coverage["missingArtifacts"].append(filename)
            record_failure(
                failures,
                "missing-content-artifact",
                f"Expected content ledger artifact is missing: {filename}",
                sourceFile=filename,
            )
            continue
        values = read_json(path)
        if not isinstance(values, list):
            record_failure(
                failures,
                "invalid-content-artifact-shape",
                f"Expected content artifact to be a JSON list: {filename}",
                sourceFile=filename,
            )
            continue
        actual = len(values)
        expected = int(content_result.get(count_key, -1))
        coverage["checkedCounts"][filename] = {"actual": actual, "expected": expected, "countKey": count_key}
        if actual != expected:
            record_failure(
                failures,
                "content-artifact-count-mismatch",
                f"Content artifact {filename} row count {actual} does not match result {count_key}={expected}",
                sourceFile=filename,
                actual=actual,
                expected=expected,
            )

    record_plugin_count = len(content_records) if isinstance(content_records, list) else -1
    result_plugin_count = int(content_result.get("pluginCount", -1))
    coverage["recordPluginCount"] = {"actual": record_plugin_count, "expected": result_plugin_count}
    if record_plugin_count != result_plugin_count:
        record_failure(
            failures,
            "records-plugin-count-mismatch",
            f"records.json plugin count {record_plugin_count} does not match result pluginCount={result_plugin_count}",
            actual=record_plugin_count,
            expected=result_plugin_count,
        )
    return coverage


def validate_content_plugin_coverage(main_rows, plugins, content_result, content_records, failures):
    harvested_plugins = {plugin.get("name", "") for plugin in plugins if plugin.get("name", "")}
    content_plugins = set(content_result.get("content", []))
    record_plugins = {plugin.get("plugin", "") for plugin in content_records if plugin.get("plugin", "")}
    coverage = {
        "harvestedPlugins": sorted(harvested_plugins),
        "contentPlugins": sorted(content_plugins),
        "recordPlugins": sorted(record_plugins),
        "explicitExclusions": [],
    }

    for plugin in sorted(content_plugins):
        if plugin not in harvested_plugins:
            record_failure(
                failures,
                "content-plugin-not-harvested",
                f"Content ledger includes plugin not present in harvest metadata: {plugin}",
                plugin=plugin,
            )
        if plugin not in record_plugins:
            record_failure(
                failures,
                "content-plugin-missing-records",
                f"Content ledger declared plugin but records.json has no row: {plugin}",
                plugin=plugin,
            )

    for plugin in sorted(record_plugins - content_plugins):
        record_failure(
            failures,
            "records-plugin-not-declared",
            f"records.json includes plugin not declared in content-ledger result content list: {plugin}",
            plugin=plugin,
        )

    for plugin in sorted(harvested_plugins - record_plugins):
        if plugin.lower() == "fnvr.esp" and plugin not in content_plugins:
            coverage["explicitExclusions"].append(plugin)
            add_main_row(
                main_rows,
                "content-record-coverage",
                "pcvr-layer-plugin-records",
                plugin,
                "intentionally-excluded-with-proof",
                "FNVR.esp is harvested as the PCVR overlay, but the current content ledger is the PC-flat-first 10-plugin layer; PCVR record execution remains a separate PCVR gate.",
                priority="pcvr-second",
                recordRowsPresent=False,
            )
            continue
        record_failure(
            failures,
            "harvested-plugin-missing-records",
            f"Harvested ESM/ESP has no records.json row and no explicit exclusion: {plugin}",
            plugin=plugin,
        )

    for plugin in sorted(record_plugins):
        add_main_row(
            main_rows,
            "content-record-coverage",
            "parsed-plugin-records",
            plugin,
            "runtime-supported",
            "Content ledger records.json contains a parsed record-count row for this active plugin layer.",
            recordRowsPresent=True,
        )
    return coverage


def validate_archive_entry_coverage(harvest_dir, manifest, archives, archive_entry_ledger, failures):
    archive_names = {archive.get("name", "") for archive in archives if archive.get("name", "")}
    entry_names = {entry.get("archive", "") for entry in archive_entry_ledger if entry.get("archive", "")}
    coverage = {
        "archiveCount": len(archives),
        "entryListCount": len(archive_entry_ledger),
        "archiveNames": sorted(archive_names),
        "entryListArchives": sorted(entry_names),
        "checkedEntryLists": [],
        "unknownExtensions": [],
    }

    manifest_counts = manifest.get("counts", {})
    expected_archive_count = int(manifest_counts.get("archives", -1))
    expected_entry_lists = int(manifest_counts.get("archiveEntryLists", -1))
    if len(archives) != expected_archive_count:
        record_failure(
            failures,
            "archive-count-mismatch",
            f"archives-metadata count {len(archives)} does not match manifest archives={expected_archive_count}",
            actual=len(archives),
            expected=expected_archive_count,
        )
    if len(archive_entry_ledger) != expected_entry_lists:
        record_failure(
            failures,
            "archive-entry-list-count-mismatch",
            f"archive-entry-ledger count {len(archive_entry_ledger)} does not match manifest archiveEntryLists={expected_entry_lists}",
            actual=len(archive_entry_ledger),
            expected=expected_entry_lists,
        )
    if archive_names != entry_names:
        missing = sorted(archive_names - entry_names)
        extra = sorted(entry_names - archive_names)
        record_failure(
            failures,
            "archive-entry-list-coverage-mismatch",
            "Archive entry list coverage does not match archive metadata.",
            missingEntryLists=missing,
            extraEntryLists=extra,
        )

    for archive_entry in archive_entry_ledger:
        list_path = harvest_dir / archive_entry["entryList"]
        if not list_path.is_file():
            record_failure(
                failures,
                "missing-archive-entry-list",
                f"Missing BSA entry list: {archive_entry['entryList']}",
                archive=archive_entry.get("archive", ""),
            )
            continue
        lines = [line for line in list_path.read_text(encoding="utf-8", errors="replace").splitlines() if line]
        expected_count = int(archive_entry.get("entryCount", -1))
        coverage["checkedEntryLists"].append(
            {"archive": archive_entry.get("archive", ""), "actual": len(lines), "expected": expected_count}
        )
        if expected_count <= 0 or len(lines) <= 0:
            record_failure(
                failures,
                "empty-archive-entry-list",
                f"BSA entry list is empty for archive {archive_entry.get('archive', '')}",
                archive=archive_entry.get("archive", ""),
                entryList=archive_entry.get("entryList", ""),
            )
        if len(lines) != expected_count:
            record_failure(
                failures,
                "archive-entry-list-row-count-mismatch",
                f"BSA entry list row count {len(lines)} does not match ledger entryCount={expected_count}",
                archive=archive_entry.get("archive", ""),
                entryList=archive_entry.get("entryList", ""),
                actual=len(lines),
                expected=expected_count,
            )
        for entry in lines:
            ext = get_ext(entry)
            if ext not in EXTENSION_RULES:
                coverage["unknownExtensions"].append(ext)
    coverage["unknownExtensions"] = sorted(set(coverage["unknownExtensions"]))
    for ext in coverage["unknownExtensions"]:
        record_failure(
            failures,
            "unknown-bsa-entry-extension",
            f"BSA entry extension lacks an explicit five-state classification rule: {ext}",
            extension=ext,
        )
    return coverage


def write_generated_output_classification(proof_dir, harvest_dir, content_dir, counters, planned_files=None):
    output = proof_dir / "generated-output-classification.jsonl"
    generated_files = set()
    for root in (harvest_dir, content_dir, proof_dir):
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file():
                generated_files.add(path)
    for path in planned_files or []:
        generated_files.add(Path(path))
    generated_files.add(output)
    with output.open("w", encoding="utf-8") as stream:
        for path in sorted(generated_files):
            size = path.stat().st_size if path.is_file() else None
            row = make_row(
                "generated-proof-output",
                "generated-file",
                rel_or_abs(path, proof_dir.parent),
                "non-runtime-support-file",
                "Generated proof/config/log output is evidence only and is not a retail runtime asset payload.",
                extension=get_ext(path.name),
                bytes=size,
            )
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
            counters["generatedOutputRows"] += 1
            counters["class:non-runtime-support-file"] += 1
    return output


def classify_runtime_profiles(rows, repo_root, pcvr_reference_config_dir):
    flat_runner = repo_root / "scripts" / "nikami" / "run-fnv-flat.ps1"
    flat_proof = repo_root / "scripts" / "nikami" / "run-fnv-flat-proof.ps1"
    headset_deploy = repo_root / "scripts" / "nikami" / "deploy-fnv-vr-headset.ps1"

    flat_text = flat_runner.read_text(encoding="utf-8", errors="replace") if flat_runner.is_file() else ""
    if (
        'data=$((Join-Path $Resources "vfs-mw").Replace("\\", "/"))' in flat_text
        and 'data=$($FnvData.Replace("\\", "/"))' in flat_text
        and "$OptionalDataLine" in flat_text
    ):
        add_main_row(
            rows,
            "runtime-config",
            "pc-flat-generated-openmw-cfg",
            "scripts/nikami/run-fnv-flat.ps1",
            "runtime-supported",
            "PC flat generator emits resources/vfs-mw, retail FNV Data, then optional overlay; OpenMW VFS last data dir wins.",
            priority="pc-flat-first",
        )
    else:
        add_main_row(
            rows,
            "runtime-config",
            "pc-flat-generated-openmw-cfg",
            "scripts/nikami/run-fnv-flat.ps1",
            "known-blocked",
            "PC flat data ordering anchors are missing from the generator.",
            priority="pc-flat-first",
        )

    flat_proof_text = flat_proof.read_text(encoding="utf-8", errors="replace") if flat_proof.is_file() else ""
    has_classified_skip_gate = (
        "Assert-UnsupportedEsm4SkipsClassified" in flat_proof_text
        and "Get-Esm4ClassificationMap" in flat_proof_text
    )
    add_main_row(
        rows,
        "runtime-config",
        "pc-flat-classified-esm4-skip-gate",
        "scripts/nikami/run-fnv-flat-proof.ps1",
        "runtime-supported" if has_classified_skip_gate else "known-blocked",
        "Flat proof checks skipped ESM4 record types against the no-silent-skip classification ledger and fails unclassified or mismatched skips."
        if has_classified_skip_gate
        else "Flat proof does not classify unsupported ESM4 record skips yet.",
        priority="pc-flat-first",
    )

    pcvr_runner = repo_root / "scripts/nikami/run-fnv-pcvr-proof.ps1"
    pcvr_runner_contract = repo_root / "scripts/nikami/test-fnv-pcvr-publish-runner-contract.ps1"
    pcvr_runner_text = pcvr_runner.read_text(encoding="utf-8", errors="replace") if pcvr_runner.is_file() else ""
    has_pcvr_runner = (
        pcvr_runner.is_file()
        and pcvr_runner_contract.is_file()
        and "Runtime mode: pcvr" in pcvr_runner_text
        and "openmw_vr.exe" in pcvr_runner_text
        and "content=FNVR.esp" in pcvr_runner_text
        and "force shaders = true" in pcvr_runner_text
        and "stereo enabled = true" in pcvr_runner_text
        and "GenerateOnly" in pcvr_runner_text
    )
    add_main_row(
        rows,
        "runtime-config",
        "pcvr-publish-runner",
        "scripts/nikami/run-fnv-pcvr-proof.ps1",
        "loaded-pending-runtime" if has_pcvr_runner else "known-blocked",
        "Publish-tree PCVR runner generates an openmw_vr.exe profile with FNVR.esp last; OpenXR runtime execution remains a separate hardware proof."
        if has_pcvr_runner
        else "No publish-tree PCVR proof runner is present yet; D:/Modlists/fnv/run_vr.bat proves an external build, not this repo build.",
        priority="pcvr-second",
    )

    if pcvr_reference_config_dir:
        pcvr_dir = Path(pcvr_reference_config_dir)
        pcvr_cfg = pcvr_dir / "openmw.cfg"
        if pcvr_cfg.is_file():
            lines = pcvr_cfg.read_text(encoding="utf-8", errors="replace").splitlines()
            data_lines = [line.strip() for line in lines if line.strip().lower().startswith("data=")]
            content_lines = [line.strip() for line in lines if line.strip().lower().startswith("content=")]
            add_main_row(
                rows,
                "runtime-config",
                "pcvr-reference-modlist-config",
                str(pcvr_cfg),
                "loaded-pending-runtime",
                "Reference PCVR config is harvested as external evidence only; publish PCVR must generate its own gated profile.",
                priority="pcvr-second",
                dataLines=data_lines,
                contentLines=content_lines,
                fnvrIsLast=bool(content_lines and content_lines[-1].lower() == "content=fnvr.esp"),
            )
        else:
            add_main_row(
                rows,
                "runtime-config",
                "pcvr-reference-modlist-config",
                str(pcvr_cfg),
                "known-blocked",
                "Expected PCVR reference openmw.cfg was not found.",
                priority="pcvr-second",
            )

        run_vr = pcvr_dir.parent / "run_vr.bat"
        if run_vr.is_file():
            text = run_vr.read_text(encoding="utf-8", errors="replace")
            add_main_row(
                rows,
                "runtime-config",
                "pcvr-reference-runner",
                str(run_vr),
                "loaded-pending-runtime",
                "Reference runner exists but targets the D:/Modlists/fnv build; publish parity requires a publish-runner gate.",
                priority="pcvr-second",
                mentionsOpenmwVr="openmw_vr.exe" in text.lower(),
            )

    if headset_deploy.is_file():
        add_main_row(
            rows,
            "runtime-config",
            "android-headset-deploy",
            "scripts/nikami/deploy-fnv-vr-headset.ps1",
            "intentionally-excluded-with-proof",
            "Android/headset deployment is explicitly out of the current PC-flat-first and PCVR-second classification lane.",
            priority="android-last",
        )


def main():
    parser = argparse.ArgumentParser(description="Build a five-state FNV no-silent-skip classification proof.")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--proof-root", required=True)
    parser.add_argument("--harvest-dir", required=True)
    parser.add_argument("--content-ledger-dir", required=True)
    parser.add_argument("--pcvr-reference-config-dir", default="")
    parser.add_argument("--fail-known-blocked", action="store_true")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    proof_root = Path(args.proof_root).resolve()
    harvest_dir = Path(args.harvest_dir).resolve()
    content_dir = Path(args.content_ledger_dir).resolve()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    proof_dir = proof_root / "fnv-no-silent-skip-classification" / stamp
    proof_dir.mkdir(parents=True, exist_ok=True)
    summary_file = proof_dir / "summary.txt"

    def proof_line(text=""):
        print(text, flush=True)
        with summary_file.open("a", encoding="utf-8") as stream:
            stream.write(text + "\n")

    coverage = parse_loader_coverage(repo_root)
    main_rows = []
    counters = Counter()
    failures = []

    manifest = read_json(harvest_dir / "manifest.json")
    plugins = read_json(harvest_dir / "plugins-metadata.json")
    archives = read_json(harvest_dir / "archives-metadata.json")
    ini_shapes = read_json(harvest_dir / "ini-shape-ledger.json")
    archive_entry_ledger = read_json(harvest_dir / "archive-entry-ledger.json")
    content_records = read_json(content_dir / "records.json")
    content_result = read_json(content_dir / "result.json")

    content_artifact_coverage = validate_content_artifacts(content_dir, content_result, content_records, failures)
    content_plugin_coverage = validate_content_plugin_coverage(
        main_rows, plugins, content_result, content_records, failures
    )
    archive_entry_coverage = validate_archive_entry_coverage(
        harvest_dir, manifest, archives, archive_entry_ledger, failures
    )

    for plugin in plugins:
        add_main_row(
            main_rows,
            "retail-plugin",
            "esm-esp-file",
            plugin["name"],
            "runtime-supported",
            "ESM/ESP file metadata is harvested and bytes are routed through the ESM4 Reader/ESMStore without copying payloads.",
            bytes=plugin.get("bytes"),
            sha256=plugin.get("sha256"),
        )

    for archive in archives:
        add_main_row(
            main_rows,
            "retail-archive",
            "bsa-file",
            archive["name"],
            "runtime-supported",
            "BSA metadata is harvested and archive bytes are registered/decompressed through the VFS without copying payloads.",
            bytes=archive.get("bytes"),
            sha256=archive.get("sha256"),
        )

    for ini in ini_shapes:
        for section in ini.get("sections", []):
            for key in section.get("keys", []):
                classification = "runtime-supported" if key in INI_RUNTIME_KEYS else "loaded-pending-runtime"
                proof = (
                    "Known FNV INI key is consumed by generated config/menu/video runtime hooks."
                    if classification == "runtime-supported"
                    else "INI key shape is harvested; exact runtime use remains pending without storing the INI payload."
                )
                add_main_row(
                    main_rows,
                    "retail-ini",
                    "ini-setting",
                    f"{ini['name']}:{section.get('section', '')}:{key}",
                    classification,
                    proof,
                    ini=ini["name"],
                    section=section.get("section", ""),
                    key=key,
                )

    record_type_totals = Counter()
    for plugin in content_records:
        plugin_name = plugin.get("plugin", "")
        for record in plugin.get("records", []):
            record_type = record["type"]
            count = int(record["count"])
            classification, proof = classify_record_type(record_type, coverage)
            record_type_totals[record_type] += count
            add_main_row(
                main_rows,
                "content-records",
                "esm4-record-type",
                f"{plugin_name}:{record_type}",
                classification,
                proof,
                plugin=plugin_name,
                recordType=record_type,
                count=count,
            )

    archive_extension_totals = Counter()
    for archive_entry in archive_entry_ledger:
        list_path = harvest_dir / archive_entry["entryList"]
        for entry in list_path.read_text(encoding="utf-8", errors="replace").splitlines():
            archive_extension_totals[get_ext(entry)] += 1

    for ext in sorted(archive_extension_totals):
        if ext not in EXTENSION_RULES:
            classification, subsystem, proof = (
                "known-blocked",
                "unclassified-extension",
                "Extension is not in the asset classification map; this is a failing gate until explicitly classified.",
            )
            record_failure(
                failures,
                "unknown-bsa-entry-extension",
                f"BSA entry extension lacks an explicit five-state classification rule: {ext}",
                extension=ext,
            )
        else:
            classification, subsystem, proof = EXTENSION_RULES[ext]
        add_main_row(
            main_rows,
            "asset-type",
            "bsa-entry-extension",
            ext,
            classification,
            proof,
            subsystem=subsystem,
            count=archive_extension_totals[ext],
        )

    entry_jsonl = proof_dir / "archive-entry-classification.jsonl"
    with entry_jsonl.open("w", encoding="utf-8") as stream:
        for archive_entry in archive_entry_ledger:
            list_path = harvest_dir / archive_entry["entryList"]
            archive = archive_entry["archive"]
            for entry in list_path.read_text(encoding="utf-8", errors="replace").splitlines():
                ext = get_ext(entry)
                if ext not in EXTENSION_RULES:
                    classification, subsystem, proof = (
                        "known-blocked",
                        "unclassified-extension",
                        "Extension is not in the asset classification map; this is a failing gate until explicitly classified.",
                    )
                else:
                    classification, subsystem, proof = EXTENSION_RULES[ext]
                row = make_row(
                    "archive-entry",
                    "bsa-entry",
                    f"{archive}:{entry}",
                    classification,
                    proof,
                    archive=archive,
                    entry=entry,
                    extension=ext,
                    subsystem=subsystem,
                )
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")
                counters["archiveEntryRows"] += 1
                counters[f"class:{classification}"] += 1

    content_jsonl = proof_dir / "content-item-classification.jsonl"
    classify_content_items(content_dir, content_jsonl, counters, failures)
    classify_runtime_profiles(main_rows, repo_root, args.pcvr_reference_config_dir)

    for row in main_rows:
        counters["mainRows"] += 1
        counters[f"class:{row['classification']}"] += 1

    unclassified = [row for row in main_rows if row["classification"] not in ALLOWED_CLASSIFICATIONS]
    known_blocked_count = counters["class:known-blocked"]
    generated_output_jsonl = write_generated_output_classification(
        proof_dir,
        harvest_dir,
        content_dir,
        counters,
        planned_files=[
            proof_dir / "classification-ledger.json",
            proof_dir / "result.json",
            summary_file,
        ],
    )
    result = {
        "status": "PASS",
        "stamp": stamp,
        "repoRoot": str(repo_root),
        "harvestDir": str(harvest_dir),
        "contentLedgerDir": str(content_dir),
        "proofDir": str(proof_dir),
        "allowedClassifications": sorted(ALLOWED_CLASSIFICATIONS),
        "counts": {
            "mainRows": counters["mainRows"],
            "archiveEntryRows": counters["archiveEntryRows"],
            "contentRows": counters["contentRows"],
            "generatedOutputRows": counters["generatedOutputRows"],
            "runtimeSupported": counters["class:runtime-supported"],
            "loadedPendingRuntime": counters["class:loaded-pending-runtime"],
            "knownBlocked": counters["class:known-blocked"],
            "nonRuntimeSupportFile": counters["class:non-runtime-support-file"],
            "intentionallyExcludedWithProof": counters["class:intentionally-excluded-with-proof"],
            "unclassified": len(unclassified),
        },
        "recordTypeTotals": dict(sorted(record_type_totals.items())),
        "artifacts": {
            "mainLedger": str(proof_dir / "classification-ledger.json"),
            "archiveEntryClassification": str(entry_jsonl),
            "contentItemClassification": str(content_jsonl),
            "generatedOutputClassification": str(generated_output_jsonl),
            "summary": str(summary_file),
            "result": str(proof_dir / "result.json"),
        },
        "harvestManifestCounts": manifest.get("counts", {}),
        "archiveExtensionTotals": dict(sorted(archive_extension_totals.items())),
        "contentArtifactCoverage": content_artifact_coverage,
        "contentPluginCoverage": content_plugin_coverage,
        "archiveEntryCoverage": archive_entry_coverage,
        "preflightFailures": failures,
    }

    if unclassified or failures:
        result["status"] = "FAIL"
        if unclassified:
            result["unclassifiedRows"] = unclassified
    if args.fail_known_blocked and known_blocked_count:
        result["status"] = "FAIL"
        result["knownBlockedFailure"] = True

    write_json(proof_dir / "classification-ledger.json", main_rows)
    write_json(proof_dir / "result.json", result)

    proof_line(f"FNV no-silent-skip classification {stamp}")
    proof_line(f"RepoRoot: {repo_root}")
    proof_line(f"HarvestDir: {harvest_dir}")
    proof_line(f"ContentLedgerDir: {content_dir}")
    proof_line(f"ProofDir: {proof_dir}")
    proof_line("")
    for key, value in result["counts"].items():
        proof_line(f"{key}: {value}")
    proof_line(f"Classification ledger: {proof_dir / 'classification-ledger.json'}")
    proof_line(f"Archive entry classification: {entry_jsonl}")
    proof_line(f"Content item classification: {content_jsonl}")

    if result["status"] != "PASS":
        raise SystemExit("FNV no-silent-skip classification failed; see " + str(summary_file))
    proof_line("")
    proof_line("FNV no-silent-skip classification PASS")


if __name__ == "__main__":
    main()
