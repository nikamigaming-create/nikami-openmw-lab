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
    "DIAL": "dialogue topics are bridged into runtime dialogue stores",
    "GMST": "game settings are bridged into runtime settings",
    "GLOB": "globals are bridged into runtime variables",
    "INFO": "dialogue INFO rows are bridged into runtime dialogue responses",
    "LAND": "land records feed terrain",
    "LTEX": "land textures feed terrain layers",
    "MSTT": "movable statics are loaded and placed",
    "NOTE": "notes are stored for runtime UI/data access",
    "NPC_": "NPC records feed actor shells, rendering, and animation",
    "QUST": "quest records are loaded for journal/dialogue bridge work",
    "REFR": "placed object references are loaded into cells",
    "SCOL": "static collections are placed into the world",
    "SCPT": "script source is bridged as source/runtime evidence",
    "SNDR": "sound reference records feed sound lookup",
    "SOUN": "sound records feed sound lookup",
    "STAT": "static records feed world geometry",
    "TACT": "talking activators feed radio/runtime activator paths",
    "TES4": "plugin header is consumed by the ESM4 reader",
    "TXST": "texture sets feed material/texture resolution",
    "WEAP": "weapon records feed inventory, actor, and HUD ammo paths",
}


KNOWN_BLOCKED_RECORDS = {
    "CLMT": "FNV climate records are parsed only as metadata; weather runtime does not consume them yet",
    "REGN": "FNV region weather/audio data is incomplete; RDWT weather data is skipped",
    "WTHR": "FNV weather records are not bridged into the weather manager yet",
    "IMGS": "image space records are not connected to runtime post-processing yet",
}


LOADED_PENDING_RECORDS = {
    "ACTI": "loader/store exists; object-class/runtime behavior still needs targeted FNV parity gates",
    "ALCH": "loader/store exists; gameplay effects are pending full FNV parity gates",
    "ARMA": "armor addon data is loaded; full equipment/body binding remains gated separately",
    "ARMO": "armor data is loaded; full equipment/body binding remains gated separately",
    "ASPC": "acoustic space data is loaded pending runtime audio-space binding",
    "BOOK": "book records are loaded pending full UI/readable-content parity",
    "BPTD": "body part data is loaded pending dismemberment/body-part runtime parity",
    "CLAS": "class records are loaded pending player/NPC gameplay parity",
    "CLOT": "clothing records are loaded pending full equipment/body binding",
    "CONT": "container records are loaded pending full inventory/container behavior parity",
    "CSTY": "combat style data is known but combat tuning parity remains pending",
    "DOOR": "door records are loaded pending full activation/lock/teleport parity",
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
    "IDLE": "idle animation records are loaded pending complete package/idle runtime parity",
    "IDLM": "idle marker records are loaded pending complete idle-marker runtime parity",
    "IMAD": "image-space modifier records are loaded pending full post-process binding",
    "IMOD": "item mod records are loaded pending full weapon-mod gameplay binding",
    "INGR": "ingredient records are loaded pending full effects/crafting parity",
    "IPCT": "impact data records are loaded pending full projectile/impact binding",
    "IPDS": "impact data sets are loaded pending full projectile/impact binding",
    "KEYM": "key records are loaded pending full lock/key gameplay parity",
    "LGTM": "lighting templates are loaded pending full interior lighting parity",
    "LIGH": "light records are loaded pending full lighting behavior parity",
    "LSCR": "load screen records are known pending full load-screen selection parity",
    "LVLC": "leveled creature records are loaded pending full leveled-spawn parity",
    "LVLI": "leveled item records are loaded pending full leveled-list parity",
    "LVLN": "leveled actor records are loaded pending full leveled-actor parity",
    "MESG": "message records are known pending message/UI parity",
    "MGEF": "magic effect records are known pending full effect runtime parity",
    "MISC": "misc item records are loaded pending complete item behavior parity",
    "MSET": "media set records are loaded pending full media selection parity",
    "MUSC": "music type records are loaded pending full radio/music selection parity",
    "NAVI": "navigation master data is loaded pending complete navmesh parity gates",
    "NAVM": "navmesh records are loaded pending complete navmesh parity gates",
    "PACK": "AI packages are loaded and partially acted on pending exhaustive package parity",
    "PERK": "perk/trait records are source-backed and stored pending player progression binding",
    "PGRE": "placed grenade records are loaded pending projectile/explosive runtime parity",
    "PROJ": "projectile records are source-backed and stored pending runtime projectile binding",
    "PWAT": "placeable water records are loaded pending full water placement parity",
    "RACE": "race records feed actors pending full character creation/body parity",
    "SCEN": "scene records are known pending full scene runtime parity",
    "SPEL": "spell records are known pending full effects/runtime parity",
    "TERM": "terminal records are loaded pending full terminal UI/script parity",
    "TREE": "tree records render through the current SpeedTree billboard fallback",
    "VTYP": "voice type records are loaded pending exhaustive voice selection parity",
    "WATR": "water records are loaded pending full water shader/behavior parity",
    "WRLD": "world records are loaded pending full climate/weather/world runtime parity",
}


