#!/usr/bin/env python3
"""Contract checks for the FNV live studio backend helpers."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

import fnv_character_viewer_live_server as live


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="nikami-studio-live-") as tmp:
        root = Path(tmp) / "proof"
        run_dir = root / "server-run"
        catalog_dir = root / "fnv-character-studio-catalog" / "20260624_000000"
        entry = {
            "id": "actor:000001",
            "domain": "actor",
            "kind": "npc",
            "recordType": "NPC_",
            "label": "ContractNpc",
            "target": "ContractNpc",
            "classification": "loaded-pending-runtime",
            "searchText": "contract npc contractnpc",
            "commands": {
                "runtimeThreeCamera": (
                    "powershell -NoProfile -ExecutionPolicy Bypass -File "
                    "scripts/nikami/run-fnv-character-viewer.ps1 -Targets 'ContractNpc' "
                    "-ActorKind npc -Phases 'body,head,face' -Angles 'front,front-left,front-right' -OpenViewer -LiveServe"
                ),
                "runtimeFrontOnly": (
                    "powershell -NoProfile -ExecutionPolicy Bypass -File "
                    "scripts/nikami/run-fnv-character-viewer.ps1 -Targets 'ContractNpc' "
                    "-ActorKind npc -Phases 'body,head,face' -Angles 'front' -OpenViewer -LiveServe"
                ),
            },
        }
        placed_entry = {
            **entry,
            "id": "actor:000002",
            "source": "placed-reference",
            "label": "ContractNpcRef",
            "target": "ContractNpcRef",
            "selectedTarget": "ContractNpcRef",
            "runtimeTarget": "ContractNpc",
            "placedTarget": "ContractNpcRef",
            "baseActorTarget": "ContractNpc",
            "assemblyTarget": "ContractNpc",
            "actorFormTarget": "FormId:0x00000001",
            "placedRefFormTarget": "FormId:0x00000011",
            "placementCommandArgs": (
                "-BootstrapCell 'FormId:0x00000021' -BootstrapX 11 -BootstrapY 22 -BootstrapZ 33 "
                "-ActorStageX 11 -ActorStageY 22 -ActorStageZ 33"
            ),
        }
        model_item_entry = {
            "id": "gameplay:000001",
            "domain": "gameplay",
            "kind": "armor",
            "recordType": "ARMO",
            "label": "ContractArmor",
            "target": "ContractArmor",
            "formId": "0x00001004",
            "plugin": "Contract.esm",
            "model": "Armor\\Contract\\ContractArmor.nif",
            "classification": "loaded-pending-runtime",
            "searchText": "contract armor contractarmor armor contract/contractarmor.nif",
            "commands": {
                "runtimeThreeCamera": (
                    "powershell -NoProfile -ExecutionPolicy Bypass -File "
                    "scripts/nikami/run-fnv-item-viewer.ps1 -ItemTarget 'ContractArmor' "
                    "-ItemKind 'armor' -ItemRecordType 'ARMO' -ItemFormId '0x00001004' "
                    "-ItemPlugin 'Contract.esm' -ItemModel 'Armor\\Contract\\ContractArmor.nif' "
                    "-Angles 'front,front-left,front-right' -RequirePass"
                ),
                "runtimeFrontOnly": (
                    "powershell -NoProfile -ExecutionPolicy Bypass -File "
                    "scripts/nikami/run-fnv-item-viewer.ps1 -ItemTarget 'ContractArmor' "
                    "-ItemKind 'armor' -ItemRecordType 'ARMO' -ItemFormId '0x00001004' "
                    "-ItemPlugin 'Contract.esm' -ItemModel 'Armor\\Contract\\ContractArmor.nif' "
                    "-Angles 'front' -RequirePass"
                ),
            },
        }
        write_json(
            catalog_dir / "character-studio-catalog.json",
            {
                "schema": "nikami-fnv-character-studio-catalog-v1",
                "status": "PASS",
                "entries": [entry, placed_entry, model_item_entry],
            },
        )
        live_authoring_path = run_dir / "live-authoring.json"
        live_runtime_path = run_dir / "live-runtime-command.json"
        runtime_stdout = run_dir / "runtime-proof.stdout.log"
        runtime_stderr = run_dir / "runtime-proof.stderr.log"
        openmw_log = root / "configs" / "fnv-flat-clean" / "openmw.log"
        run_dir.mkdir(parents=True, exist_ok=True)
        openmw_log.parent.mkdir(parents=True, exist_ok=True)
        runtime_stdout.write_text(f"OpenMW log {openmw_log}\n", encoding="utf-8")
        runtime_stderr.write_text("runtime stderr", encoding="utf-8")
        openmw_log.write_text(
            "\n".join(
                [
                    '[00:00:01.000 I] FNV/ESM4 live runtime: actor target changed from="" to="ContractNpc" file="live-runtime-command.json" runtime=runtime-supported gate=runtime-live-actor-target',
                    '[00:00:01.050 I] FNV/ESM4 live runtime: actor-kit selector key=actorKitParts value="headgear" file="live-runtime-command.json" runtime=runtime-supported gate=runtime-live-actor-kit-controls',
                    '[00:00:01.075 I] FNV/ESM4 live runtime: actor-kit post-construction selector generation=1 actor=ContractNpc ref=FormId:0x1 file="live-runtime-command.json" targetMatches=1 fingerprint="actorTarget=ContractNpc;parts=headgear;" animationRequest=issued partRebuild=loaded-pending-runtime runtime=loaded-pending-runtime gate=runtime-live-actor-kit-post-construction-selector',
                    '[00:00:01.100 I] FNV/ESM4 proof: actor part assembly target match target="ContractNpc" actor=ContractNpc refAlias=FormId:0x1 ref=FormId:0x11 runtime=runtime-supported',
                    "[00:00:01.200 I] FNV/ESM4 live authoring: frame-applied head surface authoring model=meshes/characters/head/eyelefthuman.nif prefix=OPENMW_FNV_EYE file=live-authoring.json offset=(0,0,0) rotation=(0,0,-90) pivot=(0,0,0) pivotMode=0 for FormId:0x1",
                ]
            ),
            encoding="utf-8",
        )
        write_json(
            run_dir / "live-authoring-run.json",
            {
                "schema": "nikami-fnv-live-character-authoring-run-v1",
                "runtimeProcessId": os.getpid(),
                "runtimeStdout": str(runtime_stdout),
                "runtimeStderr": str(runtime_stderr),
                "runtimeCommand": "powershell -File scripts/nikami/run-fnv-flat-proof.ps1",
            },
        )
        live_authoring_path.write_text("{}", encoding="utf-8")
        live_runtime_path.write_text("{}", encoding="utf-8")
        status = live.runtime_status(run_dir, root, live_authoring_path, live_runtime_path)
        if status["schema"] != live.LIVE_RUNTIME_STATUS_SCHEMA or "runtime-process-status-v1" not in status["schemaMarkers"]:
            raise AssertionError("runtime status schema/markers mismatch")
        if not status["runtimeRunning"] or status["runtimeProcessId"] != os.getpid():
            raise AssertionError("runtime status did not detect the active runtime process")
        if not status["liveAuthoringUpdatedAt"] or not status["liveRuntimeCommandUpdatedAt"]:
            raise AssertionError("runtime status did not expose generated control file mtimes")
        audit = live.runtime_audit(run_dir, root, live_authoring_path, live_runtime_path)
        if audit["schema"] != live.LIVE_RUNTIME_AUDIT_SCHEMA or audit["classification"] != "runtime-supported":
            raise AssertionError("runtime audit did not classify consumed runtime log evidence")
        if audit["openMwLog"] != str(openmw_log.resolve()):
            raise AssertionError("runtime audit did not resolve OpenMW log path from runtime stdout")
        if (
            audit["counts"]["targetSwitches"] != 1
            or audit["counts"]["liveActorKitControls"] != 1
            or audit["counts"]["liveActorKitPostConstruction"] != 1
            or audit["counts"]["actorAssemblyMatches"] != 1
            or audit["counts"]["liveAuthoringApplies"] != 1
        ):
            raise AssertionError("runtime audit did not count target, actor-kit, assembly, and knob consumption lines")
        runtime_store = live.LiveRuntimeCommandStore(run_dir, root, live_runtime_path)
        runtime_doc = runtime_store.update(
            {
                "command": "update-actor-kit",
                "actorTarget": "ContractNpc",
                "actorKind": "npc",
                "selectors": {
                    "phase": "headgear",
                    "parts": ["headgear"],
                    "propSlots": ["headgear"],
                    "animationGroup": "idle",
                    "animationStartPoint": "0.25",
                    "dialogueMode": "mouth-open",
                },
            }
        )
        if "runtime-live-actor-kit-controls-v1" not in runtime_doc["schemaMarkers"]:
            raise AssertionError("live runtime command did not advertise actor-kit controls")
        if runtime_doc["actorKitParts"] != "headgear" or runtime_doc["selectors"]["parts"] != ["headgear"]:
            raise AssertionError("live runtime command did not persist actor-kit part selector")
        if runtime_doc["characterBuilderPhase"] != "headgear" or runtime_doc["actorKitAnimationStartPoint"] != "0.25":
            raise AssertionError("live runtime command did not persist phase or animation scrub selector")
        if runtime_doc["lastApplied"].get("actorKitDialogueMode") != "mouth-open":
            raise AssertionError("live runtime command did not record last applied actor-kit controls")

        catalog = live.CatalogStore(root)
        loaded = catalog.load()
        if loaded.get("schema") != "nikami-fnv-character-studio-catalog-v1":
            raise AssertionError("catalog store did not load generated studio catalog")
        search = catalog.search({"q": ["contract npc"], "limit": ["5"]})
        if search["count"] < 1 or not any(item["id"] == "actor:000001" for item in search["entries"]):
            raise AssertionError("catalog search did not return the actor entry")
        if search["total"] < search["count"]:
            raise AssertionError("catalog search did not preserve total match count")
        if any("commands" in item for item in search["entries"]):
            raise AssertionError("catalog search returned full command payloads instead of compact rows")
        if any("searchText" in item for item in search["entries"]):
            raise AssertionError("catalog search returned giant searchText payloads instead of compact rows")
        if not search["entries"][0].get("runnable"):
            raise AssertionError("catalog search compact row did not preserve runnable actor status")
        item_search = catalog.search({"q": ["contract armor"], "limit": ["5"]})
        item_rows = [item for item in item_search["entries"] if item["id"] == "gameplay:000001"]
        if not item_rows or not item_rows[0].get("runnable"):
            raise AssertionError("catalog search compact row did not preserve runnable model-backed item status")
        full_entry = catalog.entry("actor:000001")
        if full_entry is None:
            raise AssertionError("catalog entry lookup failed")
        if "commands" not in full_entry:
            raise AssertionError("catalog entry lookup did not preserve full row details")
        forced_catalog = live.CatalogStore(root / "missing-root", catalog_dir / "character-studio-catalog.json")
        if forced_catalog.entry("actor:000001") is None:
            raise AssertionError("forced catalog path lookup failed")

        sessions = live.StudioSessionStore(run_dir)
        session = sessions.create({"entryId": "actor:000001"})
        if session["schema"] != live.STUDIO_SCHEMA:
            raise AssertionError("studio session schema mismatch")
        event = sessions.append_event(session["id"], "entry.open", {"entryId": "actor:000001"})
        if event["type"] != "entry.open":
            raise AssertionError("studio session event append failed")
        if not Path(session["eventsPath"]).is_file():
            raise AssertionError("studio session did not write events.jsonl")
        review_result = sessions.append_reviews(
            session["id"],
            {
                "entryId": "actor:000001",
                "jobId": "job-contract",
                "rows": [
                    {
                        "component": "face",
                        "label": "Face / Wrinkles",
                        "reviewState": "fail",
                        "machineStatus": "fail",
                        "target": "ContractNpc",
                        "runtimeTarget": "ContractNpc",
                        "placedTarget": "",
                        "phase": "face",
                        "selectors": {"parts": ["face-organs"], "angles": ["front"]},
                        "manifestUrl": "viewer/manifest.json",
                        "viewerUrl": "viewer/character-viewer.html",
                        "proofUrls": ["viewer/face_front/screenshot000.png"],
                        "failureCount": 1,
                    }
                ],
            },
        )
        if review_result["schema"] != live.STUDIO_REVIEW_SCHEMA or review_result["accepted"] != 1:
            raise AssertionError("studio session component review append failed")
        if not Path(session["reviewsPath"]).is_file():
            raise AssertionError("studio session did not write reviews.jsonl")
        reviews = sessions.reviews(session["id"])
        if len(reviews) != 1 or reviews[0]["component"] != "face" or reviews[0]["reviewState"] != "fail":
            raise AssertionError("studio session did not read back component review rows")
        try:
            sessions.append_reviews(session["id"], {"rows": [{"component": "face", "reviewState": "silent-skip"}]})
        except ValueError as exc:
            if "invalid review state" not in str(exc):
                raise
        else:
            raise AssertionError("studio session accepted an invalid component review state")

        command = live.structured_actor_command(entry, {"entryId": "actor:000001", "commandKey": "runtimeFrontOnly"})
        args = live.command_to_args(command, Path("scripts/nikami/run-fnv-character-viewer.ps1"))
        if "-OpenViewer" in args or "-LiveServe" in args:
            raise AssertionError("structured command was not sanitized through actor-kit allowlist")
        if "-Angles" not in args or "front" not in args:
            raise AssertionError("structured command did not preserve front angle")

        selector_command, selector_request = live.structured_actor_job(
            placed_entry,
            {
                "entryId": "actor:000002",
                "selectors": {
                    "phases": ["headgear"],
                    "angles": ["front", "front-left", "front-right"],
                    "parts": ["headgear"],
                    "propSlots": ["headgear"],
                    "animationGroup": "idle",
                    "dialogueMode": "mouth-open",
                },
            },
        )
        selector_args = live.command_to_args(selector_command, Path("scripts/nikami/run-fnv-character-viewer.ps1"))
        if selector_args[selector_args.index("-Targets") + 1] != "ContractNpc":
            raise AssertionError("structured command did not use runtime/base actor target")
        if selector_request["placedTarget"] != "ContractNpcRef" or selector_request["runtimeTarget"] != "ContractNpc":
            raise AssertionError("structured request did not expose placed/runtime target mapping")
        if "-ActorKitParts" not in selector_args or "headgear" not in selector_args:
            raise AssertionError("structured command ignored part selector overrides")
        if "-ActorKitPropSlots" not in selector_args or "-ActorKitAnimationGroup" not in selector_args or "-ActorKitDialogueMode" not in selector_args:
            raise AssertionError("structured command ignored prop/animation selector overrides")
        if "-BootstrapCell" not in selector_args or "FormId:0x00000021" not in selector_args:
            raise AssertionError("structured command lost placed-ref runtime target or bootstrap args")
        if selector_request["selectors"]["dialogueMode"] != "mouth-open":
            raise AssertionError("structured request did not normalize nested dialogue selector")

        item_entry = {
            "id": "gameplay:000002",
            "domain": "gameplay",
            "kind": "weapon",
            "commands": {"runtimeThreeCamera": "", "runtimeFrontOnly": ""},
        }
        item_command, item_request = live.structured_studio_job(model_item_entry, {"entryId": "gameplay:000001", "commandKey": "runtimeFrontOnly"})
        item_args = live.command_to_args(item_command, Path("scripts/nikami/run-fnv-character-viewer.ps1"))
        if str(item_args[item_args.index("-File") + 1]).replace("\\", "/") != "scripts/nikami/run-fnv-item-viewer.ps1":
            raise AssertionError("model-backed item command did not route to the generated item viewer runner")
        if "-ItemModel" not in item_args or "Armor\\Contract\\ContractArmor.nif" not in item_args:
            raise AssertionError("structured item command did not preserve model-backed item metadata")
        if item_request["kind"] != "item-runtime-visual-spawn" or item_request["selectors"]["propModels"] != ["Armor\\Contract\\ContractArmor.nif"]:
            raise AssertionError("structured item request did not expose visual-spawn gate metadata")
        try:
            live.structured_studio_job(item_entry, {"entryId": "gameplay:000002"})
        except ValueError as exc:
            if "harvested model path" not in str(exc):
                raise
        else:
            raise AssertionError("structured studio job incorrectly accepted model-less item entry")

    print("FNV character studio live server contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
