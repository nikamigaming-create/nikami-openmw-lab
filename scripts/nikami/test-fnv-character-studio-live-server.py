#!/usr/bin/env python3
"""Contract checks for the FNV live studio backend helpers."""

from __future__ import annotations

import json
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
        write_json(
            catalog_dir / "character-studio-catalog.json",
            {
                "schema": "nikami-fnv-character-studio-catalog-v1",
                "status": "PASS",
                "entries": [entry, placed_entry],
            },
        )

        catalog = live.CatalogStore(root)
        loaded = catalog.load()
        if loaded.get("schema") != "nikami-fnv-character-studio-catalog-v1":
            raise AssertionError("catalog store did not load generated studio catalog")
        search = catalog.search({"q": ["contract npc"], "limit": ["5"]})
        if search["count"] < 1 or not any(item["id"] == "actor:000001" for item in search["entries"]):
            raise AssertionError("catalog search did not return the actor entry")
        if catalog.entry("actor:000001") is None:
            raise AssertionError("catalog entry lookup failed")
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
            "id": "gameplay:000001",
            "domain": "gameplay",
            "kind": "weapon",
            "commands": {"runtimeThreeCamera": "", "runtimeFrontOnly": ""},
        }
        try:
            live.structured_actor_command(item_entry, {"entryId": "gameplay:000001"})
        except ValueError as exc:
            if "actor or creature" not in str(exc):
                raise
        else:
            raise AssertionError("structured command incorrectly accepted item entry before item summon support")

    print("FNV character studio live server contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
