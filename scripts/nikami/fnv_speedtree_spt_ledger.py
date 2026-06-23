#!/usr/bin/env python3
import argparse
import json
import os
import struct
from datetime import datetime
from pathlib import Path

from fnv_content_ledger import first, first_zstring, fourcc, get_float, hex_form, read_subrecords


DEFAULT_RETAIL_CONTENT = [
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

EXPECTED_SPT_PATHS = [
    "trees\\euonymusbush01.spt",
    "trees\\oasiselm01.spt",
    "trees\\oasiselm02.spt",
    "trees\\oasistreetop01.spt",
    "trees\\pine01.spt",
    "trees\\sugarmaple01.spt",
    "trees\\sycamore01.spt",
    "trees\\wastelandshrub01.spt",
    "trees\\wastelandundergrowth01.spt",
    "trees\\whiteoak01.spt",
]


def normalize_asset_path(value):
    return value.replace("/", "\\").lower().strip("\\")


def resolve_spt_archive_path(model, harvest_paths):
    if model in harvest_paths:
        return model
    filename = model.rsplit("\\", 1)[-1]
    trees_path = f"trees\\{filename}"
    if trees_path in harvest_paths:
        return trees_path
    return ""


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def collect_harvest_spt_paths(harvest_dir):
    entry_root = harvest_dir / "bsa-entry-lists"
    if not entry_root.is_dir():
        raise SystemExit(f"Missing harvest BSA entry lists: {entry_root}")

    paths = []
    for entry_list in sorted(entry_root.glob("*.entries.txt")):
        for line in entry_list.read_text(encoding="utf-8").splitlines():
            path = normalize_asset_path(line)
            if path.endswith(".spt"):
                paths.append({"archiveList": entry_list.name, "path": path})
    return paths


def parse_tree_records(path, plugin):
    data = path.read_bytes()
    trees = []

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
                        f"Invalid GRUP range in {path}: start={record_start} size={size} "
                        f"end={group_end} limit={end}"
                    )
                offset = read_range(offset, group_end)
                continue

            next_offset = offset + size
            if next_offset < offset or next_offset > end:
                raise ValueError(
                    f"Invalid ESM4 record range in {path}: type={rec_type} start={record_start} "
                    f"size={size} next={next_offset} limit={end}"
                )

            if rec_type == "TREE":
                subrecords = read_subrecords(data[offset:next_offset])
                model = first_zstring(subrecords, "MODL")
                normalized_model = normalize_asset_path(model)
                leaf_texture = first_zstring(subrecords, "ICON")
                modb = first(subrecords, "MODB")
                trees.append(
                    {
                        "plugin": plugin,
                        "formId": hex_form(form_id),
                        "flags": f"0x{flags:08x}",
                        "editorId": first_zstring(subrecords, "EDID"),
                        "model": model,
                        "normalizedModel": normalized_model,
                        "legacyObjectPagingPath": normalize_asset_path(f"meshes\\{normalized_model}")
                        if normalized_model
                        else "",
                        "leafTexture": leaf_texture,
                        "boundRadius": get_float(modb, 0) if modb else None,
                        "isSpt": normalized_model.endswith(".spt"),
                    }
                )

            offset = next_offset
        return offset

    read_range(0, len(data))
    return trees


def assert_text(repo_root, relative_path, needle, description, proof_line):
    path = repo_root / relative_path
    if not path.is_file():
        raise SystemExit(f"Missing file for {description}: {relative_path}")
    text = path.read_text(encoding="utf-8")
    if needle not in text:
        raise SystemExit(f"Missing {description}: {needle} in {relative_path}")
    proof_line(f"OK contract: {description}")


