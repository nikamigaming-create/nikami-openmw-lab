#!/usr/bin/env python3
"""Loopback live server for the generated FNV character viewer.

The server serves generated proof files from --root and exposes one local-only
actor-kit endpoint that can run the generated viewer rerun commands. It stores
job metadata under --run-dir and never serves or writes retail asset payloads.
"""

from __future__ import annotations

import argparse
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
FORBIDDEN_CHARS = set("&;|<>`")
STUDIO_SCHEMA = "nikami-fnv-character-studio-session-v1"


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


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


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
    if len(tokens) < len(ALLOWED_PREFIX):
        raise ValueError("command is too short")
    normalized = tokens[: len(ALLOWED_PREFIX)]
    normalized[0] = normalized[0].lower().removesuffix(".exe")
    if normalized != ALLOWED_PREFIX:
        raise ValueError("command does not match generated actor-kit runner prefix")
    args = tokens[:]
    args[0] = "powershell"
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
    def __init__(self, root: Path) -> None:
        self.root = root
        self.lock = threading.Lock()
        self.path: Path | None = None
        self.catalog: dict[str, Any] = {}

    def _latest_catalog_path(self) -> Path:
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
            matches.append(entry)
            if len(matches) >= limit:
                break
        return {
            "schema": "nikami-fnv-character-studio-catalog-search-v1",
            "catalog": str(self.path or ""),
            "count": len(matches),
            "entries": matches,
            "policy": {
                "generatedMetadataOnly": True,
                "noRetailAssetsCommitted": True,
            },
        }


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
            "policy": {
                "loopbackOnly": True,
                "generatedProofOutputsOnly": True,
                "noRetailAssetsCommitted": True,
                "allowedRunner": "scripts/nikami/run-fnv-character-viewer.ps1",
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
                self.write_job(job)
        except Exception as exc:  # pragma: no cover - defensive runtime guard
            with self.lock:
                job = self.jobs[job_id]
                job["state"] = "failed"
                job["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                job["stderr"] = str(exc)
                self.write_job(job)

    def get(self, job_id: str) -> dict[str, Any] | None:
        with self.lock:
            return self.jobs.get(job_id)

    def list(self) -> list[dict[str, Any]]:
        with self.lock:
            return list(self.jobs.values())[-50:]


def structured_actor_command(entry: dict[str, Any], payload: dict[str, Any]) -> str:
    commands = entry.get("commands") if isinstance(entry.get("commands"), dict) else {}
    command_key = str(payload.get("command") or payload.get("commandKey") or "runtimeThreeCamera")
    if command_key not in {"runtimeThreeCamera", "runtimeFrontOnly"}:
        raise ValueError("unsupported structured studio command")
    command = str(commands.get(command_key) or "")
    if command:
        return command

    target = str(payload.get("target") or entry.get("target") or "")
    actor_kind = str(payload.get("actorKind") or entry.get("kind") or "")
    if actor_kind not in {"npc", "creature"}:
        raise ValueError("structured studio job requires an actor or creature entry")
    phases = payload.get("phases") or entry.get("phases") or ["body", "head", "face", "hair", "equipment", "weapon", "headgear", "talk"]
    angles = payload.get("angles") or ("front" if command_key == "runtimeFrontOnly" else "front,front-left,front-right")
    phases_csv = ",".join(str(value) for value in as_list(phases) if str(value))
    angles_csv = ",".join(str(value) for value in as_list(angles) if str(value))
    if not target or not phases_csv or not angles_csv:
        raise ValueError("structured studio job is missing target, phases, or angles")
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(target)} -ActorKind {actor_kind} -Phases {shell_quote(phases_csv)} "
        f"-Angles {shell_quote(angles_csv)}"
    )
    if actor_kind == "creature":
        command += " -CreatureDiagnostics"
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
                },
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
                if len(parts) == 5 and parts[-1] == "jobs":
                    session_id = unquote(parts[-2])
                    payload = self.read_payload()
                    entry_id = str(payload.get("entryId") or "")
                    entry = self.server.catalog_store.entry(entry_id)  # type: ignore[attr-defined]
                    if entry is None:
                        raise ValueError("unknown catalog entry")
                    command = structured_actor_command(entry, payload)
                    job = self.server.job_store.start(command, payload, session_id)  # type: ignore[attr-defined]
                    self.server.studio_store.add_job(session_id, job["id"])  # type: ignore[attr-defined]
                    self.server.studio_store.append_event(session_id, "job.create", {"jobId": job["id"], "entryId": entry_id})  # type: ignore[attr-defined]
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
            self.send_json(400, {"error": str(exc)})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
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
    httpd.job_store = JobStore(run_dir, root, repo_root, runner)  # type: ignore[attr-defined]
    httpd.catalog_store = CatalogStore(root)  # type: ignore[attr-defined]
    httpd.studio_store = StudioSessionStore(run_dir)  # type: ignore[attr-defined]
    print(f"live-viewer-server=http://{args.host}:{args.port}", flush=True)
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
