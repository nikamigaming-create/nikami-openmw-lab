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
        write_json(
            catalog_dir / "character-studio-catalog.json",
            {
                "schema": "nikami-fnv-character-studio-catalog-v1",
                "status": "PASS",
                "entries": [entry],
            },
        )

        catalog = live.CatalogStore(root)
        loaded = catalog.load()
        if loaded.get("schema") != "nikami-fnv-character-studio-catalog-v1":
            raise AssertionError("catalog store did not load generated studio catalog")
        search = catalog.search({"q": ["contract npc"], "limit": ["5"]})
        if search["count"] != 1 or search["entries"][0]["id"] != "actor:000001":
            raise AssertionError("catalog search did not return the actor entry")
        if catalog.entry("actor:000001") is None:
            raise AssertionError("catalog entry lookup failed")

        sessions = live.StudioSessionStore(run_dir)
        session = sessions.create({"entryId": "actor:000001"})
        if session["schema"] != live.STUDIO_SCHEMA:
            raise AssertionError("studio session schema mismatch")
        event = sessions.append_event(session["id"], "entry.open", {"entryId": "actor:000001"})
        if event["type"] != "entry.open":
            raise AssertionError("studio session event append failed")
        if not Path(session["eventsPath"]).is_file():
            raise AssertionError("studio session did not write events.jsonl")

        command = live.structured_actor_command(entry, {"entryId": "actor:000001", "commandKey": "runtimeFrontOnly"})
        args = live.command_to_args(command, Path("scripts/nikami/run-fnv-character-viewer.ps1"))
        if "-OpenViewer" in args or "-LiveServe" in args:
            raise AssertionError("structured command was not sanitized through actor-kit allowlist")
        if "-Angles" not in args or "front" not in args:
            raise AssertionError("structured command did not preserve front angle")

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