def main():
    parser = argparse.ArgumentParser(description="Prove the Fallout New Vegas SpeedTree SPT runtime blocker boundary.")
    parser.add_argument("--fnv-root", default=os.environ.get("NIKAMI_FNV_ROOT", ""))
    parser.add_argument("--fnv-data", default=os.environ.get("NIKAMI_FNV_DATA", ""))
    parser.add_argument("--harvest-dir", default="")
    parser.add_argument("--proof-root", default="")
    parser.add_argument("--repo-root", default="")
    parser.add_argument("--content", nargs="+", default=DEFAULT_RETAIL_CONTENT)
    args = parser.parse_args()

    if not args.fnv_root and not args.fnv_data:
        raise SystemExit("Set --fnv-root, --fnv-data, NIKAMI_FNV_ROOT, or NIKAMI_FNV_DATA before running this proof.")

    fnv_data = Path(args.fnv_data or Path(args.fnv_root) / "Data").resolve()
    fnv_root = Path(args.fnv_root or fnv_data.parent).resolve()
    repo_root = Path(args.repo_root or Path(__file__).resolve().parents[2]).resolve()
    proof_root = Path(args.proof_root or repo_root.parent / "proof").resolve()

    if args.harvest_dir:
        harvest_dir = Path(args.harvest_dir).resolve()
    else:
        harvest_root = proof_root / "fnv-retail-harvest"
        latest = sorted((path for path in harvest_root.iterdir() if path.is_dir()), reverse=True)
        if not latest:
            raise SystemExit(f"No FNV harvest proof directories found under {harvest_root}")
        harvest_dir = latest[0]

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    proof_dir = proof_root / "fnv-speedtree-spt-ledger" / stamp
    proof_dir.mkdir(parents=True, exist_ok=True)
    summary_file = proof_dir / "summary.txt"

    def proof_line(text=""):
        print(text, flush=True)
        with summary_file.open("a", encoding="utf-8") as stream:
            stream.write(text + "\n")

    proof_line(f"FNV SpeedTree SPT ledger proof {stamp}")
    proof_line(f"RepoRoot: {repo_root}")
    proof_line(f"FnvRoot: {fnv_root}")
    proof_line(f"FnvData: {fnv_data}")
    proof_line(f"HarvestDir: {harvest_dir}")
    proof_line(f"ProofDir: {proof_dir}")
    proof_line()

    assert_text(
        repo_root,
        Path("scripts/nikami/test-fnv-harvest-action-coverage.ps1"),
        '"blocked-runtime-support" "speedtree-tree-assets"',
        "harvest gate keeps SPT as a runtime blocker",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("components/esm4/loadtree.cpp"),
        "reader.getZString(mModel)",
        "TREE loader captures MODL model paths",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("components/esm4/loadtree.cpp"),
        "normalizeFalloutTreeModel",
        "TREE loader normalizes bare Fallout SPT paths into the trees directory",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("components/esm4/loadtree.cpp"),
        "reader.getZString(mLeafTexture)",
        "TREE loader captures ICON leaf textures",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("apps/openmw/mwrender/objectpaging.cpp"),
        "correctEsm4StaticModelPath",
        "object paging uses ESM4-specific model path correction",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("apps/openmw/mwrender/objectpaging.cpp"),
        "case ESM::REC_TREE4",
        "object paging includes ESM4 TREE records",
        proof_line,
    )
    assert_text(
        repo_root,
        Path("components/resource/scenemanager.cpp"),
        "Ignoring SpeedTree data file",
        "scene manager currently emits empty SpeedTree nodes",
        proof_line,
    )

    harvest_spt = collect_harvest_spt_paths(harvest_dir)
    harvest_paths = sorted({row["path"] for row in harvest_spt})
    expected_paths = sorted(normalize_asset_path(path) for path in EXPECTED_SPT_PATHS)
    if harvest_paths != expected_paths:
        raise SystemExit(f"Unexpected harvested SPT paths: actual={harvest_paths} expected={expected_paths}")
    for path in harvest_paths:
        proof_line(f"OK SPT harvest path: {path}")

    missing_content = []
    tree_records = []
    for name in args.content:
        path = fnv_data / name
        if not path.is_file():
            missing_content.append(name)
            proof_line(f"FAIL missing content: {name} -> {path}")
            continue
        proof_line(f"Parsing content TREE records: {name}")
        tree_records.extend(parse_tree_records(path, name))
    if missing_content:
        raise SystemExit(f"Missing retail content files: {', '.join(missing_content)}")

    spt_tree_records = [row for row in tree_records if row["isSpt"]]
    unique_tree_models = sorted({row["normalizedModel"] for row in spt_tree_records})
    harvest_path_set = set(harvest_paths)
    for row in spt_tree_records:
        resolved = resolve_spt_archive_path(row["normalizedModel"], harvest_path_set)
        row["resolvedArchivePath"] = resolved
        row["runtimeResolvedModelPath"] = resolved
        row["requiresTreesDirectoryFallback"] = bool(resolved and resolved != row["normalizedModel"])
    resolved_tree_models = sorted({row["resolvedArchivePath"] for row in spt_tree_records if row["resolvedArchivePath"]})
    missing_archive_models = sorted({row["normalizedModel"] for row in spt_tree_records if not row["resolvedArchivePath"]})
    unreferenced_archive_paths = [path for path in harvest_paths if path not in resolved_tree_models]
    legacy_object_paging_paths = sorted({row["legacyObjectPagingPath"] for row in spt_tree_records})

    if not spt_tree_records:
        raise SystemExit("No TREE records reference SPT models; the SPT runtime blocker proof is invalid.")
    if missing_archive_models:
        raise SystemExit(f"TREE records reference missing SPT archive paths: {missing_archive_models}")

    ledger = {
        "stamp": stamp,
        "repoRoot": str(repo_root),
        "fnvData": str(fnv_data),
        "harvestDir": str(harvest_dir),
        "harvestSptPaths": harvest_spt,
        "treeRecordCount": len(tree_records),
        "sptTreeRecordCount": len(spt_tree_records),
        "uniqueTreeSptModels": unique_tree_models,
        "resolvedTreeSptModels": resolved_tree_models,
        "unreferencedArchiveSptPaths": unreferenced_archive_paths,
        "legacyObjectPagingPrefixedPaths": legacy_object_paging_paths,
        "treeRecords": tree_records,
    }
    ledger_path = proof_dir / "speedtree-spt-ledger.json"
    write_json(ledger_path, ledger)

    proof_line()
    proof_line(f"TREE records parsed: {len(tree_records)}")
    proof_line(f"TREE records referencing SPT: {len(spt_tree_records)}")
    proof_line(f"Unique TREE SPT models: {len(unique_tree_models)}")
    proof_line(f"Resolved archive SPT models: {len(resolved_tree_models)}")
    proof_line(f"Unreferenced archive SPT paths: {len(unreferenced_archive_paths)}")
    proof_line(f"Ledger JSON: {ledger_path}")
    proof_line()
    proof_line("FNV SpeedTree SPT ledger proof PASS")
    proof_line("SPT remains blocked until the runtime has a real SpeedTree reader/converter or a proved TREE billboard fallback.")


if __name__ == "__main__":
    main()
