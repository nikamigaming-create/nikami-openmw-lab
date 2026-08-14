#!/usr/bin/env python3
"""Build a machine-readable FNV ObScript coverage table.

The input is the aggregate ``roadmap.md`` produced by
BarryThePirate/obscript-pipeline's ``tools/make_roadmap.py``. No retail script
source is copied into this repository; only command/event occurrence counts
from the user's locally extracted official plugins are retained.

Usage:
    python tools/fnv_obscript_coverage.py ROADMAP BINDINGS RUNTIME OUTPUT
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


REQUIRED_COMMANDS = {
    # Goodsprings flora and activation.
    "activate",
    "additem",
    "getactionref",
    "getitemcount",
    "isactionref",
    "playgroup",
    "setdestroyed",
    "getdestroyed",
    # Dialogue and quest progression.
    "say",
    "sayto",
    "saytodone",
    "startconversation",
    "getstage",
    "getstagedone",
    "setstage",
    "getquestrunning",
    "getquestcompleted",
    "startquest",
    "stopquest",
    "completequest",
    "setobjectivedisplayed",
    "getobjectivedisplayed",
    "setobjectivecompleted",
    "getobjectivecompleted",
    # Actor packages, combat, crime response, and life state.
    "addscriptpackage",
    "removescriptpackage",
    "evp",
    "resetai",
    "startcombat",
    "stopcombat",
    "sendassaultalarm",
    "getdead",
    "getunconscious",
    # Inventory and weapon continuity.
    "removeitem",
    "equipitem",
    "unequipitem",
    "getequipped",
}

REQUIRED_EVENTS = {
    "gamemode",
    "onactivate",
    "onload",
    "onreset",
    "ontrigger",
    "ontriggerenter",
    "ontriggerleave",
    "ondeath",
    "onhit",
    "onhitwith",
    "onstartcombat",
    "oncombatend",
    "onpackagestart",
    "onpackagedone",
}

IGNORED_SECTIONS = {
    "Built-in references",
    "Argument enums (animation groups, actor value names)",
    "Unclassified tokens (manual triage: editor IDs, variables, missing functions)",
}


@dataclass(frozen=True)
class Entry:
    kind: str
    name: str
    calls: int
    scripts: int
    category: str


def parse_table_row(line: str) -> list[str] | None:
    if not line.startswith("|") or line.startswith("|---"):
        return None
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    if len(cells) != 3 or cells[0] in {"block", "function", "reference", "name", "token"}:
        return None
    if not cells[1].isdigit() or not cells[2].isdigit():
        return None
    return cells


def read_roadmap(path: Path) -> list[Entry]:
    entries: list[Entry] = []
    section = ""
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if raw_line.startswith("## "):
            section = raw_line[3:].strip()
            continue
        row = parse_table_row(raw_line)
        if row is None or section in IGNORED_SECTIONS:
            continue
        name, calls, scripts = row
        kind = "event" if section == "Event model (Begin-block types)" else "command"
        entries.append(Entry(kind, name.lower(), int(calls), int(scripts), section))
    if not entries:
        raise RuntimeError(f"no classified commands/events found in {path}")
    return entries


def read_supported_commands(path: Path) -> set[str]:
    source = path.read_text(encoding="utf-8")
    names = {match.lower() for match in re.findall(r"obs\.bind\(\s*['\"]([^'\"]+)['\"]", source)}
    loop = re.search(
        r"for\s+command,\s*event\s+in\s+pairs\(\{(?P<body>.*?)\}\)\s+do",
        source,
        flags=re.DOTALL,
    )
    if loop:
        names.update(match.lower() for match in re.findall(r"(\w+)\s*=", loop.group("body")))
    return names


def read_supported_events(path: Path) -> set[str]:
    source = path.read_text(encoding="utf-8")
    block = re.search(
        r"local\s+supportedEvents\s*=\s*\{(?P<body>.*?)\n\}",
        source,
        flags=re.DOTALL,
    )
    if block is None:
        raise RuntimeError(f"supportedEvents table not found in {path}")
    return {match.lower() for match in re.findall(r"\[['\"]([^'\"]+)['\"]\]\s*=", block.group("body"))}


def lua_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def status(entry: Entry, implemented: bool) -> str:
    if implemented:
        return "implemented"
    required = REQUIRED_EVENTS if entry.kind == "event" else REQUIRED_COMMANDS
    if entry.name in required:
        return "required-gap"
    return "unsupported-explicit"


def write_output(
    path: Path,
    entries: list[Entry],
    supported_commands: set[str],
    supported_events: set[str],
) -> None:
    rows: list[tuple[Entry, str]] = []
    for entry in entries:
        implemented = entry.name in (
            supported_events if entry.kind == "event" else supported_commands
        )
        rows.append((entry, status(entry, implemented)))

    command_rows = [row for row in rows if row[0].kind == "command"]
    event_rows = [row for row in rows if row[0].kind == "event"]
    required_gaps = [row for row in rows if row[1] == "required-gap"]

    lines = [
        "-- Generated by tools/fnv_obscript_coverage.py from BarryThePirate/obscript-pipeline aggregates.",
        "-- Contains no retail script source.",
        "return {",
        '    corpus = "Fallout: New Vegas + all official DLC",',
        "    scripts = 3708,",
        "    totalLines = 165335,",
        f"    classifiedCommands = {len(command_rows)},",
        f"    classifiedEvents = {len(event_rows)},",
        f"    requiredGaps = {len(required_gaps)},",
        "    commands = {",
    ]
    for entry, row_status in sorted(command_rows, key=lambda row: (-row[0].scripts, row[0].name)):
        lines.append(
            "        { name = %s, calls = %d, scripts = %d, category = %s, status = %s },"
            % (
                lua_string(entry.name),
                entry.calls,
                entry.scripts,
                lua_string(entry.category),
                lua_string(row_status),
            )
        )
    lines.append("    },")
    lines.append("    events = {")
    for entry, row_status in sorted(event_rows, key=lambda row: (-row[0].scripts, row[0].name)):
        lines.append(
            "        { name = %s, calls = %d, scripts = %d, status = %s },"
            % (
                lua_string(entry.name),
                entry.calls,
                entry.scripts,
                lua_string(row_status),
            )
        )
    lines.extend(["    },", "}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")

    print(
        f"wrote {path}: {len(command_rows)} commands, {len(event_rows)} events, "
        f"{len(required_gaps)} required gaps"
    )


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    _, roadmap, bindings, runtime, output = argv
    entries = read_roadmap(Path(roadmap))
    write_output(
        Path(output),
        entries,
        read_supported_commands(Path(bindings)),
        read_supported_events(Path(runtime)),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