EXTENSION_RULES = {
    ".bik": ("runtime-supported", "video-menu-intro", "VideoWidget plays BIK menu/intro movies through VFS."),
    ".ctl": ("loaded-pending-runtime", "facegen-control-basis", "CTL bytes are read and validated; full FaceGen control semantics remain pending."),
    ".dat": ("non-runtime-support-file", "lip-generation-support-tables", "LSDATA tables support lip generation tooling; runtime consumes baked LIP sidecars."),
    ".dds": ("runtime-supported", "textures-ui-world", "DDS files are consumed by ImageManager/MyGUI/material texture binding."),
    ".dlodsettings": ("known-blocked", "distant-lod", "DLOD settings are harvested but not consumed by terrain/object paging yet."),
    ".egm": ("runtime-supported", "facegen-morphs", "EGM morphs are loaded and applied to NPC geometry."),
    ".egt": ("loaded-pending-runtime", "facegen-tint-maps", "EGT bytes are read and tint is approximated; exact texture synthesis remains pending."),
    ".fnt": ("runtime-supported", "bitmap-fonts", "Bitmap font metadata is consumed by FontLoader."),
    ".kf": ("runtime-supported", "animation-keyframes", "KF animation sources are attached to actor/creature animation."),
    ".lip": ("loaded-pending-runtime", "voice-lip-sync", "LIP sidecars feed a mouth envelope; exact phoneme mapping remains pending."),
    ".mp3": ("runtime-supported", "music-radio", "MP3 files are streamed by the sound/music manager."),
    ".nif": ("runtime-supported", "scene-mesh-collision", "NIF files are loaded for render nodes and collision users."),
    ".ogg": ("runtime-supported", "sound-voice", "OGG files are decoded by the sound manager."),
    ".psa": ("known-blocked", "actor-deathpose-animation", "PSA death-pose runtime playback is not wired yet."),
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
    raw_pending_records = set()
    raw_pending_start = store_cpp.find("static bool isLoadedPendingEsm4Record")
    raw_pending_end = store_cpp.find("static bool readLoadedPendingEsm4Record", raw_pending_start)
    if raw_pending_start >= 0 and raw_pending_end > raw_pending_start:
        raw_pending_source = store_cpp[raw_pending_start:raw_pending_end]
        raw_pending_records = set(re.findall(r"case\s+ESM::REC_([A-Z0-9_]+)4\s*:", raw_pending_source))
    return {
        "loaderByRecord": loader_by_record,
        "typeByRecord": type_by_record,
        "rawPendingRecords": raw_pending_records,
        "recordsHeader": records_header,
        "storeHeader": store_header,
        "storeCpp": store_cpp,
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


def add_main_row(rows, scope, item_type, identifier, classification, proof, **extra):
    rows.append(make_row(scope, item_type, identifier, classification, proof, **extra))


def classify_content_items(content_dir, content_jsonl, counters):
    files = [
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
    with content_jsonl.open("w", encoding="utf-8") as stream:
        for filename, item_type, classification, proof in files:
            path = content_dir / filename
            if not path.is_file():
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

    add_main_row(
        rows,
        "runtime-config",
        "pcvr-publish-runner",
        "scripts/nikami/run-fnv-pcvr-proof.ps1",
        "known-blocked",
        "No publish-tree PCVR proof runner is present yet; D:/Modlists/fnv/run_vr.bat proves an external build, not this repo build.",
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

    manifest = read_json(harvest_dir / "manifest.json")
    plugins = read_json(harvest_dir / "plugins-metadata.json")
    archives = read_json(harvest_dir / "archives-metadata.json")
    ini_shapes = read_json(harvest_dir / "ini-shape-ledger.json")
    archive_entry_ledger = read_json(harvest_dir / "archive-entry-ledger.json")
    content_records = read_json(content_dir / "records.json")

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

    for ext, (classification, subsystem, proof) in EXTENSION_RULES.items():
        add_main_row(
            main_rows,
            "asset-type",
            "bsa-entry-extension",
            ext,
            classification,
            proof,
            subsystem=subsystem,
        )

    entry_jsonl = proof_dir / "archive-entry-classification.jsonl"
    with entry_jsonl.open("w", encoding="utf-8") as stream:
        for archive_entry in archive_entry_ledger:
            list_path = harvest_dir / archive_entry["entryList"]
            archive = archive_entry["archive"]
            for entry in list_path.read_text(encoding="utf-8", errors="replace").splitlines():
                ext = get_ext(entry)
                classification, subsystem, proof = EXTENSION_RULES.get(
                    ext,
                    ("known-blocked", "unclassified-extension", "Extension is not in the asset classification map."),
                )
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
    classify_content_items(content_dir, content_jsonl, counters)
    classify_runtime_profiles(main_rows, repo_root, args.pcvr_reference_config_dir)

    for row in main_rows:
        counters["mainRows"] += 1
        counters[f"class:{row['classification']}"] += 1

    unclassified = [row for row in main_rows if row["classification"] not in ALLOWED_CLASSIFICATIONS]
    known_blocked_count = counters["class:known-blocked"]
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
            "summary": str(summary_file),
            "result": str(proof_dir / "result.json"),
        },
        "harvestManifestCounts": manifest.get("counts", {}),
    }

    if unclassified:
        result["status"] = "FAIL"
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
