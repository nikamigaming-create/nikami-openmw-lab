#!/usr/bin/env python3
"""Loopback live server for the generated FNV character viewer.

The server serves generated proof files from --root and exposes one local-only
actor-kit endpoint that can run the generated viewer rerun commands. It stores
job metadata under --run-dir and never serves or writes retail asset payloads.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import subprocess
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlparse


ALLOWED_PREFIX = [
    "powershell",
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    "scripts/nikami/run-fnv-character-viewer.ps1",
]
ALLOWED_ITEM_PREFIX = [
    "powershell",
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    "scripts/nikami/run-fnv-item-viewer.ps1",
]
FORBIDDEN_CHARS = set("&;|<>`")
STUDIO_SCHEMA = "nikami-fnv-character-studio-session-v1"
STUDIO_REVIEW_SCHEMA = "nikami-fnv-character-studio-component-review-v1"
LIVE_AUTHORING_SCHEMA = "nikami-fnv-live-authoring-v1"
LIVE_RUNTIME_SCHEMA = "nikami-fnv-live-runtime-command-v1"
LIVE_RUNTIME_STATUS_SCHEMA = "nikami-fnv-live-runtime-status-v1"
LIVE_RUNTIME_AUDIT_SCHEMA = "nikami-fnv-live-runtime-audit-v1"
REVIEW_STATES = {"review-pending", "pass", "fail", "blocked", "needs-rerun"}
HEAD_SURFACE_PREFIXES = (
    "OPENMW_FNV_HEADGEAR",
    "OPENMW_FNV_HAIR",
    "OPENMW_FNV_BROW",
    "OPENMW_FNV_EYE",
    "OPENMW_FNV_BEARD",
    "OPENMW_FNV_MOUTH",
)
HEAD_SURFACE_FLOAT_SUFFIXES = (
    "OFFSET_X",
    "OFFSET_Y",
    "OFFSET_Z",
    "ROTATION_X",
    "ROTATION_Y",
    "ROTATION_Z",
)
HEAD_SURFACE_BOOL_SUFFIXES = ("PIVOT_MODE",)
HEAD_SURFACE_CONTROL_KEYS = {
    f"{prefix}_{suffix}"
    for prefix in HEAD_SURFACE_PREFIXES
    for suffix in (*HEAD_SURFACE_FLOAT_SUFFIXES, *HEAD_SURFACE_BOOL_SUFFIXES)
}
LIVE_RUNTIME_SELECTOR_FIELDS = (
    "characterBuilderPhase",
    "actorKitParts",
    "actorKitPartModels",
    "actorKitPropSlots",
    "actorKitPropModels",
    "actorKitAnimationSource",
    "actorKitAnimationStartPoint",
    "actorKitAnimationGroup",
    "actorKitDialogueMode",
    "fnvRotationMode",
    "neutralPreviewProfile",
)
LIVE_RUNTIME_SELECTOR_ALIASES = {
    "characterBuilderPhase": ("characterBuilderPhase", "phase", "phases"),
    "actorKitParts": ("actorKitParts", "parts"),
    "actorKitPartModels": ("actorKitPartModels", "partModels"),
    "actorKitPropSlots": ("actorKitPropSlots", "propSlots"),
    "actorKitPropModels": ("actorKitPropModels", "propModels"),
    "actorKitAnimationSource": ("actorKitAnimationSource", "animationSource"),
    "actorKitAnimationStartPoint": ("actorKitAnimationStartPoint", "animationStartPoint"),
    "actorKitAnimationGroup": ("actorKitAnimationGroup", "animationGroup"),
    "actorKitDialogueMode": ("actorKitDialogueMode", "dialogueMode"),
    "fnvRotationMode": ("fnvRotationMode",),
    "neutralPreviewProfile": ("neutralPreviewProfile",),
}
LIVE_RUNTIME_SELECTOR_LIST_FIELDS = {
    "actorKitParts",
    "actorKitPartModels",
    "actorKitPropSlots",
    "actorKitPropModels",
}
CATALOG_SEARCH_ENTRY_FIELDS = (
    "id",
    "source",
    "domain",
    "kind",
    "recordType",
    "label",
    "plugin",
    "target",
    "selectedTarget",
    "runtimeTarget",
    "placedTarget",
    "baseActorTarget",
    "assemblyTarget",
    "classification",
    "firstFailingGate",
    "formId",
    "actorFormId",
    "placedRefFormId",
    "model",
    "phases",
    "studioGates",
)


def utc_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def catalog_search_entry(entry: dict[str, Any]) -> dict[str, Any]:
    compact = {key: entry.get(key) for key in CATALOG_SEARCH_ENTRY_FIELDS if key in entry}
    commands = entry.get("commands") if isinstance(entry.get("commands"), dict) else {}
    compact["runnable"] = bool(commands.get("runtimeThreeCamera")) and entry.get("domain") in {"actor", "gameplay"}
    compact["hasFullDetails"] = False
    return compact


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def write_generated_json(path: Path, doc: dict[str, Any]) -> None:
    payload = json.dumps(doc, indent=2)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    last_error: PermissionError | None = None
    for _ in range(60):
        try:
            tmp.write_text(payload, encoding="utf-8")
            tmp.replace(path)
            return
        except PermissionError as error:
            last_error = error
            time.sleep(0.025)

    # OpenMW polls generated control files on Windows; if a rename is blocked
    # by a read handle, fall back to a direct write and retry the next frame.
    for _ in range(20):
        try:
            path.write_text(payload, encoding="utf-8")
            return
        except PermissionError as error:
            last_error = error
            time.sleep(0.025)

    if last_error is not None:
        raise last_error
    raise PermissionError(f"could not write generated JSON: {path}")


def process_is_running(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        process_query_limited_information = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(process_query_limited_information, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if not ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return False
            return exit_code.value == 259
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def file_mtime_utc(path: Path | None) -> str:
    if path is None or not path.is_file():
        return ""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(path.stat().st_mtime))


def read_tail_text(path: Path | None, max_bytes: int = 1_500_000) -> str:
    if path is None or not path.is_file():
        return ""
    size = path.stat().st_size
    with path.open("rb") as stream:
        if size > max_bytes:
            stream.seek(size - max_bytes)
        return stream.read().decode("utf-8", errors="replace")


def recent_matching_lines(text: str, patterns: tuple[str, ...], limit: int = 20) -> list[str]:
    if not text:
        return []
    matched: list[str] = []
    for line in text.splitlines():
        if any(pattern in line for pattern in patterns):
            matched.append(line.strip())
    return matched[-limit:]


def find_runtime_openmw_log(root: Path, runtime_stdout_path: Path | None) -> Path | None:
    stdout_text = read_tail_text(runtime_stdout_path, 300_000)
    candidates: list[Path] = []
    for match in re.finditer(r"OpenMW log:?\s+([^\r\n]+openmw\.log)", stdout_text, re.IGNORECASE):
        candidates.append(Path(match.group(1).strip()))
    fallback = root / "configs" / "fnv-flat-clean" / "openmw.log"
    candidates.append(fallback)
    for candidate in reversed(candidates):
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved.is_file():
            return resolved
    return None


def latest_runtime_manifest(run_dir: Path, live_authoring_path: Path | None) -> Path | None:
    candidates: list[Path] = []
    if live_authoring_path is not None:
        candidates.append(live_authoring_path.resolve().parent / "live-authoring-run.json")
    candidates.append(run_dir.resolve() / "live-authoring-run.json")
    candidates.append(run_dir.resolve().parent / "live-authoring-run.json")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def runtime_status(
    run_dir: Path, root: Path, live_authoring_path: Path | None, live_runtime_path: Path | None
) -> dict[str, Any]:
    manifest_path = latest_runtime_manifest(run_dir, live_authoring_path)
    manifest: dict[str, Any] = {}
    if manifest_path is not None:
        try:
            loaded = read_json(manifest_path)
            if isinstance(loaded, dict):
                manifest = loaded
        except Exception:
            manifest = {}
    try:
        pid = int(manifest.get("runtimeProcessId") or 0)
    except (TypeError, ValueError):
        pid = 0
    stdout = first_text(manifest.get("runtimeStdout"))
    stderr = first_text(manifest.get("runtimeStderr"))
    stdout_path = Path(stdout) if stdout else None
    stderr_path = Path(stderr) if stderr else None
    live_authoring_path = live_authoring_path.resolve() if live_authoring_path is not None else None
    live_runtime_path = live_runtime_path.resolve() if live_runtime_path is not None else None
    return {
        "schema": LIVE_RUNTIME_STATUS_SCHEMA,
        "schemaMarkers": [
            "runtime-process-status-v1",
            "live-authoring-file-status-v1",
            "live-runtime-command-file-status-v1",
            "generated-proof-output-only-v1",
        ],
        "updatedAt": utc_now(),
        "runDir": str(run_dir),
        "proofRoot": str(root),
        "manifestPath": str(manifest_path or ""),
        "runtimeProcessId": pid,
        "runtimeRunning": process_is_running(pid),
        "runtimeStdout": stdout,
        "runtimeStderr": stderr,
        "runtimeStdoutUpdatedAt": file_mtime_utc(stdout_path),
        "runtimeStderrUpdatedAt": file_mtime_utc(stderr_path),
        "runtimeCommand": first_text(manifest.get("runtimeCommand")),
        "liveAuthoringFile": str(live_authoring_path or ""),
        "liveAuthoringUpdatedAt": file_mtime_utc(live_authoring_path),
        "liveRuntimeCommandFile": str(live_runtime_path or ""),
        "liveRuntimeCommandUpdatedAt": file_mtime_utc(live_runtime_path),
        "policy": {
            "generatedProofOutputsOnly": True,
            "noRetailPayloadBytes": True,
            "statusOnlyNoProcessControl": True,
        },
    }


def runtime_audit(
    run_dir: Path, root: Path, live_authoring_path: Path | None, live_runtime_path: Path | None
) -> dict[str, Any]:
    status = runtime_status(run_dir, root, live_authoring_path, live_runtime_path)
    stdout_path = Path(first_text(status.get("runtimeStdout"))) if first_text(status.get("runtimeStdout")) else None
    openmw_log = find_runtime_openmw_log(root, stdout_path)
    openmw_text = read_tail_text(openmw_log)
    target_switches = recent_matching_lines(openmw_text, ("FNV/ESM4 live runtime: actor target changed",), 12)
    actor_kit_controls = recent_matching_lines(
        openmw_text,
        ("FNV/ESM4 live runtime: actor-kit selector",),
        24,
    )
    actor_kit_post_construction = recent_matching_lines(
        openmw_text,
        ("runtime-live-actor-kit-post-construction-selector",),
        12,
    )
    assembly_matches = recent_matching_lines(openmw_text, ("FNV/ESM4 proof: actor part assembly target match",), 12)
    authoring_applies = recent_matching_lines(
        openmw_text,
        (
            "FNV/ESM4 live authoring: frame-applied head surface authoring",
            "FNV/ESM4 live authoring: applied head surface authoring",
        ),
        24,
    )
    face_checks = recent_matching_lines(openmw_text, ("FNV/ESM4 FACE CHECK",), 6)
    consumption_status = (
        "runtime-supported"
        if target_switches or actor_kit_controls or authoring_applies or assembly_matches
        else ("loaded-pending-runtime" if status.get("runtimeRunning") else "known-blocked")
    )
    first_failing_gate = "" if consumption_status == "runtime-supported" else (
        "runtime-log-consumption" if status.get("runtimeRunning") else "runtime-process-not-running"
    )
    return {
        "schema": LIVE_RUNTIME_AUDIT_SCHEMA,
        "schemaMarkers": [
            "runtime-log-consumption-audit-v1",
            "live-target-switch-audit-v1",
            "live-actor-kit-selector-audit-v1",
            "live-actor-kit-post-construction-audit-v1",
            "live-head-surface-authoring-audit-v1",
            "generated-proof-output-only-v1",
        ],
        "updatedAt": utc_now(),
        "classification": consumption_status,
        "firstFailingGate": first_failing_gate,
        "runtimeStatus": status,
        "openMwLog": str(openmw_log or ""),
        "openMwLogUpdatedAt": file_mtime_utc(openmw_log),
        "recent": {
            "targetSwitches": target_switches,
            "liveActorKitControls": actor_kit_controls,
            "liveActorKitPostConstruction": actor_kit_post_construction,
            "actorAssemblyMatches": assembly_matches,
            "liveAuthoringApplies": authoring_applies,
            "faceChecks": face_checks,
        },
        "counts": {
            "targetSwitches": len(target_switches),
            "liveActorKitControls": len(actor_kit_controls),
            "liveActorKitPostConstruction": len(actor_kit_post_construction),
            "actorAssemblyMatches": len(assembly_matches),
            "liveAuthoringApplies": len(authoring_applies),
            "faceChecks": len(face_checks),
        },
        "policy": {
            "generatedProofOutputsOnly": True,
            "noRetailPayloadBytes": True,
            "auditReadsRuntimeLogsOnly": True,
        },
    }


def split_powershell_command(command: str) -> list[str]:
    tokens: list[str] = []
    current: list[str] = []
    in_single = False
    index = 0
    while index < len(command):
        char = command[index]
        if in_single:
            if char == "'":
                if index + 1 < len(command) and command[index + 1] == "'":
                    current.append("'")
                    index += 2
                    continue
                in_single = False
            else:
                current.append(char)
            index += 1
            continue
        if char == "'":
            in_single = True
        elif char.isspace():
            if current:
                tokens.append("".join(current))
                current = []
        else:
            current.append(char)
        index += 1
    if in_single:
        raise ValueError("unterminated single quote")
    if current:
        tokens.append("".join(current))
    return tokens


def command_to_args(command: str, runner: Path) -> list[str]:
    if any(char in FORBIDDEN_CHARS for char in command):
        raise ValueError("command contains shell metacharacters outside the actor-kit allowlist")
    tokens = split_powershell_command(command)
    allowed_prefixes = (ALLOWED_PREFIX, ALLOWED_ITEM_PREFIX)
    if len(tokens) < len(ALLOWED_PREFIX):
        raise ValueError("command is too short")
    normalized = tokens[: len(ALLOWED_PREFIX)]
    normalized[0] = normalized[0].lower().removesuffix(".exe")
    matched_prefix = None
    for prefix in allowed_prefixes:
        if normalized == prefix:
            matched_prefix = prefix
            break
    if matched_prefix is None:
        raise ValueError("command does not match generated actor-kit or item-viewer runner prefix")
    args = tokens[:]
    args[0] = "powershell"
    if matched_prefix == ALLOWED_ITEM_PREFIX:
        args[5] = str(runner.with_name("run-fnv-item-viewer.ps1"))
    else:
        args[5] = str(runner)
    args = [arg for arg in args if arg != "-OpenViewer"]
    args = [arg for arg in args if arg != "-Serve"]
    args = [arg for arg in args if arg != "-LiveServe"]
    return args


def proof_url(root: Path, path: str) -> str:
    if not path:
        return ""
    target = Path(path)
    if not target.is_absolute():
        return ""
    try:
        relative = target.resolve().relative_to(root.resolve())
    except ValueError:
        return ""
    return "/" + str(relative).replace(os.sep, "/")


def parse_run_output(stdout: str, root: Path) -> dict[str, str]:
    viewer = ""
    manifest = ""
    actor_kit = ""
    suite = ""
    for line in stdout.splitlines():
        if "Viewer for " in line and ": " in line:
            viewer = line.split(": ", 1)[1].strip()
        elif "Manifest for " in line and ": " in line:
            manifest = line.split(": ", 1)[1].strip()
        elif line.startswith("viewer-html="):
            viewer = line.split("=", 1)[1].strip()
        elif line.startswith("viewer-json="):
            manifest = line.split("=", 1)[1].strip()
        elif line.startswith("actor-kit-json="):
            actor_kit = line.split("=", 1)[1].strip()
        elif "SuiteDir:" in line:
            suite = line.split("SuiteDir:", 1)[1].strip()
    status_match = re.search(r"status=(?P<status>[A-Z]+)\s+cases=(?P<cases>[0-9]+)", stdout)
    return {
        "status": status_match.group("status") if status_match else "",
        "cases": status_match.group("cases") if status_match else "",
        "viewerHtml": viewer,
        "viewerUrl": proof_url(root, viewer),
        "viewerJson": manifest,
        "viewerJsonUrl": proof_url(root, manifest),
        "actorKitJson": actor_kit,
        "actorKitUrl": proof_url(root, actor_kit),
        "suiteDir": suite,
    }


class CatalogStore:
    def __init__(self, root: Path, catalog_path: Path | None = None) -> None:
        self.root = root
        self.catalog_path = catalog_path
        self.lock = threading.Lock()
        self.path: Path | None = None
        self.catalog: dict[str, Any] = {}

    def _latest_catalog_path(self) -> Path:
        if self.catalog_path is not None:
            if self.catalog_path.is_file():
                return self.catalog_path
            raise FileNotFoundError(f"Configured character studio catalog does not exist: {self.catalog_path}")
        catalog_root = self.root / "fnv-character-studio-catalog"
        candidates = sorted((path for path in catalog_root.glob("*") if path.is_dir()), reverse=True)
        for candidate in candidates:
            catalog = candidate / "character-studio-catalog.json"
            if catalog.is_file():
                return catalog
        raise FileNotFoundError(f"No character studio catalog found under {catalog_root}")

    def load(self) -> dict[str, Any]:
        path = self._latest_catalog_path()
        with self.lock:
            if self.path != path:
                self.catalog = read_json(path)
                self.path = path
        return self.catalog

    def entry(self, entry_id: str) -> dict[str, Any] | None:
        catalog = self.load()
        for entry in as_list(catalog.get("entries")):
            if isinstance(entry, dict) and entry.get("id") == entry_id:
                return entry
        return None

    def search(self, query: dict[str, list[str]]) -> dict[str, Any]:
        catalog = self.load()
        text = (query.get("q") or [""])[0].strip().lower()
        domain = (query.get("domain") or [""])[0].strip()
        kind = (query.get("kind") or [""])[0].strip()
        classification = (query.get("classification") or [""])[0].strip()
        try:
            limit = max(1, min(500, int((query.get("limit") or ["100"])[0])))
        except ValueError:
            limit = 100
        tokens = [token for token in text.split() if token]
        matches: list[dict[str, Any]] = []
        total = 0
        for entry in as_list(catalog.get("entries")):
            if not isinstance(entry, dict):
                continue
            if domain and entry.get("domain") != domain:
                continue
            if kind and entry.get("kind") != kind:
                continue
            if classification and entry.get("classification") != classification:
                continue
            haystack = str(entry.get("searchText") or "")
            if tokens and not all(token in haystack for token in tokens):
                continue
            total += 1
            if len(matches) < limit:
                matches.append(catalog_search_entry(entry))
        return {
            "schema": "nikami-fnv-character-studio-catalog-search-v1",
            "schemaMarkers": ["compact-catalog-search-v1"],
            "catalog": str(self.path or ""),
            "count": len(matches),
            "total": total,
            "entries": matches,
            "policy": {
                "generatedMetadataOnly": True,
                "noRetailAssetsCommitted": True,
            },
        }


class LiveAuthoringStore:
    def __init__(self, run_dir: Path, root: Path, path: Path | None = None) -> None:
        self.run_dir = run_dir.resolve()
        self.root = root.resolve()
        self.path = (path.resolve() if path is not None else self.run_dir / "live-authoring.json")
        self.lock = threading.Lock()
        if self.path.suffix.lower() != ".json":
            raise ValueError("live authoring path must be a generated JSON file")
        if not self._under_allowed_root(self.path):
            raise ValueError("live authoring path escaped generated proof outputs")

    def _under_allowed_root(self, path: Path) -> bool:
        resolved = path.resolve()
        allowed_roots = (self.run_dir, self.root)
        return any(resolved == root or root in resolved.parents for root in allowed_roots)

    def _default_controls(self) -> dict[str, Any]:
        controls: dict[str, Any] = {}
        for prefix in HEAD_SURFACE_PREFIXES:
            for suffix in HEAD_SURFACE_FLOAT_SUFFIXES:
                controls[f"{prefix}_{suffix}"] = -90.0 if suffix == "ROTATION_Z" and prefix in {
                    "OPENMW_FNV_HAIR",
                    "OPENMW_FNV_BROW",
                    "OPENMW_FNV_EYE",
                    "OPENMW_FNV_BEARD",
                    "OPENMW_FNV_MOUTH",
                } else 0.0
            for suffix in HEAD_SURFACE_BOOL_SUFFIXES:
                controls[f"{prefix}_{suffix}"] = False
        return controls

    def _default_doc(self) -> dict[str, Any]:
        return {
            "schema": LIVE_AUTHORING_SCHEMA,
            "schemaMarkers": [
                "runtime-live-authoring-v1",
                "head-surface-transform-controls-v1",
                "generated-control-file-only-v1",
            ],
            "path": str(self.path),
            "updatedAt": utc_now(),
            "controls": self._default_controls(),
            "policy": {
                "generatedProofOutputsOnly": True,
                "noRetailPayloadBytes": True,
                "numericRuntimeControlsOnly": True,
            },
        }

    def _write(self, doc: dict[str, Any]) -> None:
        write_generated_json(self.path, doc)

    def load(self) -> dict[str, Any]:
        with self.lock:
            if not self.path.is_file():
                doc = self._default_doc()
                self._write(doc)
                return doc
            doc = read_json(self.path)
            if not isinstance(doc, dict):
                raise ValueError("live authoring JSON must be an object")
            controls = doc.get("controls")
            if not isinstance(controls, dict):
                doc["controls"] = self._default_controls()
            doc["schema"] = LIVE_AUTHORING_SCHEMA
            doc["path"] = str(self.path)
            markers = doc.get("schemaMarkers") if isinstance(doc.get("schemaMarkers"), list) else []
            for marker in ["runtime-live-authoring-v1", "head-surface-transform-controls-v1"]:
                if marker not in markers:
                    markers.append(marker)
            doc["schemaMarkers"] = markers
            policy = doc.get("policy") if isinstance(doc.get("policy"), dict) else {}
            policy["noRetailPayloadBytes"] = True
            doc["policy"] = policy
            self._write(doc)
            return doc

    def update(self, payload: dict[str, Any]) -> dict[str, Any]:
        incoming = payload.get("controls") if isinstance(payload.get("controls"), dict) else payload
        if not isinstance(incoming, dict):
            raise ValueError("live authoring update must be an object")
        with self.lock:
            doc = self._default_doc() if payload.get("reset") is True or not self.path.is_file() else read_json(self.path)
            if not isinstance(doc, dict):
                doc = self._default_doc()
            controls = doc.get("controls")
            if not isinstance(controls, dict):
                controls = self._default_controls()
            applied: dict[str, Any] = {}
            for key, value in incoming.items():
                if key in {"schema", "schemaMarkers", "updatedAt", "path", "policy", "reset", "sessionId", "controls"}:
                    continue
                if key not in HEAD_SURFACE_CONTROL_KEYS:
                    raise ValueError(f"unsupported live authoring control: {key}")
                if key.endswith("_PIVOT_MODE"):
                    if not isinstance(value, bool):
                        raise ValueError(f"{key} must be boolean")
                    controls[key] = value
                else:
                    try:
                        controls[key] = float(value)
                    except (TypeError, ValueError) as exc:
                        raise ValueError(f"{key} must be numeric") from exc
                applied[key] = controls[key]
            doc.update(
                {
                    "schema": LIVE_AUTHORING_SCHEMA,
                    "path": str(self.path),
                    "updatedAt": utc_now(),
                    "controls": controls,
                    "lastApplied": applied,
                    "policy": {
                        "generatedProofOutputsOnly": True,
                        "noRetailPayloadBytes": True,
                        "numericRuntimeControlsOnly": True,
                    },
                }
            )
            doc["schemaMarkers"] = [
                "runtime-live-authoring-v1",
                "head-surface-transform-controls-v1",
                "generated-control-file-only-v1",
            ]
            self._write(doc)
            return doc


class LiveRuntimeCommandStore:
    def __init__(self, run_dir: Path, root: Path, path: Path | None = None) -> None:
        self.run_dir = run_dir.resolve()
        self.root = root.resolve()
        self.path = (path.resolve() if path is not None else self.run_dir / "live-runtime-command.json")
        self.lock = threading.RLock()
        if self.path.suffix.lower() != ".json":
            raise ValueError("live runtime command path must be a generated JSON file")
        if not self._under_allowed_root(self.path):
            raise ValueError("live runtime command path escaped generated proof outputs")

    def _under_allowed_root(self, path: Path) -> bool:
        resolved = path.resolve()
        allowed_roots = (self.run_dir, self.root)
        return any(resolved == root or root in resolved.parents for root in allowed_roots)

    def _default_doc(self) -> dict[str, Any]:
        return {
            "schema": LIVE_RUNTIME_SCHEMA,
            "schemaMarkers": [
                "runtime-live-target-switch-v1",
                "runtime-live-actor-kit-controls-v1",
                "generated-command-file-only-v1",
            ],
            "path": str(self.path),
            "updatedAt": utc_now(),
            "command": "set-actor-target",
            "actorTarget": "",
            "runtimeTarget": "",
            "actorKind": "",
            "entryId": "",
            "selectedTarget": "",
            "placedTarget": "",
            **{field: "" for field in LIVE_RUNTIME_SELECTOR_FIELDS},
            "selectors": {
                "phase": "",
                "parts": [],
                "partModels": [],
                "propSlots": [],
                "propModels": [],
                "animationSource": "",
                "animationStartPoint": "",
                "animationGroup": "",
                "dialogueMode": "",
                "fnvRotationMode": "",
                "neutralPreviewProfile": "",
            },
            "policy": {
                "generatedProofOutputsOnly": True,
                "noRetailPayloadBytes": True,
                "activeCellActorSwitchOnly": True,
                "actorKitSelectorControls": True,
                "baseNpcPreviewWhenInactive": True,
                "baseCreaturePreviewWhenInactive": True,
                "baseActorPreviewWhenInactive": True,
            },
        }

    def _write(self, doc: dict[str, Any]) -> None:
        write_generated_json(self.path, doc)

    def load(self) -> dict[str, Any]:
        with self.lock:
            if not self.path.is_file():
                doc = self._default_doc()
                self._write(doc)
                return doc
            doc = read_json(self.path)
            if not isinstance(doc, dict):
                raise ValueError("live runtime command JSON must be an object")
            default = self._default_doc()
            for key, value in default.items():
                if key not in doc:
                    doc[key] = value
            doc["schema"] = LIVE_RUNTIME_SCHEMA
            doc["path"] = str(self.path)
            policy = doc.get("policy") if isinstance(doc.get("policy"), dict) else {}
            policy.update(
                {
                    "generatedProofOutputsOnly": True,
                    "noRetailPayloadBytes": True,
                    "activeCellActorSwitchOnly": True,
                    "actorKitSelectorControls": True,
                    "baseNpcPreviewWhenInactive": True,
                    "baseCreaturePreviewWhenInactive": True,
                    "baseActorPreviewWhenInactive": True,
                }
            )
            doc["policy"] = policy
            if not isinstance(doc.get("selectors"), dict):
                doc["selectors"] = default["selectors"]
            markers = doc.get("schemaMarkers") if isinstance(doc.get("schemaMarkers"), list) else []
            for marker in [
                "runtime-live-target-switch-v1",
                "runtime-live-actor-kit-controls-v1",
                "generated-command-file-only-v1",
            ]:
                if marker not in markers:
                    markers.append(marker)
            doc["schemaMarkers"] = markers
            self._write(doc)
            return doc

    def update(self, payload: dict[str, Any]) -> dict[str, Any]:
        allowed = {
            "actorTarget",
            "runtimeTarget",
            "target",
            "actorKind",
            "entryId",
            "selectedTarget",
            "placedTarget",
            "command",
            "sessionId",
            "selectors",
            *LIVE_RUNTIME_SELECTOR_FIELDS,
            "phase",
            "phases",
            "parts",
            "partModels",
            "propSlots",
            "propModels",
            "animationSource",
            "animationStartPoint",
            "animationGroup",
            "dialogueMode",
        }
        unknown = sorted(str(key) for key in payload if key not in allowed)
        if unknown:
            raise ValueError(f"unsupported live runtime command field: {unknown[0]}")
        actor_kind = first_text(payload.get("actorKind"))
        if actor_kind and actor_kind not in {"npc", "creature", "auto"}:
            raise ValueError("live runtime actorKind must be npc, creature, or auto")
        command = first_text(payload.get("command"), "set-actor-target")
        if command not in {"set-actor-target", "update-actor-kit"}:
            raise ValueError("unsupported live runtime command")
        with self.lock:
            doc = self.load()
            target = first_text(
                payload.get("actorTarget"),
                payload.get("runtimeTarget"),
                payload.get("target"),
                doc.get("runtimeTarget"),
                doc.get("actorTarget"),
            )
            if not target:
                raise ValueError("live runtime command requires actorTarget")
            if not re.fullmatch(r"[-A-Za-z0-9_:.\\ ]{1,160}", target):
                raise ValueError("live runtime actorTarget contains unsupported characters")
            selectors = dict(doc.get("selectors") if isinstance(doc.get("selectors"), dict) else {})
            selector_payload = payload.get("selectors") if isinstance(payload.get("selectors"), dict) else {}
            applied: dict[str, str] = {}
            for field in LIVE_RUNTIME_SELECTOR_FIELDS:
                values = []
                for alias in LIVE_RUNTIME_SELECTOR_ALIASES[field]:
                    if alias in payload:
                        values.append(payload.get(alias))
                    if alias in selector_payload:
                        values.append(selector_payload.get(alias))
                if field in LIVE_RUNTIME_SELECTOR_LIST_FIELDS:
                    tokens: list[str] = []
                    for candidate in values:
                        tokens.extend(csv_values(candidate))
                    value = ",".join(tokens)
                else:
                    value = first_text(*values)
                if value:
                    if field == "characterBuilderPhase":
                        phase_values = csv_values(value)
                        value = phase_values[0] if phase_values else value
                    if not re.fullmatch(r"[-A-Za-z0-9_:.\\ /,]{0,300}", value):
                        raise ValueError(f"live runtime selector {field} contains unsupported characters")
                    doc[field] = value
                    applied[field] = value
            selectors.update(
                {
                    "phase": doc.get("characterBuilderPhase", ""),
                    "parts": csv_values(doc.get("actorKitParts")),
                    "partModels": csv_values(doc.get("actorKitPartModels")),
                    "propSlots": csv_values(doc.get("actorKitPropSlots")),
                    "propModels": csv_values(doc.get("actorKitPropModels")),
                    "animationSource": doc.get("actorKitAnimationSource", ""),
                    "animationStartPoint": doc.get("actorKitAnimationStartPoint", ""),
                    "animationGroup": doc.get("actorKitAnimationGroup", ""),
                    "dialogueMode": doc.get("actorKitDialogueMode", ""),
                    "fnvRotationMode": doc.get("fnvRotationMode", ""),
                    "neutralPreviewProfile": doc.get("neutralPreviewProfile", ""),
                }
            )
            doc.update(
                {
                    "schema": LIVE_RUNTIME_SCHEMA,
                    "schemaMarkers": [
                        "runtime-live-target-switch-v1",
                        "runtime-live-actor-kit-controls-v1",
                        "generated-command-file-only-v1",
                    ],
                    "path": str(self.path),
                    "updatedAt": utc_now(),
                    "command": command,
                    "actorTarget": target,
                    "runtimeTarget": target,
                    "actorKind": actor_kind,
                    "entryId": first_text(payload.get("entryId")),
                    "selectedTarget": first_text(payload.get("selectedTarget"), target),
                    "placedTarget": first_text(payload.get("placedTarget")),
                    "selectors": selectors,
                    "lastApplied": applied,
                    "policy": {
                        "generatedProofOutputsOnly": True,
                        "noRetailPayloadBytes": True,
                        "activeCellActorSwitchOnly": True,
                        "actorKitSelectorControls": True,
                        "baseNpcPreviewWhenInactive": True,
                        "baseCreaturePreviewWhenInactive": True,
                        "baseActorPreviewWhenInactive": True,
                    },
                }
            )
            self._write(doc)
            return doc


class StudioSessionStore:
    def __init__(self, run_dir: Path) -> None:
        self.root = run_dir / "studio-sessions"
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()

    def _session_dir(self, session_id: str) -> Path:
        if not re.fullmatch(r"session_[0-9]{8}_[0-9]{6}_[0-9]{4}", session_id):
            raise ValueError("invalid studio session id")
        path = self.root / session_id
        resolved = path.resolve()
        if self.root.resolve() not in resolved.parents and resolved != self.root.resolve():
            raise ValueError("studio session path escaped generated proof root")
        return resolved

    def create(self, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        payload = payload or {}
        with self.lock:
            session_id = time.strftime("session_%Y%m%d_%H%M%S") + f"_{len(list(self.root.glob('session_*'))) + 1:04d}"
            session_dir = self._session_dir(session_id)
            (session_dir / "recording").mkdir(parents=True, exist_ok=True)
            session = {
                "schema": STUDIO_SCHEMA,
                "id": session_id,
                "createdAt": utc_now(),
                "updatedAt": utc_now(),
                "selectedEntries": as_list(payload.get("selectedEntries") or payload.get("entryId")),
                "jobs": [],
                "eventsPath": str(session_dir / "recording" / "events.jsonl"),
                "reviewsPath": str(session_dir / "recording" / "reviews.jsonl"),
                "policy": {
                    "generatedMetadataOnly": True,
                    "noRetailPayloadBytes": True,
                    "generatedProofOutputsOnly": True,
                },
            }
            self._write(session)
            self.append_event(session_id, "session.create", {"selectedEntries": session["selectedEntries"]})
            return session

    def _write(self, session: dict[str, Any]) -> None:
        session_dir = self._session_dir(session["id"])
        session_dir.mkdir(parents=True, exist_ok=True)
        (session_dir / "session.json").write_text(json.dumps(session, indent=2), encoding="utf-8")

    def get(self, session_id: str) -> dict[str, Any] | None:
        path = self._session_dir(session_id) / "session.json"
        if not path.is_file():
            return None
        return read_json(path)

    def list(self) -> list[dict[str, Any]]:
        sessions: list[dict[str, Any]] = []
        for path in sorted(self.root.glob("session_*/session.json"), reverse=True)[:50]:
            try:
                sessions.append(read_json(path))
            except Exception:
                continue
        return sessions

    def append_event(self, session_id: str, event_type: str, payload: dict[str, Any]) -> dict[str, Any]:
        session = self.get(session_id)
        if session is None:
            raise ValueError("unknown studio session")
        session_dir = self._session_dir(session_id)
        events_path = session_dir / "recording" / "events.jsonl"
        events_path.parent.mkdir(parents=True, exist_ok=True)
        event = {
            "t": utc_now(),
            "type": event_type,
            "payload": payload,
        }
        with events_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, ensure_ascii=False) + "\n")
        session["updatedAt"] = utc_now()
        self._write(session)
        return event

    def append_reviews(self, session_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        session = self.get(session_id)
        if session is None:
            raise ValueError("unknown studio session")
        rows = payload.get("rows")
        if not isinstance(rows, list) or not rows:
            raise ValueError("component review payload must include non-empty rows")
        session_dir = self._session_dir(session_id)
        reviews_path = session_dir / "recording" / "reviews.jsonl"
        reviews_path.parent.mkdir(parents=True, exist_ok=True)
        accepted: list[dict[str, Any]] = []
        now = utc_now()
        for index, row in enumerate(rows):
            if not isinstance(row, dict):
                raise ValueError(f"component review row {index} is not an object")
            component = first_text(row.get("component"), row.get("id"))
            if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,80}", component):
                raise ValueError(f"component review row {index} has invalid component id")
            review_state = first_text(row.get("reviewState"), row.get("state"), "review-pending")
            if review_state not in REVIEW_STATES:
                raise ValueError(f"component review row {index} has invalid review state")
            proof_urls = [first_text(item) for item in as_list(row.get("proofUrls")) if first_text(item)]
            proof_urls = proof_urls[:12]
            accepted.append(
                {
                    "schema": STUDIO_REVIEW_SCHEMA,
                    "t": now,
                    "sessionId": session_id,
                    "entryId": first_text(payload.get("entryId"), row.get("entryId")),
                    "jobId": first_text(payload.get("jobId"), row.get("jobId")),
                    "component": component,
                    "label": first_text(row.get("label"))[:120],
                    "reviewState": review_state,
                    "machineStatus": first_text(row.get("machineStatus"), "not-run")[:80],
                    "target": first_text(row.get("target"))[:160],
                    "runtimeTarget": first_text(row.get("runtimeTarget"))[:160],
                    "placedTarget": first_text(row.get("placedTarget"))[:160],
                    "phase": first_text(row.get("phase"))[:80],
                    "selectors": row.get("selectors") if isinstance(row.get("selectors"), dict) else {},
                    "manifestUrl": first_text(row.get("manifestUrl"))[:500],
                    "viewerUrl": first_text(row.get("viewerUrl"))[:500],
                    "proofUrls": proof_urls,
                    "failureCount": int(row.get("failureCount") or 0),
                    "payloadPolicy": "generated review metadata only; no retail asset payload bytes",
                }
            )
        with reviews_path.open("a", encoding="utf-8") as stream:
            for row in accepted:
                stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        session["updatedAt"] = now
        self._write(session)
        self.append_event(session_id, "review.rows", {"count": len(accepted), "jobId": first_text(payload.get("jobId"))})
        return {"schema": STUDIO_REVIEW_SCHEMA, "accepted": len(accepted), "rows": accepted}

    def reviews(self, session_id: str) -> list[dict[str, Any]]:
        session_dir = self._session_dir(session_id)
        reviews_path = session_dir / "recording" / "reviews.jsonl"
        if not reviews_path.is_file():
            return []
        reviews: list[dict[str, Any]] = []
        for line in reviews_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                row = json.loads(line)
                if isinstance(row, dict):
                    reviews.append(row)
            except json.JSONDecodeError:
                reviews.append({"schema": STUDIO_REVIEW_SCHEMA, "t": utc_now(), "component": "decode-error"})
        return reviews

    def events(self, session_id: str) -> list[dict[str, Any]]:
        session_dir = self._session_dir(session_id)
        events_path = session_dir / "recording" / "events.jsonl"
        if not events_path.is_file():
            return []
        events: list[dict[str, Any]] = []
        for line in events_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.strip():
                continue
            try:
                event = json.loads(line)
                if isinstance(event, dict):
                    events.append(event)
            except json.JSONDecodeError:
                events.append({"t": utc_now(), "type": "recording.decode-error", "payload": {"line": line[:200]}})
        return events

    def add_job(self, session_id: str, job_id: str) -> None:
        session = self.get(session_id)
        if session is None:
            raise ValueError("unknown studio session")
        jobs = as_list(session.get("jobs"))
        jobs.append(job_id)
        session["jobs"] = jobs
        session["updatedAt"] = utc_now()
        self._write(session)


class JobStore:
    def __init__(self, run_dir: Path, root: Path, repo_root: Path, runner: Path) -> None:
        self.run_dir = run_dir
        self.root = root
        self.repo_root = repo_root
        self.runner = runner
        self.jobs_dir = run_dir / "live-jobs"
        self.jobs_dir.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.jobs: dict[str, dict[str, Any]] = {}

    def write_job(self, job: dict[str, Any]) -> None:
        job_path = self.jobs_dir / f"{job['id']}.json"
        job_path.write_text(json.dumps(job, indent=2), encoding="utf-8")

    def start(self, command: str, request: dict[str, Any] | None = None, session_id: str = "") -> dict[str, Any]:
        args = command_to_args(command, self.runner)
        job_id = time.strftime("%Y%m%d_%H%M%S") + f"_{len(self.jobs) + 1:04d}"
        job: dict[str, Any] = {
            "id": job_id,
            "sessionId": session_id,
            "state": "running",
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "finishedAt": "",
            "command": command,
            "request": request or {},
            "args": args,
            "returnCode": None,
            "stdout": "",
            "stderr": "",
            "result": {},
            "error": "",
            "failure": {},
            "policy": {
                "loopbackOnly": True,
                "generatedProofOutputsOnly": True,
                "noRetailAssetsCommitted": True,
                "allowedRunner": "scripts/nikami/run-fnv-character-viewer.ps1",
                "allowedItemRunner": "scripts/nikami/run-fnv-item-viewer.ps1",
            },
        }
        with self.lock:
            self.jobs[job_id] = job
            self.write_job(job)
        thread = threading.Thread(target=self._run, args=(job_id,), daemon=True)
        thread.start()
        return job

    def _run(self, job_id: str) -> None:
        with self.lock:
            job = self.jobs[job_id]
            args = list(job["args"])
        try:
            completed = subprocess.run(
                args,
                cwd=self.repo_root,
                text=True,
                capture_output=True,
                check=False,
            )
            result = parse_run_output(completed.stdout, self.root)
            with self.lock:
                job = self.jobs[job_id]
                job["state"] = "complete" if completed.returncode == 0 else "failed"
                job["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                job["returnCode"] = completed.returncode
                job["stdout"] = completed.stdout[-20000:]
                job["stderr"] = completed.stderr[-20000:]
                job["result"] = result
                if completed.returncode != 0:
                    job["error"] = f"viewer runner exited with code {completed.returncode}"
                    job["failure"] = {
                        "code": "runner-exit",
                        "stage": "runner",
                        "message": job["error"],
                        "returnCode": completed.returncode,
                        "stdoutTail": job["stdout"][-4000:],
                        "stderrTail": job["stderr"][-4000:],
                    }
                else:
                    job["error"] = ""
                    job["failure"] = {}
                self.write_job(job)
        except Exception as exc:  # pragma: no cover - defensive runtime guard
            with self.lock:
                job = self.jobs[job_id]
                job["state"] = "failed"
                job["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                job["stderr"] = str(exc)
                job["error"] = str(exc)
                job["failure"] = {
                    "code": "runner-exception",
                    "stage": "runner",
                    "message": str(exc),
                }
                self.write_job(job)

    def get(self, job_id: str) -> dict[str, Any] | None:
        with self.lock:
            return self.jobs.get(job_id)

    def list(self) -> list[dict[str, Any]]:
        with self.lock:
            return list(self.jobs.values())[-50:]


def first_text(*values: Any) -> str:
    for value in values:
        if value is None:
            continue
        text = str(value).strip()
        if text:
            return text
    return ""


def form_target(value: Any) -> str:
    text = first_text(value)
    if not text or text.lower().startswith("formid:"):
        return text
    if re.fullmatch(r"0x[0-9A-Fa-f]+", text):
        return f"FormId:{text}"
    return text


def selector_value(payload: dict[str, Any], key: str) -> Any:
    selectors = payload.get("selectors") if isinstance(payload.get("selectors"), dict) else {}
    if key in payload:
        return payload.get(key)
    return selectors.get(key)


def csv_values(value: Any) -> list[str]:
    values: list[str] = []
    for item in as_list(value):
        for part in str(item).split(","):
            text = part.strip()
            if text:
                values.append(text)
    return values


def selector_csv(payload: dict[str, Any], key: str) -> str:
    return ",".join(csv_values(selector_value(payload, key)))


def selector_arg(name: str, value: str) -> str:
    return f" -{name} {shell_quote(value)}" if value else ""


def numeric_arg(name: str, value: Any, required: bool = False) -> str:
    text = first_text(value)
    if not text:
        if required:
            raise ValueError(f"placement bootstrap is missing {name}")
        return ""
    if not re.fullmatch(r"-?[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?", text):
        raise ValueError(f"placement bootstrap has invalid numeric value for {name}")
    return f" -{name} {text}"


def placement_command_args(placement: Any) -> str:
    if not isinstance(placement, dict):
        return ""
    existing = first_text(placement.get("commandArgs"), placement.get("placementCommandArgs"))
    if existing:
        return " " + existing.strip()
    if not placement.get("runtimeBootstrapReady"):
        return ""
    position = placement.get("position") if isinstance(placement.get("position"), dict) else {}
    rotation = placement.get("rotation") if isinstance(placement.get("rotation"), dict) else {}
    args = ""
    cell = first_text(placement.get("cell"))
    if cell:
        args += f" -BootstrapCell {shell_quote(cell)}"
    args += numeric_arg("BootstrapX", position.get("x"), required=True)
    args += numeric_arg("BootstrapY", position.get("y"), required=True)
    args += numeric_arg("BootstrapZ", position.get("z"), required=True)
    args += numeric_arg("ActorStageX", position.get("x"), required=True)
    args += numeric_arg("ActorStageY", position.get("y"), required=True)
    args += numeric_arg("ActorStageZ", position.get("z"), required=True)
    args += numeric_arg("BootstrapRotX", rotation.get("x"))
    args += numeric_arg("ActorStageRotX", rotation.get("x"))
    args += numeric_arg("BootstrapRotY", rotation.get("y"))
    args += numeric_arg("ActorStageRotY", rotation.get("y"))
    args += numeric_arg("BootstrapRotZ", rotation.get("z"))
    args += numeric_arg("ActorStageRotZ", rotation.get("z"))
    return args


def target_mapping(entry: dict[str, Any], payload: dict[str, Any]) -> dict[str, Any]:
    source = first_text(payload.get("source"), entry.get("source"))
    catalog_target = first_text(entry.get("target"))
    actor_form_target = form_target(first_text(payload.get("actorFormId"), entry.get("actorFormId"), entry.get("actorFormTarget"), entry.get("formId")))
    placed_ref_form_target = form_target(first_text(payload.get("placedRefFormId"), entry.get("placedRefFormId"), entry.get("placedRefFormTarget")))
    placed_target = first_text(payload.get("placedTarget"), entry.get("placedTarget"))
    if not placed_target and source == "placed-reference":
        placed_target = first_text(catalog_target, entry.get("placedRefEditorId"), placed_ref_form_target)
    entry_runtime_target = first_text(entry.get("runtimeTarget"))
    if source == "placed-reference" and entry_runtime_target == placed_target:
        entry_runtime_target = ""
    runtime_target = first_text(
        payload.get("runtimeTarget"),
        entry_runtime_target,
        entry.get("assemblyTarget"),
        entry.get("baseActorTarget"),
        entry.get("actorEditorId"),
        entry.get("editorId"),
        actor_form_target,
    )
    if not runtime_target and source != "placed-reference":
        runtime_target = first_text(payload.get("target"), catalog_target)
    if not runtime_target:
        runtime_target = first_text(payload.get("target"), catalog_target)
    placement = payload.get("placement") if isinstance(payload.get("placement"), dict) else entry.get("placement")
    if not isinstance(placement, dict):
        placement = {}
    placement_args = first_text(payload.get("placementCommandArgs"), entry.get("placementCommandArgs"))
    if placement_args:
        placement = dict(placement)
        placement["commandArgs"] = placement_args
    return {
        "source": source,
        "selectedTarget": first_text(payload.get("selectedTarget"), entry.get("selectedTarget"), catalog_target, placed_target, runtime_target),
        "placedTarget": placed_target,
        "runtimeTarget": runtime_target,
        "actorFormTarget": actor_form_target,
        "placedRefFormTarget": placed_ref_form_target,
        "placement": placement,
        "placementCommandArgs": placement_args,
    }


def structured_actor_job(entry: dict[str, Any], payload: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    command_key = str(payload.get("command") or payload.get("commandKey") or "runtimeThreeCamera")
    if command_key not in {"runtimeThreeCamera", "runtimeFrontOnly"}:
        raise ValueError("unsupported structured studio command")
    actor_kind = first_text(payload.get("actorKind"), entry.get("kind"))
    if actor_kind not in {"npc", "creature"}:
        raise ValueError("structured studio job requires an actor or creature entry")
    targets = target_mapping(entry, payload)
    phases = csv_values(selector_value(payload, "phases") or entry.get("phases") or ["body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"])
    angles = csv_values(selector_value(payload, "angles") or ("front" if command_key == "runtimeFrontOnly" else "front,front-left,front-right"))
    if not targets["runtimeTarget"] or not phases or not angles:
        raise ValueError("structured studio job is missing runtimeTarget, phases, or angles")
    selectors = {
        "phases": phases,
        "angles": angles,
        "parts": csv_values(selector_value(payload, "parts")),
        "partModels": csv_values(selector_value(payload, "partModels")),
        "propSlots": csv_values(selector_value(payload, "propSlots")),
        "propModels": csv_values(selector_value(payload, "propModels")),
        "animationSource": first_text(selector_value(payload, "animationSource")),
        "animationStartPoint": first_text(selector_value(payload, "animationStartPoint")),
        "animationGroup": first_text(selector_value(payload, "animationGroup")),
        "dialogueMode": first_text(selector_value(payload, "dialogueMode")),
        "neutralPreviewProfile": first_text(selector_value(payload, "neutralPreviewProfile")),
        "fnvRotationMode": first_text(selector_value(payload, "fnvRotationMode")),
        "allowMissingActorVisibleHandGeometry": first_text(selector_value(payload, "allowMissingActorVisibleHandGeometry")),
        "actorVisibleHandMaxDistance": first_text(selector_value(payload, "actorVisibleHandMaxDistance"), "30"),
        "fnvSkinningMatrixAudit": first_text(selector_value(payload, "fnvSkinningMatrixAudit"), "arms,rightHand,leftHand,HeadOld,HeadHuman"),
        "fnvHairEmissionStrength": first_text(selector_value(payload, "fnvHairEmissionStrength")),
    }
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(targets['runtimeTarget'])} -ActorKind {actor_kind} -Phases {shell_quote(','.join(phases))}"
        f"{placement_command_args(targets['placement'])} -Angles {shell_quote(','.join(angles))}"
    )
    command += selector_arg("ActorKitParts", ",".join(selectors["parts"]))
    command += selector_arg("ActorKitPartModels", ",".join(selectors["partModels"]))
    command += selector_arg("ActorKitPropSlots", ",".join(selectors["propSlots"]))
    command += selector_arg("ActorKitPropModels", ",".join(selectors["propModels"]))
    command += selector_arg("ActorKitAnimationSource", selectors["animationSource"])
    command += selector_arg("ActorKitAnimationStartPoint", selectors["animationStartPoint"])
    command += selector_arg("ActorKitAnimationGroup", selectors["animationGroup"])
    command += selector_arg("ActorKitDialogueMode", selectors["dialogueMode"])
    command += selector_arg("NeutralActorPreviewProfile", selectors["neutralPreviewProfile"])
    command += selector_arg("FnvRotationMode", selectors["fnvRotationMode"])
    if actor_kind != "creature":
        command += selector_arg("ActorVisibleHandMaxDistance", selectors["actorVisibleHandMaxDistance"])
        if selectors["allowMissingActorVisibleHandGeometry"].lower() in {"1", "true", "yes"}:
            command += " -AllowMissingActorVisibleHandGeometry"
    command += selector_arg("FnvSkinningMatrixAudit", selectors["fnvSkinningMatrixAudit"])
    command += selector_arg("FnvHairEmissionStrength", selectors["fnvHairEmissionStrength"])
    if actor_kind == "creature":
        command += " -CreatureDiagnostics"
    request = {
        "schema": "nikami-fnv-character-studio-job-request-v1",
        "kind": first_text(payload.get("kind"), "actor-runtime-capture"),
        "entryId": first_text(payload.get("entryId"), entry.get("id")),
        "commandKey": command_key,
        "actorKind": actor_kind,
        "target": targets["runtimeTarget"],
        "selectedTarget": targets["selectedTarget"],
        "placedTarget": targets["placedTarget"],
        "runtimeTarget": targets["runtimeTarget"],
        "actorFormTarget": targets["actorFormTarget"],
        "placedRefFormTarget": targets["placedRefFormTarget"],
        "placement": targets["placement"],
        "placementCommandArgs": targets["placementCommandArgs"],
        "selectors": selectors,
    }
    return command, request


def structured_item_job(entry: dict[str, Any], payload: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    command_key = str(payload.get("command") or payload.get("commandKey") or "runtimeThreeCamera")
    if command_key not in {"runtimeThreeCamera", "runtimeFrontOnly"}:
        raise ValueError("unsupported structured studio command")
    if entry.get("domain") != "gameplay":
        raise ValueError("structured item job requires a gameplay catalog entry")
    model = first_text(payload.get("model"), entry.get("model"))
    target = first_text(payload.get("target"), entry.get("target"), entry.get("label"), entry.get("formId"))
    if not model:
        raise ValueError("structured item job requires a harvested model path")
    if not target:
        raise ValueError("structured item job requires an item target")
    angles = csv_values(selector_value(payload, "angles") or ("front" if command_key == "runtimeFrontOnly" else "front,front-left,front-right"))
    if not angles:
        raise ValueError("structured item job is missing camera angles")
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-item-viewer.ps1 "
        f"-ItemTarget {shell_quote(target)} "
        f"-ItemKind {shell_quote(first_text(payload.get('itemKind'), entry.get('kind')))} "
        f"-ItemRecordType {shell_quote(first_text(payload.get('recordType'), entry.get('recordType')))} "
        f"-ItemFormId {shell_quote(first_text(payload.get('formId'), entry.get('formId')))} "
        f"-ItemPlugin {shell_quote(first_text(payload.get('plugin'), entry.get('plugin')))} "
        f"-ItemModel {shell_quote(model)} -Angles {shell_quote(','.join(angles))} -RequirePass"
    )
    request = {
        "schema": "nikami-fnv-character-studio-job-request-v1",
        "kind": first_text(payload.get("kind"), "item-runtime-visual-spawn"),
        "entryId": first_text(payload.get("entryId"), entry.get("id")),
        "commandKey": command_key,
        "target": target,
        "runtimeTarget": target,
        "placedTarget": "",
        "itemKind": first_text(payload.get("itemKind"), entry.get("kind")),
        "recordType": first_text(payload.get("recordType"), entry.get("recordType")),
        "formId": first_text(payload.get("formId"), entry.get("formId")),
        "plugin": first_text(payload.get("plugin"), entry.get("plugin")),
        "model": model,
        "selectors": {
            "angles": angles,
            "parts": [],
            "partModels": [],
            "propSlots": [],
            "propModels": [model],
            "animationSource": "",
            "animationStartPoint": "",
            "animationGroup": "",
            "dialogueMode": "",
        },
        "gates": [
            {
                "gate": "runtime-visual-model-spawn",
                "classification": "loaded-pending-runtime",
                "proof": "Job must run PC-flat item viewer before this row has runtime visual proof.",
            },
            {
                "gate": "inventory-equip-or-activate-behavior",
                "classification": "loaded-pending-runtime",
                "proof": "Visual model spawn does not claim equip, activation, pickup, collision, or gameplay behavior.",
            },
        ],
    }
    return command, request


def structured_studio_job(entry: dict[str, Any], payload: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    if entry.get("domain") == "gameplay":
        return structured_item_job(entry, payload)
    return structured_actor_job(entry, payload)


def structured_actor_command(entry: dict[str, Any], payload: dict[str, Any]) -> str:
    command, _request = structured_actor_job(entry, payload)
    return command


class LiveHandler(SimpleHTTPRequestHandler):
    server_version = "NikamiFNVCharacterViewerLive/1.0"

    def send_json(self, status: int, payload: dict[str, Any] | list[dict[str, Any]]) -> None:
        data = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def read_payload(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        payload = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(payload, dict):
            raise ValueError("JSON payload must be an object")
        return payload

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/nikami/health":
            self.send_json(
                200,
                {
                    "status": "PASS",
                    "server": self.server_version,
                    "policy": {
                        "loopbackOnly": True,
                        "generatedProofOutputsOnly": True,
                        "noRetailAssetsCommitted": True,
                    },
                    "liveAuthoring": {
                        "schema": LIVE_AUTHORING_SCHEMA,
                        "endpoint": "/nikami/live-authoring",
                        "path": str(self.server.live_authoring_store.path),  # type: ignore[attr-defined]
                    },
                    "liveRuntime": {
                        "schema": LIVE_RUNTIME_SCHEMA,
                        "endpoint": "/nikami/live-runtime",
                        "path": str(self.server.live_runtime_store.path),  # type: ignore[attr-defined]
                    },
                    "runtimeStatus": {
                        "schema": LIVE_RUNTIME_STATUS_SCHEMA,
                        "endpoint": "/nikami/runtime-status",
                    },
                    "runtimeAudit": {
                        "schema": LIVE_RUNTIME_AUDIT_SCHEMA,
                        "endpoint": "/nikami/runtime-audit",
                    },
                },
            )
            return
        if parsed.path == "/nikami/live-authoring":
            self.send_json(200, self.server.live_authoring_store.load())  # type: ignore[attr-defined]
            return
        if parsed.path == "/nikami/live-runtime":
            self.send_json(200, self.server.live_runtime_store.load())  # type: ignore[attr-defined]
            return
        if parsed.path == "/nikami/runtime-status":
            self.send_json(
                200,
                runtime_status(
                    self.server.run_dir,  # type: ignore[attr-defined]
                    self.server.root_dir,  # type: ignore[attr-defined]
                    self.server.live_authoring_store.path,  # type: ignore[attr-defined]
                    self.server.live_runtime_store.path,  # type: ignore[attr-defined]
                ),
            )
            return
        if parsed.path == "/nikami/runtime-audit":
            self.send_json(
                200,
                runtime_audit(
                    self.server.run_dir,  # type: ignore[attr-defined]
                    self.server.root_dir,  # type: ignore[attr-defined]
                    self.server.live_authoring_store.path,  # type: ignore[attr-defined]
                    self.server.live_runtime_store.path,  # type: ignore[attr-defined]
                ),
            )
            return
        if parsed.path == "/nikami/catalog":
            try:
                self.send_json(200, self.server.catalog_store.load())  # type: ignore[attr-defined]
            except Exception as exc:
                self.send_json(404, {"error": str(exc)})
            return
        if parsed.path == "/nikami/catalog/search":
            try:
                self.send_json(200, self.server.catalog_store.search(parse_qs(parsed.query)))  # type: ignore[attr-defined]
            except Exception as exc:
                self.send_json(400, {"error": str(exc)})
            return
        if parsed.path.startswith("/nikami/catalog/entries/"):
            entry_id = unquote(parsed.path.rsplit("/", 1)[-1])
            try:
                entry = self.server.catalog_store.entry(entry_id)  # type: ignore[attr-defined]
            except Exception as exc:
                self.send_json(404, {"error": str(exc)})
                return
            if entry is None:
                self.send_json(404, {"error": "unknown catalog entry"})
                return
            self.send_json(200, entry)
            return
        if parsed.path == "/nikami/studio/sessions":
            self.send_json(200, self.server.studio_store.list())  # type: ignore[attr-defined]
            return
        if parsed.path.startswith("/nikami/studio/sessions/"):
            parts = [part for part in parsed.path.split("/") if part]
            if len(parts) == 4:
                session = self.server.studio_store.get(unquote(parts[-1]))  # type: ignore[attr-defined]
                if session is None:
                    self.send_json(404, {"error": "unknown studio session"})
                    return
                self.send_json(200, session)
                return
            if len(parts) == 5 and parts[-1] == "events":
                session_id = unquote(parts[-2])
                self.send_json(200, self.server.studio_store.events(session_id))  # type: ignore[attr-defined]
                return
            if len(parts) == 5 and parts[-1] == "reviews":
                session_id = unquote(parts[-2])
                session = self.server.studio_store.get(session_id)  # type: ignore[attr-defined]
                if session is None:
                    self.send_json(404, {"error": "unknown studio session"})
                    return
                self.send_json(200, self.server.studio_store.reviews(session_id))  # type: ignore[attr-defined]
                return
            if len(parts) == 5 and parts[-1] == "jobs":
                session_id = unquote(parts[-2])
                session = self.server.studio_store.get(session_id)  # type: ignore[attr-defined]
                if session is None:
                    self.send_json(404, {"error": "unknown studio session"})
                    return
                jobs = []
                for job_id in as_list(session.get("jobs")):
                    job = self.server.job_store.get(str(job_id))  # type: ignore[attr-defined]
                    jobs.append(job if job is not None else {"id": str(job_id), "state": "missing"})
                self.send_json(200, jobs)
                return
        if parsed.path.startswith("/nikami/studio/jobs/"):
            job_id = unquote(parsed.path.rsplit("/", 1)[-1])
            job = self.server.job_store.get(job_id)  # type: ignore[attr-defined]
            if job is None:
                self.send_json(404, {"error": "unknown job"})
                return
            self.send_json(200, job)
            return
        if parsed.path == "/nikami/actor-kit/jobs":
            self.send_json(200, self.server.job_store.list())  # type: ignore[attr-defined]
            return
        if parsed.path.startswith("/nikami/actor-kit/jobs/"):
            job_id = unquote(parsed.path.rsplit("/", 1)[-1])
            job = self.server.job_store.get(job_id)  # type: ignore[attr-defined]
            if job is None:
                self.send_json(404, {"error": "unknown job"})
                return
            self.send_json(200, job)
            return
        super().do_GET()

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/nikami/live-authoring":
                payload = self.read_payload()
                doc = self.server.live_authoring_store.update(payload)  # type: ignore[attr-defined]
                session_id = str(payload.get("sessionId") or "")
                if session_id:
                    self.server.studio_store.append_event(  # type: ignore[attr-defined]
                        session_id,
                        "live-authoring.update",
                        {
                            "path": doc.get("path", ""),
                            "controls": doc.get("lastApplied", {}),
                            "schema": LIVE_AUTHORING_SCHEMA,
                        },
                    )
                self.send_json(200, doc)
                return
            if parsed.path == "/nikami/live-runtime":
                payload = self.read_payload()
                doc = self.server.live_runtime_store.update(payload)  # type: ignore[attr-defined]
                session_id = str(payload.get("sessionId") or "")
                if session_id:
                    self.server.studio_store.append_event(  # type: ignore[attr-defined]
                        session_id,
                        "live-runtime.update",
                        {
                            "path": doc.get("path", ""),
                            "actorTarget": doc.get("actorTarget", ""),
                            "actorKind": doc.get("actorKind", ""),
                            "selectors": doc.get("selectors", {}),
                            "lastApplied": doc.get("lastApplied", {}),
                            "schema": LIVE_RUNTIME_SCHEMA,
                        },
                    )
                self.send_json(200, doc)
                return
            if parsed.path == "/nikami/studio/sessions":
                session = self.server.studio_store.create(self.read_payload())  # type: ignore[attr-defined]
                self.send_json(201, session)
                return
            if parsed.path.startswith("/nikami/studio/sessions/"):
                parts = [part for part in parsed.path.split("/") if part]
                if len(parts) == 5 and parts[-1] == "events":
                    session_id = unquote(parts[-2])
                    payload = self.read_payload()
                    event = self.server.studio_store.append_event(  # type: ignore[attr-defined]
                        session_id, str(payload.get("type") or "note"), payload.get("payload") if isinstance(payload.get("payload"), dict) else payload
                    )
                    self.send_json(201, event)
                    return
                if len(parts) == 5 and parts[-1] == "reviews":
                    session_id = unquote(parts[-2])
                    payload = self.read_payload()
                    reviews = self.server.studio_store.append_reviews(session_id, payload)  # type: ignore[attr-defined]
                    self.send_json(201, reviews)
                    return
                if len(parts) == 5 and parts[-1] == "jobs":
                    session_id = unquote(parts[-2])
                    payload = self.read_payload()
                    entry_id = str(payload.get("entryId") or "")
                    entry = self.server.catalog_store.entry(entry_id)  # type: ignore[attr-defined]
                    if entry is None:
                        raise ValueError("unknown catalog entry")
                    command, request = structured_studio_job(entry, payload)
                    if "run-fnv-character-viewer.ps1" in command:
                        live_path = str(self.server.live_authoring_store.path)  # type: ignore[attr-defined]
                        command += " -LiveAuthoringFile " + shell_quote(live_path)
                        request["liveAuthoringFile"] = live_path
                    job = self.server.job_store.start(command, request, session_id)  # type: ignore[attr-defined]
                    self.server.studio_store.add_job(session_id, job["id"])  # type: ignore[attr-defined]
                    self.server.studio_store.append_event(
                        session_id,
                        "job.create",
                        {
                            "jobId": job["id"],
                            "entryId": entry_id,
                            "placedTarget": request.get("placedTarget", ""),
                            "runtimeTarget": request.get("runtimeTarget", ""),
                            "selectors": request.get("selectors", {}),
                        },
                    )  # type: ignore[attr-defined]
                    self.send_json(202, job)
                    return
            if parsed.path != "/nikami/actor-kit/run":
                self.send_json(404, {"error": "unknown endpoint"})
                return
            payload = self.read_payload()
            command = str(payload.get("command") or "")
            job = self.server.job_store.start(command)  # type: ignore[attr-defined]
            self.send_json(202, job)
        except Exception as exc:
            self.send_json(
                400,
                {
                    "error": str(exc),
                    "failure": {
                        "code": "studio-request-invalid",
                        "stage": "request",
                        "message": str(exc),
                    },
                },
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--catalog-path", type=Path)
    parser.add_argument("--live-authoring-path", type=Path)
    parser.add_argument("--live-runtime-path", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()

    root = args.root.resolve()
    repo_root = args.repo_root.resolve()
    run_dir = args.run_dir.resolve()
    runner = args.runner.resolve()
    os.chdir(root)
    handler = lambda *handler_args, **handler_kwargs: LiveHandler(  # noqa: E731
        *handler_args, directory=str(root), **handler_kwargs
    )
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    httpd.run_dir = run_dir  # type: ignore[attr-defined]
    httpd.root_dir = root  # type: ignore[attr-defined]
    httpd.job_store = JobStore(run_dir, root, repo_root, runner)  # type: ignore[attr-defined]
    httpd.catalog_store = CatalogStore(root, args.catalog_path.resolve() if args.catalog_path else None)  # type: ignore[attr-defined]
    httpd.studio_store = StudioSessionStore(run_dir)  # type: ignore[attr-defined]
    httpd.live_authoring_store = LiveAuthoringStore(  # type: ignore[attr-defined]
        run_dir, root, args.live_authoring_path.resolve() if args.live_authoring_path else None
    )
    httpd.live_authoring_store.load()  # type: ignore[attr-defined]
    httpd.live_runtime_store = LiveRuntimeCommandStore(  # type: ignore[attr-defined]
        run_dir, root, args.live_runtime_path.resolve() if args.live_runtime_path else None
    )
    httpd.live_runtime_store.load()  # type: ignore[attr-defined]
    print(f"live-viewer-server=http://{args.host}:{args.port}", flush=True)
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
